#include "mesh_import_job.hpp"
#include <array>

namespace race::mesh_import {
Result<std::unique_ptr<Job>> Job::start(std::unique_ptr<platform::process::Service> process,
    std::shared_ptr<platform::RootedFiles> staging,std::shared_ptr<platform::RootedFiles> output,
    std::string_view executable,std::string_view directory,assets::AssetId id,std::span<const std::byte> source,mesh_pipeline::Profile profile){
  auto admitted=mesh_pipeline::profile_import_stamp(profile,{});if(!admitted)return admitted.error();
  if(!process||!staging||!output||staging==output||!id.valid()||source.empty())
    return fail(ErrorCode::InvalidInput,"Invalid mesh worker admission");
  if(source.size()>mesh_pipeline::Limits{}.source_bytes)return fail(ErrorCode::Exhausted,"Mesh worker source limit");
  try{
    auto job=std::unique_ptr<Job>{new Job};
    job->staging_=std::move(staging);job->output_=std::move(output);job->process_=std::move(process);job->id_=id;
    // No previous source/result is admissible, even if byte-identical. Fresh
    // staging identities prevent an old successful output satisfying a new job.
    for(auto leaf:{"input.obj","result.rasset"}){
      auto existing=job->staging_->read(leaf,1);
      if(existing||existing.error().code!=ErrorCode::NotFound)return fail(ErrorCode::InvalidInput,"Mesh worker staging is not fresh");
    }
    job->source_.assign(source.begin(),source.end());
    if(job->source_.capacity()>mesh_pipeline::Limits{}.source_bytes)return fail(ErrorCode::Exhausted,"Mesh worker source capacity limit");
    job->profile_=profile;
    job->expected_=mesh_pipeline::profile_import_stamp(profile,assets::digest(job->source_)).value();
    auto written=job->staging_->publish_immutable("input.obj",job->source_);if(!written)return written.error();
    auto readback=job->staging_->read("input.obj",mesh_pipeline::Limits{}.source_bytes);if(!readback)return readback.error();
    if(readback.value()!=job->source_)return fail(ErrorCode::IoFailure,"Mesh worker source readback mismatch");
    // Launch copies all arguments. Child sees only staging, not final output.
    const auto identity=assets::id_text(id);
    const std::array<std::string_view,3> args{directory,identity,"geometry-uv"};
    const std::span<const std::string_view> arguments(args.data(),profile==mesh_pipeline::Profile::GeometryUv?3:2);
    auto started=job->process_->start({executable,directory,arguments});if(!started)return started.error();
    job->request_=started.value();return job;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Mesh worker admission allocation");}
}
namespace {
Result<platform::process::Snapshot> checked(Result<platform::process::Snapshot> state,platform::process::Request request){
  if(state&&state.value().request!=request)return fail(ErrorCode::Invariant,"Mesh worker completion request mismatch");
  return state;
}
}
Result<platform::process::Snapshot> Job::poll() noexcept {return checked(process_->poll(request_),request_);}
Result<platform::process::Snapshot> Job::wait(std::uint32_t milliseconds) noexcept {return checked(process_->wait(request_,milliseconds),request_);}
Result<std::span<const std::byte>> Job::stderr_prefix() const noexcept {return process_->stderr_prefix(request_);}
Result<void> Job::cancel() noexcept {
  if(publication_attempted_)return fail(ErrorCode::Busy,"Mesh publication was already attempted");
  // Check owner affinity before changing local state.
  auto stopped=process_->request_stop(request_);if(!stopped)return stopped.error();cancelled_=true;return {};
}
Result<std::vector<std::byte>> Job::take_product(){
  auto observed=poll();if(!observed)return observed.error();
  if(observed.value().phase!=platform::process::Phase::Completed)return fail(ErrorCode::Busy,"Mesh worker is not quiescent");
  if(publication_attempted_)return fail(ErrorCode::Busy,"Mesh publication was already attempted");
  publication_attempted_=true;
  if(cancelled_)return fail(ErrorCode::Cancelled,"Mesh worker admission cancelled");
  if(observed.value().failure)return *observed.value().failure;
  if(!observed.value().exit_code||*observed.value().exit_code!=0||!observed.value().stdout_finished||!observed.value().stderr_finished)
    return fail(ErrorCode::Invariant,"Mesh worker completion missing terminal evidence");
  try{
    auto product=staging_->read("result.rasset",assets::mesh::Limits{}.product_bytes);if(!product)return product.error();
    auto view=assets::mesh::validate(product.value());if(!view)return view.error();
    if(view.value().id()!=id_||view.value().stamp()!=expected_||
        view.value().has_uv()!=(profile_==mesh_pipeline::Profile::GeometryUv))
      return fail(ErrorCode::InvalidInput,"Mesh worker product identity or provenance mismatch");
    return product;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Mesh worker publication allocation");}
}
Result<std::string> Job::publish(){
  auto product=take_product();if(!product)return product.error();
  return mesh_pipeline::publish(*output_,product.value());
}
} // namespace race::mesh_import
