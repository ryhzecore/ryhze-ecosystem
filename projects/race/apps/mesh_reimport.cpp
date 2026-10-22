#include "mesh_reimport.hpp"

namespace race::mesh_import {
namespace m=assets::mesh;
namespace p=platform::process;
namespace {
bool quiescent(const p::Snapshot& s){return s.phase==p::Phase::Completed&&s.stdout_finished&&s.stderr_finished;}
}
bool Reimport::owner_thread() const noexcept {return thread_==std::this_thread::get_id();}
Result<std::unique_ptr<Reimport>> Reimport::create(std::uint64_t identity,assets::AssetId id,
    std::shared_ptr<platform::RootedFiles> output,TrackedAllocator allocator,const ReimportLimits& limits,mesh_pipeline::Profile profile){
  auto admitted=mesh_pipeline::profile_import_stamp(profile,{});if(!admitted)return admitted.error();
  if(!identity||!id.valid()||!output||!limits.generations)
    return fail(ErrorCode::InvalidInput,"Invalid reimport owner or limits");
  auto store=m::Store::create(limits.residency,std::move(allocator));if(!store)return store.error();
  try{
    auto result=std::unique_ptr<Reimport>(new Reimport);
    result->identity_=identity;result->id_=id;result->limits_=limits;result->profile_=profile;
    result->store_=std::move(store).value();result->output_=std::move(output);return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Reimport owner allocation");}
}
void Reimport::clear() noexcept {
  if(update_){(void)store_->cancel(*update_);update_.reset();}
  job_.reset();active_.reset();
}
Result<m::ResourceHandle> Reimport::restore_initial(std::span<const std::byte> bytes){
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Initial mesh restoration requires owner");
  if(generation_||attempted_||desired_||active_||update_||job_)return fail(ErrorCode::Busy,"Initial mesh restoration requires a fresh owner");
  auto existing=store_->current(id_);if(existing)return fail(ErrorCode::Busy,"Initial mesh restoration cannot replace current");
  if(existing.error().code!=ErrorCode::NotFound)return existing.error();
  auto view=m::validate(bytes,limits_.residency);if(!view)return view.error();
  const auto expected=mesh_pipeline::cook_stamp(mesh_pipeline::profile_import_stamp(profile_,view.value().stamp().source).value());
  if(view.value().id()!=id_||view.value().stamp()!=expected||view.value().has_uv()!=(profile_==mesh_pipeline::Profile::GeometryUv))
    return fail(ErrorCode::InvalidInput,"Initial mesh restoration provenance differs");
  auto update=store_->begin_update(id_,view.value().stamp());if(!update)return update.error();
  auto published=store_->publish(update.value(),bytes);
  if(!published)(void)store_->cancel(update.value());return published;
}
Result<void> Reimport::invalidate(){
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport invalidation requires owner");
  desired_.reset();
  if(update_){(void)store_->cancel(*update_);update_.reset();}
  // Even failed cancellation or exhausted identities obsolete the old job.
  if(job_){auto stopped=job_->cancel();if(!stopped)return stopped.error();}
  return {};
}
Result<Desired> Reimport::desire(assets::Digest source){
  auto invalidated=invalidate();if(!invalidated)return invalidated.error();
  if(generation_==limits_.generations)return fail(ErrorCode::Exhausted,"Reimport generations exhausted");
  desired_=Desired{identity_,++generation_,source};return *desired_;
}
Result<void> Reimport::start(Desired desired,std::unique_ptr<p::Service> process,
    std::shared_ptr<platform::RootedFiles> stage,std::string_view executable,
    std::string_view directory,std::span<const std::byte> source){
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport start requires owner");
  if(!desired_||desired!=*desired_)return fail(ErrorCode::StaleHandle,"Obsolete desired mesh source");
  if(job_)return fail(ErrorCode::Busy,"Reimport worker slot is retained");
  if(attempted_==desired.generation)return fail(ErrorCode::Busy,"Reimport generation already attempted");
  if(source.empty())return fail(ErrorCode::InvalidInput,"Reimport source is empty");
  if(source.size()>mesh_pipeline::Limits{}.source_bytes)return fail(ErrorCode::Exhausted,"Reimport source limit");
  if(assets::digest(source)!=desired.source)return fail(ErrorCode::InvalidInput,"Reimport desired source mismatch");
  attempted_=desired.generation;
  const auto stamp=mesh_pipeline::cook_stamp(mesh_pipeline::profile_import_stamp(profile_,desired.source).value());
  auto update=store_->begin_update(id_,stamp);if(!update)return update.error();update_=update.value();
  auto job=Job::start(std::move(process),std::move(stage),output_,executable,directory,id_,source,profile_);
  if(!job){clear();return job.error();}
  active_=desired;job_=std::move(job).value();return {};
}
Result<p::Snapshot> Reimport::poll() noexcept {
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport poll requires owner");
  if(!job_)return fail(ErrorCode::NotFound,"No retained reimport job");return job_->poll();
}
Result<p::Snapshot> Reimport::wait(std::uint32_t milliseconds) noexcept {
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport wait requires owner");
  if(!job_)return fail(ErrorCode::NotFound,"No retained reimport job");return job_->wait(milliseconds);
}
Result<m::ResourceHandle> Reimport::finish(Desired desired){
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport finish requires owner");
  if(!desired_||desired!=*desired_||!active_||desired!=*active_||!update_)
    return fail(ErrorCode::StaleHandle,"Obsolete reimport completion");
  auto state=poll();if(!state)return state.error();
  if(!quiescent(state.value()))return fail(ErrorCode::Busy,"Reimport worker is not quiescent");
  struct Clear {Reimport* self;~Clear(){self->clear();}} guard{this};
  try{
    auto bytes=job_->take_product();if(!bytes)return bytes.error();
    auto imported=mesh_pipeline::publish(*output_,bytes.value(),limits_.residency);if(!imported)return imported.error();
    auto cooked=mesh_pipeline::cook(bytes.value(),limits_.residency);if(!cooked)return cooked.error();
    auto published=mesh_pipeline::publish(*output_,cooked.value(),limits_.residency);if(!published)return published.error();
    return store_->publish(*update_,cooked.value());
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Reimport completion allocation");}
}
Result<void> Reimport::reap() noexcept {
  auto state=poll();if(!state)return state.error();
  if(!quiescent(state.value()))return fail(ErrorCode::Busy,"Reimport reap requires quiescence");
  clear();return {};
}
Result<m::ResourceHandle> Reimport::current() const {return store_->current(id_);}
Result<m::Lease> Reimport::acquire(m::ResourceHandle handle) const {return store_->acquire(handle);}
m::Stats Reimport::residency() const {return store_->stats();}
Result<std::size_t> Reimport::pending_source_bytes() const noexcept {
  if(!owner_thread())return fail(ErrorCode::WrongThread,"Reimport source count requires owner");
  return job_?job_->retained_source_bytes():0;
}
} // namespace race::mesh_import
