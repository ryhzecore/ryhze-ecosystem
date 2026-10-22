#include "desktop_material_assignment.hpp"
#include "race/assets/surface.hpp"
#include <algorithm>
#include <atomic>
#include <system_error>

namespace race::desktop {
struct MaterialAssignmentTask::Pending {
  std::atomic<bool> done{};
  MaterialAssignmentCompletion completion;
  PersistenceStamp stamp;
  PersistentId selected;
  ObjectRow row;
  std::shared_ptr<const MeshSet> previous;
  std::string leaf;
};
MaterialAssignmentTask::MaterialAssignmentTask()=default;
MaterialAssignmentTask::~MaterialAssignmentTask(){if(worker_.joinable())worker_.join();}
Result<void> MaterialAssignmentTask::start(const Document& document,const ObjectRow& row,std::shared_ptr<const MeshSet> previous,std::string leaf){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Material admission requires its owner");
  if(busy())return fail(ErrorCode::Busy,"Material request remains retained");
  if(document.playing())return fail(ErrorCode::Busy,"Stop Play before assigning a material");
  if(!row.id.valid()||!previous||!previous->files()||!platform::valid_content_leaf(leaf)||!leaf.ends_with(".rasset"))
    return fail(ErrorCode::InvalidInput,"Material assignment requires a selection, root and native leaf");
  if(leaf.capacity()>96)return fail(ErrorCode::Exhausted,"Material filename retained capacity exceeds 96 bytes");
  try{
    auto rows=document.objects();if(!rows)return rows.error();
    const auto actual=std::find_if(rows.value().begin(),rows.value().end(),[&](const auto& current){return current.id==row.id;});
    if(actual==rows.value().end()||actual->handle!=row.handle||actual->mesh!=row.mesh||!actual->visible)
      return fail(ErrorCode::StaleHandle,"Material selection does not match the current Scene");
    auto state=std::make_unique<Pending>();state->stamp=document.persistence_stamp();state->selected=row.id;state->row=row;state->previous=std::move(previous);state->leaf=std::move(leaf);
    std::thread worker([state=state.get()]() noexcept {
      {
        const auto prepare=[&]()->Result<void>{
          auto bytes=state->previous->files()->read(state->leaf,assets::surface::Limits{}.product_bytes);if(!bytes)return bytes.error();
          if(bytes.value().capacity()>assets::surface::Limits{}.product_bytes)return fail(ErrorCode::Exhausted,"Material input buffer capacity limit");
          auto view=assets::surface::validate_material(bytes.value());if(!view)return view.error();
          if(assets::hex(view.value().revision())+".rasset"!=state->leaf)return fail(ErrorCode::InvalidInput,"Material filename must match its full revision");
          state->completion.reference={view.value().id(),view.value().revision()};
          auto prepared=MeshSet::extend_material(state->previous,bytes.value());if(!prepared)return prepared.error();
          auto material=prepared.value()->find_material(state->completion.reference);if(!material)return material.error();
          if(material.value()->material->texture()&&state->row.mesh){
            auto mesh=prepared.value()->find(*state->row.mesh);if(!mesh)return mesh.error();
            if(!mesh.value()->geometry->has_uv())return fail(ErrorCode::Unsupported,"A textured material requires mesh UV coordinates");
          }
          state->completion.prepared=std::move(prepared).value();return {};
        };
        try{state->completion.result=prepare();}
        catch(const std::bad_alloc&){state->completion.result=fail(ErrorCode::Exhausted,"Material preparation allocation");}
        catch(...){state->completion.result=fail(ErrorCode::PlatformFailure,"Material preparation failed");}
      }
      state->previous.reset();state->done.store(true,std::memory_order_release);
    });
    pending_=std::move(state);worker_=std::move(worker);cancelled_=false;return {};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Material request allocation");}
   catch(const std::system_error&){return fail(ErrorCode::PlatformFailure,"Material worker could not start");}
}
Result<void> MaterialAssignmentTask::cancel(){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Material cancellation requires its owner");
  if(!busy())return fail(ErrorCode::NotFound,"No material request is retained");cancelled_=true;return {};
}
Result<std::optional<MaterialAssignmentCompletion>> MaterialAssignmentTask::poll(Document& document,PersistentId selection,bool allow_publication){
  if(std::this_thread::get_id()!=owner_)return fail(ErrorCode::WrongThread,"Material completion requires its owner");
  if(!busy()||!pending_->done.load(std::memory_order_acquire))return std::optional<MaterialAssignmentCompletion>{};
  worker_.join();auto completion=std::move(pending_->completion);
  if(cancelled_||!allow_publication||selection!=pending_->selected)completion.result=fail(ErrorCode::Cancelled,"Material assignment cancelled after worker quiescence");
  else if(!document.current(pending_->stamp))completion.result=fail(ErrorCode::StaleHandle,"Scene changed during material preparation");
  else if(completion.result){
    try{completion.result=document.assign_material(pending_->selected,completion.reference);}
    catch(const std::bad_alloc&){completion.result=fail(ErrorCode::Exhausted,"Material Scene publication allocation");}
    catch(...){completion.result=fail(ErrorCode::PlatformFailure,"Material Scene publication failed");}
  }
  if(!completion.result)completion.prepared.reset();pending_.reset();return std::optional<MaterialAssignmentCompletion>{std::move(completion)};
}
}
