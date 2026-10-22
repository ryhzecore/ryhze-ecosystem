// Opt-in hardware integration check. Creates a private hidden Win32 window;
// never changes the user's desktop, selects WARP, or claims visible editor QA.
#include "windows_viewport.hpp"
#include "mesh_pipeline.hpp"
#include "mesh_render.hpp"
#include "race/build_identity.hpp"
#include "race/render/graph.hpp"
#include "race/render/resource_contract.hpp"
#include "race/assets/surface.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <iterator>
#include <iomanip>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

using namespace race;
using namespace race::render;
namespace {
void require(bool passed,const char* message) { if (!passed) throw std::runtime_error(message); }
void require(Result<void> result,const char* stage) {
  if (!result) { std::fprintf(stderr,"%s: %s\n",stage,result.error().message); throw std::runtime_error(stage); }
}
void rejected(Result<void> result,ErrorCode expected,const char* message) {
  require(!result && result.error().code==expected,message);
}
// Failure diagnostics: the retained counts and the last debug-layer message, so a failed
// hardware/debug completion names its cause in stderr instead of only its predicate.
void report_completion(const char* scope,const ViewportStats& stats) {
  std::fprintf(stderr,"%s completion: hardware=%d device_lost=%d submitted=%llu completed=%llu debug_errors=%llu debug_warnings=%llu "
      "discarded=%llu diagnostics_incomplete=%d last_debug_message=\"%s\"\n",scope,int(stats.hardware),int(stats.device_lost),
      static_cast<unsigned long long>(stats.submitted_frames),static_cast<unsigned long long>(stats.completed_frames),
      static_cast<unsigned long long>(stats.debug_errors),static_cast<unsigned long long>(stats.debug_warnings),
      static_cast<unsigned long long>(stats.debug_messages_discarded),int(stats.diagnostics_incomplete),stats.last_debug_message.data());
}
void valid_profile(const ViewportStats& stats) {
  require(stats.last_cpu_frame==stats.submitted_frames,"CPU profiling stamp is not its submitted frame");
  const std::array phases{stats.last_validation_cpu_ms,stats.last_inflight_wait_cpu_ms,
      stats.last_record_cpu_ms,stats.last_execute_cpu_ms,stats.last_present_cpu_ms,
      stats.last_signal_cpu_ms,stats.last_completion_cpu_ms};
  double total{};
  for (double value:phases) { require(std::isfinite(value) && value>=0,"invalid CPU profiling phase"); total+=value; }
  require(std::isfinite(stats.last_draw_cpu_ms) && stats.last_draw_cpu_ms+1e-8>=total,"CPU profiling phases overlap or exceed draw");
  require(std::isfinite(stats.last_pending_fence_cpu_ms) && stats.last_pending_fence_cpu_ms>=0 &&
      stats.last_pending_fence_cpu_ms<=stats.last_inflight_wait_cpu_ms+1e-8,"pending fence scope exceeded wait phase");
  require(stats.last_inflight_wait_pending || stats.last_pending_fence_cpu_ms==0,"completed-on-entry fence reported pending time");
  require(stats.gpu_sample_count<=stats.gpu_samples.size(),"GPU profiling history exceeded bound");
  std::uint64_t previous{};
  for (std::size_t i=0;i<stats.gpu_sample_count;++i) {
    const auto& sample=stats.gpu_samples[i];
    require(sample.device_epoch==stats.device_epoch,"GPU sample belongs to an obsolete device epoch");
    require(sample.frame>previous && sample.frame<=stats.completed_frames,"GPU timing IDs are duplicated, unordered or unretired");
    require(std::isfinite(sample.milliseconds) && sample.milliseconds>=0,"invalid completed GPU duration");
    previous=sample.frame;
  }
  require(previous==stats.last_timed_frame,"latest GPU timing identity does not match history");
  if (stats.gpu_sample_count) require(stats.gpu_milliseconds==stats.gpu_samples.at(stats.gpu_sample_count-1).milliseconds,
      "latest GPU duration does not match its frame identity");
}
struct NativeWindow {
  HINSTANCE instance=GetModuleHandleW(nullptr);
  HWND handle{};
  static constexpr const wchar_t* name=L"RACEViewportHardwareContracts";
  NativeWindow() {
    WNDCLASSW type{}; type.lpfnWndProc=DefWindowProcW; type.hInstance=instance; type.lpszClassName=name;
    require(RegisterClassW(&type)!=0,"window class registration failed");
    RECT rectangle{0,0,640,480}; require(AdjustWindowRect(&rectangle,WS_OVERLAPPEDWINDOW,FALSE)!=0,"client rectangle failed");
    handle=CreateWindowExW(0,name,L"RACE GPU contract probe",WS_OVERLAPPEDWINDOW,0,0,
        rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,nullptr,nullptr,instance,nullptr);
    require(handle!=nullptr,"native test window creation failed");
  }
  ~NativeWindow() { if (handle) DestroyWindow(handle); UnregisterClassW(name,instance); }
};
void shader_admission_contracts(HWND window,WindowsViewport& retained,const std::filesystem::path& shaders,const std::filesystem::path& output) {
  const auto read=[](const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary);require(bool(in),"shader rejection fixture read");
    return std::vector<char>(std::istreambuf_iterator<char>{in},{});
  };
  const auto write=[](const std::filesystem::path& path,const std::vector<char>& bytes){
    std::ofstream out(path,std::ios::binary);out.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    require(bool(out),"shader rejection fixture write");
  };
  const auto manifest=read(shaders/"viewport-bindings.rmanifest"),vertex=read(shaders/"viewport.vs.dxil"),pixel=read(shaders/"viewport.ps.dxil");
  const auto light_manifest=read(shaders/"viewport-light-output.rmanifest"),physical=read(shaders/"viewport-physical.ps.dxil"),
      output_vertex=read(shaders/"viewport-output.vs.dxil"),output_pixel=read(shaders/"viewport-output.ps.dxil");
  require(manifest.size()==viewport_shader_manifest_bytes,"shader manifest fixture size");
  require(light_manifest.size()==light_output_shader_manifest_bytes,"light/output manifest fixture size");
  const auto previous=retained.stats();
  // Variants 0-5 damage the legacy program; 6-10 damage the P03.04 light/output
  // programs (missing manifest, checksum, vertex link, output pixel bytes, missing stage).
  for(unsigned variant=0;variant<11;++variant){
    const auto dir=output/("shader-rejection-"+std::to_string(variant));require(std::filesystem::create_directory(dir),"unique shader rejection directory");
    auto bad_manifest=manifest,bad_vertex=vertex,bad_light=light_manifest,bad_output_pixel=output_pixel;
    if(variant==1)bad_manifest.back()^=1;
    if(variant==2||variant==3){
      bad_manifest[variant==2?448+32+8:64]^=1; // reflected texture space or compiled DXC identity
      const auto sum=sha256(std::as_bytes(std::span(bad_manifest)).first(608));
      std::memcpy(bad_manifest.data()+608,sum.data(),sum.size());
    }
    if(variant==4)bad_vertex.back()^=1;
    if(variant==7)bad_light.back()^=1;
    if(variant==8){
      bad_light[224]^=1; // legacy vertex link identity, resealed
      const auto sum=sha256(std::as_bytes(std::span(bad_light)).first(864));
      std::memcpy(bad_light.data()+864,sum.data(),sum.size());
    }
    if(variant==9)bad_output_pixel.back()^=1;
    if(variant!=0)write(dir/"viewport-bindings.rmanifest",bad_manifest);
    write(dir/"viewport.vs.dxil",bad_vertex);if(variant!=5)write(dir/"viewport.ps.dxil",pixel);
    if(variant!=6)write(dir/"viewport-light-output.rmanifest",bad_light);
    if(variant!=10)write(dir/"viewport-physical.ps.dxil",physical);
    write(dir/"viewport-output.vs.dxil",output_vertex);write(dir/"viewport-output.ps.dxil",bad_output_pixel);
    auto rejected=WindowsViewport::create(window,dir);
    require(!rejected,"invalid or mismatched shader pipeline published");
    const auto expected=variant==0||variant==5||variant==6||variant==10?ErrorCode::IoFailure:
        variant==2||variant==3||variant==8?ErrorCode::IncompatibleVersion:ErrorCode::InvalidInput;
    require(rejected.error().code==expected,"shader admission rejection reason differs");
    const auto current=retained.stats();
    require(current.submitted_frames==previous.submitted_frames&&current.completed_frames==previous.completed_frames&&
        current.debug_errors==previous.debug_errors&&current.debug_warnings==previous.debug_warnings&&!current.device_lost,
        "failed pipeline candidate changed retained viewport");
  }
}
std::shared_ptr<const ViewportGeometry> imported_mesh() {
  constexpr std::string_view source="v 0 0 0\nv 2 0 0\nv 0 3 0\nv 0 0 2\nf 1 3 2\nf 1 2 4\nf 1 4 3\nf 2 3 4\n";
  auto imported=mesh_pipeline::import_obj({72,1},std::as_bytes(std::span(source)));
  require(static_cast<bool>(imported),"mesh source import");
  auto cooked=mesh_pipeline::cook(imported.value()); require(static_cast<bool>(cooked),"mesh native cooking");
  auto decoded=assets::mesh::validate(cooked.value()); require(static_cast<bool>(decoded),"mesh native validation");
  auto allocator=TrackedAllocator::create({1024*1024}); require(static_cast<bool>(allocator),"mesh allocator");
  auto store=assets::mesh::Store::create({},allocator.value()); require(static_cast<bool>(store),"mesh store");
  auto update=store.value()->begin_update(decoded.value().id(),decoded.value().stamp()); require(static_cast<bool>(update),"mesh update");
  auto published=store.value()->publish(update.value(),cooked.value()); require(static_cast<bool>(published),"mesh publication");
  auto lease=store.value()->acquire(published.value()); require(static_cast<bool>(lease),"mesh owning lease");
  auto packet=desktop::copy_mesh_geometry(lease.value()); require(static_cast<bool>(packet),"mesh render copy");
  require(store.value()->retire(decoded.value().id()),"mesh retire after copy");
  store.value().reset(); cooked.value().clear(); imported.value().clear();
  return packet.value(); // All source bytes, store and lease die before any draw.
}
struct Image { unsigned width{},height{}; std::vector<unsigned char> pixels; };
Image read_ppm(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary); Image image; std::string magic; unsigned maximum{};
  input>>magic>>image.width>>image.height>>maximum;
  require(input.good() && magic=="P6" && maximum==255 && image.width>0 && image.height>0 &&
      image.width<=viewport_max_dimension && image.height<=viewport_max_dimension,"invalid PPM header");
  require(input.get()=='\n',"invalid PPM separator");
  image.pixels.resize(static_cast<std::size_t>(image.width)*image.height*3);
  require(static_cast<bool>(input.read(reinterpret_cast<char*>(image.pixels.data()),static_cast<std::streamsize>(image.pixels.size()))),"short GPU readback image");
  require(input.peek()==std::char_traits<char>::eof(),"extra GPU readback bytes"); return image;
}
std::string json_quote(std::string_view text) {
  std::string result="\"";
  for (unsigned char c:text) {
    if (c=='\\' || c=='\"') { result+='\\'; result+=static_cast<char>(c); }
    else if (c>=32) result+=static_cast<char>(c);
  }
  return result+'\"';
}
RenderFrame fixture() {
  RenderFrame frame; frame.scene_generation=5; frame.origin_revision=3;
  frame.camera.eye={6,5,8}; frame.camera.target={0,.5,0};
  DrawPrimitive ground; ground.id={71,1}; ground.world={10,0,0,0, 0,.125,0,-.125, 0,0,10,0}; ground.color={.08f,.12f,.17f,1};
  DrawPrimitive blue; blue.id={71,2}; blue.world[3]=-1; blue.world[7]=.75; blue.world[5]=1.5;
  DrawPrimitive orange; orange.id={71,3}; orange.world[3]=1; orange.world[7]=.5; orange.color={.8f,.2f,.08f,1};
  DrawPrimitive green; green.id={71,4}; green.world[7]=.375; green.world[11]=-2; green.world[5]=.75; green.color={.1f,.6f,.2f,1};
  frame.draws={ground,blue,orange,green}; return frame;
}
struct OverlayEvidence { std::size_t changed_pixels{},rejected_frames{},maximum_complete_vertices{}; };
bool same_bytes(const std::filesystem::path& left,const std::filesystem::path& right) {
  std::ifstream a(left,std::ios::binary),b(right,std::ios::binary);
  require(a.good()&&b.good(),"overlay comparison input missing");
  using Bytes=std::istreambuf_iterator<char>;
  return std::equal(Bytes(a),Bytes{},Bytes(b),Bytes{});
}
OverlayEvidence overlay_contracts(WindowsViewport& viewport,const RenderFrame& scene,
                                 const std::filesystem::path& output) {
  OverlayEvidence evidence;
  auto baseline=scene;baseline.overlays.clear();
  require(viewport.draw(baseline),"overlay plain baseline draw");
  require(viewport.capture(output/"overlay-baseline.ppm"),"overlay plain baseline capture");
  const auto before=read_ppm(output/"overlay-baseline.ppm");
  require(viewport.stats().overlay_draws==0&&viewport.stats().overlay_vertices==0,"plain frame retained overlay stats");
  OverlayDraw triangle;triangle.color={4.f,.02f,.8f,1};
  triangle.vertices={{-.6f,-.6f},{.6f,-.6f},{0,.7f}};
  auto visible=baseline;visible.overlays={triangle};
  require(viewport.draw(visible),"overlay on selected imported mesh draw");
  require(viewport.capture(output/"overlay-visible.ppm"),"overlay visible capture");
  const auto after=read_ppm(output/"overlay-visible.ppm");
  require(after.width==before.width&&after.height==before.height,"overlay changed image dimensions");
  for (std::size_t i=0;i<before.pixels.size();i+=3)
    if (before.pixels[i]!=after.pixels[i]||before.pixels[i+1]!=after.pixels[i+1]||before.pixels[i+2]!=after.pixels[i+2])
      ++evidence.changed_pixels;
  require(evidence.changed_pixels>1000&&evidence.changed_pixels<before.pixels.size()/6,
      "overlay did not produce a bounded visible triangle");
  require(viewport.stats().overlay_draws==1&&viewport.stats().overlay_vertices==3,"overlay submitted stats mismatch");
  visible.overlays.clear();
  require(viewport.draw(visible),"cleared overlay draw");
  require(viewport.capture(output/"overlay-cleared.ppm"),"cleared overlay capture");
  require(same_bytes(output/"overlay-baseline.ppm",output/"overlay-cleared.ppm"),"clearing overlays changed baseline PPM bytes");

  // Preserve draw order: the later opaque overlay must exactly match drawing it
  // alone, even over a selected mesh whose outline is submitted before overlays.
  auto last=triangle;last.color={.02f,4.f,.3f,1};
  auto ordered=baseline;ordered.overlays={triangle,last};
  require(viewport.draw(ordered),"ordered overlapping overlays draw");
  require(viewport.capture(output/"overlay-ordered.ppm"),"ordered overlay capture");
  ordered.overlays={last};
  require(viewport.draw(ordered),"last overlay control draw");
  require(viewport.capture(output/"overlay-last-control.ppm"),"last overlay control capture");
  require(same_bytes(output/"overlay-ordered.ppm",output/"overlay-last-control.ppm"),"overlay draw order differs from last opaque draw");

  auto boundary=baseline;boundary.overlays={triangle};
  boundary.overlays[0].vertices={{-16.f,-16.f},{16.f,-16.f},{0,16.f}};
  boundary.overlays[0].color={0,64,0,1};
  require(viewport.draw(boundary),"inclusive overlay coordinate and color bounds draw");
  require(viewport.capture(output/"overlay-boundary.ppm"),"overlay boundary capture");
  // Keep the capacity probe's screen area tiny so maximum vertex admission does
  // not turn into an accidental full-screen overdraw benchmark.
  auto tiny=triangle;tiny.vertices={{-.9f,-.9f},{-.89f,-.9f},{-.9f,-.89f}};
  auto capacity=baseline;capacity.overlays.assign(viewport_max_overlay_draws,tiny);
  require(viewport.draw(capacity),"exact overlay draw cap");
  require(viewport.stats().overlay_draws==viewport_max_overlay_draws&&
      viewport.stats().overlay_vertices==viewport_max_overlay_draws*3,"overlay draw-cap stats mismatch");
  require(viewport.capture(output/"overlay-draw-cap.ppm"),"overlay draw-cap capture");
  evidence.maximum_complete_vertices=viewport_max_overlay_vertices-viewport_max_overlay_vertices%3;
  capacity.overlays={tiny};capacity.overlays[0].vertices.resize(evidence.maximum_complete_vertices);
  for (std::size_t i=0;i<capacity.overlays[0].vertices.size();++i)capacity.overlays[0].vertices[i]=tiny.vertices[i%3];
  require(viewport.draw(capacity),"largest complete overlay triangle list");
  require(viewport.stats().overlay_draws==1&&viewport.stats().overlay_vertices==evidence.maximum_complete_vertices,
      "overlay vertex-cap stats mismatch");
  require(viewport.capture(output/"overlay-vertex-cap.ppm"),"overlay vertex-cap capture");
  require(viewport.draw(baseline),"overlay validation baseline draw");
  require(viewport.capture(output/"overlay-validation-before.ppm"),"overlay validation baseline capture");
  require(same_bytes(output/"overlay-baseline.ppm",output/"overlay-validation-before.ppm"),"overlay slot growth changed plain frame bytes");

  const auto reject_overlay=[&](const RenderFrame& candidate,ErrorCode expected,const char* message) {
    const auto prior=viewport.stats();rejected(viewport.draw(candidate),expected,message);
    const auto current=viewport.stats();
    require(current.submitted_frames==prior.submitted_frames&&current.last_cpu_frame==prior.last_cpu_frame&&
        current.overlay_draws==prior.overlay_draws&&current.overlay_vertices==prior.overlay_vertices,
        "invalid overlay changed submitted frame or profiling identity");
    ++evidence.rejected_frames;
  };
  auto bad=baseline;bad.overlays.assign(viewport_max_overlay_draws+1,tiny);
  reject_overlay(bad,ErrorCode::Exhausted,"overlay draw count overflow accepted");
  bad=capacity;bad.overlays[0].vertices.resize(evidence.maximum_complete_vertices+3,tiny.vertices[0]);
  reject_overlay(bad,ErrorCode::Exhausted,"overlay vertex count overflow accepted");
  bad=capacity;bad.overlays.push_back(tiny);
  reject_overlay(bad,ErrorCode::Exhausted,"aggregate overlay vertex overflow accepted");
  bad=baseline;bad.overlays={triangle};bad.overlays[0].vertices.clear();
  reject_overlay(bad,ErrorCode::InvalidInput,"empty overlay draw accepted");
  bad.overlays[0].vertices={tiny.vertices[0],tiny.vertices[1]};
  reject_overlay(bad,ErrorCode::InvalidInput,"incomplete overlay triangle accepted");
  // 4096 is the byte-independent vertex ceiling; complete triangle lists stop at
  // 4095. Reaching the numerical ceiling with a dangling vertex must still fail.
  bad=capacity;bad.overlays[0].vertices.resize(viewport_max_overlay_vertices,tiny.vertices[0]);
  reject_overlay(bad,ErrorCode::InvalidInput,"overlay vertex ceiling accepted an incomplete triangle");
  const float infinity=std::numeric_limits<float>::infinity(),nan=std::numeric_limits<float>::quiet_NaN();
  const std::array invalid_coordinates{nan,infinity,-infinity,std::nextafter(16.f,infinity),std::nextafter(-16.f,-infinity)};
  for (std::size_t axis=0;axis<2;++axis)for (float coordinate:invalid_coordinates) {
    bad=baseline;bad.overlays={triangle};bad.overlays[0].vertices[1][axis]=coordinate;
    reject_overlay(bad,ErrorCode::InvalidInput,"invalid overlay NDC coordinate accepted");
  }
  for (std::size_t channel=0;channel<4;++channel)for (float value:{nan,infinity}) {
    bad=baseline;bad.overlays={triangle};bad.overlays[0].color[channel]=value;
    reject_overlay(bad,ErrorCode::InvalidInput,"nonfinite overlay color accepted");
  }
  for (std::size_t channel=0;channel<3;++channel)for (float value:{-.01f,std::nextafter(64.f,infinity)}) {
    bad=baseline;bad.overlays={triangle};bad.overlays[0].color[channel]=value;
    reject_overlay(bad,ErrorCode::InvalidInput,"out-of-range overlay color accepted");
  }
  for (float alpha:{0.f,.5f,2.f}) {
    bad=baseline;bad.overlays={triangle};bad.overlays[0].color[3]=alpha;
    reject_overlay(bad,ErrorCode::InvalidInput,"nonopaque overlay accepted");
  }
  require(viewport.capture(output/"overlay-rejections-after.ppm"),"overlay rejection preserved capture");
  require(same_bytes(output/"overlay-baseline.ppm",output/"overlay-rejections-after.ppm"),"rejected overlay altered the last submitted image");
  require(viewport.draw(baseline),"valid plain frame after overlay rejections");
  require(viewport.capture(output/"overlay-final-plain.ppm"),"final plain overlay capture");
  require(same_bytes(output/"overlay-baseline.ppm",output/"overlay-final-plain.ppm"),"valid recovery after overlay rejection changed baseline");
  valid_profile(viewport.stats());
  return evidence;
}
struct MaterialEvidence {
  std::array<unsigned,3> gray_expected{},gray_observed{};
  std::size_t orientation_samples{},rejected_frames{};
  std::uint64_t stable_uploads{},stable_cache_hits{};
};
std::shared_ptr<const ViewportMaterial> material_packet(unsigned width,unsigned height,
    std::span<const std::byte> pixels,std::array<float,4> factor={1,1,1,1}) {
  auto texture=ViewportTexture::create(width,height,pixels); require(static_cast<bool>(texture),"texture packet factory");
  auto material=ViewportMaterial::create(factor,std::move(texture).value());
  require(static_cast<bool>(material),"material packet factory"); return std::move(material).value();
}
unsigned expected_display(double linear) {
  linear/=1+linear;
  const double encoded=linear<=.0031308 ? 12.92*linear : 1.055*std::pow(linear,1/2.4)-.055;
  return static_cast<unsigned>(std::lround(encoded*255));
}
MaterialEvidence material_contracts(WindowsViewport& viewport,const std::filesystem::path& output) {
  MaterialEvidence evidence;
  // Independent camera fixture: a two-metre square at z=0, front viewed from
  // z=3 with a 90-degree vertical field. Texture row zero is the upper edge.
  const std::array<GeometryVertex,4> vertices{{{{-1,1,0},{0,0,1},{0,0}},
      {{1,1,0},{0,0,1},{1,0}},{{1,-1,0},{0,0,1},{1,1}},{{-1,-1,0},{0,0,1},{0,1}}}};
  const std::array<std::uint32_t,6> indices{0,1,2,0,2,3};
  auto geometry=ViewportGeometry::create(vertices,indices,true); require(static_cast<bool>(geometry),"material quad geometry");
  RenderFrame baseline; baseline.camera.eye={0,0,3}; baseline.camera.target={0,0,0};
  baseline.camera.vertical_fov_radians=1.5707963267948966;
  // Keep the viewport's optimized default clear; color probes lie inside the quad.
  baseline.grid=false; baseline.ambient_intensity=1; baseline.directional_intensity=0;
  DrawPrimitive quad; quad.id={79,1}; quad.color={1,1,1,1}; quad.geometry=std::move(geometry).value(); baseline.draws={quad};
  require(viewport.draw(baseline),"material baseline draw");
  require(viewport.capture(output/"material-baseline.ppm"),"material baseline capture");
  auto textured=baseline;
  auto white=ViewportMaterial::create(); require(static_cast<bool>(white),"white material");
  textured.draws[0].material=std::move(white).value();
  require(viewport.draw(textured),"white factor draw");
  require(viewport.capture(output/"material-white-factor.ppm"),"white factor capture");
  require(same_bytes(output/"material-baseline.ppm",output/"material-white-factor.ppm"),"white factor changed untextured pixels");
  const std::array white_pixel{std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  textured.draws[0].material=material_packet(1,1,white_pixel);
  require(viewport.draw(textured),"white texture draw");
  require(viewport.capture(output/"material-white-texture.ppm"),"white texture capture");
  require(same_bytes(output/"material-baseline.ppm",output/"material-white-texture.ppm"),"white texture changed untextured pixels");

  // Gray is checked against the analytic transfer functions, not another render
  // path. Tint and linear material factor are both deliberately non-unit.
  std::array gray{std::byte{128},std::byte{128},std::byte{128},std::byte{255}};
  textured.draws[0].color={.5f,.75f,1,1};
  textured.draws[0].material=material_packet(1,1,gray,{.5f,1,.25f,1}); gray.fill(std::byte{0});
  require(viewport.draw(textured),"sRGB gray texture draw");
  require(viewport.capture(output/"material-gray.ppm"),"sRGB gray capture");
  const auto gray_image=read_ppm(output/"material-gray.ppm");
  const double decoded=std::pow((128.0/255+.055)/1.055,2.4);
  const std::array<double,3> factors{.25,.75,.25};
  for (std::size_t channel=0;channel<3;++channel) {
    evidence.gray_expected[channel]=expected_display(decoded*factors[channel]);
    evidence.gray_observed[channel]=gray_image.pixels[(static_cast<std::size_t>(gray_image.height/2)*gray_image.width+gray_image.width/2)*3+channel];
    require(std::abs(static_cast<int>(evidence.gray_expected[channel])-static_cast<int>(evidence.gray_observed[channel]))<=2,
        "sRGB decode/factor/tint/output transfer disagrees with gray oracle");
  }
  // Prime the slots selected by Present, then reuse the exact immutable packet
  // six more times without another upload. This does not assert physical slot coverage.
  for (unsigned i=0;i<3;++i) require(viewport.draw(textured),"prime texture slots");
  const auto primed=viewport.stats();
  for (unsigned i=0;i<6;++i) require(viewport.draw(textured),"unchanged cached texture draw");
  evidence.stable_uploads=viewport.stats().texture_uploads-primed.texture_uploads;
  evidence.stable_cache_hits=viewport.stats().texture_cache_hits-primed.texture_cache_hits;
  require(evidence.stable_uploads==0 && evidence.stable_cache_hits==6,"unchanged slot texture was reuploaded");
  std::weak_ptr<const ViewportTexture> retained=textured.draws[0].material->texture();
  textured.draws[0].material.reset();
  require(!retained.expired(),"GPU slot did not retain submitted texture packet");
  require(viewport.capture(output/"material-retained.ppm"),"retained packet capture");
  require(same_bytes(output/"material-gray.ppm",output/"material-retained.ppm"),"source release changed retained texture pixels");
  for (unsigned i=0;i<3;++i) require(viewport.draw(baseline),"retire both texture slots");
  require(retained.expired(),"unused completed texture packet remained retained");

  // Odd-width rows require 256-byte GPU pitch; corner probes are inside the
  // CLAMP regions and therefore have exact texel colors despite linear sampling.
  const std::array<unsigned char,24> colors{255,0,0,255, 0,255,0,255, 0,0,255,255,
      0,255,255,255, 255,0,255,255, 255,255,0,255};
  textured=baseline; textured.draws[0].material=material_packet(3,2,std::as_bytes(std::span(colors)));
  require(viewport.draw(textured),"odd-width top-left texture draw");
  require(viewport.capture(output/"material-odd-width.ppm"),"odd-width texture capture");
  const auto orientation=read_ppm(output/"material-odd-width.ppm");
  const std::array<std::array<double,2>,4> points{{{-.9,.9},{.9,.9},{-.9,-.9},{.9,-.9}}};
  const std::array<std::array<unsigned,3>,4> expected{{{1,0,0},{0,0,1},{0,1,1},{1,1,0}}};
  for (std::size_t point=0;point<points.size();++point) {
    const double focal=static_cast<double>(orientation.height)/2;
    const auto x=static_cast<unsigned>(static_cast<double>(orientation.width)/2+points[point][0]*focal/3);
    const auto y=static_cast<unsigned>(static_cast<double>(orientation.height)/2-points[point][1]*focal/3);
    for (std::size_t channel=0;channel<3;++channel) {
      const auto actual=orientation.pixels[(static_cast<std::size_t>(y)*orientation.width+x)*3+channel];
      require(std::abs(static_cast<int>(actual)-static_cast<int>(expected_display(expected[point][channel])))<=2,
          "odd-width texture upload or top-left UV orientation mismatch");
    }
    ++evidence.orientation_samples;
  }
  // Selected lines and owned overlays must never inherit the last texture flag.
  textured.draws[0].selected=true;
  OverlayDraw overlay; overlay.color={.1f,.4f,.8f,1}; overlay.vertices={{-.1f,-.1f},{.1f,-.1f},{0,.1f}};
  textured.overlays={overlay}; require(viewport.draw(textured),"textured selected overlay draw");
  require(viewport.capture(output/"material-selected-overlay.ppm"),"textured selection overlay capture");
  auto decoration_control=textured; decoration_control.draws[0].material.reset();
  require(viewport.draw(decoration_control),"untextured decoration control");
  require(viewport.capture(output/"material-decoration-control.ppm"),"decoration control capture");
  const auto decorated=read_ppm(output/"material-selected-overlay.ppm"),control=read_ppm(output/"material-decoration-control.ppm");
  std::size_t gold_pixels{};
  for (std::size_t i=0;i<control.pixels.size();i+=3) {
    if (control.pixels[i]>220 && control.pixels[i+1]>180 && control.pixels[i+2]<100) {
      require(std::equal(control.pixels.begin()+i,control.pixels.begin()+i+3,decorated.pixels.begin()+i),"texture leaked into selection lines");
      ++gold_pixels;
    }
  }
  require(gold_pixels>100,"selection line oracle had no visible gold pixels");
  const auto center=(static_cast<std::size_t>(control.height/2)*control.width+control.width/2)*3;
  require(std::equal(control.pixels.begin()+center,control.pixels.begin()+center+3,decorated.pixels.begin()+center),"texture leaked into overlay");

  // Built-in Box carries per-face UVs and accepts the same native texture.
  auto box=textured; box.overlays.clear(); box.draws[0].geometry.reset(); box.draws[0].selected=false;
  require(viewport.draw(box),"textured native unit cube draw");
  require(viewport.capture(output/"material-box.ppm"),"textured cube capture");
  auto uvless=ViewportGeometry::create(vertices,indices); require(static_cast<bool>(uvless),"legacy UV-less packet");
  auto bad=box; bad.draws[0].geometry=std::move(uvless).value();
  const auto reject=[&](const RenderFrame& invalid,ErrorCode code,const char* message) {
    const auto previous=viewport.stats(); rejected(viewport.draw(invalid),code,message);
    require(viewport.stats().submitted_frames==previous.submitted_frames && viewport.stats().texture_uploads==previous.texture_uploads,
        "rejected texture frame submitted work"); ++evidence.rejected_frames;
  };
  reject(bad,ErrorCode::InvalidInput,"texture accepted on UV-less mesh");
  require(viewport.capture(output/"material-rejection-preserved.ppm"),"texture rejection capture");
  require(same_bytes(output/"material-box.ppm",output/"material-rejection-preserved.ppm"),"texture rejection changed prior image");
  auto capacity=box; capacity.draws.clear();
  for (std::size_t i=0;i<viewport_max_textures+1;++i) {
    auto draw=box.draws[0]; draw.id={80,i+1}; draw.material=material_packet(1,1,white_pixel); capacity.draws.push_back(std::move(draw));
  }
  reject(capacity,ErrorCode::Exhausted,"17 unique textures accepted");
  capacity.draws.pop_back(); require(viewport.draw(capacity),"16 unique textures draw");
  require(viewport.stats().frame_textures==16 && viewport.stats().frame_texture_bytes==64,"texture identity deduplication count incorrect");
  capacity.draws.clear(); std::vector<std::byte> maximum(1024*1024*4,std::byte{255});
  for (std::size_t i=0;i<9;++i) {
    auto draw=box.draws[0]; draw.id={81,i+1}; draw.material=material_packet(1024,1024,maximum); capacity.draws.push_back(std::move(draw));
  }
  reject(capacity,ErrorCode::Exhausted,"texture texel bytes above32MiB accepted");
  capacity.draws.pop_back(); require(viewport.draw(capacity),"exact32MiB texture frame");
  require(viewport.stats().frame_texture_bytes==viewport_texture_frame_bytes,"texture byte boundary not reached");
  // Many instances of one shared packet count once, not once per draw.
  for (auto& draw:capacity.draws) draw.material=capacity.draws[0].material;
  require(viewport.draw(capacity),"shared texture deduplication draw");
  require(viewport.stats().frame_textures==1 && viewport.stats().frame_texture_bytes==maximum.size(),"shared texture charged repeatedly");
  for (unsigned i=0;i<3;++i) require(viewport.draw(baseline),"texture cache replacement retirement");
  require(viewport.capture(output/"material-final-baseline.ppm"),"post texture baseline capture");
  require(same_bytes(output/"material-baseline.ppm",output/"material-final-baseline.ppm"),"texture tests changed untextured baseline");
  return evidence;
}
struct MipProbe {
  std::string name,capture;
  TextureSampling sampling{};
  unsigned mip_count{},x{},y{};
  std::size_t chain_bytes{};
  double distance{},vertical_fov{},uv_span{},world_scale{},footprint_lod{},resolved_lod{};
  std::array<unsigned,3> expected{},observed{};
  unsigned maximum_error{};
};
struct CheckerProbe {
  std::string name,capture;
  TextureSampling sampling{};
  ViewportVec3 eye;
  std::array<unsigned,64> expected{},observed{};
  unsigned maximum_error{},expected_min{255},expected_max{},observed_min{255},observed_max{};
};
struct MipRegion {
  std::string name,capture,category;
  std::array<unsigned,4> rectangle{};
  std::array<unsigned,3> expected{};
  std::map<std::array<unsigned,3>,std::size_t> colors;
  ViewportVec3 eye,target;
  double footprint_lod{},world_scale{};
  std::size_t pixels{},outside_tolerance{};
  unsigned maximum_error{};
};
struct MipEvidence {
  std::vector<MipRegion> regions;
  std::size_t region_pixels{};
  unsigned region_maximum_error{};
  std::vector<CheckerProbe> checker_probes;
  std::vector<MipProbe> probes;
  std::size_t motion_frames{},maximum_chain_frame_bytes{};
  unsigned motion_maximum_error{};
  std::uint64_t stable_uploads{},stable_cache_hits{},upload_bytes{};
};
using MipPalette=std::array<std::array<unsigned,3>,viewport_texture_max_mips>;
std::vector<std::byte> diagnostic_chain(unsigned width,unsigned height,const MipPalette& palette) {
  std::vector<std::byte> bytes;
  std::size_t capacity{};
  for (auto w=width,h=height;;w=std::max(1u,w/2),h=std::max(1u,h/2)) {
    capacity+=static_cast<std::size_t>(w)*h*4;
    if (w==1 && h==1) break;
  }
  bytes.reserve(capacity);
  for (unsigned level=0;;++level) {
    require(level<palette.size(),"diagnostic mip palette overflow");
    for (std::size_t texel=0;texel<static_cast<std::size_t>(width)*height;++texel) {
      for (unsigned channel:palette[level]) bytes.push_back(static_cast<std::byte>(channel));
      bytes.push_back(std::byte{255});
    }
    if (width==1 && height==1) break;
    width=std::max(1u,width/2); height=std::max(1u,height/2);
  }
  return bytes;
}
std::shared_ptr<const ViewportGeometry> mip_quad(double span) {
  const auto low=static_cast<float>(.5-span/2),high=static_cast<float>(.5+span/2);
  const std::array<GeometryVertex,4> vertices{{{{-1,1,0},{0,0,1},{low,low}},
      {{1,1,0},{0,0,1},{high,low}},{{1,-1,0},{0,0,1},{high,high}},{{-1,-1,0},{0,0,1},{low,high}}}};
  const std::array<std::uint32_t,6> indices{0,1,2,0,2,3};
  auto geometry=ViewportGeometry::create(vertices,indices,true);
  require(static_cast<bool>(geometry),"mip diagnostic quad"); return std::move(geometry).value();
}
std::shared_ptr<const ViewportMaterial> mip_material(std::shared_ptr<const ViewportTexture> texture,TextureSampling sampling) {
  auto material=ViewportMaterial::create({1,1,1,1},std::move(texture),sampling);
  require(static_cast<bool>(material),"mip diagnostic material"); return std::move(material).value();
}
double decode_srgb(unsigned channel) {
  const double value=static_cast<double>(channel)/255;
  return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4);
}
MipEvidence mip_contracts(WindowsViewport& viewport,const std::filesystem::path& output) {
  // Distinct constant colors per level independently expose SRV coverage, level
  // selection and linear-space blending; these deliberately are not cooked art.
  const MipPalette palette{{{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},
      {0,255,255},{255,255,255},{128,128,128},{32,192,64},{255,128,0},{255,255,255}}};
  auto chain=diagnostic_chain(256,256,palette);
  require(chain.size()==349524,"diagnostic mip chain byte oracle");
  auto made=ViewportTexture::create_mips(256,256,chain);
  require(static_cast<bool>(made),"diagnostic full-chain packet");
  auto texture=std::move(made).value();
  auto filtered=mip_material(texture,TextureSampling::LinearClampMips);
  auto legacy=mip_material(texture,TextureSampling::LinearClampMip0);
  constexpr double right_angle=1.5707963267948966;
  const double narrow=2*std::atan(.5);
  RenderFrame baseline; baseline.camera.eye={0,0,3}; baseline.camera.target={0,0,0};
  baseline.camera.vertical_fov_radians=right_angle; baseline.grid=false;
  baseline.ambient_intensity=1; baseline.directional_intensity=0;
  DrawPrimitive quad; quad.id={82,1}; quad.color={1,1,1,1}; quad.geometry=mip_quad(1); baseline.draws={quad};
  require(viewport.draw(baseline),"mip untextured baseline");
  require(viewport.capture(output/"mip-baseline.ppm"),"mip baseline capture");
  const auto initial=viewport.stats();
  require(initial.width==640 && initial.height==480,"mip footprint fixture dimensions");
  MipEvidence evidence;
  const auto observe=[&](std::string name,const std::string& capture,const Image& image,
      const ViewportMaterial& material,const MipPalette& colors,double distance,double fov,
      double uv_span,double world_scale,unsigned x,unsigned y) {
    const auto& packet=*material.texture();
    MipProbe probe; probe.name=std::move(name); probe.capture=capture; probe.sampling=material.sampling();
    probe.mip_count=packet.mip_count(); probe.chain_bytes=packet.texels().size(); probe.x=x; probe.y=y;
    probe.distance=distance; probe.vertical_fov=fov; probe.uv_span=uv_span; probe.world_scale=world_scale;
    // The planar square is two world units wide before scaling. A vertical FOV
    // gives height/(distance*tan(fov/2)) pixels per full square. Hardware's
    // isotropic footprint is the larger of the two texture-space derivatives.
    probe.footprint_lod=std::log2(std::max(packet.width(),packet.height())*uv_span*distance*
        std::tan(fov/2)/(image.height*world_scale));
    probe.resolved_lod=material.sampling()==TextureSampling::LinearClampMip0 ? 0 :
        std::clamp(probe.footprint_lod,0.0,static_cast<double>(packet.mip_count()-1));
    const auto low=static_cast<unsigned>(std::floor(probe.resolved_lod));
    const auto high=std::min(low+1,packet.mip_count()-1);
    const double fraction=probe.resolved_lod-low;
    require(x<image.width && y<image.height,"mip probe pixel is outside capture");
    for (std::size_t channel=0;channel<3;++channel) {
      const double linear=decode_srgb(colors[low][channel])*(1-fraction)+decode_srgb(colors[high][channel])*fraction;
      probe.expected[channel]=expected_display(linear);
      probe.observed[channel]=image.pixels[(static_cast<std::size_t>(y)*image.width+x)*3+channel];
      const auto error=static_cast<unsigned>(std::abs(static_cast<int>(probe.expected[channel])-static_cast<int>(probe.observed[channel])));
      probe.maximum_error=std::max(probe.maximum_error,error);
    }
    evidence.probes.push_back(probe);
    if (probe.maximum_error>3) {
      std::fprintf(stderr,"mip oracle %s LOD %.9f: expected %u,%u,%u observed %u,%u,%u\n",probe.name.c_str(),
          probe.resolved_lod,probe.expected[0],probe.expected[1],probe.expected[2],probe.observed[0],probe.observed[1],probe.observed[2]);
      throw std::runtime_error("mip hardware sampling disagrees with analytic oracle");
    }
    return probe.maximum_error;
  };
  const auto region=[&](const std::string& name,const std::string& capture,const std::string& category,
      std::array<unsigned,4> rectangle,ViewportVec3 eye,ViewportVec3 target,double world_scale,double world_x=0.0) {
    const auto image=read_ppm(output/capture);
    require(image.width==640 && image.height==480 && rectangle[2] && rectangle[3] &&
        rectangle[0]<image.width && rectangle[1]<image.height && rectangle[2]<=image.width-rectangle[0] &&
        rectangle[3]<=image.height-rectangle[1],"mip region lies outside its capture");
    require(eye.x==target.x && eye.y==target.y && target.z==0 && eye.z>0 && world_scale>0,
        "mip region oracle requires a front-facing planar camera");
    const double focal=image.height/(2*std::tan(right_angle/2));
    const double half_width=world_scale*focal/eye.z;
    const double center_x=image.width/2.0+(world_x-eye.x)*focal/eye.z;
    const double center_y=image.height/2.0+eye.y*focal/eye.z;
    require(rectangle[0]+.5>center_x-half_width && rectangle[0]+rectangle[2]-.5<center_x+half_width &&
        rectangle[1]+.5>center_y-half_width && rectangle[1]+rectangle[3]-.5<center_y+half_width,
        "mip region includes a pixel center outside the projected quad");
    MipRegion record; record.name=name; record.capture=capture; record.category=category;
    record.rectangle=rectangle; record.eye=eye; record.target=target; record.world_scale=world_scale;
    // Independent planar-camera footprint. Derive the continuous expected level
    // in double precision and interpolate decoded colors without fixed-point
    // weights or snapping. The test never reads a shader-calculated level.
    record.footprint_lod=std::log2(256*eye.z*std::tan(right_angle/2)/(image.height*world_scale));
    const double lod=std::clamp(record.footprint_lod,0.0,8.0);
    const auto low=static_cast<unsigned>(std::floor(lod)),high=std::min(low+1,8u);
    const double fraction=lod-low;
    for (std::size_t channel=0;channel<3;++channel)
      record.expected[channel]=expected_display(decode_srgb(palette[low][channel])*(1-fraction)+
          decode_srgb(palette[high][channel])*fraction);
    for (unsigned y=rectangle[1];y<rectangle[1]+rectangle[3];++y)
      for (unsigned x=rectangle[0];x<rectangle[0]+rectangle[2];++x) {
        std::array<unsigned,3> rgb{}; unsigned pixel_error{};
        const auto offset=(static_cast<std::size_t>(y)*image.width+x)*3;
        for (std::size_t channel=0;channel<3;++channel) {
          rgb[channel]=image.pixels[offset+channel];
          pixel_error=std::max(pixel_error,static_cast<unsigned>(std::abs(
              static_cast<int>(rgb[channel])-static_cast<int>(record.expected[channel]))));
        }
        ++record.colors[rgb]; ++record.pixels;
        record.maximum_error=std::max(record.maximum_error,pixel_error);
        if (pixel_error>3) ++record.outside_tolerance;
      }
    require(record.pixels==static_cast<std::size_t>(rectangle[2])*rectangle[3],"mip region skipped pixels");
    evidence.region_pixels+=record.pixels;
    evidence.region_maximum_error=std::max(evidence.region_maximum_error,record.maximum_error);
    evidence.regions.push_back(record);
    if (record.outside_tolerance) {
      std::fprintf(stderr,"mip region %s LOD %.12f: expected %u,%u,%u maximum error %u; %zu/%zu pixels exceed tolerance3\n",
          name.c_str(),record.footprint_lod,record.expected[0],record.expected[1],record.expected[2],
          record.maximum_error,record.outside_tolerance,record.pixels);
      throw std::runtime_error("mip whole-region sampling disagrees with independent continuous oracle");
    }
  };
  const auto sample=[&](const std::string& name,const std::shared_ptr<const ViewportMaterial>& material,
      const MipPalette& colors,double distance,double fov=1.5707963267948966,double uv_span=1.0) {
    auto frame=baseline; frame.camera.eye.z=distance; frame.camera.vertical_fov_radians=fov;
    frame.draws[0].geometry=mip_quad(uv_span); frame.draws[0].material=material;
    require(viewport.draw(frame),"mip sample draw");
    require(viewport.stats().frame_textures==1 && viewport.stats().frame_texture_bytes==material->texture()->texels().size(),
        "mip frame charged base level rather than full chain");
    const auto capture=name+".ppm"; require(viewport.capture(output/capture),"mip sample capture");
    const auto image=read_ppm(output/capture);
    return observe(name,capture,image,*material,colors,distance,fov,uv_span,1,image.width/2,image.height/2);
  };
  sample("mip-zero-magnified",filtered,palette,.9375);
  sample("mip-legacy-zero-minified",legacy,palette,7.5);
  sample("mip-level-one",filtered,palette,3.75);
  // Full raster coverage, including both triangles and all derivative stamps:
  // pixel centers [256.5,383.5] x [176.5,303.5] lie inside the 128-square quad.
  region("mip-level-one-full","mip-level-one.ppm","original-speckle-region",{256,176,128,128},{0,0,3.75},{0,0,0},1);
  sample("mip-level-two",filtered,palette,7.5);
  region("mip-level-two-full","mip-level-two.ppm","original-speckle-region",{288,208,64,64},{0,0,7.5},{0,0,0},1);
  sample("mip-fractional-one-half",filtered,palette,1.875*std::exp2(1.5));
  sample("mip-narrow-fov-zero",filtered,palette,3.75,narrow);
  sample("mip-equal-footprint-one",filtered,palette,7.5,narrow);
  sample("mip-last-level",filtered,palette,30,right_angle,16);
  sample("mip-last-level-clamped",filtered,palette,30,right_angle,32);
  auto odd_bytes=diagnostic_chain(3,5,palette); auto odd=ViewportTexture::create_mips(3,5,odd_bytes);
  require(static_cast<bool>(odd) && odd_bytes.size()==72,"NPOT full-chain fixture");
  sample("mip-npot-padded-rows",mip_material(std::move(odd).value(),TextureSampling::LinearClampMips),palette,
      480.0/(5*64)*std::exp2(1.5),right_angle,64);
  // An explicitly complete 1x1 chain has one level, unlike an incomplete legacy
  // packet. Both magnification and a level-six footprint must clamp to level
  // zero without requesting an upper mip from the single-level SRV.
  auto one_level_packet=ViewportTexture::create_mips(1,1,diagnostic_chain(1,1,palette));
  require(static_cast<bool>(one_level_packet) && one_level_packet.value()->complete_mip_chain() &&
      one_level_packet.value()->mip_count()==1,"explicit complete 1x1 mip fixture");
  auto one_level_material=mip_material(std::move(one_level_packet).value(),TextureSampling::LinearClampMips);
  sample("mip-single-level-magnified",one_level_material,palette,.9375);
  sample("mip-single-level-minified",one_level_material,palette,30,right_angle,1024);
  // A +/-1/512 continuous LOD shift introduces a six-code red/green/blue
  // component for this palette. Snapping either side to its integer endpoint
  // would therefore fail the existing three-code tolerance. Central 60x60
  // regions stay at least 1.45 pixels inside the smallest projected quad.
  const auto continuity_sample=[&](const std::string& name,const std::string& category,double lod,
      double horizontal_pixels=0.0,double vertical_pixels=0.0) {
    auto frame=baseline; const double distance=1.875*std::exp2(lod);
    const double x=horizontal_pixels*distance/240,y=vertical_pixels*distance/240;
    frame.camera.eye={x,y,distance}; frame.camera.target={x,y,0}; frame.draws[0].material=filtered;
    require(viewport.draw(frame),"mip continuity region draw");
    require(viewport.stats().frame_textures==1 && viewport.stats().frame_texture_bytes==chain.size(),
        "mip continuity changed chain accounting");
    const auto capture=name+".ppm";
    require(viewport.capture(output/capture),"mip continuity region capture");
    region(name,capture,category,{290,210,60,60},frame.camera.eye,frame.camera.target,1);
  };
  for (unsigned boundary=1;boundary<=2;++boundary) {
    continuity_sample("mip-boundary-"+std::to_string(boundary)+"-below","boundary-continuity",boundary-1.0/512);
    continuity_sample("mip-boundary-"+std::to_string(boundary)+"-above","boundary-continuity",boundary+1.0/512);
  }
  // Parallel eye/target translations preserve the exact planar footprint while
  // moving coverage by subpixels, exposing a dependence on derivative stamps.
  continuity_sample("mip-subpixel-camera-a","subpixel-camera",2,.25,.375);
  continuity_sample("mip-subpixel-camera-b","subpixel-camera",2,.75,-.625);
  // A bounded camera-only sequence crosses two mip boundaries, checking every
  // frame against its fractional linear-space oracle instead of another path.
  for (unsigned frame=0;frame<9;++frame) {
    const double lod=.25+static_cast<double>(frame)*.25;
    const auto error=sample("mip-motion-0"+std::to_string(frame),filtered,palette,1.875*std::exp2(lod));
    evidence.motion_maximum_error=std::max(evidence.motion_maximum_error,error); ++evidence.motion_frames;
  }
  // A real one-texel black/white checker averages to encoded 188 after an
  // opaque linear-light reduction. All remaining complete levels are constant
  // 188, independently of the diagnostic palette used above. Each central 8x8
  // sample remains safely inside the quad for these bounded camera positions.
  MipPalette checker_palette; checker_palette.fill({188,188,188});
  auto checker_bytes=diagnostic_chain(256,256,checker_palette);
  for (unsigned y=0;y<256;++y) for (unsigned x=0;x<256;++x)
    for (std::size_t channel=0;channel<3;++channel)
      checker_bytes[(static_cast<std::size_t>(y)*256+x)*4+channel]=((x+y)%2) ? std::byte{255} : std::byte{0};
  auto checker_packet=ViewportTexture::create_mips(256,256,checker_bytes);
  require(static_cast<bool>(checker_packet),"checker complete-chain packet");
  auto checker_filtered=mip_material(checker_packet.value(),TextureSampling::LinearClampMips);
  auto checker_legacy=mip_material(std::move(checker_packet).value(),TextureSampling::LinearClampMip0);
  const auto checker_sample=[&](const std::string& name,const std::shared_ptr<const ViewportMaterial>& material,ViewportVec3 eye) {
    auto frame=baseline; frame.camera.eye=eye; frame.draws[0].material=material;
    require(viewport.draw(frame),"checker minification draw");
    require(viewport.stats().frame_textures==1 && viewport.stats().frame_texture_bytes==checker_bytes.size(),
        "checker chain accounting mismatch");
    CheckerProbe probe; probe.name=name; probe.capture=name+".ppm"; probe.sampling=material->sampling(); probe.eye=eye;
    require(viewport.capture(output/probe.capture),"checker minification capture");
    const auto image=read_ppm(output/probe.capture);
    for (unsigned y=236;y<244;++y) for (unsigned x=316;x<324;++x) {
      const auto sample_index=static_cast<std::size_t>(y-236)*8+x-316;
      unsigned expected=expected_display(decode_srgb(188));
      if (probe.sampling==TextureSampling::LinearClampMip0) {
        require(eye.x==0 && eye.y==0,"legacy checker oracle requires its front-facing planar camera");
        // Pixel-center projection -> normalized UV -> mip-zero texel centers.
        // Reconstruct the four black/white texels and bilinear weights in linear
        // space; no rendered reference or arbitrary noise threshold is used.
        const double tx=127.5+(static_cast<double>(x)+.5-320)*eye.z*256/480;
        const double ty=127.5+(static_cast<double>(y)+.5-240)*eye.z*256/480;
        const auto ix=static_cast<unsigned>(std::floor(tx)),iy=static_cast<unsigned>(std::floor(ty));
        require(ix<255 && iy<255,"legacy checker oracle reached the clamp boundary");
        const double fx=tx-ix,fy=ty-iy,corner=static_cast<double>((ix+iy)%2);
        expected=expected_display(corner*(1-fx)*(1-fy)+(1-corner)*fx*(1-fy)+(1-corner)*(1-fx)*fy+corner*fx*fy);
      }
      probe.expected[sample_index]=expected;
      probe.expected_min=std::min(probe.expected_min,expected); probe.expected_max=std::max(probe.expected_max,expected);
      const auto offset=(static_cast<std::size_t>(y)*image.width+x)*3;
      probe.observed[sample_index]=image.pixels[offset];
      for (std::size_t channel=0;channel<3;++channel) {
        const auto observed=static_cast<unsigned>(image.pixels[offset+channel]);
        probe.observed_min=std::min(probe.observed_min,observed); probe.observed_max=std::max(probe.observed_max,observed);
        probe.maximum_error=std::max(probe.maximum_error,static_cast<unsigned>(std::abs(static_cast<int>(expected)-static_cast<int>(observed))));
      }
    }
    evidence.checker_probes.push_back(probe);
    if (probe.maximum_error>3) {
      std::fprintf(stderr,"checker oracle %s: error %u expected range %u..%u observed %u..%u\n",probe.name.c_str(),
          probe.maximum_error,probe.expected_min,probe.expected_max,probe.observed_min,probe.observed_max);
      throw std::runtime_error("checker minification differs from independent pixel oracle");
    }
  };
  const std::array checker_distances{13.7,14.5,15.4,16.3,17.1};
  for (std::size_t i=0;i<checker_distances.size();++i)
    checker_sample("mip-checker-motion-0"+std::to_string(i),checker_filtered,{0,0,checker_distances[i]});
  checker_sample("mip-checker-legacy-minified",checker_legacy,{0,0,15.4});
  const auto& checker_comparison=evidence.checker_probes.back();
  const auto average=expected_display(decode_srgb(188));
  require(checker_comparison.expected_min+3<average-3 && checker_comparison.expected_max-3>average+3,
      "checker legacy oracle did not distinguish spatial alias from filtered average");
  // This bounded oblique view checks that gradients are still used. It does not
  // establish anisotropic filtering quality; the profile remains isotropic.
  checker_sample("mip-checker-oblique",checker_filtered,{6.16,0,15.4});
  // One immutable texture can serve both profiles in the same frame. The half-
  // scale quads have a level-two footprint at this distance, but the legacy one
  // must still sample level zero. Resource accounting deduplicates the packet.
  auto mixed=baseline; mixed.camera.eye.z=3.75; mixed.draws.resize(2,quad);
  for (std::size_t i=0;i<2;++i) {
    auto& draw=mixed.draws[i]; draw.id={83,i+1}; draw.world[0]=draw.world[5]=draw.world[10]=.5;
    draw.world[3]=i==0 ? -.8 : .8; draw.material=i==0 ? legacy : filtered;
  }
  for (unsigned frame=0;frame<3;++frame) require(viewport.draw(mixed),"prime mip profile cache slots");
  const auto primed=viewport.stats();
  for (unsigned frame=0;frame<6;++frame) require(viewport.draw(mixed),"stable mixed mip profile draw");
  evidence.stable_uploads=viewport.stats().texture_uploads-primed.texture_uploads;
  evidence.stable_cache_hits=viewport.stats().texture_cache_hits-primed.texture_cache_hits;
  require(evidence.stable_uploads==0 && evidence.stable_cache_hits==6,"mixed mip profiles reuploaded unchanged texture");
  require(viewport.stats().frame_textures==1 && viewport.stats().frame_texture_bytes==chain.size(),"mixed profiles charged shared chain twice");
  require(viewport.capture(output/"mip-mixed-profiles.ppm"),"mixed mip profiles capture");
  const auto mixed_image=read_ppm(output/"mip-mixed-profiles.ppm");
  for (std::size_t i=0;i<2;++i) {
    const auto x=static_cast<unsigned>(320+mixed.draws[i].world[3]*240/3.75);
    observe(i==0 ? "mip-mixed-legacy" : "mip-mixed-filtered","mip-mixed-profiles.ppm",mixed_image,
        *mixed.draws[i].material,palette,3.75,right_angle,1,.5,x,240);
  }
  // The filtered half-scale quad covers x=[339.2,403.2], y=[208,272]. All
  // 4096 selected pixel centers are inside it; the legacy quad is disjoint.
  region("mip-mixed-filtered-full","mip-mixed-profiles.ppm","original-speckle-region",
      {339,208,64,64},{0,0,3.75},{0,0,0},.5,.8);
  require(evidence.regions.size()==9 && evidence.region_pixels==46176,"mip region coverage count changed");
  std::weak_ptr<const ViewportTexture> retained=texture;
  mixed.draws.clear(); filtered.reset(); legacy.reset(); texture.reset();
  require(!retained.expired(),"mip slots failed to retain producer-released packet");
  auto revised_palette=palette; revised_palette[2]={255,0,255};
  auto revised=ViewportTexture::create_mips(256,256,diagnostic_chain(256,256,revised_palette));
  require(static_cast<bool>(revised),"revised mip packet");
  auto revised_material=mip_material(std::move(revised).value(),TextureSampling::LinearClampMips);
  sample("mip-revision-replacement",revised_material,revised_palette,7.5);
  auto replacement=baseline; replacement.camera.eye.z=7.5; replacement.draws[0].material=revised_material;
  for (unsigned frame=0;frame<3;++frame) require(viewport.draw(replacement),"retire replaced mip packet");
  require(retained.expired(),"replaced mip packet survived completed slot retirement");
  // A full 1024-square chain is 5,592,404 bytes: six leave eight bytes below
  // the existing 32 MiB frame cap. Seven must fail before submission or upload.
  const auto maximum=diagnostic_chain(1024,1024,palette);
  require(maximum.size()==5592404,"maximum mip byte oracle");
  auto capacity=baseline; capacity.draws.clear();
  for (std::size_t i=0;i<7;++i) {
    auto maximum_packet=ViewportTexture::create_mips(1024,1024,maximum);
    require(static_cast<bool>(maximum_packet),"maximum full-chain packet");
    auto draw=quad; draw.id={84,i+1}; draw.material=mip_material(std::move(maximum_packet).value(),TextureSampling::LinearClampMips);
    capacity.draws.push_back(std::move(draw));
  }
  const auto before_rejection=viewport.stats();
  rejected(viewport.draw(capacity),ErrorCode::Exhausted,"seven maximum mip chains exceeded frame budget without rejection");
  require(viewport.stats().submitted_frames==before_rejection.submitted_frames &&
      viewport.stats().texture_uploads==before_rejection.texture_uploads,"rejected mip budget mutated submitted work");
  capacity.draws.pop_back(); require(viewport.draw(capacity),"six maximum mip chains admitted");
  evidence.maximum_chain_frame_bytes=viewport.stats().frame_texture_bytes;
  require(viewport.stats().frame_textures==6 && evidence.maximum_chain_frame_bytes==viewport_texture_frame_bytes-8,
      "maximum mip frame did not account for every level");
  for (unsigned frame=0;frame<3;++frame) require(viewport.draw(baseline),"retire mip diagnostic slots");
  require(viewport.capture(output/"mip-final-baseline.ppm"),"mip final baseline capture");
  require(same_bytes(output/"mip-baseline.ppm",output/"mip-final-baseline.ppm"),"mip sampling changed legacy untextured pixels");
  evidence.upload_bytes=viewport.stats().texture_upload_bytes-initial.texture_upload_bytes;
  std::ofstream receipt(output/"mip-evidence.json",std::ios::binary); require(receipt.good(),"mip evidence output");
  receipt<<std::setprecision(17)<<"{\"schema\":1,\"passed\":true,\"source_commit\":"<<json_quote(build::source_commit)
      <<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"scope\":\"bounded hardware isotropic mip sampling and ownership diagnostics\",\"chain_mips\":9,\"chain_bytes\":349524,"
      <<"\"stable_uploads\":"<<evidence.stable_uploads<<",\"stable_cache_hits\":"<<evidence.stable_cache_hits
      <<",\"padded_upload_bytes\":"<<evidence.upload_bytes<<",\"motion_frames\":"<<evidence.motion_frames
      <<",\"motion_maximum_error\":"<<evidence.motion_maximum_error<<",\"channel_tolerance\":3,"
      <<"\"mixed_profiles_deduplicated\":true,\"revision_retired\":true,\"seven_chain_rejected\":true,\"six_chain_bytes\":"
      <<evidence.maximum_chain_frame_bytes<<",\"legacy_baseline_exact\":true,\"probes\":[";
  for (std::size_t i=0;i<evidence.probes.size();++i) {
    const auto& probe=evidence.probes[i]; if (i) receipt<<',';
    receipt<<"{\"name\":"<<json_quote(probe.name)<<",\"capture\":"<<json_quote(probe.capture)
        <<",\"sampling\":"<<static_cast<unsigned>(probe.sampling)<<",\"sampling_profile\":"
        <<json_quote(probe.sampling==TextureSampling::LinearClampMips ? "linear_clamp_mips" : "linear_clamp_mip0")
        <<",\"mip_count\":"<<probe.mip_count<<",\"chain_bytes\":"<<probe.chain_bytes
        <<",\"pixel\":["<<probe.x<<','<<probe.y<<"],\"distance\":"<<probe.distance<<",\"vertical_fov\":"<<probe.vertical_fov
        <<",\"uv_span\":"<<probe.uv_span<<",\"world_scale\":"<<probe.world_scale
        <<",\"footprint_lod\":"<<probe.footprint_lod<<",\"resolved_lod\":"<<probe.resolved_lod
        <<",\"expected\":["<<probe.expected[0]<<','<<probe.expected[1]<<','<<probe.expected[2]
        <<"],\"observed\":["<<probe.observed[0]<<','<<probe.observed[1]<<','<<probe.observed[2]
        <<"],\"maximum_error\":"<<probe.maximum_error<<'}';
  }
  receipt<<"],\"checker_channel_tolerance\":3,\"checker_motion_frames\":5,\"checker_probes\":[";
  for (std::size_t i=0;i<evidence.checker_probes.size();++i) {
    const auto& probe=evidence.checker_probes[i]; if (i) receipt<<',';
    receipt<<"{\"name\":"<<json_quote(probe.name)<<",\"capture\":"<<json_quote(probe.capture)
        <<",\"sampling\":"<<static_cast<unsigned>(probe.sampling)<<",\"mip_count\":9,\"chain_bytes\":349524,"
        <<"\"camera_eye\":["<<probe.eye.x<<','<<probe.eye.y<<','<<probe.eye.z<<"],\"vertical_fov\":"<<right_angle
        <<",\"sample_rectangle\":[316,236,8,8],\"maximum_error\":"<<probe.maximum_error
        <<",\"expected_min\":"<<probe.expected_min<<",\"expected_max\":"<<probe.expected_max
        <<",\"observed_min\":"<<probe.observed_min<<",\"observed_max\":"<<probe.observed_max<<",\"expected\":[";
    for (std::size_t j=0;j<probe.expected.size();++j) { if (j) receipt<<','; receipt<<probe.expected[j]; }
    receipt<<"],\"observed_gray\":[";
    for (std::size_t j=0;j<probe.observed.size();++j) { if (j) receipt<<','; receipt<<probe.observed[j]; }
    receipt<<"]}";
  }
  receipt<<"],\"region_channel_tolerance\":3,\"region_pixels\":"<<evidence.region_pixels
      <<",\"region_maximum_error\":"<<evidence.region_maximum_error<<",\"regions\":[";
  for (std::size_t i=0;i<evidence.regions.size();++i) {
    const auto& record=evidence.regions[i]; if (i) receipt<<',';
    receipt<<"{\"name\":"<<json_quote(record.name)<<",\"capture\":"<<json_quote(record.capture)
        <<",\"category\":"<<json_quote(record.category)<<",\"sampling\":2,\"mip_count\":9,\"chain_bytes\":349524,"
        <<"\"rectangle\":["<<record.rectangle[0]<<','<<record.rectangle[1]<<','<<record.rectangle[2]<<','<<record.rectangle[3]
        <<"],\"camera_eye\":["<<record.eye.x<<','<<record.eye.y<<','<<record.eye.z
        <<"],\"camera_target\":["<<record.target.x<<','<<record.target.y<<','<<record.target.z<<"],\"vertical_fov\":"<<right_angle
        <<",\"world_scale\":"<<record.world_scale<<",\"footprint_lod\":"<<record.footprint_lod
        <<",\"expected\":["<<record.expected[0]<<','<<record.expected[1]<<','<<record.expected[2]
        <<"],\"pixels\":"<<record.pixels<<",\"maximum_error\":"<<record.maximum_error
        <<",\"outside_tolerance\":"<<record.outside_tolerance<<",\"colors\":[";
    bool first=true;
    for (const auto& [rgb,count]:record.colors) {
      if (!first) receipt<<','; first=false;
      receipt<<"{\"rgb\":["<<rgb[0]<<','<<rgb[1]<<','<<rgb[2]<<"],\"pixels\":"<<count<<'}';
    }
    receipt<<"]}";
  }
  receipt<<"]}\n"; receipt.close(); require(!receipt.fail(),"mip evidence write failed");
  return evidence;
}
std::string scalar_contracts(WindowsViewport& viewport,const std::filesystem::path& output) {
  // Constant per-level bytes isolate the linear R semantic from authored cooking.
  // The front-facing two-metre quad gives rho=texture_width*distance/480.
  RenderFrame baseline; baseline.camera.eye={0,0,3}; baseline.camera.target={0,0,0};
  baseline.camera.vertical_fov_radians=1.5707963267948966; baseline.grid=false;
  baseline.ambient_intensity=1; baseline.directional_intensity=0;
  DrawPrimitive quad; quad.id={86,1}; quad.color={1,1,1,1}; quad.geometry=mip_quad(1); baseline.draws={quad};
  require(viewport.draw(baseline),"scalar untextured baseline draw");
  require(viewport.capture(output/"scalar-baseline.ppm"),"scalar baseline capture");
  const auto initial=viewport.stats(); require(initial.width==640&&initial.height==480,"scalar fixture dimensions");
  const auto make_scalar=[](unsigned width,unsigned height,std::span<const std::byte> bytes,
      std::array<float,4> factor=std::array<float,4>{1,1,1,1}) {
    auto texture=ViewportTexture::create_scalar_mips(width,height,bytes);
    require(static_cast<bool>(texture),"scalar texture packet");
    auto material=ViewportMaterial::create(factor,std::move(texture).value(),TextureSampling::LinearClampMips,TextureRole::DiffuseReflectanceR);
    require(static_cast<bool>(material),"scalar material packet"); return std::move(material).value();
  };
  bool passed=true; std::size_t probes{},pixels{},bad_pixels{}; unsigned maximum_error{};
  std::ostringstream records; records<<std::setprecision(17);
  const auto observe=[&](std::string_view name,const std::string& capture,const Image& image,
      const std::array<unsigned,3>& expected,unsigned center_x,unsigned center_y,double lod) {
    require(image.width==640&&image.height==480,"scalar capture dimensions");
    // Every centre of this 16x16 square is strictly inside the projected quad,
    // even at the furthest sampled distance. No edge/background pixels skipped.
    unsigned error{};std::size_t bad{};
    for(unsigned y=center_y-8;y<center_y+8;++y)for(unsigned x=center_x-8;x<center_x+8;++x) {
      unsigned pixel_error{};const auto at=(static_cast<std::size_t>(y)*image.width+x)*3;
      for(std::size_t c=0;c<3;++c)pixel_error=std::max(pixel_error,static_cast<unsigned>(
          std::abs(static_cast<int>(image.pixels[at+c])-static_cast<int>(expected[c]))));
      error=std::max(error,pixel_error);if(pixel_error>3)++bad;
    }
    if(probes++)records<<',';
    const auto at=(static_cast<std::size_t>(center_y)*image.width+center_x)*3;
    records<<"{\"name\":"<<json_quote(std::string(name))<<",\"capture\":"<<json_quote(capture)
        <<",\"lod\":"<<lod<<",\"rectangle\":["<<center_x-8<<','<<center_y-8<<",16,16],\"pixels\":256,\"expected_rgb\":["
        <<expected[0]<<','<<expected[1]<<','<<expected[2]<<"],\"center_rgb\":["<<unsigned(image.pixels[at])<<','
        <<unsigned(image.pixels[at+1])<<','<<unsigned(image.pixels[at+2])<<"],\"color_errors\":"<<bad<<",\"maximum_channel_error\":"<<error<<'}';
    pixels+=256;bad_pixels+=bad;maximum_error=std::max(maximum_error,error);passed=passed&&bad==0;
  };
  const auto sample=[&](const std::string& name,const std::shared_ptr<const ViewportMaterial>& material,double distance,
      const std::array<unsigned,3>& expected,double lod, double uv_span=1,
      std::array<float,4> tint=std::array<float,4>{1,1,1,1},float ambient=1,float directional=0) {
    auto frame=baseline;frame.camera.eye.z=distance;frame.draws[0].material=material;frame.draws[0].geometry=mip_quad(uv_span);
    frame.draws[0].color=tint;frame.ambient_intensity=ambient;frame.directional_intensity=directional;frame.direction_to_light={0,0,1};
    require(viewport.draw(frame),"scalar diagnostic draw");
    require(viewport.stats().frame_textures==1&&viewport.stats().frame_texture_bytes==material->texture()->texels().size(),
        "scalar chain accounting");
    const auto capture=name+".ppm";require(viewport.capture(output/capture),"scalar diagnostic capture");
    observe(name,capture,read_ppm(output/capture),expected,320,240,lod);
  };
  for(const unsigned r:{0u,128u,255u}) {
    const std::array bytes{static_cast<std::byte>(r),std::byte{0},std::byte{255},std::byte{255}};
    const auto expected=expected_display(static_cast<double>(r)/255);
    sample("scalar-red-"+std::to_string(r),make_scalar(1,1,bytes),3,{expected,expected,expected},0);
  }
  const std::array middle{std::byte{128},std::byte{255},std::byte{0},std::byte{255}};
  const auto middle_display=expected_display(128.0/255);
  auto one=make_scalar(1,1,middle);
  sample("scalar-red-128-gb-swapped",one,3,{middle_display,middle_display,middle_display},0);
  const bool one_gb_exact=same_bytes(output/"scalar-red-128.ppm",output/"scalar-red-128-gb-swapped.ppm");
  passed=passed&&one_gb_exact;
  sample("scalar-one-level-minified",one,15,{middle_display,middle_display,middle_display},0,1024);
  const std::array<float,4> factor{.25f,.5f,1,1},tint{.5f,.75f,1,1};
  std::array<unsigned,3> tinted{};
  for(std::size_t c=0;c<3;++c)tinted[c]=expected_display((128.0/255)*factor[c]*tint[c]*1.5);
  sample("scalar-factor-tint-light",make_scalar(1,1,middle,factor),3,tinted,0,1,tint,.5f,1);
  // Identical gray bytes must decode differently through the two resource formats.
  const std::array gray{std::byte{128},std::byte{128},std::byte{128},std::byte{255}};
  const auto decoded=expected_display(decode_srgb(128));
  require(middle_display>decoded+20,"independent linear/sRGB discriminant");
  sample("scalar-color-control",material_packet(1,1,gray),3,{decoded,decoded,decoded},0);
  const MipPalette palette{{{0,255,17},{128,19,255},{255,0,128},{64,255,0},{192,32,224},
      {32,192,64},{224,64,192},{96,224,32},{160,96,255},{0,128,255},{255,0,0}}};
  auto bytes=diagnostic_chain(256,256,palette);
  auto scalar=make_scalar(256,256,bytes);
  const auto scalar_expected=[&](double lod) {
    const auto low=static_cast<unsigned>(std::floor(lod)),high=std::min(low+1,8u);
    const double fraction=lod-low;
    const auto value=expected_display((palette[low][0]*(1-fraction)+palette[high][0]*fraction)/255);
    return std::array{value,value,value};
  };
  struct Case {const char* name;double lod;};
  const std::array cases{Case{"magnified",0},Case{"integer-one",1},Case{"integer-two",2},Case{"fractional",1.5},
      Case{"integer-below",2-1.0/512},Case{"integer-above",2+1.0/512},Case{"level-three",3}};
  for(const auto& probe:cases) {
    const double distance=probe.lod==0?.9375:1.875*std::exp2(probe.lod);
    // Oracle follows projection and stored byte values, never shader output.
    const double lod=std::clamp(std::log2(256*distance/480),0.0,8.0);
    sample(std::string("scalar-mips-")+probe.name,scalar,distance,scalar_expected(lod),lod);
  }
  auto changed=palette;for(auto& level:changed){level[1]=255-level[1];level[2]=255-level[2];}
  auto ignored=make_scalar(256,256,diagnostic_chain(256,256,changed));
  sample("scalar-mips-fractional-gb",ignored,1.875*std::exp2(1.5),scalar_expected(1.5),1.5);
  const bool chain_gb_exact=same_bytes(output/"scalar-mips-fractional.ppm",output/"scalar-mips-fractional-gb.ppm");
  passed=passed&&chain_gb_exact;
  const MipPalette odd_palette{{{32,0,255},{128,255,0},{255,0,128}}};
  const auto odd_expected=expected_display(128.0/255);
  sample("scalar-npot-padded",make_scalar(3,5,diagnostic_chain(3,5,odd_palette)),3.75,
      {odd_expected,odd_expected,odd_expected},1,51.2);
  // Same bytes, different encoding: alternate resident packets in the same slots.
  auto color_packet=ViewportTexture::create_mips(256,256,bytes);require(static_cast<bool>(color_packet),"scalar sRGB control chain");
  auto color=mip_material(std::move(color_packet).value(),TextureSampling::LinearClampMips);
  auto mixed=baseline;mixed.camera.eye.z=3.75;mixed.draws.resize(2,quad);
  for(std::size_t i=0;i<2;++i){auto& draw=mixed.draws[i];draw.id={87,i+1};draw.world[0]=draw.world[5]=draw.world[10]=.5;
    draw.world[3]=i==0?-.8:.8;draw.material=i==0?scalar:color;}
  for(unsigned i=0;i<3;++i)require(viewport.draw(mixed),"prime mixed encoding cache");
  const auto primed=viewport.stats();
  for(unsigned i=0;i<6;++i)require(viewport.draw(mixed),"reuse mixed encoding cache");
  const auto stable=viewport.stats();
  const auto uploads=stable.texture_uploads-primed.texture_uploads,hits=stable.texture_cache_hits-primed.texture_cache_hits;
  require(uploads==0&&hits==12&&stable.frame_textures==2&&stable.frame_texture_bytes==2*bytes.size(),"mixed encoding cache/accounting");
  require(viewport.capture(output/"scalar-mixed-encodings.ppm"),"mixed encoding capture");
  const auto image=read_ppm(output/"scalar-mixed-encodings.ppm");
  observe("scalar-cached","scalar-mixed-encodings.ppm",image,scalar_expected(2),268,240,2);
  std::array<unsigned,3> color_expected{};for(std::size_t c=0;c<3;++c)color_expected[c]=expected_display(decode_srgb(palette[2][c]));
  observe("color-cached","scalar-mixed-encodings.ppm",image,color_expected,371,240,2);
  mixed.draws[1].material=scalar;
  require(viewport.draw(mixed),"shared scalar packet draw");
  require(viewport.stats().frame_textures==1&&viewport.stats().frame_texture_bytes==bytes.size(),"shared scalar packet charged twice");
  std::weak_ptr<const ViewportTexture> retained=scalar->texture();scalar.reset();mixed.draws.clear();
  require(!retained.expired(),"scalar slot lost immutable packet before retirement");
  for(unsigned i=0;i<3;++i)require(viewport.draw(baseline),"retire scalar slots");
  require(retained.expired(),"scalar packet survived completed slot retirement");
  require(viewport.capture(output/"scalar-final-baseline.ppm"),"scalar final baseline capture");
  const bool baseline_exact=same_bytes(output/"scalar-baseline.ppm",output/"scalar-final-baseline.ppm");passed=passed&&baseline_exact;
  const auto stats=viewport.stats();passed=passed&&stats.debug_errors==0&&stats.debug_warnings==0;
  std::ostringstream receipt;receipt<<"{\"schema\":1,\"scalar_gpu_contracts_passed\":"<<(passed?"true":"false")
      <<",\"source_commit\":"<<json_quote(build::source_commit)<<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"adapter\":"<<json_quote(stats.adapter)<<",\"texture_encoding\":2,\"texture_role\":2,\"sampling\":2,\"maximum_allowed_channel_error\":3"
      <<",\"probes_count\":"<<probes<<",\"pixels\":"<<pixels<<",\"color_errors\":"<<bad_pixels<<",\"maximum_channel_error\":"<<maximum_error
      <<",\"one_level_gb_exact\":"<<(one_gb_exact?"true":"false")<<",\"chain_gb_exact\":"<<(chain_gb_exact?"true":"false")
      <<",\"legacy_baseline_exact\":"<<(baseline_exact?"true":"false")<<",\"stable_uploads\":"<<uploads<<",\"stable_cache_hits\":"<<hits
      <<",\"shared_packet_accounting\":true,\"slot_retirement\":true,\"debug_errors\":"<<stats.debug_errors<<",\"debug_warnings\":"<<stats.debug_warnings
      <<",\"probes\":["<<records.str()<<"]}\n";
  const auto result=receipt.str();std::ofstream file(output/"scalar-evidence.json",std::ios::binary);file<<result;file.close();
  require(static_cast<bool>(file),"scalar evidence write");require(passed,"scalar independent color oracle failed");return result;
}
bool same_graph_stats(const ViewportGraphStats& a,const ViewportGraphStats& b) {
  return a.compiles==b.compiles && a.submissions==b.submissions && a.resources==b.resources &&
      a.passes==b.passes && a.uses==b.uses && a.transitions==b.transitions &&
      a.operation_mask==b.operation_mask && a.last_compile_cpu_ms==b.last_compile_cpu_ms;
}
struct GraphExpected {
  std::uint32_t resources{},passes{},uses{},transitions{},operation_mask{};
};
std::string graph_contracts(WindowsViewport& viewport,const std::filesystem::path& output) {
  // Independently enumerated operation bits and resource/use counts. The four
  // ordinary transitions are color CopySource -> RT -> CopySource and backbuffer
  // Present -> CopyDestination -> Present. Each cold texture adds one transition.
  constexpr std::uint32_t upload=1u<<1,opaque=1u<<2,selection=1u<<3,overlay=1u<<4,
      present=1u<<5,timing=1u<<6,capture=1u<<7;
  const GraphExpected empty_plan{3,2,4,4,opaque|present};
  const GraphExpected overlay_plan{4,3,6,4,opaque|overlay|present};
  const GraphExpected cold_plan{37,5,60,20,upload|opaque|selection|overlay|present};
  const GraphExpected warm_plan{21,4,28,4,opaque|selection|overlay|present};
  const GraphExpected replacement_plan{22,5,30,5,upload|opaque|selection|overlay|present};
  const auto initial=viewport.stats();
  require(initial.width==640 && initial.height==480,"graph fixture dimensions");
  std::ostringstream records;records<<std::setprecision(17);
  std::size_t cases{},draws{},captures{},rejections{},partial_replacements{},replacement_cold_slots{};
  std::uint64_t retained_bytes{};
  const auto storage=[&](const ViewportStats& stats) {
    require(stats.graph_plan_bytes==sizeof(graph::Plan) && stats.graph_retained_bytes>stats.graph_plan_bytes,
        "graph fixed object storage accounting");
    if (!retained_bytes) retained_bytes=stats.graph_retained_bytes;
    require(stats.graph_retained_bytes==retained_bytes,"graph fixed object storage changed with workload");
  };
  const auto record=[&](std::string_view name,const ViewportGraphStats& actual,const GraphExpected& expected,
      std::uint64_t uploaded,std::uint64_t hits) {
    if (cases++) records<<',';
    records<<"{\"name\":"<<json_quote(name)<<",\"expected\":["<<expected.resources<<','<<expected.passes<<','
        <<expected.uses<<','<<expected.transitions<<','<<expected.operation_mask<<"],\"observed\":["
        <<actual.resources<<','<<actual.passes<<','<<actual.uses<<','<<actual.transitions<<','<<actual.operation_mask
        <<"],\"compiles\":"<<actual.compiles<<",\"submissions\":"<<actual.submissions
        <<",\"compile_cpu_ms\":"<<actual.last_compile_cpu_ms<<",\"texture_uploads\":"<<uploaded
        <<",\"texture_cache_hits\":"<<hits<<'}';
    if (actual.resources!=expected.resources || actual.passes!=expected.passes || actual.uses!=expected.uses ||
        actual.transitions!=expected.transitions || actual.operation_mask!=expected.operation_mask) {
      std::fprintf(stderr,"graph case %.*s: got %u/%u/%u/%u/%u expected %u/%u/%u/%u/%u\n",
          static_cast<int>(name.size()),name.data(),actual.resources,actual.passes,actual.uses,actual.transitions,actual.operation_mask,
          expected.resources,expected.passes,expected.uses,expected.transitions,expected.operation_mask);
      require(false,"native graph description differs from independent oracle");
    }
    require(std::isfinite(actual.last_compile_cpu_ms) && actual.last_compile_cpu_ms>=0,"invalid graph compile CPU duration");
  };
  const auto checked_draw=[&](std::string_view name,const RenderFrame& frame,GraphExpected expected,
      std::uint64_t expected_uploads,std::uint64_t expected_hits,bool priming=false,bool replacing=false) {
    const auto before=viewport.stats();require(viewport.draw(frame),"graph draw");const auto after=viewport.stats();
    const auto uploaded=after.texture_uploads-before.texture_uploads,hits=after.texture_cache_hits-before.texture_cache_hits;
    // During slot priming the current swapchain slot can already be resident.
    // Texture-copy counters independently distinguish the two admitted cases.
    if (priming && uploaded==0) { expected=warm_plan;expected_uploads=0;expected_hits=16; }
    // An occluded window may expose a previously unobserved slot only after the
    // replacement. That slot needs all packets, while an old resident slot needs
    // exactly the one replacement. Neither case changes the pixel oracle.
    if (replacing && uploaded==16) { expected=cold_plan;expected_uploads=16;expected_hits=0;++replacement_cold_slots; }
    if (replacing && uploaded==1) ++partial_replacements;
    require(uploaded==expected_uploads && hits==expected_hits,"graph texture cache/upload accounting");
    require(after.draw_graph.compiles==before.draw_graph.compiles+1 &&
        after.draw_graph.submissions==before.draw_graph.submissions+1 && after.submitted_frames==before.submitted_frames+1,
        "draw graph did not admit and submit exactly once");
    require(same_graph_stats(before.capture_graph,after.capture_graph),"draw changed capture graph statistics");
    if (before.gpu_timing_available) {
      ++expected.resources;++expected.passes;++expected.uses;expected.operation_mask|=timing;
    }
    require(before.gpu_timing_available==after.gpu_timing_available,"graph timing availability changed during draw");
    record(name,after.draw_graph,expected,uploaded,hits);storage(after);++draws;return uploaded==0;
  };
  const auto checked_capture=[&](const std::string& name) {
    const auto before=viewport.stats();require(viewport.capture(output/(name+".ppm")),"graph capture");const auto after=viewport.stats();
    require(after.capture_graph.compiles==before.capture_graph.compiles+1 &&
        after.capture_graph.submissions==before.capture_graph.submissions+1 && after.submitted_frames==before.submitted_frames,
        "capture graph admission/submission accounting");
    require(same_graph_stats(before.draw_graph,after.draw_graph),"capture changed draw graph statistics");
    require(before.texture_uploads==after.texture_uploads && before.texture_cache_hits==after.texture_cache_hits,
        "capture changed texture cache statistics");
    record(name,after.capture_graph,{2,1,2,0,capture},0,0);storage(after);++captures;
    const auto image=read_ppm(output/(name+".ppm"));
    require(image.width==640 && image.height==480,"graph capture dimensions");return image;
  };
  const auto reject_frame=[&](const RenderFrame& frame,ErrorCode expected) {
    const auto before=viewport.stats();rejected(viewport.draw(frame),expected,"invalid graph fixture frame accepted");
    const auto after=viewport.stats();
    require(same_graph_stats(before.draw_graph,after.draw_graph) && same_graph_stats(before.capture_graph,after.capture_graph) &&
        before.submitted_frames==after.submitted_frames && before.last_cpu_frame==after.last_cpu_frame &&
        before.texture_uploads==after.texture_uploads && before.texture_cache_hits==after.texture_cache_hits,
        "rejected frame changed admitted/submitted graph or image identity");++rejections;
  };
  const auto changed_pixels=[](const Image& a,const Image& b) {
    require(a.width==b.width && a.height==b.height,"graph image comparison dimensions");
    std::size_t changed{};
    for (std::size_t i=0;i<a.pixels.size();i+=3)
      if (a.pixels[i]!=b.pixels[i] || a.pixels[i+1]!=b.pixels[i+1] || a.pixels[i+2]!=b.pixels[i+2]) ++changed;
    return changed;
  };
  RenderFrame empty;empty.grid=false;
  checked_draw("empty-no-grid",empty,empty_plan,0,0);
  const auto background=checked_capture("graph-empty");
  OverlayDraw triangle;triangle.color={4.f,.02f,.8f,1};triangle.vertices={{-.6f,-.6f},{.6f,-.6f},{0,.7f}};
  auto overlay_only=empty;overlay_only.overlays={triangle};
  checked_draw("overlay-no-grid-no-selection",overlay_only,overlay_plan,0,0);
  const auto overlay_image=checked_capture("graph-overlay");
  const auto overlay_changed=changed_pixels(background,overlay_image);
  require(overlay_changed>1000 && overlay_changed<background.pixels.size()/6,"graph overlay did not produce bounded visible pixels");
  const std::array expected_rgb{expected_display(triangle.color[0]),expected_display(triangle.color[1]),expected_display(triangle.color[2])};
  const auto overlay_pixels=[&](const Image& image) {
    // A 16x16 square at the triangle centre is strictly interior; the corner is
    // outside both overlay and the textured control quad below.
    for (unsigned y=232;y<248;++y) for (unsigned x=312;x<328;++x) {
      const auto at=(static_cast<std::size_t>(y)*image.width+x)*3;
      for (std::size_t c=0;c<3;++c) require(std::abs(int(image.pixels[at+c])-int(expected_rgb[c]))<=2,
          "graph unlit overlay inherited texture or lighting");
    }
    for (std::size_t c=0;c<3;++c) require(image.pixels[c]==background.pixels[c],"graph overlay changed outside pixel");
  };
  overlay_pixels(overlay_image);
  auto invalid=overlay_only;invalid.overlays[0].vertices[0][0]=std::numeric_limits<float>::quiet_NaN();
  reject_frame(invalid,ErrorCode::InvalidInput);
  require(checked_capture("graph-overlay-rejected").pixels==overlay_image.pixels,"graph rejected overlay changed last image");
  checked_draw("empty-after-rejection",empty,empty_plan,0,0);
  require(checked_capture("graph-empty-restored").pixels==background.pixels,"graph valid recovery changed empty image");

  // A preceding textured opaque draw in this same graph leaves a non-null SRV.
  // No grid/selection pass can incidentally reset it for the overlay.
  auto textured_overlay=overlay_only;textured_overlay.camera.eye={0,0,3};textured_overlay.camera.target={0,0,0};
  textured_overlay.camera.vertical_fov_radians=1.5707963267948966;
  textured_overlay.ambient_intensity=1;textured_overlay.directional_intensity=0;
  DrawPrimitive quad;quad.id={181,1};quad.geometry=mip_quad(1);quad.color={1,1,1,1};
  const std::array blue{std::byte{8},std::byte{32},std::byte{240},std::byte{255}};
  quad.material=material_packet(1,1,blue);textured_overlay.draws={quad};
  checked_draw("textured-overlay-no-grid-no-selection",textured_overlay,{6,4,10,5,upload|opaque|overlay|present},1,0);
  overlay_pixels(checked_capture("graph-textured-overlay"));

  RenderFrame maximum;maximum.camera.eye={0,0,8};maximum.camera.target={0,0,0};
  maximum.camera.vertical_fov_radians=1.5707963267948966;maximum.ambient_intensity=1;maximum.directional_intensity=0;
  auto corner=triangle;corner.vertices={{-.95f,-.95f},{-.90f,-.95f},{-.95f,-.90f}};maximum.overlays={corner};
  require(viewport_max_textures==16,"graph maximum-texture fixture requires reviewed bound");
  for (std::size_t i=0;i<16;++i) {
    DrawPrimitive draw;draw.id={182,i+1};draw.color={1,1,1,1};draw.selected=i<2;
    draw.world[0]=draw.world[5]=draw.world[10]=i==0?.4:.8;
    draw.world[3]=(static_cast<double>(i%4)-1.5)*2;draw.world[7]=(static_cast<double>(i/4)-1.5)*2;
    if (i==0) draw.geometry=mip_quad(1);
    const std::array texel{static_cast<std::byte>(32+i*8),std::byte{96},std::byte{192},std::byte{255}};
    draw.material=material_packet(1,1,texel);maximum.draws.push_back(std::move(draw));
  }
  // 16 textures + 16 uploads + cube/mesh vertex buffers + color/depth/backbuffer.
  // Uses: 32 uploads + 20 opaque + 4 grid/selection + 2 overlay + 2 present.
  checked_draw("maximum-cold",maximum,cold_plan,16,0);
  require(viewport.stats().grid_visible && viewport.stats().frame_textures==16 && viewport.stats().frame_texture_bytes==64,
      "graph maximum fixture omitted grid or unique textures");
  const auto cold_image=checked_capture("graph-textures-cold");
  // Hidden-window Present does not promise a particular slot rotation. Accept
  // either independently checked residency case until four actual warm draws;
  // this certifies observed slots, without claiming forced use of both slots.
  unsigned warm_draws{};
  for (unsigned i=0;i<12 && warm_draws<4;++i)
    warm_draws=checked_draw("maximum-cache-"+std::to_string(i),maximum,cold_plan,16,0,true)?warm_draws+1:0;
  require(warm_draws==4,"maximum graph cache did not stabilize within bounded warmup");
  const auto warm_image=checked_capture("graph-textures-warm");
  require(cold_image.pixels==warm_image.pixels,"warm graph cache changed cold image");
  const std::array replacement{std::byte{240},std::byte{220},std::byte{8},std::byte{255}};
  maximum.draws[0].material=material_packet(1,1,replacement);
  checked_draw("maximum-one-replacement",maximum,replacement_plan,1,15,false,true);
  // If that draw first exposed a cold slot, replace only this packet again.
  // Fresh immutable identity on each attempt forces a copy even if the same slot
  // repeats. At most two slots can be all-cold: require real 1-upload/15-hit proof.
  for (unsigned i=0;i<2 && partial_replacements==0;++i) {
    maximum.draws[0].material=material_packet(1,1,replacement);
    checked_draw("selective-replacement-retry-"+std::to_string(i),maximum,replacement_plan,1,15,false,true);
  }
  require(partial_replacements>0,"graph did not exercise selective one-texture replacement");
  warm_draws=0;
  for (unsigned i=0;i<12 && warm_draws<4;++i)
    warm_draws=checked_draw("replacement-cache-"+std::to_string(i),maximum,replacement_plan,1,15,true,true)?warm_draws+1:0;
  require(warm_draws==4,"replacement graph cache did not stabilize within bounded warmup");
  const auto replacement_image=checked_capture("graph-textures-replaced");
  const auto replacement_changed=changed_pixels(warm_image,replacement_image);
  require(replacement_changed>20 && replacement_changed<2000,"replacement graph failed to change one visible material");
  auto duplicate=maximum;duplicate.draws[1].id=duplicate.draws[0].id;
  reject_frame(duplicate,ErrorCode::Duplicate);
  require(checked_capture("graph-textures-rejected").pixels==replacement_image.pixels,"rejected textured frame changed last image");
  checked_draw("valid-after-texture-rejection",maximum,replacement_plan,1,15,true,true);
  require(checked_capture("graph-textures-recovered").pixels==replacement_image.pixels,"valid graph recovery changed cached image");
  const auto stats=viewport.stats();valid_profile(stats);
  require(stats.draw_graph.compiles-initial.draw_graph.compiles==draws && stats.draw_graph.submissions-initial.draw_graph.submissions==draws &&
      stats.capture_graph.compiles-initial.capture_graph.compiles==captures && stats.capture_graph.submissions-initial.capture_graph.submissions==captures,
      "graph cumulative lane accounting");
  const bool graph_clean=stats.hardware && !stats.device_lost && stats.completed_frames==stats.submitted_frames &&
      stats.debug_errors==0 && stats.debug_warnings==0;
  if (!graph_clean) report_completion("graph",stats);
  require(graph_clean,"graph hardware/debug completion");
  std::ostringstream receipt;receipt<<"{\"schema\":1,\"passed\":true,\"graph_gpu_contracts_passed\":true"
      <<",\"source_commit\":"<<json_quote(build::source_commit)<<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"adapter\":"<<json_quote(stats.adapter)<<",\"hardware\":true,\"debug_layer\":"<<(stats.debug_layer?"true":"false")
      <<",\"gpu_timing_available\":"<<(stats.gpu_timing_available?"true":"false")
      <<",\"debug_errors\":"<<stats.debug_errors<<",\"debug_warnings\":"<<stats.debug_warnings
      <<",\"retained_object_bytes\":"<<retained_bytes<<",\"plan_object_bytes\":"<<stats.graph_plan_bytes
      <<",\"storage_measurement\":\"sizeof fixed objects; excludes native allocations and compile stack; not an allocation count\""
      <<",\"draw_admissions\":"<<draws<<",\"draw_submissions\":"<<draws<<",\"capture_admissions\":"<<captures
      <<",\"capture_submissions\":"<<captures<<",\"rejected_frames\":"<<rejections
      <<",\"overlay_changed_pixels\":"<<overlay_changed<<",\"overlay_oracle_pixels_per_capture\":256,\"overlay_oracle_captures\":2"
      <<",\"overlay_expected_rgb\":["<<expected_rgb[0]<<','<<expected_rgb[1]<<','<<expected_rgb[2]<<']'
      <<",\"cold_warm_pixels_exact\":true,\"replacement_changed_pixels\":"<<replacement_changed
      <<",\"cache_scope\":\"observed swapchain slots; no forced slot rotation\""
      <<",\"partial_replacement_draws\":"<<partial_replacements<<",\"replacement_cold_slot_draws\":"<<replacement_cold_slots
      <<",\"rejections_kept_image\":true,\"valid_recovery_pixels_exact\":true"
      <<",\"count_order\":[\"resources\",\"passes\",\"uses\",\"transitions\",\"operation_mask\"],\"cases\":["<<records.str()<<']'
      <<",\"output_directory\":"<<json_quote(output.string())<<"}\n";
  const auto result=receipt.str();std::ofstream file(output/"graph-evidence.json",std::ios::binary);file<<result;file.close();
  require(static_cast<bool>(file),"graph evidence write");return result;
}
struct FilterCostCase {
  std::string name,capture;
  double lod{},uv_span{};
  std::uint64_t first_frame{},last_frame{},warm_uploads{},measured_uploads{},measured_upload_bytes{},cache_hits{};
  std::array<GpuTimingSample,64> gpu{};
  std::array<double,64> cpu_draw_ms{};
};
std::string triangle_edge_contracts(WindowsViewport& viewport,const std::filesystem::path& output) {
  // The declared front-facing camera maps these vertices to quarter-pixel screen
  // coordinates. For each edge, (dx-dy) is nonzero modulo four: no pixel center
  // lies on an edge. This makes coverage independent of a top-left tie rule.
  // Every coordinate is exactly on the D3D12 subpixel grid; no boundary samples
  // are discarded. UV is affine in the world plane, so helper lanes have the
  // same analytically known derivatives, including outside the primitive.
  constexpr std::array<std::array<double,2>,3> projected{{{196.25,131.25},{435.25,159.25},{278.25,372.25}}};
  struct Case {const char* name;double lod;unsigned offset_x,offset_y;};
  const std::array cases{Case{"integer",2,0,0},Case{"fractional-00",1.5,0,0},
      Case{"fractional-10",1.5,1,0},Case{"fractional-01",1.5,0,1},
      Case{"fractional-11",1.5,1,1},Case{"integer-below",2-1.0/512,0,0},
      Case{"integer-above",2+1.0/512,0,0}};
  const MipPalette palette{{{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},
      {0,255,255},{255,255,255},{128,128,128},{32,192,64},{255,128,0},{255,255,255}}};
  auto packet=ViewportTexture::create_mips(256,256,diagnostic_chain(256,256,palette));
  require(static_cast<bool>(packet),"triangle edge diagnostic texture");
  auto material=mip_material(std::move(packet).value(),TextureSampling::LinearClampMips);
  RenderFrame frame; frame.camera.eye={0,0,3.75};frame.camera.target={0,0,0};
  frame.camera.vertical_fov_radians=1.5707963267948966;frame.grid=false;
  frame.ambient_intensity=1;frame.directional_intensity=0;
  require(viewport.draw(frame),"triangle edge empty draw");
  require(viewport.capture(output/"edge-clear.ppm"),"triangle edge empty capture");
  const auto clear=read_ppm(output/"edge-clear.ppm");
  require(clear.width==640&&clear.height==480,"triangle edge fixture dimensions");
  std::ostringstream records;records<<std::setprecision(17);
  bool passed=true;std::size_t total_pixels{},total_edge_pixels{},total_helper_pixels{},total_coverage_errors{},total_color_errors{};
  unsigned maximum_error{};
  for(std::size_t index=0;index<cases.size();++index){
    const auto& sample=cases[index];
    const double distance=1.875*std::exp2(sample.lod);
    auto screen=projected;
    std::array<GeometryVertex,3> vertices{};
    for(std::size_t i=0;i<vertices.size();++i){
      screen[i][0]+=sample.offset_x;screen[i][1]+=sample.offset_y;
      const double x=(screen[i][0]-320)*distance/240;
      const double y=(240-screen[i][1])*distance/240;
      vertices[i]={{static_cast<float>(x),static_cast<float>(y),0},{0,0,1},
          {static_cast<float>(.5+x/2),static_cast<float>(.5-y/2)}};
    }
    const std::array<std::uint32_t,3> indices{0,1,2};
    auto geometry=ViewportGeometry::create(vertices,indices,true);
    require(static_cast<bool>(geometry),"triangle edge geometry");
    DrawPrimitive draw;draw.id={84,1};draw.color={1,1,1,1};draw.geometry=std::move(geometry).value();draw.material=material;
    frame.camera.eye.z=distance;frame.draws={draw};
    require(viewport.draw(frame),"triangle edge draw");
    const auto name=std::string("edge-")+sample.name+".ppm";
    require(viewport.capture(output/name),"triangle edge capture");
    const auto image=read_ppm(output/name);
    require(image.width==clear.width&&image.height==clear.height,"triangle edge image dimensions changed");
    // u=.5+x/2 and v=.5-y/2: with 90-degree vertical FOV,
    // |du/d(pixel x)|=|dv/d(pixel y)|=distance/height. This double
    // reference uses camera/geometry inputs, never shader-observed derivatives.
    const double lod=std::clamp(std::log2(256*distance/clear.height),0.0,8.0);
    const auto low=static_cast<unsigned>(std::floor(lod)),high=std::min(low+1,8u);
    const double fraction=lod-low;
    std::array<unsigned,3> expected{};
    for(std::size_t c=0;c<3;++c)expected[c]=expected_display(
        decode_srgb(palette[low][c])*(1-fraction)+decode_srgb(palette[high][c])*fraction);
    const auto edge_values=[&](double x,double y){
      std::array<double,3> edges{};
      for(std::size_t i=0;i<3;++i){const auto& a=screen[i];const auto& b=screen[(i+1)%3];
        edges[i]=(b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]);}
      return edges;
    };
    const auto covered=[&](unsigned x,unsigned y){const auto e=edge_values(x+.5,y+.5);return e[0]>0&&e[1]>0&&e[2]>0;};
    std::array<double,3> edge_lengths{};
    for(std::size_t i=0;i<3;++i)edge_lengths[i]=std::hypot(screen[(i+1)%3][0]-screen[i][0],screen[(i+1)%3][1]-screen[i][1]);
    std::size_t pixels{},edge_pixels{},helper_pixels{},coverage_errors{},color_errors{};
    std::array<std::size_t,3> per_edge{};unsigned error{};
    std::ostringstream mismatches;unsigned recorded_mismatches{};
    for(unsigned y=0;y<image.height;++y)for(unsigned x=0;x<image.width;++x){
      const auto at=(static_cast<std::size_t>(y)*image.width+x)*3;
      const bool inside=covered(x,y);
      bool is_clear=true;for(std::size_t c=0;c<3;++c)is_clear=is_clear&&image.pixels[at+c]==clear.pixels[at+c];
      if(inside==is_clear)++coverage_errors;
      if(!inside)continue;
      ++pixels;const auto values=edge_values(x+.5,y+.5);bool near_edge=false;
      for(std::size_t e=0;e<3;++e)if(values[e]/edge_lengths[e]<=1.5){near_edge=true;++per_edge[e];}
      if(near_edge)++edge_pixels;
      bool uncovered_lane=false;
      for(unsigned yy=y&~1u;yy<(y&~1u)+2;++yy)for(unsigned xx=x&~1u;xx<(x&~1u)+2;++xx)
        uncovered_lane=uncovered_lane||!covered(xx,yy);
      if(uncovered_lane)++helper_pixels;
      unsigned pixel_error{};
      for(std::size_t c=0;c<3;++c)pixel_error=std::max(pixel_error,static_cast<unsigned>(
          std::abs(static_cast<int>(image.pixels[at+c])-static_cast<int>(expected[c]))));
      error=std::max(error,pixel_error);
      if(pixel_error>3){++color_errors;if(recorded_mismatches++<16){
        if(recorded_mismatches>1)mismatches<<',';
        mismatches<<"{\"x\":"<<x<<",\"y\":"<<y<<",\"near_edge\":"<<(near_edge?"true":"false")
            <<",\"helper_quad\":"<<(uncovered_lane?"true":"false")<<",\"rgb\":["
            <<unsigned(image.pixels[at])<<','<<unsigned(image.pixels[at+1])<<','<<unsigned(image.pixels[at+2])<<"]}";
      }}
    }
    const bool bounded=pixels>20000&&edge_pixels>500&&helper_pixels>100&&
        std::all_of(per_edge.begin(),per_edge.end(),[](std::size_t count){return count>100;});
    const bool sample_passed=bounded&&coverage_errors==0&&color_errors==0;
    passed=passed&&sample_passed;total_pixels+=pixels;total_edge_pixels+=edge_pixels;total_helper_pixels+=helper_pixels;
    total_coverage_errors+=coverage_errors;total_color_errors+=color_errors;maximum_error=std::max(maximum_error,error);
    if(index)records<<',';
    records<<"{\"name\":"<<json_quote(sample.name)<<",\"capture\":"<<json_quote(name)
        <<",\"passed\":"<<(sample_passed?"true":"false")<<",\"lod\":"<<lod<<",\"distance\":"<<distance
        <<",\"pixel_offset\":["<<sample.offset_x<<','<<sample.offset_y<<"],\"expected_rgb\":["<<expected[0]<<','<<expected[1]<<','<<expected[2]
        <<"],\"covered_pixels\":"<<pixels<<",\"inner_edge_pixels\":"<<edge_pixels<<",\"helper_quad_pixels\":"<<helper_pixels
        <<",\"per_edge_pixels\":["<<per_edge[0]<<','<<per_edge[1]<<','<<per_edge[2]<<"],\"coverage_errors\":"<<coverage_errors
        <<",\"color_errors\":"<<color_errors<<",\"maximum_channel_error\":"<<error<<",\"first_mismatches\":["<<mismatches.str()<<"]}";
  }
  const auto stats=viewport.stats();valid_profile(stats);
  passed=passed&&stats.hardware&&!stats.device_lost&&stats.completed_frames==stats.submitted_frames&&stats.debug_errors==0&&stats.debug_warnings==0;
  std::ostringstream receipt;receipt<<"{\"schema\":1,\"edge_gpu_contracts_passed\":"<<(passed?"true":"false")
      <<",\"source_commit\":"<<json_quote(build::source_commit)<<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"adapter\":"<<json_quote(stats.adapter)<<",\"width\":640,\"height\":480,\"hardware\":"<<(stats.hardware?"true":"false")
      <<",\"debug_layer\":"<<(stats.debug_layer?"true":"false")<<",\"debug_errors\":"<<stats.debug_errors<<",\"debug_warnings\":"<<stats.debug_warnings
      <<",\"maximum_allowed_channel_error\":3,\"cases\":7,\"covered_pixels\":"<<total_pixels<<",\"inner_edge_pixels\":"<<total_edge_pixels
      <<",\"helper_quad_pixels\":"<<total_helper_pixels<<",\"coverage_errors\":"<<total_coverage_errors<<",\"color_errors\":"<<total_color_errors
      <<",\"maximum_channel_error\":"<<maximum_error<<",\"probes\":["<<records.str()<<"],\"output_directory\":"<<json_quote(output.string())<<"}\n";
  const auto result=receipt.str();std::ofstream file(output/"edge-evidence.json",std::ios::binary);file<<result;file.close();
  require(static_cast<bool>(file),"triangle edge evidence write");
  require(passed,"triangle edge coverage or independent color oracle failed");
  return result;
}

