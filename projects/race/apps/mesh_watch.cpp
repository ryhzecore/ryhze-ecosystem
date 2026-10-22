#include "mesh_watch.hpp"
#include <limits>

namespace race::mesh_import {
namespace w=platform::directory_watch;
namespace p=platform::process;
Result<std::unique_ptr<Watch>> Watch::create(std::shared_ptr<platform::RootedFiles> source,std::string_view leaf,
    std::unique_ptr<w::Service> watch,std::unique_ptr<WorkerFactory> factory,std::unique_ptr<Reimport> owner,const WatchLimits& limits){
  if(!source||!watch||!factory||!owner||!platform::valid_content_leaf(leaf)||!leaf.ends_with(".obj")||
      !limits.imports||limits.imports>64||!limits.read_retries||limits.read_retries>8)
    return fail(ErrorCode::InvalidInput,"Invalid watched source or limits");
  auto pending=owner->pending_source_bytes();if(!pending)return pending.error();
  if(pending.value())return fail(ErrorCode::Busy,"Watched owner already retains a child");
  try{
    auto result=std::unique_ptr<Watch>(new Watch);
    result->leaf_=leaf;result->limits_=limits;result->source_=std::move(source);result->factory_=std::move(factory);
    result->watch_=std::move(watch);result->owner_=std::move(owner);
    auto current=result->owner_->current();
    if(current){auto lease=result->owner_->acquire(current.value());if(!lease)return lease.error();result->status_.current_revision=lease.value()->view().revision();}
    else if(current.error().code!=ErrorCode::NotFound)return current.error();
    result->refresh();return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Watched owner allocation");}
}
void Watch::event(WatchOutcome outcome,std::optional<Error> error){
  status_.outcome=outcome;status_.last_error=error;
  if(status_.events==std::numeric_limits<std::uint64_t>::max()){
    stop(fail(ErrorCode::Exhausted,"Watched event identities exhausted"));return;
  }
  ++status_.events;
}
void Watch::stop(std::optional<Error> reason){
  if(reason&&!status_.terminal_error)status_.terminal_error=reason;
  if(status_.phase!=WatchPhase::Running)return;
  status_.phase=WatchPhase::Stopping;scan_pending_=false;active_.reset();
  // Always attempt both stops, including when the first reports failure.
  auto child=owner_->invalidate();auto watcher=watch_->request_stop();
  if(!child&&!status_.terminal_error)status_.terminal_error=child.error();
  if(!watcher&&!status_.terminal_error)status_.terminal_error=watcher.error();
}
Result<void> Watch::request_stop(){
  if(thread_!=std::this_thread::get_id())return fail(ErrorCode::WrongThread,"Watched stop requires owner");
  stop();return status_.terminal_error?Result<void>(*status_.terminal_error):Result<void>{};
}
void Watch::refresh(){
  status_.retained_bytes=owner_->residency().retained_bytes;
  auto pending=owner_->pending_source_bytes();if(pending)status_.pending_source_bytes=pending.value();
}
Result<WatchStatus> Watch::poll(){
  if(thread_!=std::this_thread::get_id())return fail(ErrorCode::WrongThread,"Watched poll requires owner");
  if(status_.phase==WatchPhase::Stopped)return status_;
  try{
    auto observation=watch_->poll();
    if(!observation)stop(observation.error());
    else{
      const auto& next=observation.value();
      const auto token=next.observation;
      const bool identity_ok=token.watcher&&token.generation&&(!status_.observed.watcher||
        (token.watcher==status_.observed.watcher&&token.generation>=status_.observed.generation));
      if(!identity_ok)stop(fail(ErrorCode::Invariant,"Foreign or regressed watch observation"));
      else{
        watch_stopped_=next.phase==w::Phase::Stopped;
        if(next.failure)stop(*next.failure);
        if(status_.phase==WatchPhase::Running&&next.phase!=w::Phase::Watching)
          stop(fail(ErrorCode::Stopping,"Watcher ended during authoring"));
        if(status_.phase==WatchPhase::Running&&token!=status_.observed){
          if(!next.dirty)stop(fail(ErrorCode::Invariant,"New observation lacks invalidation"));
          else{
            status_.observed=token;status_.read_failures=0;scan_pending_=true;
            if(active_){++status_.superseded;active_.reset();}
            auto invalidated=owner_->invalidate();event(WatchOutcome::Invalidated);
            if(!invalidated)stop(invalidated.error());
          }
        }
      }
    }
    // Only matching child+stream quiescence permits release or publication.
    if(status_.worker_retained){
      auto child=owner_->poll();
      if(!child)stop(child.error());
      else if(child.value().phase==p::Phase::Completed&&child.value().stdout_finished&&child.value().stderr_finished){
        if(status_.phase==WatchPhase::Running&&active_){
          auto finished=owner_->finish(*active_);
          auto pending=owner_->pending_source_bytes();
          if(!pending||pending.value()!=0)stop(finished?fail(ErrorCode::Invariant,"Finished worker remains retained"):finished.error());
          else{
            status_.worker_retained=false;active_.reset();
            if(!finished)event(WatchOutcome::Rejected,finished.error());
            else{
              auto lease=owner_->acquire(finished.value());
              if(!lease)stop(lease.error());
              else{status_.current_revision=lease.value()->view().revision();event(WatchOutcome::Published);}
            }
          }
        }else{
          auto reaped=owner_->reap();if(!reaped)stop(reaped.error());else status_.worker_retained=false;
        }
      }
    }
    if(status_.phase==WatchPhase::Running&&!status_.worker_retained&&scan_pending_){
      if(status_.source_reads==std::numeric_limits<std::uint64_t>::max())stop(fail(ErrorCode::Exhausted,"Watched read counter exhausted"));
      else{
        ++status_.source_reads;
        auto source=source_->read(leaf_,mesh_pipeline::Limits{}.source_bytes);
        if(source&&(source.value().empty()||source.value().size()>mesh_pipeline::Limits{}.source_bytes||source.value().capacity()>mesh_pipeline::Limits{}.source_bytes))
          source=fail(ErrorCode::Exhausted,"Watched source snapshot limit");
        if(!source){
          ++status_.read_failures;event(WatchOutcome::SourceUnavailable,source.error());
          if(status_.read_failures>=limits_.read_retries){
            auto ack=watch_->acknowledge(status_.observed);
            scan_pending_=false;
            if(!ack&&ack.error().code!=ErrorCode::StaleHandle)stop(ack.error());
          }
        }else{
          // Acknowledge the observation captured BEFORE read. Never upgrade it
          // after the read; that could discard a source change during the copy.
          auto ack=watch_->acknowledge(status_.observed);scan_pending_=false;
          if(!ack){if(ack.error().code!=ErrorCode::StaleHandle)stop(ack.error());}
          else{
            status_.source=assets::digest(source.value());
            bool same{};
            if(limits_.coalesce_identical){
              auto current=owner_->current();if(current){auto lease=owner_->acquire(current.value());if(!lease)stop(lease.error());
                else {auto expected=mesh_pipeline::profile_import_stamp(owner_->profile(),status_.source);
                  if(!expected)stop(expected.error());
                  else same=lease.value()->view().stamp()==mesh_pipeline::cook_stamp(expected.value());}}
            }
            if(status_.phase==WatchPhase::Running){
              if(same){++status_.unchanged;event(WatchOutcome::Unchanged);}
              else if(status_.attempts==limits_.imports)stop(fail(ErrorCode::Exhausted,"Watched import attempts exhausted"));
              else{
                // Charge failed preparation too; it may have created a stage.
                ++status_.attempts;
                auto desired=owner_->desire(status_.source);
                if(!desired)stop(desired.error());
                else{
                  status_.desired_generation=desired.value().generation;
                  auto prepared=factory_->prepare(status_.attempts);
                  if(!prepared)event(WatchOutcome::Rejected,prepared.error());
                  else{
                    auto& worker=prepared.value();
                    auto started=owner_->start(desired.value(),std::move(worker.process),worker.stage,worker.executable,worker.directory,source.value());
                    if(!started)event(WatchOutcome::Rejected,started.error());
                    else{active_=desired.value();status_.worker_retained=true;event(WatchOutcome::Admitted);}
                  }
                }
              }
            }
          }
        }
      }
    }
  }catch(const std::bad_alloc&){stop(fail(ErrorCode::Exhausted,"Watched poll allocation"));}
  if(status_.phase==WatchPhase::Stopping&&watch_stopped_&&!status_.worker_retained){
    status_.phase=WatchPhase::Stopped;event(WatchOutcome::Stopped,status_.terminal_error);
  }
  refresh();return status_;
}
} // namespace race::mesh_import
