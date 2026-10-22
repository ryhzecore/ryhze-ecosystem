// Explicit opt-in native test process. Never link this adapter variant into an app.
#ifndef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
#error Native removal contracts require the isolated test adapter.
#endif
#include "windows_viewport.hpp"
#include "race/build_identity.hpp"
#include "race/render/resource_contract.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace race;
using namespace race::render;
namespace fs=std::filesystem;
namespace {
constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
constexpr std::string_view sentinel="RACE P03.03 diagnostic retention sentinel";
struct Check {std::string name;bool passed{};};
struct Evidence {
  std::string lane,status{"failed"},error;
  std::vector<Check> checks;
  std::vector<std::pair<std::string,NativeRemovalStatus>> observations;
  std::vector<std::pair<std::string,ViewportStats>> epochs;
  unsigned removal_calls{};
  void check(bool value,const char* name){checks.push_back({name,value});if(!value)throw std::runtime_error(name);}
  void require(Result<void> value,const char* name){
    checks.push_back({name,bool(value)});if(!value)throw std::runtime_error(std::string(name)+": "+value.error().message);
  }
  void reject(Result<void> value,ErrorCode code,const char* name){check(!value&&value.error().code==code,name);}
  NativeRemovalStatus observe(WindowsViewport& view,const std::string& label){
    auto value=WindowsViewportRemovalProbe::observe(view);if(!value)throw std::runtime_error(value.error().message);
    observations.emplace_back(label,value.value());return value.value();
  }
  ViewportStats stats(WindowsViewport& view,const std::string& label){auto value=view.stats();epochs.emplace_back(label,value);return value;}
};
std::string quote(std::string_view value){
  std::string result="\"";for(unsigned char c:value){if(c=='\\'||c=='\"'){result+='\\';result+=char(c);}else if(c=='\n')result+="\\n";else if(c=='\r')result+="\\r";else if(c=='\t')result+="\\t";else if(c>=32)result+=char(c);}return result+'\"';
}
std::vector<char> read(const fs::path& path){
  if(fs::file_size(path)>32u*1024u*1024u)throw std::runtime_error("test artifact exceeded read bound");
  std::ifstream input(path,std::ios::binary);if(!input)throw std::runtime_error("test artifact unreadable");
  return {std::istreambuf_iterator<char>{input},{}};
}
std::string hash(const std::vector<char>& bytes){
  const auto sum=sha256(std::as_bytes(std::span(bytes)));constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(64);
  for(const auto byte:sum){const auto value=std::to_integer<unsigned>(byte);out+=digits[value>>4];out+=digits[value&15];}return out;
}
struct Window {
  HWND value{};
  Window(){
    static const ATOM registered=[] {WNDCLASSW type{};type.lpfnWndProc=DefWindowProcW;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"RACENativeRemovalContracts";return RegisterClassW(&type);}();
    if(!registered)throw std::runtime_error("test window class registration failed");
    RECT rectangle{0,0,640,480};if(!AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE))throw std::runtime_error("test client size failed");
    value=CreateWindowExW(0,L"RACENativeRemovalContracts",L"RACE native lifecycle contract probe",WS_OVERLAPPEDWINDOW,
        0,0,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!value)throw std::runtime_error("test window creation failed");
  }
  ~Window(){if(value)DestroyWindow(value);}
};
std::unique_ptr<WindowsViewport> create(Evidence& evidence,HWND window,const fs::path& shaders){
  auto made=WindowsViewport::create(window,shaders);if(!made)throw std::runtime_error(made.error().message);
  auto owner=std::move(made).value();const auto s=owner->stats();
  evidence.check(s.hardware&&s.debug_layer&&s.debug_layer_configured&&!s.device_lost&&!s.diagnostics_incomplete,
      "native_hardware_and_actual_debug_validation_required");return owner;
}
RenderFrame fixture(){
  RenderFrame frame;frame.scene_generation=41;frame.origin_revision=7;frame.simulation_tick=23;
  const std::array texel{std::byte{73},std::byte{139},std::byte{201},std::byte{255}};
  auto color=ViewportTexture::create_mips(1,1,texel),scalar=ViewportTexture::create_scalar_mips(1,1,texel);
  if(!color||!scalar)throw std::runtime_error("owned texture fixtures failed");
  auto a=ViewportMaterial::create({1,1,1,1},color.value(),TextureSampling::LinearClampMips);
  auto b=ViewportMaterial::create({1,1,1,1},scalar.value(),TextureSampling::LinearClampMips,TextureRole::DiffuseReflectanceR);
  if(!a||!b)throw std::runtime_error("owned material fixtures failed");
  DrawPrimitive left;left.id={201,1};left.world[3]=-1;left.material=a.value();
  DrawPrimitive right;right.id={201,2};right.world[3]=1;right.material=b.value();
  frame.draws={left,right};return frame;
}
bool same_adapter(const NativeRemovalStatus& a,const NativeRemovalStatus& b){return a.adapter_luid_low==b.adapter_luid_low&&a.adapter_luid_high==b.adapter_luid_high;}
void clean(Evidence& e,const NativeRemovalStatus& s,bool expected_sentinel=false){
  e.check(s.debug_errors==0&&s.unexpected_warnings==0&&!s.diagnostics_incomplete&&s.messages_discarded==0&&
      s.sentinel_warnings==(expected_sentinel?1u:0u),"native_queue_has_only_the_explicitly_admitted_diagnostic");
}
NativeRemovalStatus remove_and_observe(Evidence& e,WindowsViewport& owner,const std::string& label){
  e.require(WindowsViewportRemovalProbe::remove_test_device(owner),"actual_RemoveDevice_call");++e.removal_calls;
  // A deadline ends evidence gathering, never authorizes releasing live submitted work.
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  NativeRemovalStatus observed{};
  do {
    auto value=WindowsViewportRemovalProbe::observe(owner);if(!value)throw std::runtime_error(value.error().message);
    observed=value.value();if(observed.reason<0&&observed.completed_fence==maximum)break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(std::chrono::steady_clock::now()<deadline);
  e.observations.emplace_back(label,observed);
  e.check(observed.reason<0&&observed.completed_fence==maximum,"native_failed_reason_and_removal_fence_observed");
  clean(e,observed);return observed;
}
void removal(Evidence& e,Window& window,const fs::path& shaders,const fs::path& output){
  const auto frame=fixture();auto owner=create(e,window.value,shaders);
  const auto supported=e.observe(*owner,"initial-native");
  if(supported.device5_query==static_cast<std::int64_t>(E_NOINTERFACE)){e.status="unsupported";throw std::runtime_error("ID3D12Device5 unavailable; removal lane unqualified");}
  e.check(supported.device5_query>=0&&supported.reason>=0&&supported.completed_fence!=maximum,"native_removal_interface_and_healthy_device");clean(e,supported);
  e.require(owner->draw(frame),"initial_owned_frame");e.require(owner->capture(output/"before.ppm"),"initial_capture");
  auto token=owner->image_token();e.check(bool(token),"initial_image_token");
  e.require(owner->draw(frame),"submission_immediately_before_removal");const auto before=e.stats(*owner,"before-removal");
  const auto removed=remove_and_observe(e,*owner,"first-actual-removal");
  e.reject(owner->draw(frame),ErrorCode::PlatformFailure,"production_draw_observes_native_loss");
  const auto lost=e.stats(*owner,"observed-loss");
  e.check(lost.state==ViewportState::Removed&&!lost.has_image&&lost.gpu_sample_count==0&&lost.last_timed_frame==0,
      "loss_invalidates_current_image_and_timing");
  e.reject(owner->recover_removed(nullptr),ErrorCode::InvalidInput,"absent_surface_cannot_consume_recovery_budget");
  bool wrong_thread{};std::thread foreign([&]{auto result=owner->recover_removed(window.value);wrong_thread=!result&&result.error().code==ErrorCode::WrongThread;});foreign.join();
  e.check(wrong_thread&&owner->stats().removal_recovery_attempts==0,"foreign_thread_cannot_consume_recovery_budget");
  e.require(owner->recover_removed(window.value),"production_bounded_recovery");const auto fresh=e.stats(*owner,"recovered-empty");
  e.check(fresh.device_epoch==before.device_epoch+1&&fresh.removal_recovery_attempts==1&&fresh.retired.removed&&
      fresh.retired.removal_reason==removed.reason&&fresh.recreation==ViewportRecreation::DeviceRemoval,"first_recovery_identity_and_retired_native_reason");
  e.check(fresh.submitted_frames==0&&fresh.completed_frames==0&&fresh.texture_uploads==0&&fresh.texture_cache_hits==0&&
      !fresh.has_image&&fresh.gpu_sample_count==0&&!fresh.debug_errors&&!fresh.debug_warnings&&!fresh.diagnostics_incomplete,
      "recovery_has_no_old_native_results_or_hidden_diagnostics");
  e.reject(owner->capture(output/"stale-token.ppm",token.value()),ErrorCode::StaleHandle,"old_owner_epoch_image_rejected");
  e.check(!fs::exists(output/"stale-token.ppm"),"old_epoch_rejected_before_output");
  const auto recovered=e.observe(*owner,"recovered-native");e.check(same_adapter(supported,recovered),"recovered_native_adapter_LUID_matches");clean(e,recovered);
  e.require(owner->draw(frame),"same_owned_frame_after_recovery");e.require(owner->capture(output/"recovered.ppm"),"recovered_capture");
  const auto redrawn=e.stats(*owner,"recovered-redraw");
  e.check(redrawn.simulation_tick==23&&redrawn.scene_generation==41&&redrawn.origin_revision==7&&redrawn.texture_uploads==2,
      "reupload_preserves_owned_scene_and_tick");
  e.check(read(output/"before.ppm")==read(output/"recovered.ppm"),"actual_removal_recovery_pixels_exact");
  e.require(owner->draw(frame),"submission_before_second_removal");remove_and_observe(e,*owner,"second-actual-removal");
  e.reject(owner->recover_removed(window.value),ErrorCode::FailedPrerequisite,"second_automatic_recovery_rejected");
  const auto terminal=e.stats(*owner,"second-loss-terminal");
  e.check(terminal.state==ViewportState::Removed&&terminal.device_epoch==fresh.device_epoch&&terminal.removal_recovery_attempts==1,
      "failed_owner_does_not_loop_or_advance_epoch");
  owner.reset(); // Real removed-device destructor; no borrowed COM interface survives the probe.
  auto independent=create(e,window.value,shaders);
  e.check(same_adapter(supported,e.observe(*independent,"independent-owner-after-terminal-release")),"removed_singleton_fully_released");
  e.require(independent->draw(frame),"explicit_independent_owner_draw");
  e.reject(independent->capture(output/"expired-token.ppm",token.value()),ErrorCode::StaleHandle,"expired_owner_token_rejected");
  e.require(independent->capture(output/"independent.ppm"),"explicit_independent_owner_capture");
  e.check(read(output/"before.ppm")==read(output/"independent.ppm")&&!fs::exists(output/"expired-token.ppm"),"independent_owner_pixels_exact");
  clean(e,e.observe(*independent,"final-native"));e.stats(*independent,"final-independent-owner");
}
void fence_boundaries(Evidence& e,Window& window,const fs::path& shaders,const fs::path& output){
  const auto frame=fixture();unsigned index{};
  for(const auto next:{maximum-2,maximum-1,maximum}){
    auto owner=create(e,window.value,shaders);e.require(owner->draw(frame),"counter_fixture_draw");
    const auto prefix=std::string("counter-")+std::to_string(index++);
    e.require(owner->capture(output/(prefix+"-before.ppm")),"counter_fixture_capture");
    e.require(WindowsViewportRemovalProbe::set_next_fence(*owner,next),"quiescent_monotonic_counter_boundary");
    const auto before=e.stats(*owner,prefix+"-before");
    if(next==maximum-2){
      e.require(owner->capture(output/(prefix+"-last.ppm")),"capture_reserves_both_remaining_signals");
      e.check(read(output/(prefix+"-before.ppm"))==read(output/(prefix+"-last.ppm")),"last_legal_capture_pixels_exact");
    }else{
      e.reject(owner->capture(output/(prefix+"-rejected.ppm")),ErrorCode::Overflow,"capture_rejects_before_predrain_or_copy");
      const auto after=e.stats(*owner,prefix+"-after-rejection");
      e.check(after.capture_graph.submissions==before.capture_graph.submissions&&after.capture_graph.compiles==before.capture_graph.compiles&&
          !fs::exists(output/(prefix+"-rejected.ppm"))&&e.observe(*owner,prefix+"-capacity").next_signal==next,
          "rejected_capture_preserves_graph_counter_and_output");
      if(next==maximum-1)e.require(owner->draw(frame),"draw_uses_last_legal_signal");
    }
    e.check(e.observe(*owner,prefix+"-exhausted").next_signal==maximum,"last_legal_signal_never_uses_removal_sentinel");
    const auto exhausted=e.stats(*owner,prefix+"-exhausted");
    e.reject(owner->draw(frame),ErrorCode::Overflow,"exhausted_draw_rejects_before_Execute");
    e.check(owner->stats().submitted_frames==exhausted.submitted_frames&&owner->stats().draw_graph.submissions==exhausted.draw_graph.submissions,
        "exhausted_draw_preserves_submission_identity");
    e.require(owner->retire_surface(),"exhausted_counter_retires_via_already_issued_fences");
    const auto retired=e.stats(*owner,prefix+"-retired");
    e.check(!retired.retired.removed&&retired.retired.submitted_frames==retired.retired.completed_frames&&
        !retired.debug_errors&&!retired.debug_warnings&&!retired.diagnostics_incomplete,"counter_exhaustion_is_not_device_removal_or_unsafe_retirement");
  }
}
void diagnostics(Evidence& e,Window& first,const fs::path& shaders,const fs::path& output){
  Window second;const auto frame=fixture();auto a=create(e,first.value,shaders),b=create(e,second.value,shaders);
  e.require(a->draw(frame),"diagnostic_first_owner_draw");e.require(b->draw(frame),"diagnostic_second_owner_draw");
  const auto original=e.observe(*a,"A-before"),other=e.observe(*b,"B-before");clean(e,original);clean(e,other);
  e.check(same_adapter(original,other),"diagnostic_fixture_shares_native_adapter_singleton");
  e.require(WindowsViewportRemovalProbe::add_warning(*a),"actual_native_application_warning_added");
  e.require(WindowsViewportRemovalProbe::collect_diagnostics(*b),"B_reads_shared_warning_first");
  e.require(WindowsViewportRemovalProbe::collect_diagnostics(*a),"A_can_still_read_shared_warning");
  const auto a_seen=e.stats(*a,"A-warning"),b_seen=e.stats(*b,"B-warning");
  e.check(a_seen.debug_warnings==1&&b_seen.debug_warnings==1&&!a_seen.debug_errors&&!b_seen.debug_errors,
      "one_owner_does_not_clear_another_owners_warning");
  clean(e,e.observe(*a,"A-sentinel"),true);clean(e,e.observe(*b,"B-sentinel"),true);
  e.require(b->recreate(second.value),"B_recreation_retains_prior_diagnostics");
  const auto b_new=e.stats(*b,"B-after-recreation");
  e.check(b_new.retired.debug_warnings==1&&b_new.debug_warnings==2&&b_new.retired.last_debug_message[0]&&
      std::string_view(b_new.last_debug_message.data())==sentinel&&!b_new.diagnostics_incomplete,
      "retired_warning_visible_after_new_epoch_reobserves_shared_queue");
  e.require(WindowsViewportRemovalProbe::collect_diagnostics(*a),"A_rechecks_without_double_counting");
  e.check(a->stats().debug_warnings==1,"owner_cursor_does_not_repeat_same_native_message");
  e.require(a->capture(output/"diagnostic-A.ppm"),"A_capture_survives_B_recreation");
  clean(e,e.observe(*b,"B-recreated-sentinel"),true);
  e.require(a->retire_surface(),"A_diagnostic_retirement");const auto absent=e.stats(*a,"A-absent");
  e.check(absent.retired.debug_warnings==1&&absent.debug_warnings==1&&std::string_view(absent.last_debug_message.data())==sentinel,
      "diagnostics_survive_absent_Impl");
}
void epoch_boundaries(Evidence& e,Window& window,const fs::path& shaders,const fs::path& output){
  const auto frame=fixture();auto owner=create(e,window.value,shaders);
  e.require(owner->draw(frame),"initialization_failure_fixture_draw");
  e.require(owner->capture(output/"before.ppm"),"initialization_failure_fixture_capture");
  auto token=owner->image_token();e.check(bool(token),"initialization_failure_fixture_token");
  const auto before=e.stats(*owner,"before-injected-failure");
  e.require(WindowsViewportRemovalProbe::fail_next_initialization(*owner),"arm_explicit_initialization_Result_failure");
  e.reject(owner->recreate(window.value),ErrorCode::Exhausted,"injected_initialization_failure_propagates");
  const auto failed=e.stats(*owner,"failed-candidate");
  e.check(failed.state==ViewportState::Unavailable&&failed.device_epoch==before.device_epoch+1&&
      failed.recreation_attempts==before.recreation_attempts+1&&failed.removal_recovery_attempts==0&&
      failed.retired.submitted_frames==failed.retired.completed_frames&&!failed.retired.removed&&
      !failed.has_image&&!failed.gpu_sample_count&&failed.lifecycle_error&&
      failed.lifecycle_error->code==ErrorCode::Exhausted&&!failed.debug_errors&&!failed.debug_warnings&&!failed.diagnostics_incomplete,
      "failed_candidate_consumes_epoch_retains_error_and_quiescent_retirement_without_faking_loss");
  e.reject(owner->draw(frame),ErrorCode::FailedPrerequisite,"unavailable_owner_cannot_draw_or_automatically_retry");
  e.reject(owner->capture(output/"stale.ppm",token.value()),ErrorCode::StaleHandle,"failed_candidate_invalidates_old_image");
  e.check(!fs::exists(output/"stale.ppm")&&owner->stats().device_epoch==failed.device_epoch,
      "failed_operations_cannot_create_output_or_new_epoch");
  e.require(owner->recreate(window.value),"explicit_recreation_after_injected_failure");
  e.require(owner->draw(frame),"explicit_recreation_redraws_same_owned_frame");
  e.require(owner->capture(output/"restored.ppm"),"explicit_recreation_capture");
  e.check(read(output/"before.ppm")==read(output/"restored.ppm"),"injected_failure_recovery_pixels_exact");
  e.require(WindowsViewportRemovalProbe::set_epoch(*owner,maximum-1),"quiescent_monotonic_epoch_boundary");
  e.check(!owner->image_token()&&owner->stats().gpu_sample_count==0,"epoch_seam_invalidates_old_results");
  e.require(owner->recreate(window.value),"last_legal_device_epoch");
  e.require(owner->draw(frame),"last_epoch_draw");e.require(owner->capture(output/"last-epoch.ppm"),"last_epoch_capture");
  const auto last=e.stats(*owner,"last-epoch");auto current=owner->image_token();e.check(bool(current),"last_epoch_token");
  e.reject(owner->recreate(window.value),ErrorCode::Overflow,"epoch_exhaustion_rejects_before_retirement");
  const auto rejected=e.stats(*owner,"epoch-rejection");
  e.check(rejected.device_epoch==maximum&&rejected.state==ViewportState::Ready&&rejected.has_image&&
      rejected.recreation_attempts==last.recreation_attempts&&rejected.submitted_frames==last.submitted_frames,
      "epoch_exhaustion_preserves_current_owner_and_image");
  e.require(owner->capture(output/"after-rejection.ppm",current.value()),"last_epoch_image_still_captures");
  e.check(read(output/"before.ppm")==read(output/"last-epoch.ppm")&&read(output/"before.ppm")==read(output/"after-rejection.ppm"),
      "epoch_boundary_pixels_exact");
  clean(e,e.observe(*owner,"final-native"));e.require(owner->retire_surface(),"last_epoch_retires_safely");
}
void graph_rejections(Evidence& e,Window& window,const fs::path& shaders,const fs::path& output){
  const auto frame=fixture();auto changed=frame;
  const std::array texel{std::byte{16},std::byte{225},std::byte{40},std::byte{255}};
  auto texture=ViewportTexture::create_mips(1,1,texel);e.check(bool(texture),"graph_rejection_replacement_texture");
  auto material=ViewportMaterial::create({1,1,1,1},texture.value(),TextureSampling::LinearClampMips);
  e.check(bool(material),"graph_rejection_replacement_material");changed.draws[0].material=material.value();
  for(bool binding:{false,true}){
    const auto prefix=std::string(binding?"bind":"compile");auto owner=create(e,window.value,shaders);
    for(unsigned i=0;i<3;++i)e.require(owner->draw(frame),"warm_both_slots_before_native_rejection");
    e.require(owner->capture(output/(prefix+"-before.ppm")),"pre_rejection_capture");
    auto token=owner->image_token();e.check(bool(token),"pre_rejection_current_image_token");
    const auto before=e.stats(*owner,prefix+"-before");const auto native_before=e.observe(*owner,prefix+"-native-before");
    e.check(native_before.nonresident_textures==0,"all_original_slot_textures_resident");
    e.require(WindowsViewportRemovalProbe::reject_next_graph(*owner,binding),"arm_test_candidate_corruption");
    e.reject(owner->draw(changed),binding?ErrorCode::Invariant:ErrorCode::InvalidInput,"real_native_graph_validator_rejects_corrupted_candidate");
    const auto after=e.stats(*owner,prefix+"-rejected");const auto rejected=e.observe(*owner,prefix+"-native-rejected");
    e.check(after.submitted_frames==before.submitted_frames&&after.draw_graph.submissions==before.draw_graph.submissions&&
        after.texture_uploads==before.texture_uploads&&after.texture_cache_hits==before.texture_cache_hits&&
        after.draw_graph.compiles==before.draw_graph.compiles+(binding?1:0)&&rejected.next_signal==native_before.next_signal&&after.has_image,
        "rejection_precedes_Execute_signal_and_residency_publication");
    e.check(rejected.graph_plan_installed==binding&&rejected.nonresident_textures==(binding?1u:0u),
        "compile_failure_clears_old_plan_and_bind_failure_keeps_new_texture_unpublished");
    e.require(owner->capture(output/(prefix+"-rejected.ppm"),token.value()),"rejected_candidate_preserves_current_capture_identity");
    e.check(read(output/(prefix+"-before.ppm"))==read(output/(prefix+"-rejected.ppm")),"rejected_graph_preserves_pixels_exactly");
    e.require(owner->draw(changed),"valid_retry_after_native_graph_rejection");
    e.require(owner->capture(output/(prefix+"-recovered.ppm")),"valid_retry_capture");
    const auto recovered=e.stats(*owner,prefix+"-recovered");
    e.check(recovered.texture_uploads==before.texture_uploads+1&&recovered.texture_cache_hits==before.texture_cache_hits+1,
        "retry_uploads_only_replacement_and_reuses_other_resident_texture");
    Window other;auto oracle=create(e,other.value,shaders);e.require(oracle->draw(changed),"independent_fresh_owner_oracle");
    e.require(oracle->capture(output/(prefix+"-oracle.ppm")),"independent_oracle_capture");
    e.check(read(output/(prefix+"-recovered.ppm"))==read(output/(prefix+"-oracle.ppm"))&&
        read(output/(prefix+"-recovered.ppm"))!=read(output/(prefix+"-before.ppm")),"retry_matches_fresh_native_oracle_and_changes_requested_pixels");
    clean(e,e.observe(*owner,prefix+"-final-native"));clean(e,e.observe(*oracle,prefix+"-oracle-native"));
  }
}
// P03.04 oracle group 6: an owned output set over its ceiling is rejected with Exhausted before
// anything is installed; the legacy image and submission identity stay current, and the declared
// ceiling then admits the same frame. The ceiling is lowered only through the probe seam.
void output_budget(Evidence& e,Window& window,const fs::path& shaders,const fs::path& output){
  const auto frame=fixture();auto owner=create(e,window.value,shaders);
  e.require(owner->draw(frame),"legacy_fixture_draw");e.require(owner->capture(output/"legacy-before.ppm"),"legacy_fixture_capture");
  auto token=owner->image_token();e.check(bool(token),"legacy_fixture_token");
  const auto before=e.stats(*owner,"before-limited-install");
  e.check(!before.physical_output_installed&&before.output_allocation_bytes==0&&before.lighting_profile==LightingProfile::LegacyViewportV1,"no_physical_output_before_request");
  auto physical=frame;physical.lighting_profile=LightingProfile::PhysicalDiffuseV1;physical.physical.illuminance_lux=1000;
  e.require(WindowsViewportRemovalProbe::limit_output_budget(*owner,1),"arm_test_limited_output_ceiling");
  e.reject(owner->draw(physical),ErrorCode::Exhausted,"over_ceiling_output_set_rejected_before_install");
  const auto rejected=e.stats(*owner,"after-rejection");
  e.check(!rejected.physical_output_installed&&rejected.output_allocation_bytes==0&&rejected.submitted_frames==before.submitted_frames&&
      rejected.draw_graph.submissions==before.draw_graph.submissions&&rejected.lighting_profile==LightingProfile::LegacyViewportV1&&rejected.has_image,
      "rejection_installed_nothing_and_kept_submission_identity");
  e.require(owner->capture(output/"legacy-after-rejection.ppm",token.value()),"legacy_image_still_current_after_rejection");
  e.check(read(output/"legacy-before.ppm")==read(output/"legacy-after-rejection.ppm"),"rejection_preserved_pixels_exactly");
  e.require(owner->draw(frame),"legacy_frame_after_rejection");
  e.require(WindowsViewportRemovalProbe::limit_output_budget(*owner,0),"restore_declared_ceiling");
  e.require(owner->draw(physical),"declared_ceiling_admits_the_same_physical_frame");
  const auto installed=e.stats(*owner,"installed");
  e.check(installed.physical_output_installed&&installed.output_allocation_bytes>0&&installed.output_allocation_bytes<=installed.output_budget_bytes&&
      installed.lighting_profile==LightingProfile::PhysicalDiffuseV1&&installed.hdr_target_bytes>0,"physical_output_installed_within_declared_ceiling");
  e.require(owner->capture(output/"physical.ppm"),"physical_capture");
  e.check(read(output/"physical.ppm")!=read(output/"legacy-before.ppm"),"physical_frame_differs_from_legacy");
  e.require(owner->draw(frame),"legacy_after_physical");e.require(owner->capture(output/"legacy-after-physical.ppm"),"legacy_after_physical_capture");
  e.check(read(output/"legacy-before.ppm")==read(output/"legacy-after-physical.ppm"),"legacy_pixels_exact_after_physical_output");
  clean(e,e.observe(*owner,"final-native"));
}
void write_receipt(const fs::path& output,const Evidence& e){
  std::ostringstream out;out<<std::boolalpha<<"{\"schema\":1,\"lane\":"<<quote(e.lane)<<",\"status\":"<<quote(e.status)
      <<",\"passed\":"<<(e.status=="passed")<<",\"source_commit\":"<<quote(build::source_commit)
      <<",\"authored_input_sha256\":"<<quote(build::authored_input_sha256)<<",\"programmatic_removal_calls\":"<<e.removal_calls
      <<",\"spontaneous_loss_exercised\":false,\"injected_initialization_failure\":"<<(e.lane=="epoch-boundary")
      <<",\"injected_graph_candidate_corruption\":"<<(e.lane=="graph-rejection")
      <<",\"injected_output_ceiling\":"<<(e.lane=="output-budget")
      <<",\"error\":"<<quote(e.error)<<",\"checks\":[";
  for(std::size_t i=0;i<e.checks.size();++i){if(i)out<<',';out<<"{\"name\":"<<quote(e.checks[i].name)<<",\"passed\":"<<e.checks[i].passed<<'}';}
  out<<"],\"native_observations\":[";
  for(std::size_t i=0;i<e.observations.size();++i){if(i)out<<',';const auto& [name,s]=e.observations[i];
    out<<"{\"name\":"<<quote(name)<<",\"removed_reason\":"<<s.reason<<",\"device5_query\":"<<s.device5_query
        <<",\"completed_fence\":"<<quote(std::to_string(s.completed_fence))<<",\"next_signal\":"<<quote(std::to_string(s.next_signal))
        <<",\"graph_plan_installed\":"<<s.graph_plan_installed<<",\"nonresident_textures\":"<<s.nonresident_textures
        <<",\"adapter_luid\":["<<s.adapter_luid_low<<','<<s.adapter_luid_high<<"],\"debug_errors\":"<<s.debug_errors
        <<",\"unexpected_warnings\":"<<s.unexpected_warnings<<",\"sentinel_warnings\":"<<s.sentinel_warnings
        <<",\"messages_discarded\":"<<s.messages_discarded<<",\"diagnostics_incomplete\":"<<s.diagnostics_incomplete
        <<",\"last_message_id\":"<<s.last_message_id<<",\"last_message\":"<<quote(s.last_message.data())<<'}';}
  out<<"],\"epoch_observations\":[";
  for(std::size_t i=0;i<e.epochs.size();++i){if(i)out<<',';const auto& [name,s]=e.epochs[i];
    out<<"{\"name\":"<<quote(name)<<",\"epoch\":"<<s.device_epoch<<",\"state\":"<<unsigned(s.state)
        <<",\"removal_recovery_attempts\":"<<s.removal_recovery_attempts<<",\"submitted\":"<<s.submitted_frames
        <<",\"completed\":"<<s.completed_frames<<",\"debug_errors\":"<<s.debug_errors<<",\"debug_warnings\":"<<s.debug_warnings
        <<",\"messages_discarded\":"<<s.debug_messages_discarded<<",\"diagnostics_incomplete\":"<<s.diagnostics_incomplete
        <<",\"last_message\":"<<quote(s.last_debug_message.data())<<",\"retired_epoch\":"<<s.retired.device_epoch
        <<",\"retired_removed\":"<<s.retired.removed<<",\"retired_reason\":"<<s.retired.removal_reason
        <<",\"retired_debug_errors\":"<<s.retired.debug_errors<<",\"retired_debug_warnings\":"<<s.retired.debug_warnings
        <<",\"retired_last_message\":"<<quote(s.retired.last_debug_message.data())<<'}';}
  out<<"],\"diagnostic_exception\":{"<<"\"enabled\":"<<(e.lane=="diagnostic-retention")
      <<",\"message_id\":1,\"severity\":\"WARNING\",\"exact_text\":"<<quote(sentinel)
      <<",\"scope\":\"Only this exact native application message; all other warnings/errors fail. Owner totals count epoch observations, not unique device messages.\"},\"artifacts\":[";
  bool first=true;for(const auto& entry:fs::directory_iterator(output)){if(!entry.is_regular_file())continue;const auto bytes=read(entry.path());
    if(!first)out<<',';first=false;out<<"{\"path\":"<<quote(entry.path().filename().string())<<",\"bytes\":"<<bytes.size()<<",\"sha256\":"<<quote(hash(bytes))<<'}';}
  out<<"]}\n";const auto text=out.str();std::ofstream file(output/"receipt.json",std::ios::binary);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();
  if(file.fail())throw std::runtime_error("native lifecycle receipt write failed");std::cout<<text;
}
}
int main(int argc,char** argv){
  fs::path shaders,output;std::string source,digest;Evidence evidence;
  for(int i=1;i<argc;++i){const std::string_view arg=argv[i];
    if((arg=="--programmatic-removal"||arg=="--fence-boundary"||arg=="--epoch-boundary"||arg=="--diagnostic-retention"||arg=="--graph-rejection"||arg=="--output-budget")&&evidence.lane.empty())evidence.lane=std::string(arg.substr(2));
    else if(arg=="--shader-dir"&&i+1<argc&&shaders.empty())shaders=argv[++i];
    else if(arg=="--output-dir"&&i+1<argc&&output.empty())output=argv[++i];
    else if(arg=="--expected-source"&&i+1<argc&&source.empty())source=argv[++i];
    else if(arg=="--expected-authored-digest"&&i+1<argc&&digest.empty())digest=argv[++i];
    else return 2;
  }
  if(evidence.lane.empty()||shaders.empty()||output.empty()||source!=build::source_commit||digest!=build::authored_input_sha256)return 2;
  try{if(!fs::create_directory(output))throw std::runtime_error("native lane needs an existing parent and a fresh output directory");}
  catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 2;}
  try{
    Window window;
    if(evidence.lane=="programmatic-removal")removal(evidence,window,shaders,output);
    else if(evidence.lane=="fence-boundary")fence_boundaries(evidence,window,shaders,output);
    else if(evidence.lane=="epoch-boundary")epoch_boundaries(evidence,window,shaders,output);
    else if(evidence.lane=="graph-rejection")graph_rejections(evidence,window,shaders,output);
    else if(evidence.lane=="output-budget")output_budget(evidence,window,shaders,output);
    else diagnostics(evidence,window,shaders,output);
    evidence.status="passed";
  }catch(const std::exception& error){evidence.error=error.what();}
  try{write_receipt(output,evidence);}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
  return evidence.status=="passed"?0:evidence.status=="unsupported"?3:1;
}