std::string filter_cost_contracts(WindowsViewport& viewport,const std::filesystem::path& shaders,
                                 const std::filesystem::path& output) {
  constexpr unsigned warm_frames=16,measured_frames=64;
  constexpr double right_angle=1.5707963267948966;
  const auto initial=viewport.stats();
  require(initial.hardware && initial.gpu_timing_available && !initial.device_lost &&
      initial.width==640 && initial.height==480,"filter cost requires 640x480 hardware GPU timestamps");
  const MipPalette palette{{{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},
      {0,255,255},{255,255,255},{128,128,128},{32,192,64},{255,128,0},{255,255,255}}};
  auto texture=ViewportTexture::create_mips(256,256,diagnostic_chain(256,256,palette));
  require(static_cast<bool>(texture),"filter cost complete-chain texture");
  const auto material=mip_material(std::move(texture).value(),TextureSampling::LinearClampMips);
  std::array<FilterCostCase,3> cases{};
  const std::array names{"magnified-minus-one","fractional-one-half","integer-two"};
  const std::array levels{-1.0,1.5,2.0};
  for (std::size_t index=0;index<cases.size();++index) {
    auto& evidence=cases[index]; evidence.name=names[index]; evidence.lod=levels[index];
    // A two-unit square at distance one and vertical FOV90 covers the viewport
    // after x expands to aspect 4/3. Isotropic LOD follows the larger vertical
    // derivative: 256*span/480. Geometry, light and material stay fixed per case.
    evidence.uv_span=480.0/256*std::exp2(evidence.lod);
    RenderFrame frame; frame.camera.eye={0,0,1}; frame.camera.target={0,0,0};
    frame.camera.vertical_fov_radians=right_angle; frame.grid=false;
    frame.ambient_intensity=1; frame.directional_intensity=0;
    DrawPrimitive quad; quad.id={85,index+1}; quad.color={1,1,1,1}; quad.world[0]=4.0/3;
    quad.geometry=mip_quad(evidence.uv_span); quad.material=material; frame.draws={quad};
    const auto before_warm=viewport.stats();
    for (unsigned i=0;i<warm_frames;++i) require(viewport.draw(frame),"filter cost warm draw");
    const auto warm=viewport.stats(); valid_profile(warm);
    require(warm.hardware && warm.gpu_timing_available && !warm.device_lost &&
        warm.submitted_frames==before_warm.submitted_frames+warm_frames,"filter cost warm frames unavailable");
    evidence.warm_uploads=warm.texture_uploads-before_warm.texture_uploads;
    evidence.first_frame=warm.submitted_frames+1; evidence.last_frame=warm.submitted_frames+measured_frames;
    // The viewport retains only the newest four completed samples. Poll every
    // draw and after the final capture drain, retaining each frame identity once.
    const auto collect=[&](const ViewportStats& stats) {
      valid_profile(stats);
      require(stats.hardware && stats.gpu_timing_available && !stats.device_lost &&
          stats.width==640 && stats.height==480,"filter cost lost hardware timestamps or dimensions");
      for (std::size_t i=0;i<stats.gpu_sample_count;++i) {
        const auto& sample=stats.gpu_samples[i];
        if (sample.frame<evidence.first_frame || sample.frame>evidence.last_frame) continue;
        auto& retained=evidence.gpu[static_cast<std::size_t>(sample.frame-evidence.first_frame)];
        if (retained.frame)
          require(retained.frame==sample.frame && retained.milliseconds==sample.milliseconds,"filter cost duplicate timestamp changed");
        else retained=sample;
      }
    };
    for (unsigned i=0;i<measured_frames;++i) {
      require(viewport.draw(frame),"filter cost measured draw");
      const auto stats=viewport.stats();
      require(stats.submitted_frames==evidence.first_frame+i && stats.last_cpu_frame==stats.submitted_frames &&
          stats.frame_textures==1 && stats.frame_texture_bytes==349524 && stats.last_draw_count==1,
          "filter cost measured frame identity or texture state changed");
      evidence.cpu_draw_ms[i]=stats.last_draw_cpu_ms; collect(stats);
    }
    evidence.capture=evidence.name+".ppm";
    require(viewport.capture(output/evidence.capture),"filter cost final capture drain");
    const auto drained=viewport.stats(); collect(drained);
    require(drained.completed_frames==evidence.last_frame && drained.last_timed_frame==evidence.last_frame,
        "filter cost final frame did not retire with a timestamp");
    for (std::size_t i=0;i<evidence.gpu.size();++i)
      require(evidence.gpu[i].frame==evidence.first_frame+i && std::isfinite(evidence.gpu[i].milliseconds) &&
          evidence.gpu[i].milliseconds>=0,"filter cost requires exactly 64 unique completed GPU samples");
    evidence.measured_uploads=drained.texture_uploads-warm.texture_uploads;
    evidence.measured_upload_bytes=drained.texture_upload_bytes-warm.texture_upload_bytes;
    evidence.cache_hits=drained.texture_cache_hits-warm.texture_cache_hits;
    require(evidence.measured_uploads==0 && evidence.measured_upload_bytes==0 && evidence.cache_hits==measured_frames,
        "filter cost measured window changed warmed texture residency");
    require(drained.debug_errors==0 && drained.debug_warnings==0,"filter cost D3D12 diagnostic messages");
  }
  const auto stats=viewport.stats();
  std::ostringstream json; json<<std::setprecision(17);
  json<<"{\"schema\":1,\"filter_cost_diagnostic\":true,\"passed\":true,\"source_commit\":"<<json_quote(build::source_commit)
      <<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"scope\":\"Matched 640x480 static viewport GPU timestamps: clears, raster draws and color copy; excludes Present and fence wait. No speed threshold, visual acceptance, sustained Present or 4K claim.\""
      <<",\"shader_directory\":"<<json_quote(std::filesystem::absolute(shaders).string())
      <<",\"output_directory\":"<<json_quote(output.string())<<",\"evidence\":\"filter-cost-evidence.json\""
      <<",\"adapter\":"<<json_quote(stats.adapter)<<",\"vendor_id\":"<<stats.vendor_id<<",\"device_id\":"<<stats.device_id
      <<",\"hardware\":true,\"gpu_timing_available\":true,\"debug_layer\":"<<(stats.debug_layer ? "true" : "false")
      <<",\"debug_errors\":"<<stats.debug_errors<<",\"debug_warnings\":"<<stats.debug_warnings
      <<",\"submitted_frames\":"<<stats.submitted_frames<<",\"completed_frames\":"<<stats.completed_frames
      <<",\"cpu_scope\":\"Draw API duration, including CPU validation, recording, Present and fence waits; distinct from GPU timestamps.\""
      <<",\"width\":640,\"height\":480,\"warm_frames_per_case\":16,\"measured_frames_per_case\":64,\"cases\":[";
  for (std::size_t i=0;i<cases.size();++i) {
    const auto& evidence=cases[i]; if (i) json<<',';
    json<<"{\"name\":"<<json_quote(evidence.name)<<",\"capture\":"<<json_quote(evidence.capture)
        <<",\"expected_footprint_lod\":"<<evidence.lod<<",\"uv_span\":"<<evidence.uv_span
        <<",\"camera_eye\":[0,0,1],\"camera_target\":[0,0,0],\"vertical_fov\":"<<right_angle
        <<",\"world\":["<<4.0/3<<",0,0,0,0,1,0,0,0,0,1,0],\"draws\":1,\"triangles\":2,\"grid\":false,\"overlays\":0,"
        <<"\"ambient\":1,\"directional\":0,\"sampling\":2,\"sampling_profile\":\"linear-clamp-mips\","
        <<"\"texture_width\":256,\"texture_height\":256,\"mip_count\":9,\"chain_bytes\":349524,"
        <<"\"first_frame\":"<<evidence.first_frame<<",\"last_frame\":"<<evidence.last_frame<<",\"sample_count\":64"
        <<",\"warm_uploads\":"<<evidence.warm_uploads<<",\"measured_uploads\":"<<evidence.measured_uploads
        <<",\"measured_upload_bytes\":"<<evidence.measured_upload_bytes<<",\"measured_cache_hits\":"<<evidence.cache_hits
        <<",\"gpu_samples\":[";
    for (std::size_t j=0;j<evidence.gpu.size();++j) {
      if (j) json<<',';
      json<<"{\"frame\":"<<evidence.gpu[j].frame<<",\"milliseconds\":"<<evidence.gpu[j].milliseconds<<'}';
    }
    json<<"],\"cpu_draw_samples\":[";
    for (std::size_t j=0;j<evidence.cpu_draw_ms.size();++j) {
      if (j) json<<',';
      json<<"{\"frame\":"<<evidence.first_frame+j<<",\"milliseconds\":"<<evidence.cpu_draw_ms[j]<<'}';
    }
    json<<"]}";
  }
  json<<"]}\n";
  require(json.good(),"filter cost JSON serialization");
  const auto result=json.str();
  std::ofstream receipt(output/"filter-cost-evidence.json",std::ios::binary);
  require(receipt.good(),"filter cost evidence output"); receipt<<result; receipt.close();
  require(!receipt.fail(),"filter cost evidence write failed");
  return result;
}
// P03.04 PhysicalDiffuseV1 native oracles (groups 2, 3 and 6 of the contract): raw FP16
// readback through an independent half decoder, one CPU double output transform on both
// sides of the sRGB breakpoint and at two exposures, stored-HDR exposure invariance,
// annotation stability and absence, independent graph counts, exposure-only texture
// accounting, the 4096^2 output set within its ceiling, and legacy byte identity.
float decode_half(std::uint16_t half) {
  const int sign=(half>>15)?-1:1;const int exponent=(half>>10)&31;const int mantissa=half&1023;
  if (exponent==31) return mantissa ? std::numeric_limits<float>::quiet_NaN() : sign*std::numeric_limits<float>::infinity();
  return static_cast<float>(sign*(exponent ? std::ldexp(1024.0+mantissa,exponent-25) : std::ldexp(static_cast<double>(mantissa),-24)));
}
// Round-to-nearest-even onto the binary16 grid: the quantisation the stored target applies.
double quantize_half(double value) {
  if (value==0) return 0;
  const int exponent=std::max(static_cast<int>(std::floor(std::log2(value))),-14);
  const double step=std::ldexp(1.0,exponent-10);return std::nearbyint(value/step)*step;
}
struct FloatImage { unsigned width{},height{};std::vector<float> rgb; }; // Top-to-bottom rows.
FloatImage read_pfm(const std::filesystem::path& path) {
  std::ifstream input(path,std::ios::binary);FloatImage image;std::string magic;double scale{};
  input>>magic>>image.width>>image.height>>scale;
  require(input.good()&&magic=="PF"&&scale==-1.0&&image.width>0&&image.height>0,"invalid PFM header");
  require(input.get()=='\n',"invalid PFM separator");
  image.rgb.resize(static_cast<std::size_t>(image.width)*image.height*3);
  std::vector<float> row(static_cast<std::size_t>(image.width)*3);
  for (unsigned y=0;y<image.height;++y) { // PFM stores the bottom row first.
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(row.data()),static_cast<std::streamsize>(row.size()*sizeof(float)))),"short PFM row");
    std::copy(row.begin(),row.end(),image.rgb.begin()+static_cast<std::size_t>(image.height-1-y)*image.width*3);
  }
  require(input.peek()==std::char_traits<char>::eof(),"extra PFM bytes");return image;
}
std::vector<std::uint16_t> read_half_rows(const std::filesystem::path& path,unsigned width,unsigned height) {
  std::ifstream input(path,std::ios::binary);std::vector<std::uint16_t> values(static_cast<std::size_t>(width)*height*4);
  require(static_cast<bool>(input.read(reinterpret_cast<char*>(values.data()),static_cast<std::streamsize>(values.size()*2))),"short raw RGBA16F rows");
  require(input.peek()==std::char_traits<char>::eof(),"extra raw RGBA16F bytes");return values;
}
std::string hex(const Digest256& digest) {
  static constexpr char digits[]="0123456789abcdef";std::string out;out.reserve(64);
  for (const auto byte:digest) { const auto value=std::to_integer<unsigned>(byte);out+=digits[value>>4];out+=digits[value&15]; }
  return out;
}
std::string physical_contracts(WindowsViewport& viewport,const std::filesystem::path& shaders,const std::filesystem::path& output) {
  constexpr std::uint32_t bit_opaque=1u<<2,bit_selection=1u<<3,bit_present=1u<<5,bit_timing=1u<<6,bit_output=1u<<8;
  std::vector<std::string> checks;std::ostringstream probes,cases,raw_records;probes<<std::setprecision(17);cases<<std::setprecision(17);
  const auto check=[&](bool value,const char* name){require(value,name);checks.emplace_back(name);};
  std::vector<char> light_manifest;
  { std::ifstream in(shaders/"viewport-light-output.rmanifest",std::ios::binary);require(static_cast<bool>(in),"light/output manifest read");
    light_manifest.assign(std::istreambuf_iterator<char>{in},{}); }
  const auto light_manifest_sha256=hex(sha256(std::as_bytes(std::span(light_manifest))));
  // Legacy control before any physical frame: installing the output set must not change it.
  const auto legacy=fixture();
  require(viewport.draw(legacy),"physical legacy control draw");require(viewport.capture(output/"physical-legacy-before.ppm"),"physical legacy control capture");
  const auto initial=viewport.stats();
  check(!initial.physical_output_installed&&initial.lighting_profile==LightingProfile::LegacyViewportV1&&initial.output_allocation_bytes==0,"legacy_frames_install_no_physical_output");
  // Independent camera fixture shared with the material oracle: a 2 m quad at z=0 seen from
  // z=3 with a 90-degree field fills the central third; the corners show the background.
  const std::array<GeometryVertex,4> vertices{{{{-1,1,0},{0,0,1},{0,0}},{{1,1,0},{0,0,1},{1,0}},{{1,-1,0},{0,0,1},{1,1}},{{-1,-1,0},{0,0,1},{0,1}}}};
  const std::array<std::uint32_t,6> indices{0,1,2,0,2,3};
  auto geometry=ViewportGeometry::create(vertices,indices,true);require(static_cast<bool>(geometry),"physical quad geometry");
  RenderFrame frame;frame.camera.eye={0,0,3};frame.camera.target={0,0,0};frame.camera.vertical_fov_radians=1.5707963267948966;frame.grid=false;
  frame.lighting_profile=LightingProfile::PhysicalDiffuseV1;
  frame.physical.direction_to_light={0,0,1};frame.physical.environment_cd_m2=0;frame.physical.background_cd_m2={.25,.5,1};
  frame.physical.exposure_stops=0;frame.physical.reference_luminance_cd_m2=1; // exposure scale 1: x equals the stored radiance
  DrawPrimitive quad;quad.id={301,1};quad.color={1,1,1,1};quad.geometry=geometry.value();frame.draws={quad};
  const auto centre=[](unsigned width,unsigned height){return static_cast<std::size_t>(height/2)*width+width/2;};
  // Rejections never reach the GPU and install nothing.
  std::size_t rejections{};
  const auto reject=[&](const RenderFrame& bad,ErrorCode code,const char* name){
    const auto before=viewport.stats();rejected(viewport.draw(bad),code,name);const auto after=viewport.stats();
    require(after.submitted_frames==before.submitted_frames&&after.physical_output_installed==before.physical_output_installed&&
        same_graph_stats(before.draw_graph,after.draw_graph),"rejected physical frame changed submission or output state");
    checks.emplace_back(name);++rejections;
  };
  auto mixed=legacy;mixed.physical.illuminance_lux=100;reject(mixed,ErrorCode::InvalidInput,"legacy_frame_with_physical_fields_rejected");
  auto excess=frame;excess.draws[0].color={1.5f,1,1,1};reject(excess,ErrorCode::InvalidInput,"physical_reflectance_above_one_rejected");
  auto scrgb=frame;scrgb.physical.target=OutputTarget::DisplayScrgb;reject(scrgb,ErrorCode::Unsupported,"hdr_presentation_request_rejected_as_unsupported");
  auto degenerate=frame;degenerate.physical.direction_to_light={0,0,0};reject(degenerate,ErrorCode::InvalidInput,"degenerate_physical_direction_rejected");
  auto over=frame;over.physical.illuminance_lux=physical_illuminance_max_lux*2;reject(over,ErrorCode::InvalidInput,"illuminance_above_declared_bound_rejected");
  check(!viewport.stats().physical_output_installed,"rejections_installed_nothing");
  // Oracle group 2: the raw FP16 readback preserves exactly representable scene-linear values.
  const std::array<double,6> values{0,.5,1,4,100,30000};std::size_t half_mismatches{};
  for (std::size_t i=0;i<values.size();++i) {
    frame.physical.illuminance_lux=values[i]*std::numbers::pi; // normal incidence, rho 1, environment 0: Lo equals the value
    require(viewport.draw(frame),"physical quad draw");const auto after=viewport.stats();
    check(after.lighting_profile==LightingProfile::PhysicalDiffuseV1&&after.physical_output_installed&&after.output_target==OutputTarget::DisplaySdr,"physical_frame_installed_declared_output");
    check(after.output_allocation_bytes>0&&after.output_allocation_bytes<=after.output_budget_bytes&&after.output_budget_bytes==output_budget_physical_sdr_bytes&&
        after.hdr_target_bytes>=static_cast<std::uint64_t>(after.width)*after.height*8,"output_allocation_preflight_within_declared_ceiling");
    check((after.draw_graph.operation_mask&bit_output)&&(after.draw_graph.operation_mask&bit_opaque)&&(after.draw_graph.operation_mask&bit_present)&&
        !(after.draw_graph.operation_mask&bit_selection),"physical_graph_runs_one_output_transform");
    auto token=viewport.image_token();require(static_cast<bool>(token),"physical image token");
    const auto name="physical-raw-"+std::to_string(i);
    auto capture=viewport.capture_scene_linear(output/(name+".pfm"),output/(name+".rgba16f"),token.value());require(static_cast<bool>(capture),"scene-linear capture");
    const auto& meta=capture.value();
    check(meta.width==after.width&&meta.height==after.height&&meta.submitted_frame==after.submitted_frames&&meta.device_epoch==after.device_epoch&&
        !meta.annotations_included&&meta.dxgi_format==10&&meta.effective_target==OutputTarget::DisplaySdr&&meta.exposure_scale==1.f,"scene_linear_capture_metadata_consistent");
    const auto pfm=read_pfm(output/(name+".pfm"));const auto raw=read_half_rows(output/(name+".rgba16f"),meta.width,meta.height);
    require(pfm.width==meta.width&&pfm.height==meta.height,"PFM dimensions");
    // The PFM is accepted only if every float equals the independently decoded raw half bit for bit.
    for (std::size_t px=0;px<static_cast<std::size_t>(meta.width)*meta.height;++px) for (unsigned c=0;c<3;++c)
      if (std::bit_cast<std::uint32_t>(decode_half(raw[px*4+c]))!=std::bit_cast<std::uint32_t>(pfm.rgb[px*3+c])) ++half_mismatches;
    check(half_mismatches==0,"pfm_matches_independent_half_decoder");
    const auto c=centre(meta.width,meta.height);
    for (unsigned ch=0;ch<3;++ch) check(decode_half(raw[c*4+ch])==static_cast<float>(values[i]),"raw_fp16_preserves_exact_scene_linear_value");
    check(decode_half(raw[0])==.25f&&decode_half(raw[1])==.5f&&decode_half(raw[2])==1.f&&decode_half(raw[3])==1.f,"raw_fp16_background_radiance_exact");
    // Digest-bearing manifest, published after the image it describes.
    std::ofstream manifest(output/(name+".json"),std::ios::binary);
    manifest<<"{\"schema\":1,\"capture_domain\":\"scene-linear\",\"profile\":\"PhysicalDiffuseV1\",\"image\":"<<json_quote(name+".pfm")<<",\"image_sha256\":"<<json_quote(hex(meta.image_sha256))
        <<",\"raw\":"<<json_quote(name+".rgba16f")<<",\"raw_sha256\":"<<json_quote(hex(meta.raw_sha256))<<",\"width\":"<<meta.width<<",\"height\":"<<meta.height
        <<",\"source_commit\":"<<json_quote(build::source_commit)<<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
        <<",\"light_output_manifest_sha256\":"<<json_quote(light_manifest_sha256)<<",\"working_space\":"<<json_quote(std::string(physical_working_space_id))
        <<",\"units\":\"cd/m^2\",\"tone_transform\":"<<json_quote(std::string(physical_tone_transform_id))<<",\"transfer\":\"IEC61966-2-1 sRGB, display image only\""
        <<",\"exposure_scale\":"<<meta.exposure_scale<<",\"reference_luminance\":"<<meta.reference_luminance<<",\"requested_target\":"<<unsigned(meta.requested_target)
        <<",\"effective_target\":"<<unsigned(meta.effective_target)<<",\"dxgi_format\":"<<meta.dxgi_format<<",\"annotations_included\":false"
        <<",\"scene_generation\":"<<meta.scene_generation<<",\"origin_revision\":"<<meta.origin_revision<<",\"simulation_tick\":"<<meta.simulation_tick
        <<",\"submitted_frame\":"<<meta.submitted_frame<<",\"device_epoch\":"<<meta.device_epoch<<"}\n";
    manifest.close();require(!manifest.fail(),"capture manifest write");
    if (i) raw_records<<',';
    raw_records<<"{\"value\":"<<values[i]<<",\"pfm\":"<<json_quote(name+".pfm")<<",\"raw\":"<<json_quote(name+".rgba16f")<<",\"manifest\":"<<json_quote(name+".json")
        <<",\"raw_sha256\":"<<json_quote(hex(meta.raw_sha256))<<",\"image_sha256\":"<<json_quote(hex(meta.image_sha256))<<'}';
  }
  // Oracle group 2b: another background radiance re-creates the FP16 target around its clear
  // value exactly once per change; the readback carries the new background and the declared
  // allocation is unchanged. An unchanged background keeps the installed target.
  {
    const auto before=viewport.stats();auto shifted=frame;shifted.physical.background_cd_m2={1,2,3};
    require(viewport.draw(shifted),"background change draw");const auto after=viewport.stats();
    check(after.hdr_target_installs==before.hdr_target_installs+1&&after.physical_output_installed&&after.output_allocation_bytes==before.output_allocation_bytes&&
        after.hdr_target_bytes==before.hdr_target_bytes,"background_change_reinstalls_target_once");
    auto token=viewport.image_token();require(static_cast<bool>(token),"background change token");
    auto capture=viewport.capture_scene_linear(output/"physical-background.pfm",output/"physical-background.rgba16f",token.value());require(static_cast<bool>(capture),"background change capture");
    const auto raw=read_half_rows(output/"physical-background.rgba16f",capture.value().width,capture.value().height);
    check(decode_half(raw[0])==1.f&&decode_half(raw[1])==2.f&&decode_half(raw[2])==3.f&&decode_half(raw[3])==1.f,"background_change_raw_exact");
    require(viewport.draw(shifted),"unchanged background draw");
    check(viewport.stats().hdr_target_installs==after.hdr_target_installs,"unchanged_background_keeps_target");
    require(viewport.draw(frame),"background restore draw");
    check(viewport.stats().hdr_target_installs==after.hdr_target_installs+1,"background_restore_reinstalls_target_once");
  }
  // Oracle group 3: exactly one output transform. CPU double oracle on both sides of the sRGB
  // breakpoint and along a ramp at two exposures; deliberately double-encoded controls differ.
  const std::array<double,8> ramp{.001,.0031,.0032,.01,.18,1,4,100};unsigned maximum_error{};std::size_t probe_count{},double_encoded_distinct{};
  for (const double value:ramp) for (const double stops:{0.0,1.0}) {
    frame.physical.illuminance_lux=value*std::numbers::pi;frame.physical.exposure_stops=stops;
    require(viewport.draw(frame),"physical ramp draw");
    const auto name="physical-display-"+std::to_string(probe_count);
    require(viewport.capture(output/(name+".ppm")),"physical display capture");const auto image=read_ppm(output/(name+".ppm"));
    const auto at=centre(image.width,image.height)*3;
    const double stored=quantize_half(value),scale=std::exp2(stops);
    const auto expected=unorm8(display_sdr_encoded(stored,scale));
    const auto twice=unorm8(srgb_encode(srgb_encode(reinhard_rgb_v1(stored*scale))));
    unsigned error{};for (unsigned c=0;c<3;++c) error=std::max(error,static_cast<unsigned>(std::abs(int(image.pixels[at+c])-int(expected))));
    maximum_error=std::max(maximum_error,error);
    if (std::abs(int(twice)-int(expected))>6) { for (unsigned c=0;c<3;++c) require(std::abs(int(image.pixels[at+c])-int(twice))>2,"display pixel matches a double-encoded control");++double_encoded_distinct; }
    if (probe_count) probes<<',';
    probes<<"{\"capture\":"<<json_quote(name+".ppm")<<",\"scene_linear\":"<<value<<",\"stored_half\":"<<stored<<",\"exposure_stops\":"<<stops<<",\"expected\":"<<unsigned(expected)
        <<",\"observed\":["<<unsigned(image.pixels[at])<<','<<unsigned(image.pixels[at+1])<<','<<unsigned(image.pixels[at+2])<<"],\"error\":"<<error<<",\"double_encoded_control\":"<<unsigned(twice)<<'}';
    ++probe_count;
  }
  check(maximum_error<=2,"display_pixels_match_cpu_double_output_transform");
  check(double_encoded_distinct>=4,"display_pixels_differ_from_double_encoded_controls");
  // Exposure changes leave the stored scene-linear bytes untouched.
  frame.physical.illuminance_lux=std::numbers::pi;std::array<std::string,2> exposure_digests;
  for (unsigned pass=0;pass<2;++pass) {
    frame.physical.exposure_stops=pass ? 1.0 : 0.0;require(viewport.draw(frame),"exposure invariance draw");
    auto token=viewport.image_token();require(static_cast<bool>(token),"exposure invariance token");
    const auto name="physical-exposure-"+std::to_string(pass);
    auto capture=viewport.capture_scene_linear(output/(name+".pfm"),output/(name+".rgba16f"),token.value());require(static_cast<bool>(capture),"exposure invariance capture");
    exposure_digests[pass]=hex(capture.value().raw_sha256);
    check(capture.value().exposure_scale==(pass ? 2.f : 1.f),"capture_metadata_records_exposure_scale");
  }
  check(exposure_digests[0]==exposure_digests[1],"stored_hdr_bytes_invariant_under_exposure");
  // Annotations keep their legacy SDR colour under exposure and never enter the scene-linear target.
  auto gridded=frame;gridded.grid=true;gridded.draws.clear();gridded.camera.eye={6,5,8};gridded.camera.target={0,.5,0};gridded.camera.vertical_fov_radians=0.7853981633974483;
  const std::array<unsigned,3> grid_rgb{expected_display(.11),expected_display(.13),expected_display(.17)};
  std::array<std::vector<std::size_t>,2> grid_pixels;
  for (unsigned pass=0;pass<2;++pass) {
    gridded.physical.exposure_stops=pass ? 2.0 : 0.0;require(viewport.draw(gridded),"physical grid draw");
    const auto name="physical-grid-"+std::to_string(pass);require(viewport.capture(output/(name+".ppm")),"physical grid capture");
    const auto image=read_ppm(output/(name+".ppm"));
    for (std::size_t px=0;px<image.pixels.size()/3;++px) {
      bool grid=true;for (unsigned c=0;c<3;++c) grid=grid&&std::abs(int(image.pixels[px*3+c])-int(grid_rgb[c]))<=1;
      if (grid) grid_pixels[pass].push_back(px);
    }
    if (pass==0) {
      auto token=viewport.image_token();require(static_cast<bool>(token),"grid token");
      auto capture=viewport.capture_scene_linear(output/"physical-grid.pfm",output/"physical-grid.rgba16f",token.value());require(static_cast<bool>(capture),"grid scene-linear capture");
      const auto raw=read_half_rows(output/"physical-grid.rgba16f",capture.value().width,capture.value().height);
      std::size_t annotated{};
      for (const auto px:grid_pixels[0]) if (decode_half(raw[px*4])!=.25f||decode_half(raw[px*4+1])!=.5f||decode_half(raw[px*4+2])!=1.f) ++annotated;
      check(annotated==0,"annotations_absent_from_scene_linear_capture");
    }
  }
  check(grid_pixels[0].size()>100&&grid_pixels[0]==grid_pixels[1],"annotations_stable_under_exposure");
  // Oracle group 6: independently enumerated physical graph counts.
  struct Expected { std::uint32_t resources,passes,uses,transitions,mask; };
  const auto expect=[&](const char* name,const ViewportStats& stats,Expected e){
    if (stats.gpu_timing_available) { ++e.resources;++e.passes;++e.uses;e.mask|=bit_timing; }
    const auto& g=stats.draw_graph;
    if (cases.tellp()>0) cases<<',';
    cases<<"{\"name\":"<<json_quote(name)<<",\"expected\":["<<e.resources<<','<<e.passes<<','<<e.uses<<','<<e.transitions<<','<<e.mask<<"],\"observed\":["
        <<g.resources<<','<<g.passes<<','<<g.uses<<','<<g.transitions<<','<<g.operation_mask<<"]}";
    if (g.resources!=e.resources||g.passes!=e.passes||g.uses!=e.uses||g.transitions!=e.transitions||g.operation_mask!=e.mask) {
      std::fprintf(stderr,"physical graph case %s: got %u/%u/%u/%u/%u expected %u/%u/%u/%u/%u\n",name,g.resources,g.passes,g.uses,g.transitions,g.operation_mask,
          e.resources,e.passes,e.uses,e.transitions,e.mask);
      require(false,"physical graph description differs from independent oracle");
    }
    checks.emplace_back(name);
  };
  frame.physical.exposure_stops=0;require(viewport.draw(frame),"count fixture quad");
  // Quad: mesh vertices, HDR, display, depth, back buffer; Opaque(mesh,hdr,depth), OutputTransform(hdr,display),
  // PresentCopy(display,back). Transitions: HDR CopySource->RT->SampledRead->CopySource, display CopySource->RT->CopySource,
  // back buffer Present->CopyDestination->Present.
  expect("physical-quad-no-grid",viewport.stats(),{5,3,7,7,bit_opaque|bit_output|bit_present});
  require(viewport.draw(gridded),"count fixture grid");
  expect("physical-grid-no-draws",viewport.stats(),{5,4,9,7,bit_opaque|bit_output|bit_selection|bit_present});
  // Exposure-only changes upload no texture once both slots hold the packet.
  auto textured=frame;const std::array texel{std::byte{200},std::byte{120},std::byte{40},std::byte{255}};
  textured.draws[0].material=material_packet(1,1,texel,{.5f,.5f,.5f,1});
  for (unsigned i=0;i<3;++i) require(viewport.draw(textured),"prime physical texture slots");
  const auto primed=viewport.stats();
  for (unsigned i=0;i<4;++i) { textured.physical.exposure_stops=(i%2) ? -1.0 : 1.0;require(viewport.draw(textured),"exposure-only textured draw"); }
  const auto exposed=viewport.stats();
  check(exposed.texture_uploads==primed.texture_uploads&&exposed.texture_cache_hits==primed.texture_cache_hits+4,"exposure_only_changes_upload_no_texture");
  expect("physical-textured-quad",exposed,{6,3,8,7,bit_opaque|bit_output|bit_present});
  // 4096^2 physical output: the whole declared set stays within the accepted ceiling; then the ordinary size returns.
  require(viewport.resize(4096,4096),"physical 4096 resize");require(viewport.draw(frame),"physical 4096 draw");
  const auto large=viewport.stats();
  check(large.width==4096&&large.height==4096&&large.physical_output_installed&&large.hdr_target_bytes>=128ull*1024*1024&&
      large.output_allocation_bytes>=512ull*1024*1024&&large.output_allocation_bytes<=large.output_budget_bytes,"maximum_output_set_within_declared_ceiling");
  require(viewport.resize(640,480),"physical restore size");require(viewport.draw(frame),"physical restored draw");
  check(viewport.stats().width==640&&viewport.stats().height==480&&viewport.stats().physical_output_installed,"physical_output_survives_resize");
  // Legacy frames keep their exact pixels after the physical output set is installed.
  require(viewport.draw(legacy),"physical legacy control redraw");require(viewport.capture(output/"physical-legacy-after.ppm"),"physical legacy control recapture");
  check(same_bytes(output/"physical-legacy-before.ppm",output/"physical-legacy-after.ppm"),"legacy_output_unchanged_after_physical_frames");
  const auto final_stats=viewport.stats();valid_profile(final_stats);
  check(final_stats.lighting_profile==LightingProfile::LegacyViewportV1&&final_stats.physical_output_installed,"installed_output_retained_across_profiles");
  const bool physical_clean=final_stats.hardware&&!final_stats.device_lost&&final_stats.completed_frames==final_stats.submitted_frames&&
      final_stats.debug_errors==0&&final_stats.debug_warnings==0;
  if (!physical_clean) report_completion("physical",final_stats);
  require(physical_clean,"physical hardware/debug completion");
  std::ostringstream receipt;receipt<<std::setprecision(17)<<std::boolalpha;
  receipt<<"{\"schema\":1,\"passed\":true,\"physical_gpu_contracts_passed\":true,\"profile\":\"PhysicalDiffuseV1\""
      <<",\"source_commit\":"<<json_quote(build::source_commit)<<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)
      <<",\"light_output_manifest_sha256\":"<<json_quote(light_manifest_sha256)<<",\"adapter\":"<<json_quote(final_stats.adapter)<<",\"hardware\":true,\"debug_layer\":"<<final_stats.debug_layer
      <<",\"debug_errors\":"<<final_stats.debug_errors<<",\"debug_warnings\":"<<final_stats.debug_warnings<<",\"gpu_timing_available\":"<<final_stats.gpu_timing_available
      <<",\"rejected_frames\":"<<rejections<<",\"raw_values\":[";
  for (std::size_t i=0;i<values.size();++i) receipt<<(i?",":"")<<values[i];
  receipt<<"],\"raw_captures\":["<<raw_records.str()<<"],\"half_decoder\":\"independent ldexp decoder; every PFM float bit-identical to the decoded raw half\",\"half_mismatches\":"<<half_mismatches
      <<",\"display_probes\":["<<probes.str()<<"],\"display_maximum_error\":"<<maximum_error<<",\"display_channel_tolerance\":2,\"double_encoded_controls_distinct\":"<<double_encoded_distinct
      <<",\"exposure_raw_sha256\":["<<json_quote(exposure_digests[0])<<','<<json_quote(exposure_digests[1])<<"],\"grid_pixels\":"<<grid_pixels[0].size()
      <<",\"graph_cases\":["<<cases.str()<<"],\"count_order\":[\"resources\",\"passes\",\"uses\",\"transitions\",\"operation_mask\"]"
      <<",\"output_allocation_bytes_640x480\":"<<exposed.output_allocation_bytes<<",\"output_allocation_bytes_4096x4096\":"<<large.output_allocation_bytes
      <<",\"output_budget_bytes\":"<<large.output_budget_bytes<<",\"hdr_target_bytes_4096x4096\":"<<large.hdr_target_bytes<<",\"hdr_target_installs\":"<<final_stats.hdr_target_installs
      <<",\"video_memory_budget_bytes\":"<<large.video_memory_budget_bytes<<",\"video_memory_usage_bytes\":"<<large.video_memory_usage_bytes
      <<",\"allocation_scope\":\"API-reported allocation sizes of the owned output set: FP16 target, display image, depth, two swapchain estimates, worst-case HDR readback; video memory is a separate adapter observation\""
      <<",\"checks\":[";
  for (std::size_t i=0;i<checks.size();++i) receipt<<(i?",":"")<<json_quote(checks[i]);
  receipt<<"],\"scope\":\"Windows D3D12 development adapter; scene-linear and display oracles for PhysicalDiffuseV1; no HDR presentation, performance or 8K claim\"}\n";
  const auto result=receipt.str();std::ofstream file(output/"physical-evidence.json",std::ios::binary);file<<result;file.close();
  require(!file.fail(),"physical evidence write");return result;
}
// P03.05 native perspective/grazing-angle fixture. The historical editor scene of the acfbe55
// capture (default box plus the pinned textured mesh) is rebuilt from the retained digest-named
// cooked products at the recovered editor camera (yaw .65, pitch .42, mesh-bounds framing) and
// rendered by the current shader at 1446x675. Every pixel the offline oracle classified must lie
// within its declared envelope plus the calibrated hardware residual (3 + 4 codes); the retained
// current-shader capture is compared byte for byte and recorded, never assumed.
struct OraclePixel { unsigned x{},y{};std::string cls;bool differing{},has_envelope{};std::array<int,3> low{},high{}; };
std::array<int,3> parse_rgb(std::string_view text) {
  std::array<int,3> out{};std::size_t pos=0;
  for (auto& value:out) {
    while (pos<text.size()&&(text[pos]<'0'||text[pos]>'9')) ++pos;
    value=0;while (pos<text.size()&&text[pos]>='0'&&text[pos]<='9') value=value*10+(text[pos++]-'0');
  }
  return out;
}
std::vector<OraclePixel> read_oracle_pixels(const std::filesystem::path& path) {
  std::ifstream in(path);require(static_cast<bool>(in),"oracle pixel table missing");
  std::string line;require(static_cast<bool>(std::getline(in,line)),"oracle pixel table header");
  require(line.rfind("x,y,owner,class,differing,edge,partial_quad,current_rgb,old_rgb,envelope_low,envelope_high",0)==0,"oracle pixel table columns");
  std::vector<OraclePixel> rows;
  while (std::getline(in,line)) {
    if (line.empty()) continue;
    std::vector<std::string> fields;std::string current;
    for (const char c:line) { if (c==',') { fields.push_back(current);current.clear(); } else current+=c; }
    fields.push_back(current);require(fields.size()>=11,"oracle pixel row columns");
    OraclePixel pixel;pixel.x=static_cast<unsigned>(std::stoul(fields[0]));pixel.y=static_cast<unsigned>(std::stoul(fields[1]));
    pixel.cls=fields[3];if (pixel.cls.size()>=2&&pixel.cls.front()=='"') pixel.cls=pixel.cls.substr(1,pixel.cls.size()-2);
    pixel.differing=fields[4]=="true";pixel.has_envelope=fields[9]!="null";
    if (pixel.has_envelope) { pixel.low=parse_rgb(fields[9]);pixel.high=parse_rgb(fields[10]); }
    rows.push_back(pixel);
  }
  return rows;
}
std::vector<std::byte> file_bytes(const std::filesystem::path& path) {
  std::ifstream in(path,std::ios::binary);require(static_cast<bool>(in),"perspective fixture input missing");
  std::vector<char> chars((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(chars.size());std::memcpy(bytes.data(),chars.data(),chars.size());return bytes;
}
std::string perspective_contracts(WindowsViewport& viewport,const std::filesystem::path& assets,const std::filesystem::path& oracle,
    const std::filesystem::path& historical,const std::filesystem::path& output) {
  // Retained cooked products of the historical scene; each file is named by its own digest.
  const auto pinned=[&](const char* name){
    auto bytes=file_bytes(assets/(std::string(name)+".rasset"));require(hex(sha256(bytes))==name,"retained product digest differs from its name");return bytes;
  };
  const auto mesh_bytes=pinned("365bfe88e27a9ee77e3901c1eb6df00ddfb2af61a2a2755f961971cc24f22256");
  const auto material_bytes=pinned("ac185341760c20d13da5fa5e4c70af927ca46de433fb69bfc9313c381d09f2fe");
  const auto texture_bytes=pinned("adfb3db32f6501f25a7917c61acb58fb4a76b576606c284b52444f7f7526cbe5");
  auto mesh_view=assets::mesh::validate(mesh_bytes);require(static_cast<bool>(mesh_view),"historical mesh product");
  auto allocator=TrackedAllocator::create({1024*1024});require(static_cast<bool>(allocator),"perspective mesh allocator");
  auto store=assets::mesh::Store::create({},allocator.value());require(static_cast<bool>(store),"perspective mesh store");
  auto update=store.value()->begin_update(mesh_view.value().id(),mesh_view.value().stamp());require(static_cast<bool>(update),"perspective mesh update");
  auto published=store.value()->publish(update.value(),mesh_bytes);require(static_cast<bool>(published),"perspective mesh publication");
  auto lease=store.value()->acquire(published.value());require(static_cast<bool>(lease),"perspective mesh lease");
  auto geometry=desktop::copy_mesh_geometry(lease.value());require(static_cast<bool>(geometry),"perspective mesh render copy");
  auto texture_view=assets::surface::validate_texture(texture_bytes);require(static_cast<bool>(texture_view),"historical texture product");
  const auto& t=texture_view.value();
  auto texture=t.schema()==3?ViewportTexture::create_scalar_mips(t.width(),t.height(),t.texels()):
      t.schema()==2?ViewportTexture::create_mips(t.width(),t.height(),t.texels()):ViewportTexture::create(t.width(),t.height(),t.pixels());
  require(static_cast<bool>(texture),"historical texture packet");
  auto material_view=assets::surface::validate_material(material_bytes);require(static_cast<bool>(material_view),"historical material product");
  const auto& m=material_view.value().material();
  const auto sampling=m.sampling==assets::surface::Sampling::LinearClampMips?TextureSampling::LinearClampMips:TextureSampling::LinearClampMip0;
  const auto role=m.texture_role==assets::surface::TextureRole::DiffuseReflectanceR?TextureRole::DiffuseReflectanceR:TextureRole::Color;
  auto material=ViewportMaterial::create({float(m.base_color[0]),float(m.base_color[1]),float(m.base_color[2]),1},texture.value(),sampling,role);
  require(static_cast<bool>(material),"historical material packet");
  // Scene of acfbe55 mesh.scene.json: the default box at (0, .5, 0) and the mesh object at the origin.
  RenderFrame frame;frame.scene_generation=1;frame.origin_revision=1;
  DrawPrimitive box;box.id={1,1};box.world[7]=.5;box.color={.28f,.52f,.78f,1};
  DrawPrimitive mesh;mesh.id={1,2};mesh.color={.28f,.52f,.78f,1};mesh.geometry=std::move(geometry).value();mesh.material=std::move(material).value();
  frame.draws={box,mesh};
  // Editor camera at acfbe55: yaw .65, pitch .42, target = mesh bounds centre, distance = clamp(2*diagonal, 2, 10000), far = max(2000, 4*distance).
  const auto& bounds=mesh_view.value().bounds();
  const std::array<double,3> low{bounds.minimum[0],bounds.minimum[1],bounds.minimum[2]},high{bounds.maximum[0],bounds.maximum[1],bounds.maximum[2]};
  const std::array<double,3> target{(low[0]+high[0])/2,(low[1]+high[1])/2,(low[2]+high[2])/2};
  const double diagonal=std::sqrt((high[0]-low[0])*(high[0]-low[0])+(high[1]-low[1])*(high[1]-low[1])+(high[2]-low[2])*(high[2]-low[2]));
  const double distance=std::clamp(diagonal*2,2.0,10000.0),yaw=.65,pitch=.42;
  frame.camera.target={target[0],target[1],target[2]};
  frame.camera.eye={target[0]+distance*std::cos(pitch)*std::sin(yaw),target[1]+distance*std::sin(pitch),target[2]+distance*std::cos(pitch)*std::cos(yaw)};
  frame.camera.far_metres=std::max(2000.0,distance*4);
  require(viewport.resize(1446,675),"perspective fixture size");require(viewport.draw(frame),"perspective fixture draw");
  require(viewport.capture(output/"perspective.ppm"),"perspective capture");
  const auto image=read_ppm(output/"perspective.ppm");require(image.width==1446&&image.height==675,"perspective dimensions");
  // Retained current-shader capture: compared, recorded, never assumed identical.
  const auto reference=read_ppm(historical);require(reference.width==image.width&&reference.height==image.height,"historical capture dimensions");
  std::size_t historical_differing{};unsigned historical_maximum{};
  for (std::size_t i=0;i<image.pixels.size();i+=3) {
    bool differs=false;
    for (unsigned c=0;c<3;++c) { const auto d=static_cast<unsigned>(std::abs(int(image.pixels[i+c])-int(reference.pixels[i+c])));if (d) { differs=true;historical_maximum=std::max(historical_maximum,d); } }
    if (differs) ++historical_differing;
  }
  // Oracle envelope: every classified pixel within [low-7, high+7] (tolerance 3 plus calibrated residual 4).
  const auto rows=read_oracle_pixels(oracle/"pixels.csv");
  std::size_t classified{},within{},outside{},differing_rows{},differing_within{},ambiguous{},overlay{};unsigned maximum_distance{},differing_maximum{};
  std::map<unsigned,std::size_t> histogram;
  for (const auto& row:rows) {
    if (row.cls=="coverage-ambiguous") { ++ambiguous;continue; }
    if (row.cls=="overlay") { ++overlay;continue; }
    require(row.cls=="classified"&&row.has_envelope&&row.x<image.width&&row.y<image.height,"oracle row without a usable envelope");
    ++classified;const auto at=(static_cast<std::size_t>(row.y)*image.width+row.x)*3;unsigned envelope_distance{};
    for (unsigned c=0;c<3;++c) {
      const int value=image.pixels[at+c];
      if (value<row.low[c]) envelope_distance=std::max<unsigned>(envelope_distance,static_cast<unsigned>(row.low[c]-value));
      if (value>row.high[c]) envelope_distance=std::max<unsigned>(envelope_distance,static_cast<unsigned>(value-row.high[c]));
    }
    ++histogram[envelope_distance];maximum_distance=std::max(maximum_distance,envelope_distance);
    if (envelope_distance<=7) ++within; else ++outside;
    if (row.differing) { ++differing_rows;differing_maximum=std::max(differing_maximum,envelope_distance);if (envelope_distance<=7) ++differing_within; }
  }
  require(classified>0&&differing_rows==237,"oracle classified set differs from the retained run");
  require(outside==0,"native perspective pixel outside the declared envelope plus the calibrated residual");
  const auto stats=viewport.stats();valid_profile(stats);
  require(stats.hardware&&!stats.device_lost&&stats.completed_frames==stats.submitted_frames&&stats.debug_errors==0&&stats.debug_warnings==0,"perspective hardware/debug completion");
  std::ostringstream receipt;receipt<<std::setprecision(17)<<std::boolalpha;
  receipt<<"{\"schema\":1,\"passed\":true,\"perspective_gpu_contracts_passed\":true,\"source_commit\":"<<json_quote(build::source_commit)
      <<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)<<",\"adapter\":"<<json_quote(stats.adapter)<<",\"hardware\":true,\"debug_layer\":"<<stats.debug_layer
      <<",\"debug_errors\":"<<stats.debug_errors<<",\"debug_warnings\":"<<stats.debug_warnings<<",\"width\":"<<image.width<<",\"height\":"<<image.height
      <<",\"capture\":\"perspective.ppm\",\"capture_sha256\":"<<json_quote(hex(sha256(file_bytes(output/"perspective.ppm"))))
      <<",\"historical_capture\":"<<json_quote(historical.string())<<",\"historical_sha256\":"<<json_quote(hex(sha256(file_bytes(historical))))
      <<",\"historical_exact\":"<<(historical_differing==0)<<",\"historical_differing_pixels\":"<<historical_differing<<",\"historical_maximum_channel_delta\":"<<historical_maximum
      <<",\"oracle_pixels\":"<<json_quote((oracle/"pixels.csv").string())<<",\"oracle_pixels_sha256\":"<<json_quote(hex(sha256(file_bytes(oracle/"pixels.csv"))))
      <<",\"oracle_predicted_sha256\":"<<json_quote(hex(sha256(file_bytes(oracle/"predicted-current.ppm"))))
      <<",\"fixture_assets\":[\"365bfe88e27a9ee77e3901c1eb6df00ddfb2af61a2a2755f961971cc24f22256\",\"ac185341760c20d13da5fa5e4c70af927ca46de433fb69bfc9313c381d09f2fe\",\"adfb3db32f6501f25a7917c61acb58fb4a76b576606c284b52444f7f7526cbe5\"]"
      <<",\"camera\":{\"yaw\":"<<yaw<<",\"pitch\":"<<pitch<<",\"distance\":"<<distance<<",\"target\":["<<target[0]<<','<<target[1]<<','<<target[2]<<"],\"eye\":["<<frame.camera.eye.x<<','<<frame.camera.eye.y<<','<<frame.camera.eye.z<<"],\"far\":"<<frame.camera.far_metres<<"}"
      <<",\"classified\":"<<classified<<",\"within_envelope_plus_residual\":"<<within<<",\"outside\":"<<outside<<",\"ambiguous\":"<<ambiguous<<",\"overlay\":"<<overlay
      <<",\"differing_pixels\":"<<differing_rows<<",\"differing_within\":"<<differing_within<<",\"differing_maximum_distance\":"<<differing_maximum
      <<",\"maximum_distance\":"<<maximum_distance<<",\"tolerance\":3,\"calibrated_residual\":4,\"distance_histogram\":{";
  bool first=true;for (const auto& [bucket,count]:histogram) { receipt<<(first?"":",")<<'"'<<bucket<<"\":"<<count;first=false; }
  receipt<<"},\"scope\":\"Windows D3D12 development adapter; the current explicit-LOD shader at the historical perspective edge against the offline oracle envelope; no other adapter, driver or performance claim\"}\n";
  const auto result=receipt.str();std::ofstream file(output/"perspective-evidence.json",std::ios::binary);file<<result;file.close();
  require(!file.fail(),"perspective evidence write");return result;
}
// Controlled recreation only: this never calls RemoveDevice or sets a removed flag.
std::string epoch_contracts(HWND window,const std::filesystem::path& shaders,const std::filesystem::path& output) {
  std::vector<std::string> checks;
  const auto check=[&](bool value,const char* name){require(value,name);checks.emplace_back(name);};
  const auto retained_shaders=output/"retained-shaders";
  check(std::filesystem::create_directory(retained_shaders),"unique_retained_shader_fixture");
  for(const auto* name:{"viewport-bindings.rmanifest","viewport.vs.dxil","viewport.ps.dxil","viewport-light-output.rmanifest",
      "viewport-physical.ps.dxil","viewport-output.vs.dxil","viewport-output.ps.dxil"})
    check(std::filesystem::copy_file(shaders/name,retained_shaders/name),name);
  auto made=WindowsViewport::create(window,retained_shaders);require(bool(made),"epoch owner creation");
  auto viewport=std::move(made).value();
  auto frame=fixture();frame.simulation_tick=17;
  const std::array<std::byte,4> texel{std::byte{73},std::byte{129},std::byte{201},std::byte{255}};
  auto color=ViewportTexture::create_mips(1,1,texel),scalar=ViewportTexture::create_scalar_mips(1,1,texel);
  require(bool(color)&&bool(scalar),"epoch owned texture packets");
  auto color_material=ViewportMaterial::create({1,1,1,1},color.value(),TextureSampling::LinearClampMips);
  auto scalar_material=ViewportMaterial::create({1,1,1,1},scalar.value(),TextureSampling::LinearClampMips,TextureRole::DiffuseReflectanceR);
  require(bool(color_material)&&bool(scalar_material),"epoch owned material packets");
  frame.draws[1].material=color_material.value();frame.draws[2].material=scalar_material.value();
  require(viewport->draw(frame),"epoch initial draw");
  auto first_token=viewport->image_token();require(bool(first_token),"epoch initial image token");
  require(viewport->capture(output/"epoch-before.ppm",first_token.value()),"epoch baseline capture");
  const auto initial=viewport->stats();
  check(initial.device_epoch==1&&initial.state==ViewportState::Ready&&initial.has_image,"initial_owner_epoch_and_image");
  check(initial.hardware&&!initial.device_lost&&initial.debug_errors==0&&initial.debug_warnings==0,"initial_hardware_diagnostics");
  check(initial.debug_layer_configured&&initial.debug_layer&&initial.debug_interface_result>=0,"debug_validation_required_for_epoch_qualification");
  // Deliberately equal integer epoch/frame values must not permit another owner.
  // Both native owners remain alive: SDK debug configuration must run only once.
  struct OtherWindow { HWND value{};~OtherWindow(){if(value)DestroyWindow(value);} };
  OtherWindow other{CreateWindowExW(0,NativeWindow::name,L"RACE epoch owner isolation",WS_OVERLAPPEDWINDOW,
      0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr)};
  check(other.value!=nullptr,"second_owner_surface");
  auto other_made=WindowsViewport::create(other.value,shaders);require(bool(other_made),"second owner creation");
  auto other_view=std::move(other_made).value();require(other_view->draw(frame),"second owner draw");
  auto other_token=other_view->image_token();require(bool(other_token),"second owner token");
  check(other_token.value().device_epoch()==first_token.value().device_epoch()&&
      other_token.value().frame()==first_token.value().frame(),"cross_owner_equal_integer_stamps_fixture");
  rejected(other_view->capture(output/"cross-owner-rejected.ppm",first_token.value()),ErrorCode::StaleHandle,"cross owner token accepted");
  check(!std::filesystem::exists(output/"cross-owner-rejected.ppm"),"cross_owner_rejected_before_output");
  require(other_view->recreate(other.value),"second owner controlled recreation while first remains alive");
  require(other_view->draw(frame),"second owner recreated draw");
  require(viewport->capture(output/"epoch-other-owner-recreated.ppm",first_token.value()),"first owner survives second recreation");
  const auto other_stats=other_view->stats();
  check(other_stats.debug_layer&&!other_stats.device_lost&&other_stats.debug_errors==0&&other_stats.debug_warnings==0&&
      viewport->stats().device_epoch==initial.device_epoch&&same_bytes(output/"epoch-before.ppm",output/"epoch-other-owner-recreated.ppm"),
      "simultaneous_owner_debug_configuration_and_lifecycle");
  other_view.reset();
  rejected(viewport->capture(output/"expired-owner-rejected.ppm",other_token.value()),ErrorCode::StaleHandle,"expired owner token accepted");
  check(!std::filesystem::exists(output/"expired-owner-rejected.ppm"),"expired_owner_rejected_before_output");
  const auto before_rejections=viewport->stats();
  // Every lifecycle entry point rejects a foreign thread before changing native state.
  std::array<bool,4> wrong_thread{};
  std::thread foreign([&]{
    const auto a=viewport->recreate(window),b=viewport->recover_removed(window),c=viewport->retire_surface();
    const auto d=viewport->capture(output/"foreign-thread-rejected.ppm",first_token.value());
    wrong_thread={!a&&a.error().code==ErrorCode::WrongThread,!b&&b.error().code==ErrorCode::WrongThread,
        !c&&c.error().code==ErrorCode::WrongThread,!d&&d.error().code==ErrorCode::WrongThread};
  });foreign.join();
  check(std::all_of(wrong_thread.begin(),wrong_thread.end(),[](bool value){return value;}),"wrong_thread_lifecycle_rejected");
  rejected(viewport->recreate(nullptr),ErrorCode::InvalidInput,"absent surface recreation accepted");
  rejected(viewport->recover_removed(nullptr),ErrorCode::InvalidInput,"absent surface recovery accepted");
  rejected(viewport->recover_removed(window),ErrorCode::FailedPrerequisite,"healthy device removal recovery accepted");
  check(viewport->stats().device_epoch==before_rejections.device_epoch&&viewport->stats().removal_recovery_attempts==0&&
      !std::filesystem::exists(output/"foreign-thread-rejected.ppm"),"rejected_requests_preserve_owner");
  // Modify only the private fixture. Existing ownership must not reread shader disk bytes.
  {std::fstream file(retained_shaders/"viewport.ps.dxil",std::ios::binary|std::ios::in|std::ios::out);
    char first{};file.read(&first,1);require(bool(file),"retained shader fixture read");first^=1;
    file.seekp(0);file.write(&first,1);file.close();require(!file.fail(),"retained shader fixture mutate");}
  auto invalid_program=WindowsViewport::create(window,retained_shaders);
  check(!invalid_program,"mutated_shader_fixture_rejected_by_new_owner");
  require(viewport->draw(frame),"pending draw before controlled recreation");
  auto pending_token=viewport->image_token();require(bool(pending_token),"pending image token");
  const auto pending=viewport->stats();
  // Do not capture/drain here: recreation itself must retire whatever remains outstanding.
  require(viewport->recreate(window),"controlled recreation");
  const auto recreated=viewport->stats();
  check(recreated.device_epoch==pending.device_epoch+1&&recreated.recreation==ViewportRecreation::Controlled&&
      recreated.recreation_attempts==before_rejections.recreation_attempts+1&&recreated.removal_recovery_attempts==0,"controlled_epoch_increment");
  check(!recreated.retired.removed&&recreated.retired.device_epoch==pending.device_epoch&&
      recreated.retired.submitted_frames==pending.submitted_frames&&
      recreated.retired.completed_frames==pending.submitted_frames,"controlled_fence_retirement");
  check(recreated.submitted_frames==0&&recreated.completed_frames==0&&recreated.gpu_sample_count==0&&
      recreated.last_timed_frame==0&&recreated.texture_uploads==0&&recreated.texture_cache_hits==0&&
      recreated.draw_graph.submissions==0&&!recreated.has_image,"fresh_epoch_has_no_old_native_results");
  check(recreated.adapter==initial.adapter&&recreated.vendor_id==initial.vendor_id&&recreated.device_id==initial.device_id,
      "controlled_recreation_adapter_identity");
  rejected(viewport->capture(output/"old-epoch-rejected.ppm",pending_token.value()),ErrorCode::StaleHandle,"old epoch token accepted");
  rejected(viewport->capture(output/"not-redrawn-rejected.ppm"),ErrorCode::Busy,"undrawn new epoch accepted");
  check(!std::filesystem::exists(output/"old-epoch-rejected.ppm")&&!std::filesystem::exists(output/"not-redrawn-rejected.ppm"),
      "stale_epoch_rejected_before_output");
  require(viewport->draw(frame),"same owned frame after recreation");
  require(viewport->capture(output/"epoch-after.ppm"),"new epoch capture");
  auto redrawn=viewport->stats();valid_profile(redrawn);
  check(same_bytes(output/"epoch-before.ppm",output/"epoch-after.ppm"),"controlled_pixels_exact");
  check(redrawn.scene_generation==frame.scene_generation&&redrawn.origin_revision==frame.origin_revision&&
      redrawn.simulation_tick==17&&redrawn.texture_uploads==2,"owned_inputs_reuploaded_without_tick_advance");
  check(std::all_of(redrawn.gpu_samples.begin(),redrawn.gpu_samples.begin()+redrawn.gpu_sample_count,
      [&](const auto& sample){return sample.device_epoch==redrawn.device_epoch;}),"new_epoch_timing_stamps");
  auto restored_token=viewport->image_token();require(bool(restored_token),"restored token");
  require(viewport->resize(0,0),"epoch minimize");require(viewport->resize(640,480),"epoch restore dimensions");
  check(viewport->stats().device_epoch==redrawn.device_epoch,"resize_preserves_device_epoch");
  rejected(viewport->capture(output/"minimized-image-rejected.ppm",restored_token.value()),ErrorCode::StaleHandle,"pre-minimize image reused");
  require(viewport->draw(frame),"before surface retirement draw");
  require(viewport->retire_surface(),"surface retirement");
  const auto absent=viewport->stats();
  check(absent.state==ViewportState::SurfaceRetired&&!absent.has_image&&absent.gpu_sample_count==0&&
      absent.retired.completed_frames==absent.retired.submitted_frames,"absent_surface_retains_drained_diagnostics");
  rejected(viewport->draw(frame),ErrorCode::FailedPrerequisite,"absent surface draw accepted");
  rejected(viewport->capture(output/"absent-surface-rejected.ppm"),ErrorCode::FailedPrerequisite,"absent surface capture accepted");
  require(viewport->retire_surface(),"idempotent surface retirement");
  require(viewport->recreate(window),"surface restore");
  check(viewport->stats().device_epoch==redrawn.device_epoch+1&&viewport->stats().recreation==ViewportRecreation::SurfaceRestore,
      "surface_restore_monotonic_epoch");
  require(viewport->draw(frame),"surface restore owned frame");require(viewport->capture(output/"epoch-restored.ppm"),"surface restore capture");
  check(same_bytes(output/"epoch-before.ppm",output/"epoch-restored.ppm"),"surface_restore_pixels_exact");
  const auto final=viewport->stats();valid_profile(final);
  check(final.hardware&&!final.device_lost&&final.debug_errors==0&&final.debug_warnings==0&&
      final.completed_frames==final.submitted_frames,"final_hardware_diagnostics_and_quiescence");
  std::ostringstream report;report<<"{\"viewport_epoch_contracts_passed\":true,\"lane\":\"controlled_recreation\","
      "\"programmatic_removal_exercised\":false,\"spontaneous_loss_exercised\":false,\"source_commit\":"<<json_quote(build::source_commit)
      <<",\"authored_input_sha256\":"<<json_quote(build::authored_input_sha256)<<",\"adapter\":"<<json_quote(final.adapter)
      <<",\"hardware\":true,\"debug_layer\":"<<(final.debug_layer?"true":"false")<<",\"debug_errors\":0,\"debug_warnings\":0,"
      "\"initial_epoch\":"<<initial.device_epoch<<",\"final_epoch\":"<<final.device_epoch<<",\"same_frame_tick\":17,\"checks\":[";
  for(std::size_t i=0;i<checks.size();++i){if(i)report<<',';report<<"{\"name\":"<<json_quote(checks[i])<<",\"passed\":true}";}
  report<<"],\"output_directory\":"<<json_quote(output.string())<<"}\n";
  return report.str();
}
} // namespace

int main(int argc,char** argv) {
  std::filesystem::path shaders,output,fixture_assets,oracle_dir,historical;
  bool smoke=false,filter_cost=false,edge_smoke=false,scalar_smoke=false,graph_smoke=false,epoch_smoke=false,perspective_smoke=false;
  for (int i=1;i<argc;++i) {
    const std::string_view flag(argv[i]);
    if (flag=="--smoke" && !smoke) smoke=true;
    else if (flag=="--filter-cost" && !filter_cost) filter_cost=true;
    else if (flag=="--edge-smoke" && !edge_smoke) edge_smoke=true;
    else if (flag=="--scalar-smoke" && !scalar_smoke) scalar_smoke=true;
    else if (flag=="--graph-smoke" && !graph_smoke) graph_smoke=true;
    else if (flag=="--epoch-smoke" && !epoch_smoke) epoch_smoke=true;
    else if (flag=="--perspective-smoke" && !perspective_smoke) perspective_smoke=true;
    else if (flag=="--fixture-assets" && i+1<argc && fixture_assets.empty()) fixture_assets=argv[++i];
    else if (flag=="--oracle-dir" && i+1<argc && oracle_dir.empty()) oracle_dir=argv[++i];
    else if (flag=="--historical" && i+1<argc && historical.empty()) historical=argv[++i];
    else if (flag=="--shader-dir" && i+1<argc && shaders.empty()) shaders=argv[++i];
    else if (flag=="--output-dir" && i+1<argc && output.empty()) output=argv[++i];
    else { std::fputs("usage: race_viewport_tests (--smoke | --edge-smoke | --scalar-smoke | --graph-smoke | --epoch-smoke | --filter-cost | --perspective-smoke --fixture-assets DIR --oracle-dir DIR --historical FILE) --shader-dir DIR --output-dir BASE\n",stderr); return 2; }
  }
  if (unsigned(smoke)+unsigned(filter_cost)+unsigned(edge_smoke)+unsigned(scalar_smoke)+unsigned(graph_smoke)+unsigned(epoch_smoke)+unsigned(perspective_smoke)!=1 || shaders.empty() || output.empty()) return 2;
  if (perspective_smoke!=(!fixture_assets.empty()&&!oracle_dir.empty()&&!historical.empty())) return 2;
  try {
    const auto run=std::string(filter_cost ? "filter-cost-" : edge_smoke ? "edge-" : scalar_smoke ? "scalar-" : graph_smoke ? "graph-" : epoch_smoke ? "epoch-" : perspective_smoke ? "perspective-" : "viewport-")+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
    std::filesystem::create_directories(output);
    output=std::filesystem::absolute(output/run);
    require(std::filesystem::create_directory(output),"unique probe output already exists");
    NativeWindow window;
    auto invalid_window=WindowsViewport::create(nullptr,shaders);
    require(!invalid_window && invalid_window.error().code==ErrorCode::InvalidInput,"invalid native window accepted");
    if(epoch_smoke){const auto receipt=epoch_contracts(window.handle,shaders,output);std::fputs(receipt.c_str(),stdout);return 0;}
    auto created=WindowsViewport::create(window.handle,shaders);
    if (!created) { std::fprintf(stderr,"viewport create: %s\n",created.error().message); return 3; }
    auto viewport=std::move(created).value();
    if (filter_cost) {
      const auto receipt=filter_cost_contracts(*viewport,shaders,output);
      viewport.reset(); std::fputs(receipt.c_str(),stdout); return 0;
    }
    if (edge_smoke) {
      const auto receipt=triangle_edge_contracts(*viewport,output);
      viewport.reset(); std::fputs(receipt.c_str(),stdout); return 0;
    }
    if (scalar_smoke) {
      const auto receipt=scalar_contracts(*viewport,output);
      viewport.reset(); std::fputs(receipt.c_str(),stdout); return 0;
    }
    if (graph_smoke) {
      const auto receipt=graph_contracts(*viewport,output);
      viewport.reset(); std::fputs(receipt.c_str(),stdout); return 0;
    }
    if (perspective_smoke) {
      const auto receipt=perspective_contracts(*viewport,fixture_assets,oracle_dir,historical,output);
      viewport.reset(); std::fputs(receipt.c_str(),stdout); return 0;
    }
    shader_admission_contracts(window.handle,*viewport,shaders,output);
    auto frame=fixture();
    rejected(viewport->capture(output/"not-drawn.ppm"),ErrorCode::Busy,"capture accepted without frame");
    std::uint64_t last_timed{};
    for (unsigned tick=0;tick<5;++tick) {
      frame.simulation_tick=tick; require(viewport->draw(frame),"draw cube scene");
      const auto profile=viewport->stats(); valid_profile(profile);
      require(profile.last_timed_frame>=last_timed,"completed GPU timing regressed across frames");
      last_timed=profile.last_timed_frame;
    }
    require(viewport->capture(output/"normal.ppm"),"normal capture");
    const auto drained_profile=viewport->stats(); valid_profile(drained_profile);
    if (drained_profile.gpu_timing_available) {
      require(drained_profile.last_timed_frame==5 && drained_profile.gpu_sample_count==4,"drain lost latest GPU timing or history bound");
      for (std::size_t i=0;i<4;++i) require(drained_profile.gpu_samples[i].frame==i+2,"GPU history did not retain newest four frames");
    }
    const auto normal=read_ppm(output/"normal.ppm"); require(normal.width==640 && normal.height==480,"initial capture dimensions");
    std::set<std::array<unsigned char,3>> colors;
    for (std::size_t i=0;i<normal.pixels.size();i+=3) colors.insert({normal.pixels[i],normal.pixels[i+1],normal.pixels[i+2]});
    require(colors.size()>=12,"GPU image missing lit cube/grid colors");
    frame.draws[1].selected=true; require(viewport->draw(frame),"selected draw");
    require(viewport->capture(output/"selected.ppm"),"selected capture");
    const auto selected=read_ppm(output/"selected.ppm");
    std::size_t changed{};
    for (std::size_t i=0;i<normal.pixels.size();i+=3)
      if (normal.pixels[i]!=selected.pixels[i] || normal.pixels[i+1]!=selected.pixels[i+1] || normal.pixels[i+2]!=selected.pixels[i+2]) ++changed;
    require(changed>20 && changed<normal.pixels.size()/30,"selection edge did not produce bounded visible change");
    // Geometry and camera both translate by 1e9 metres. Input coordinates in this
    // fixture are binary-exact; camera-relative conversion must retain pixels.
    frame.grid=false; require(viewport->draw(frame),"geometry-only draw");
    require(viewport->capture(output/"near-world.ppm"),"near-world capture");
    auto distant=frame;
    for (auto& draw:distant.draws) { draw.world[3]+=1e9; draw.world[7]+=1e9; draw.world[11]+=1e9; }
    distant.camera.eye.x+=1e9; distant.camera.eye.y+=1e9; distant.camera.eye.z+=1e9;
    distant.camera.target.x+=1e9; distant.camera.target.y+=1e9; distant.camera.target.z+=1e9;
    ++distant.origin_revision;
    require(viewport->draw(distant),"large-world draw"); require(viewport->capture(output/"distant-world.ppm"),"large-world capture");
    require(read_ppm(output/"near-world.ppm").pixels==read_ppm(output/"distant-world.ppm").pixels,"large-world geometry pixels changed");
    distant.grid=true; require(viewport->draw(distant),"large-world high-altitude grid omission");
    require(!viewport->stats().grid_visible,"out-of-range ground grid was not omitted");
    // A horizontally distant ground grid still works, with true world axes absent.
    for (auto& draw:distant.draws) draw.world[7]-=1e9;
    distant.camera.eye.y-=1e9; distant.camera.target.y-=1e9;
    require(viewport->draw(distant),"large-world ground grid"); require(viewport->stats().grid_visible,"near ground grid was omitted");
    // Source -> native import -> CPU cook -> Assets lease -> immutable Render
    // packet -> actual D3D12. The store/source/lease are already destroyed.
    auto mesh_frame=frame; mesh_frame.grid=true;
    mesh_frame.camera.eye={10,8,12}; mesh_frame.camera.target={0,1,0};
    mesh_frame.draws[1].geometry=imported_mesh(); mesh_frame.draws[1].selected=false;
    mesh_frame.draws[1].world={0,0,1,-1, 0,1.5,0,.25, -2,0,0,1};
    require(viewport->draw(mesh_frame),"mixed mesh cube grid draw");
    require(viewport->capture(output/"mesh-mixed.ppm"),"mesh mixed capture");
    const auto mesh_unselected=read_ppm(output/"mesh-mixed.ppm");
    auto cube_control=mesh_frame; cube_control.draws[1].geometry.reset();
    require(viewport->draw(cube_control),"matched cube control draw");
    require(viewport->capture(output/"mesh-cube-control.ppm"),"matched cube control capture");
    require(mesh_unselected.pixels!=read_ppm(output/"mesh-cube-control.ppm").pixels,"mesh fell back to unit cube geometry");
    mesh_frame.draws[1].selected=true;
    require(viewport->draw(mesh_frame),"selected mesh edges draw");
    require(viewport->capture(output/"mesh-selected.ppm"),"mesh selected capture");
    require(mesh_unselected.pixels!=read_ppm(output/"mesh-selected.ppm").pixels,"mesh selection edges missing");
    const auto overlay_evidence=overlay_contracts(*viewport,mesh_frame,output);
    const auto material_evidence=material_contracts(*viewport,output);
    const auto mip_evidence=mip_contracts(*viewport,output);
    (void)triangle_edge_contracts(*viewport,output);
    (void)scalar_contracts(*viewport,output);
    (void)graph_contracts(*viewport,output);
    (void)physical_contracts(*viewport,shaders,output);
    mesh_frame.grid=false; mesh_frame.draws={mesh_frame.draws[1]};
    std::vector<GeometryVertex> far_vertices(mesh_frame.draws[0].geometry->vertices().begin(),mesh_frame.draws[0].geometry->vertices().end());
    for (auto& vertex:far_vertices) for (auto& coordinate:vertex.position) coordinate+=1e9;
    auto far_packet=ViewportGeometry::create(far_vertices,mesh_frame.draws[0].geometry->indices());
    require(static_cast<bool>(far_packet),"large-local immutable packet");
    far_vertices.clear();
    for (unsigned reflected=0;reflected<2;++reflected) {
      if (reflected) for (std::size_t row=0;row<3;++row) mesh_frame.draws[0].world[row*4]*=-1;
      require(viewport->draw(mesh_frame),"nonuniform rotated mesh");
      require(viewport->capture(output/(reflected ? "mesh-mirrored.ppm" : "mesh-near.ppm")),"mesh near capture");
      const auto near_mesh=read_ppm(output/(reflected ? "mesh-mirrored.ppm" : "mesh-near.ppm"));
      auto far_mesh=mesh_frame; far_mesh.draws[0].geometry=far_packet.value();
      for (std::size_t row=0;row<3;++row) {
        auto& world=far_mesh.draws[0].world;
        world[row*4+3]-=(world[row*4]+world[row*4+1]+world[row*4+2])*1e9;
      }
      require(viewport->draw(far_mesh),"large-local cancellation mesh draw");
      require(viewport->capture(output/(reflected ? "mesh-far-mirrored.ppm" : "mesh-far.ppm")),"mesh far capture");
      require(near_mesh.pixels==read_ppm(output/(reflected ? "mesh-far-mirrored.ppm" : "mesh-far.ppm")).pixels,
          "large-local mesh geometry lost precision");
    }
    // Alternate immutable revisions and upload sizes through both swapchain slots.
    for (unsigned i=0;i<8;++i) {
      auto changing=frame; changing.draws[1].geometry=imported_mesh(); changing.draws[1].selected=(i%2)==0;
      if (i%3) changing.draws[2].geometry=changing.draws[1].geometry;
      require(viewport->draw(changing),"mesh slot replacement");
    }
    auto far_rejected=mesh_frame; far_rejected.draws[0].world[3]=1e12;
    rejected(viewport->draw(far_rejected),ErrorCode::Exhausted,"out-of-range mesh vertex accepted");
    std::vector<std::uint32_t> many_indices(viewport_mesh_max_indices);
    for (std::size_t i=0;i<many_indices.size();++i) many_indices[i]=static_cast<std::uint32_t>(i%3);
    auto many_packet=ViewportGeometry::create(mesh_frame.draws[0].geometry->vertices(),many_indices);
    require(static_cast<bool>(many_packet),"mesh exact packet index cap");
    auto over=frame; over.draws[0].geometry=many_packet.value(); over.draws[1].geometry=many_packet.value();
    rejected(viewport->draw(over),ErrorCode::Exhausted,"aggregate mesh index overflow accepted");
    std::vector<GeometryVertex> many_vertices(viewport_mesh_max_vertices,mesh_frame.draws[0].geometry->vertices()[0]);
    many_vertices[1]=mesh_frame.draws[0].geometry->vertices()[1]; many_vertices[2]=mesh_frame.draws[0].geometry->vertices()[2];
    const std::array<std::uint32_t,3> one_triangle{0,1,2};
    auto vertex_packet=ViewportGeometry::create(many_vertices,one_triangle);
    require(static_cast<bool>(vertex_packet),"mesh exact packet vertex cap");
    over=mesh_frame; over.draws.resize(5,over.draws[0]);
    for (std::size_t i=0;i<over.draws.size();++i) { over.draws[i].id={99,i+1}; over.draws[i].geometry=vertex_packet.value(); }
    rejected(viewport->draw(over),ErrorCode::Exhausted,"aggregate mesh vertex overflow accepted");
    require(viewport->draw(mesh_frame),"valid mesh after rejected frames");
    frame.grid=true;
    SetWindowPos(window.handle,nullptr,0,0,337,256,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    require(viewport->resize(321,217),"resize"); frame.draws[1].geometry=imported_mesh(); require(viewport->draw(frame),"resized draw");
    require(viewport->capture(output/"resized.ppm"),"resized capture");
    const auto resized=read_ppm(output/"resized.ppm"); require(resized.width==321 && resized.height==217,"resized readback dimensions");
    const auto before_minimize=viewport->stats();const auto submitted=before_minimize.submitted_frames;
    require(viewport->resize(0,0),"minimize"); require(viewport->draw(frame),"minimized no-op");
    require(viewport->stats().submitted_frames==submitted && viewport->stats().last_cpu_frame==submitted,
        "minimized viewport advanced work or profiling stamp");
    rejected(viewport->capture(output/"minimized.ppm"),ErrorCode::Busy,"minimized stale capture accepted");
    require(same_graph_stats(before_minimize.draw_graph,viewport->stats().draw_graph) &&
        same_graph_stats(before_minimize.capture_graph,viewport->stats().capture_graph),"minimized no-op changed graph statistics");
    require(viewport->resize(321,217),"restore"); require(viewport->draw(frame),"restored draw");
    require(viewport->capture(output/"restored.ppm"),"restored capture");
    require(resized.pixels==read_ppm(output/"restored.ppm").pixels,"resize/restore changed pixels");
    auto excessive=frame; excessive.draws.resize(viewport_max_draws+1);
    rejected(viewport->draw(excessive),ErrorCode::Exhausted,"draw budget overflow accepted");
    auto duplicate=frame; duplicate.draws[1].id=duplicate.draws[0].id;
    rejected(viewport->draw(duplicate),ErrorCode::Duplicate,"duplicate stable ID accepted");
    auto singular=frame; singular.draws[1].world[0]=0;
    rejected(viewport->draw(singular),ErrorCode::InvalidInput,"singular affine accepted");
    auto invalid=frame; invalid.camera.eye=invalid.camera.target;
    rejected(viewport->draw(invalid),ErrorCode::InvalidInput,"degenerate camera accepted");
    invalid=frame; invalid.draws[1].world[3]=std::numeric_limits<double>::quiet_NaN();
    rejected(viewport->draw(invalid),ErrorCode::InvalidInput,"NaN affine accepted");
    rejected(viewport->resize(4097,217),ErrorCode::Exhausted,"viewport dimensions exceeded bound");
    bool thread_rejected=false;
    std::thread foreign([&]{auto r=viewport->draw(frame); thread_rejected=!r && r.error().code==ErrorCode::WrongThread;});
    foreign.join(); require(thread_rejected,"foreign-thread draw accepted");
    const auto stats=viewport->stats(); valid_profile(stats);
    if (stats.gpu_timing_available) require(stats.last_timed_frame==stats.submitted_frames,
        "final drain did not associate GPU timing with final submitted frame");
    require(stats.hardware && !stats.device_lost && stats.completed_frames==stats.submitted_frames,"hardware work did not complete");
    if (stats.debug_errors || stats.debug_warnings)
      std::fprintf(stderr,"D3D12 diagnostics: errors=%llu warnings=%llu last=%s\n",
          static_cast<unsigned long long>(stats.debug_errors),static_cast<unsigned long long>(stats.debug_warnings),
          stats.last_debug_message.data());
    require(stats.debug_errors==0 && stats.debug_warnings==0,"D3D12 debug validation reported messages");
    // Leave one final real submission for destructor-driven fenced retirement.
    require(viewport->draw(frame),"final in-flight draw"); viewport.reset();
    std::printf("{\"viewport_contracts_passed\":true,\"shader_admission_contracts_passed\":true,\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\",\"adapter\":%s,\"hardware\":true,"
        "\"debug_layer\":%s,\"debug_errors\":%llu,\"debug_warnings\":%llu,"
        "\"gpu_timing_available\":%s,\"gpu_ms\":%.9f,\"last_timed_frame\":%llu,\"gpu_sample_count\":%zu,"
        "\"last_cpu_frame\":%llu,\"last_draw_cpu_ms\":%.9f,\"last_present_cpu_ms\":%.9f,"
        "\"last_validation_cpu_ms\":%.9f,\"last_record_cpu_ms\":%.9f,\"last_inflight_wait_cpu_ms\":%.9f,"
        "\"submitted_before_shutdown\":%llu,"
        "\"completed_before_shutdown\":%llu,\"selection_changed_pixels\":%zu,"
        "\"large_world_pixels_exact\":true,\"resize_restore_pixels_exact\":true,"
        "\"imported_mesh_gpu_draw\":true,\"large_local_mesh_pixels_exact\":true,\"mesh_slot_retirement\":true,"
        "\"overlay_gpu_draw\":true,\"overlay_clear_bytes_exact\":true,\"overlay_rejection_kept_image\":true,"
        "\"overlay_draw_cap_accepted\":%zu,\"overlay_vertex_budget\":%zu,\"overlay_largest_complete_triangle_vertices\":%zu,"
        "\"overlay_changed_pixels\":%zu,\"overlay_rejected_frames\":%zu,"
        "\"material_gpu_contracts_passed\":true,\"texture_gray_expected\":[%u,%u,%u],\"texture_gray_observed\":[%u,%u,%u],"
        "\"texture_orientation_samples\":%zu,\"texture_rejected_frames\":%zu,"
        "\"texture_stable_uploads\":%llu,\"texture_stable_cache_hits\":%llu,"
        "\"scalar_gpu_contracts_passed\":true,\"scalar_evidence\":\"scalar-evidence.json\","
        "\"graph_gpu_contracts_passed\":true,\"graph_evidence\":\"graph-evidence.json\","
        "\"physical_gpu_contracts_passed\":true,\"physical_evidence\":\"physical-evidence.json\","
        "\"edge_gpu_contracts_passed\":true,\"edge_evidence\":\"edge-evidence.json\",\"mip_gpu_contracts_passed\":true,\"mip_numerical_probes\":%zu,\"mip_checker_probes\":%zu,\"mip_motion_frames\":%zu,\"mip_motion_maximum_error\":%u,"
        "\"mip_region_probes\":%zu,\"mip_region_pixels\":%zu,\"mip_region_maximum_error\":%u,"
        "\"mip_stable_uploads\":%llu,\"mip_stable_cache_hits\":%llu,\"mip_padded_upload_bytes\":%llu,\"mip_six_chain_bytes\":%zu,"
        "\"mip_evidence\":\"mip-evidence.json\",\"texture_uploads\":%llu,\"texture_upload_bytes\":%llu,\"texture_cache_hits\":%llu,"
        "\"shutdown_after_submission\":true,\"output_directory\":%s}\n",
        build::source_commit,build::authored_input_sha256,json_quote(stats.adapter).c_str(),stats.debug_layer?"true":"false",
        static_cast<unsigned long long>(stats.debug_errors),static_cast<unsigned long long>(stats.debug_warnings),
        stats.gpu_timing_available?"true":"false",stats.gpu_milliseconds,
        static_cast<unsigned long long>(stats.last_timed_frame),stats.gpu_sample_count,
        static_cast<unsigned long long>(stats.last_cpu_frame),stats.last_draw_cpu_ms,stats.last_present_cpu_ms,
        stats.last_validation_cpu_ms,stats.last_record_cpu_ms,stats.last_inflight_wait_cpu_ms,
        static_cast<unsigned long long>(stats.submitted_frames),static_cast<unsigned long long>(stats.completed_frames),
        changed,viewport_max_overlay_draws,viewport_max_overlay_vertices,overlay_evidence.maximum_complete_vertices,
        overlay_evidence.changed_pixels,overlay_evidence.rejected_frames,
        material_evidence.gray_expected[0],material_evidence.gray_expected[1],material_evidence.gray_expected[2],
        material_evidence.gray_observed[0],material_evidence.gray_observed[1],material_evidence.gray_observed[2],
        material_evidence.orientation_samples,material_evidence.rejected_frames,
        static_cast<unsigned long long>(material_evidence.stable_uploads),static_cast<unsigned long long>(material_evidence.stable_cache_hits),
        mip_evidence.probes.size(),mip_evidence.checker_probes.size(),mip_evidence.motion_frames,mip_evidence.motion_maximum_error,
        mip_evidence.regions.size(),mip_evidence.region_pixels,mip_evidence.region_maximum_error,
        static_cast<unsigned long long>(mip_evidence.stable_uploads),static_cast<unsigned long long>(mip_evidence.stable_cache_hits),
        static_cast<unsigned long long>(mip_evidence.upload_bytes),mip_evidence.maximum_chain_frame_bytes,
        static_cast<unsigned long long>(stats.texture_uploads),static_cast<unsigned long long>(stats.texture_upload_bytes),
        static_cast<unsigned long long>(stats.texture_cache_hits),json_quote(output.string()).c_str());
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr,"viewport hardware contracts: %s\n",error.what()); return 1; }
}
