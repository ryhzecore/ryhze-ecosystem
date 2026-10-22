#include "surface_pipeline.hpp"
#include "assets_support.hpp"
#include <cstdio>
#include <new>

namespace {
thread_local std::size_t failure=SIZE_MAX;
void allocation_point(){if(failure!=SIZE_MAX){if(!failure){failure=SIZE_MAX;throw std::bad_alloc();}--failure;}}
}
// Isolated offline-pipeline allocation injection; no production allocator changes.
void* operator new(std::size_t count){allocation_point();if(auto memory=std::malloc(count?count:1))return memory;throw std::bad_alloc();}
void* operator new[](std::size_t count){return ::operator new(count);}
void operator delete(void* memory) noexcept{std::free(memory);}
void operator delete[](void* memory) noexcept{std::free(memory);}
void operator delete(void* memory,std::size_t) noexcept{std::free(memory);}
void operator delete[](void* memory,std::size_t) noexcept{std::free(memory);}

namespace {
namespace s=race::assets::surface;
namespace p=race::surface_pipeline;
std::span<const std::byte> bytes(std::string_view value){return std::as_bytes(std::span(value));}
struct ReservedFiles:FakeFiles{
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{
    auto value=FakeFiles::read(leaf,limit);if(value)value.value().reserve(s::Limits{}.product_bytes+1);return value;
  }
};
void run(){
  CHECK_ERROR(p::import_ppm({},bytes("P6\n1 1\n255\nRGB"),s::Encoding::SrgbColor),ErrorCode::InvalidInput);
  CHECK_ERROR(p::import_ppm({4,8},bytes(std::string(4096,' ')+"P6"),s::Encoding::SrgbColor),ErrorCode::Exhausted);
  const std::string source="P6\n# bounded header\n2 1\n255\n\n \tRGB";
  auto first=p::import_ppm({4,8},bytes(source),s::Encoding::SrgbColor);CHECK(first);
  CHECK(p::import_ppm({4,8},bytes(source),s::Encoding::SrgbColor).value()==first.value());
  auto view=s::validate_texture(first.value());CHECK(view);CHECK(view.value().width()==2&&view.value().height()==1);
  CHECK(view.value().pixels()[0]==std::byte{10}&&view.value().pixels()[1]==std::byte{32}&&view.value().pixels()[2]==std::byte{9});
  CHECK(view.value().pixels()[3]==std::byte{255}&&view.value().pixels()[7]==std::byte{255});
  // Independent struct.pack/hashlib legacy PPM profile vectors.
  CHECK(assets::hex(assets::digest(first.value()))=="94e699afb8d993e35435c8473a23ad6df55fe1cc081f6202e8b0a2a2c15a293e");
  auto cooked=p::cook(first.value());CHECK(cooked);CHECK(!p::cook(cooked.value()));
  CHECK(assets::hex(assets::digest(cooked.value()))=="1126bcc83615edc6375fa6dad4c1f463adb874095cb8aaf83865902cf1bfbc89");
  CHECK(p::cook(first.value()).value()==cooked.value());
  FakeFiles files;auto leaf=p::publish(files,cooked.value());CHECK(leaf);CHECK(p::publish(files,cooked.value()).value()==leaf.value());
  auto texture=s::validate_texture(cooked.value());CHECK(texture);
  const auto reference=std::string("{\"asset\":\"")+assets::id_text(texture.value().id())+"\",\"revision\":\""+assets::hex(texture.value().revision())+"\"}";
  const auto definition=std::string("{\"schema\":1,\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,1e-1],\"base_color_texture\":")+reference+"}";
  auto imported=p::import_material({5,9},bytes(definition));CHECK(imported);CHECK(!p::cook(imported.value()));
  auto material=p::cook(imported.value(),&files);CHECK(material);CHECK(p::publish(files,material.value()));
  auto m=s::validate_material(material.value());CHECK(m);CHECK(m.value().material().base_color==std::array<double,3>{1,.25,.1});
  CHECK(s::validate_binding(m.value(),texture.value()));
  FakeFiles absent;CHECK(!p::cook(imported.value(),&absent));
  ReservedFiles oversized;oversized.data=files.data;CHECK_ERROR(p::cook(imported.value(),&oversized),ErrorCode::Exhausted);
  CHECK_ERROR(p::publish(oversized,cooked.value()),ErrorCode::Exhausted);
  const std::string plain="{\"base_color_texture\":null,\"base_color\":[0,-0,1],\"model\":\"opaque-diffuse\",\"schema\":1}";
  auto plain_import=p::import_material({5,10},bytes(plain));CHECK(plain_import);CHECK(p::cook(plain_import.value()));
  for(const std::string& invalid:std::array<std::string,6>{"P3\n2 1\n255\n123456","P6\n0 1\n255\n","P6\n1025 1\n255\n","P6\n2 1\n65535\n123456",source+"x",source.substr(0,source.size()-1)})
    CHECK(!p::import_ppm({4,8},bytes(invalid),s::Encoding::SrgbColor));
  for(const std::string value:{"NaN","1.",".5","01","+1","-0.1","1.1","1e","1e999"}){
    const auto invalid=std::string("{\"schema\":1,\"model\":\"opaque-diffuse\",\"base_color\":[")+value+",0,0],\"base_color_texture\":null}";
    CHECK(!p::import_material({5,9},bytes(invalid)));
  }
  CHECK(!p::import_material({5,9},bytes(plain+"x")));
  CHECK(!p::import_material({5,9},bytes("{\"schema\":1,\"schema\":1,\"base_color\":[1,1,1],\"base_color_texture\":null}")));
  CHECK(!p::import_material({5,9},bytes("{\"schema\":1,\"model\":\"metallic-roughness\",\"base_color\":[1,1,1],\"base_color_texture\":null}")));
  auto corrupt=cooked.value();corrupt.back()^=std::byte{1};CHECK(!p::publish(files,corrupt));
  auto linear=p::import_ppm({4,8},bytes(source),s::Encoding::LinearData);CHECK(linear);auto linear_cooked=p::cook(linear.value());CHECK(linear_cooked);
  CHECK(!s::validate_binding(m.value(),s::validate_texture(linear_cooked.value()).value()));
}
Result<std::vector<std::byte>> imported_texture(std::uint32_t width,std::uint32_t height,std::span<const std::byte> rgba,
                                                 s::Encoding encoding=s::Encoding::SrgbColor){
  s::Stamp stamp;stamp.source=assets::digest(rgba);stamp.settings=assets::digest(bytes("test source settings"));stamp.importer=assets::digest(bytes("test source importer"));
  return s::encode_texture({4,8},stamp,width,height,encoding,rgba);
}
void pixels_equal(std::span<const std::byte> actual,std::initializer_list<unsigned> expected){
  CHECK(actual.size()==expected.size());std::size_t at{};
  for(const auto value:expected)CHECK(std::to_integer<unsigned>(actual[at++])==value);
}
void bmp_put16(std::vector<std::byte>& source,std::size_t at,std::uint32_t value){
  source[at]=static_cast<std::byte>(value&255);source[at+1]=static_cast<std::byte>((value>>8)&255);
}
void bmp_put32(std::vector<std::byte>& source,std::size_t at,std::uint32_t value){
  bmp_put16(source,at,value);bmp_put16(source,at+2,value>>16);
}
std::vector<std::byte> bmp_source(std::uint32_t width,std::uint32_t height,std::span<const std::byte> rgb,
                                  bool top_down=false,bool declared_size=true){
  CHECK(rgb.size()==static_cast<std::size_t>(width)*height*3);
  const auto stride=(static_cast<std::size_t>(width)*3+3)&~std::size_t{3};
  std::vector<std::byte> result(54+stride*height,std::byte{});
  result[0]=std::byte{'B'};result[1]=std::byte{'M'};bmp_put32(result,2,static_cast<std::uint32_t>(result.size()));
  bmp_put32(result,10,54);bmp_put32(result,14,40);bmp_put32(result,18,width);bmp_put32(result,22,top_down?0U-height:height);
  bmp_put16(result,26,1);bmp_put16(result,28,24);bmp_put32(result,34,declared_size?static_cast<std::uint32_t>(stride*height):0);
  for(std::uint32_t y=0;y<height;++y){
    const auto row=54+stride*(top_down?y:height-1-y);
    for(std::uint32_t x=0;x<width;++x){
      const auto pixel=(static_cast<std::size_t>(y)*width+x)*3;
      result[row+x*3]=rgb[pixel+2];result[row+x*3+1]=rgb[pixel+1];result[row+x*3+2]=rgb[pixel];
    }
    for(auto at=row+static_cast<std::size_t>(width)*3;at<row+stride;++at)result[at]=std::byte{0xa5};
  }
  return result;
}
void texture_dispatch_compatibility(){
  const std::string legacy="P6\n# bounded header\n2 1\n255\n\n \tRGB";
  const std::vector<std::string> sources{
      legacy,"\n# leading comment\r\nP6\r\n1 1\r\n255\r\nRGB","", "B",
      "P3\n1 1\n255\nRGB","P6\n0 1\n255\n","P6\n1025 1\n255\n","P6\n1 1\n65535\nRGB",
      "P6\n-1 1\n255\nRGB","P6\n1 1\n255",legacy+"x",legacy.substr(0,legacy.size()-1),std::string(4096,' ')+"P6"};
  for(const auto& source:sources){
    for(const auto encoding:{s::Encoding::SrgbColor,s::Encoding::LinearData,static_cast<s::Encoding>(99)}){
      for(const auto id:std::array<assets::AssetId,2>{{{4,8},{}}}){
        auto before=p::import_ppm(id,bytes(source),encoding);auto after=p::import_texture(id,bytes(source),encoding);
        CHECK(static_cast<bool>(before)==static_cast<bool>(after));
        if(before)CHECK(before.value()==after.value());
        else{CHECK(before.error().code==after.error().code);CHECK(std::string_view(before.error().message)==after.error().message);}
      }
    }
  }
  auto imported=p::import_texture({4,8},bytes(legacy),s::Encoding::SrgbColor);CHECK(imported);
  CHECK(assets::hex(assets::digest(imported.value()))=="94e699afb8d993e35435c8473a23ad6df55fe1cc081f6202e8b0a2a2c15a293e");
  auto cooked=p::cook(imported.value());CHECK(cooked);
  CHECK(assets::hex(assets::digest(cooked.value()))=="1126bcc83615edc6375fa6dad4c1f463adb874095cb8aaf83865902cf1bfbc89");
}
void bmp_import(){
  const std::array<unsigned char,18> rgb{10,32,9,255,0,0,0,255,0,0,0,255,128,128,128,255,255,0};
  const auto raster=std::as_bytes(std::span(rgb));auto source=bmp_source(3,2,raster);
  // Independent Python struct.pack/hashlib BMP/native-wire vectors, not produced by this importer.
  CHECK(assets::hex(assets::digest(source))=="d29b18185dd4b637c9dc0869e59a9daa1f6d677fed63a6d6cf70ffd5575ec25a");
  CHECK(source.size()==78&&source[54]==std::byte{255}&&source[55]==std::byte{}&&source[56]==std::byte{});
  CHECK(source[63]==std::byte{0xa5}&&source[66]==std::byte{9}&&source[67]==std::byte{32}&&source[68]==std::byte{10});
  auto imported=p::import_texture({4,8},source,s::Encoding::SrgbColor);CHECK(imported);
  CHECK(p::import_texture({4,8},source,s::Encoding::SrgbColor).value()==imported.value());
  CHECK(assets::hex(assets::digest(imported.value()))=="28fa59931cc4742c089b4dffd976b942fee683f21697c76b0513a762547bfc15");
  auto view=s::validate_texture(imported.value());CHECK(view&&view.value().schema()==1&&view.value().mip_count()==1);
  CHECK(view.value().width()==3&&view.value().height()==2&&view.value().encoding()==s::Encoding::SrgbColor);
  pixels_equal(view.value().pixels(),{10,32,9,255,255,0,0,255,0,255,0,255,0,0,255,255,128,128,128,255,255,255,0,255});
  CHECK(view.value().stamp().source==assets::digest(source));
  CHECK(view.value().stamp().importer==assets::digest(bytes("race-bmp-bgr24-info40-importer-1")));
  CHECK(view.value().stamp().settings==assets::digest(bytes("race-bmp-1:rgba8;top-left;srgb;opaque")));
  CHECK(view.value().stamp().representation==s::Representation::Imported&&view.value().stamp().target==0);
  auto cooked=p::cook(imported.value());CHECK(cooked&&p::cook(imported.value()).value()==cooked.value());
  CHECK(assets::hex(assets::digest(cooked.value()))=="705aa4b2d6fa94634929b4d7da278c9f0236c8af8b2a2d82d53ae0c441e00957");
  auto mips=p::cook_mips(imported.value());CHECK(mips&&p::cook_mips(imported.value()).value()==mips.value());
  auto mip_view=s::validate_texture(mips.value());CHECK(mip_view&&mip_view.value().schema()==2&&mip_view.value().mip_count()==2&&mip_view.value().texels().size()==28);
  CHECK(mip_view.value().stamp().source==view.value().stamp().source&&mip_view.value().stamp().importer==view.value().stamp().importer);
  std::string ppm="P6\n3 2\n255\n";ppm.append(reinterpret_cast<const char*>(rgb.data()),rgb.size());
  auto ppm_import=p::import_ppm({4,8},bytes(ppm),s::Encoding::SrgbColor);CHECK(ppm_import&&ppm_import.value()!=imported.value());
  auto ppm_mips=p::cook_mips(ppm_import.value());CHECK(ppm_mips&&ppm_mips.value()!=mips.value());
  const auto ppm_pixels=s::validate_texture(ppm_import.value()).value().pixels();
  CHECK(std::equal(ppm_pixels.begin(),ppm_pixels.end(),view.value().pixels().begin(),view.value().pixels().end()));
  const auto ppm_texels=s::validate_texture(ppm_mips.value()).value().texels();
  CHECK(std::equal(ppm_texels.begin(),ppm_texels.end(),mip_view.value().texels().begin(),mip_view.value().texels().end()));
  for(const bool top_down:{false,true})for(const bool declared_size:{false,true}){
    auto variant=bmp_source(3,2,raster,top_down,declared_size);
    auto result=p::import_texture({4,8},variant,s::Encoding::SrgbColor);CHECK(result);auto decoded=s::validate_texture(result.value());CHECK(decoded);
    CHECK(std::equal(view.value().pixels().begin(),view.value().pixels().end(),decoded.value().pixels().begin(),decoded.value().pixels().end()));
    CHECK(decoded.value().stamp().source==assets::digest(variant));
    CHECK((result.value()==imported.value())==(!top_down&&declared_size));
  }
  for(const auto at:std::array<std::size_t,5>{38,42,63,64,77}){
    auto variant=source;variant[at]^=std::byte{1};auto result=p::import_texture({4,8},variant,s::Encoding::SrgbColor);CHECK(result&&result.value()!=imported.value());
    auto decoded=s::validate_texture(result.value());CHECK(decoded&&decoded.value().stamp().source==assets::digest(variant));
    CHECK(decoded.value().stamp().source!=view.value().stamp().source);
    CHECK(std::equal(view.value().pixels().begin(),view.value().pixels().end(),decoded.value().pixels().begin(),decoded.value().pixels().end()));
  }
  auto linear=p::import_texture({4,8},source,s::Encoding::LinearData);CHECK(linear&&linear.value()!=imported.value());
  auto linear_view=s::validate_texture(linear.value());CHECK(linear_view&&linear_view.value().encoding()==s::Encoding::LinearData);
  CHECK(linear_view.value().stamp().settings==assets::digest(bytes("race-bmp-1:rgba8;top-left;linear-data;opaque")));
  CHECK(p::cook(linear.value()));CHECK_ERROR(p::cook_mips(linear.value()),ErrorCode::IncompatibleVersion);
  // All row-padding lengths, including no padding; asymmetric pixels expose flips and channel swaps.
  for(std::uint32_t width=1;width<=4;++width){
    std::vector<std::byte> colors(static_cast<std::size_t>(width)*2*3);
    for(std::size_t at=0;at<colors.size();++at)colors[at]=static_cast<std::byte>(at*7+3);
    for(const bool top_down:{false,true}){
      auto input=bmp_source(width,2,colors,top_down);auto result=p::import_texture({4,8},input,s::Encoding::SrgbColor);CHECK(result);
      auto decoded=s::validate_texture(result.value());CHECK(decoded);
      for(std::size_t pixel=0;pixel<width*2;++pixel){
        for(std::size_t c=0;c<3;++c)CHECK(decoded.value().pixels()[pixel*4+c]==colors[pixel*3+c]);
        CHECK(decoded.value().pixels()[pixel*4+3]==std::byte{255});
      }
    }
  }
  for(const auto dimensions:std::array<std::array<std::uint32_t,2>,7>{{{1,1},{1,5},{5,1},{3,5},{1024,1},{1,1024},{1024,1024}}}){
    std::vector<std::byte> colors(static_cast<std::size_t>(dimensions[0])*dimensions[1]*3,std::byte{83});
    auto input=bmp_source(dimensions[0],dimensions[1],colors);auto result=p::import_texture({4,8},input,s::Encoding::SrgbColor);CHECK(result);
    auto decoded=s::validate_texture(result.value());CHECK(decoded&&decoded.value().width()==dimensions[0]&&decoded.value().height()==dimensions[1]);
    CHECK(decoded.value().pixels().size()==static_cast<std::size_t>(dimensions[0])*dimensions[1]*4);
    for(std::size_t at=0;at<decoded.value().pixels().size();++at)CHECK(decoded.value().pixels()[at]==(at%4==3?std::byte{255}:std::byte{83}));
  }
  // Two allocations only: one decoded raster, then one encoded native product.
  for(std::size_t step=0;step<2;++step){
    failure=step;auto result=p::import_texture({4,8},source,s::Encoding::SrgbColor);failure=SIZE_MAX;
    CHECK_ERROR(result,ErrorCode::Exhausted);CHECK(p::import_texture({4,8},source,s::Encoding::SrgbColor).value()==imported.value());
  }
  failure=2;auto bounded=p::import_texture({4,8},source,s::Encoding::SrgbColor);const auto remaining=failure;failure=SIZE_MAX;
  CHECK(bounded&&remaining==0&&bounded.value()==imported.value());
  FakeFiles files;auto leaf=p::publish(files,cooked.value());CHECK(leaf&&p::publish(files,cooked.value()).value()==leaf.value());
  const auto old_files=files.data;files.fail_write=files.writes;
  CHECK_ERROR(p::publish(files,mips.value()),ErrorCode::IoFailure);CHECK(files.data==old_files);
  CHECK(p::publish(files,mips.value()));
  std::fill(source.begin(),source.end(),std::byte{}); // Imported product owns all decoded bytes.
  pixels_equal(s::validate_texture(imported.value()).value().pixels(),{10,32,9,255,255,0,0,255,0,255,0,255,0,0,255,255,128,128,128,255,255,255,0,255});
}
void bmp_rejections(){
  const std::array<unsigned char,18> rgb{10,32,9,255,0,0,0,255,0,0,0,255,128,128,128,255,255,0};
  const auto source=bmp_source(3,2,std::as_bytes(std::span(rgb)));
  const auto reject=[&](std::span<const std::byte> input,ErrorCode code,std::string_view message){
    failure=0;auto result=p::import_texture({4,8},input,s::Encoding::SrgbColor);const auto untouched=failure;failure=SIZE_MAX;
    CHECK(!result&&result.error().code==code&&message==result.error().message);CHECK(untouched==0);
  };
  for(std::size_t length=2;length<source.size();++length)
    reject(std::span(source).first(length),ErrorCode::InvalidInput,length<54?"BMP header is truncated":"BMP declared file size differs from source");
  struct Field{std::size_t at;std::uint32_t value;bool short_field;ErrorCode code;const char* message;};
  const std::array fields{
    Field{6,1,true,ErrorCode::InvalidInput,"BMP reserved fields must be zero"},Field{8,1,true,ErrorCode::InvalidInput,"BMP reserved fields must be zero"},
    Field{2,0,false,ErrorCode::InvalidInput,"BMP declared file size differs from source"},Field{2,79,false,ErrorCode::InvalidInput,"BMP declared file size differs from source"},
    Field{14,12,false,ErrorCode::Unsupported,"BMP requires a 40-byte INFOHEADER"},Field{14,108,false,ErrorCode::Unsupported,"BMP requires a 40-byte INFOHEADER"},
    Field{14,124,false,ErrorCode::Unsupported,"BMP requires a 40-byte INFOHEADER"},Field{14,0xffffffffU,false,ErrorCode::Unsupported,"BMP requires a 40-byte INFOHEADER"},
    Field{10,53,false,ErrorCode::InvalidInput,"BMP pixel offset must be 54"},Field{10,55,false,ErrorCode::InvalidInput,"BMP pixel offset must be 54"},
    Field{10,0xffffffffU,false,ErrorCode::InvalidInput,"BMP pixel offset must be 54"},Field{26,0,true,ErrorCode::InvalidInput,"BMP planes must be one"},
    Field{26,2,true,ErrorCode::InvalidInput,"BMP planes must be one"},Field{28,8,true,ErrorCode::Unsupported,"BMP profile requires 24-bit pixels"},
    Field{28,16,true,ErrorCode::Unsupported,"BMP profile requires 24-bit pixels"},Field{28,32,true,ErrorCode::Unsupported,"BMP profile requires 24-bit pixels"},
    Field{30,1,false,ErrorCode::Unsupported,"BMP profile requires BI_RGB"},Field{30,2,false,ErrorCode::Unsupported,"BMP profile requires BI_RGB"},
    Field{30,3,false,ErrorCode::Unsupported,"BMP profile requires BI_RGB"},Field{30,5,false,ErrorCode::Unsupported,"BMP profile requires BI_RGB"},
    Field{46,1,false,ErrorCode::Unsupported,"BMP profile does not admit color tables"},Field{50,1,false,ErrorCode::Unsupported,"BMP profile does not admit color tables"},
    Field{18,0,false,ErrorCode::InvalidInput,"BMP width must be positive"},Field{18,0xffffffffU,false,ErrorCode::InvalidInput,"BMP width must be positive"},
    Field{18,0x80000000U,false,ErrorCode::InvalidInput,"BMP width must be positive"},Field{22,0,false,ErrorCode::InvalidInput,"BMP height must be nonzero"},
    Field{22,0x80000000U,false,ErrorCode::InvalidInput,"BMP height magnitude is invalid"},Field{18,1025,false,ErrorCode::Exhausted,"Texture dimension limit is 1024"},
    Field{18,0x7fffffffU,false,ErrorCode::Exhausted,"Texture dimension limit is 1024"},Field{22,1025,false,ErrorCode::Exhausted,"Texture dimension limit is 1024"},
    Field{22,0U-1025U,false,ErrorCode::Exhausted,"Texture dimension limit is 1024"},Field{22,0x7fffffffU,false,ErrorCode::Exhausted,"Texture dimension limit is 1024"},
    Field{34,23,false,ErrorCode::InvalidInput,"BMP declared image size differs from raster"},Field{34,25,false,ErrorCode::InvalidInput,"BMP declared image size differs from raster"}
  };
  for(const auto& field:fields){
    auto changed=source;if(field.short_field)bmp_put16(changed,field.at,field.value);else bmp_put32(changed,field.at,field.value);
    reject(changed,field.code,field.message);
  }
  for(std::size_t length=54;length<source.size();++length){
    auto truncated=source;truncated.resize(length);bmp_put32(truncated,2,static_cast<std::uint32_t>(length));
    reject(truncated,ErrorCode::InvalidInput,"BMP raster length mismatch or trailing data");
  }
  auto trailing=source;trailing.push_back(std::byte{});bmp_put32(trailing,2,static_cast<std::uint32_t>(trailing.size()));
  reject(trailing,ErrorCode::InvalidInput,"BMP raster length mismatch or trailing data");
  auto gap=source;gap.insert(gap.begin()+54,std::byte{});bmp_put32(gap,2,static_cast<std::uint32_t>(gap.size()));bmp_put32(gap,10,55);
  reject(gap,ErrorCode::InvalidInput,"BMP pixel offset must be 54");
  std::vector<std::byte> oversized(p::source_bytes+1,std::byte{});oversized[0]=std::byte{'B'};oversized[1]=std::byte{'M'};
  reject(oversized,ErrorCode::Exhausted,"BMP source byte limit");
  CHECK_ERROR(p::import_texture({},source,s::Encoding::SrgbColor),ErrorCode::InvalidInput);
  CHECK_ERROR(p::import_texture({4,8},source,static_cast<s::Encoding>(99)),ErrorCode::Unsupported);
  auto signature=source;signature[0]=std::byte{'X'};
  auto before=p::import_ppm({4,8},signature,s::Encoding::SrgbColor);auto after=p::import_texture({4,8},signature,s::Encoding::SrgbColor);
  CHECK(!before&&!after&&before.error().code==after.error().code&&std::string_view(before.error().message)==after.error().message);
}
void mip_numerics(){
  const std::array<std::byte,16> checker{std::byte{},std::byte{},std::byte{},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255},
      std::byte{},std::byte{},std::byte{},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  auto imported=imported_texture(2,2,checker);CHECK(imported);auto cooked=p::cook_mips(imported.value());CHECK(cooked);
  auto view=s::validate_texture(cooked.value());CHECK(view&&view.value().schema()==2&&view.value().mip_count()==2);
  // Continuous-sRGB, high-precision Decimal oracle: gamma-byte averaging gives 128 and is wrong.
  pixels_equal(view.value().mip(1)->pixels,{188,188,188,255});
  CHECK(std::equal(checker.begin(),checker.end(),view.value().pixels().begin(),view.value().pixels().end()));
  CHECK(p::cook_mips(imported.value()).value()==cooked.value());
  auto legacy=p::cook(imported.value());CHECK(legacy&&legacy.value()!=cooked.value());
  auto legacy_view=s::validate_texture(legacy.value());CHECK(legacy_view&&legacy_view.value().schema()==1&&legacy_view.value().mip_count()==1);
  CHECK(view.value().stamp().source==legacy_view.value().stamp().source&&view.value().stamp().importer==legacy_view.value().stamp().importer);
  CHECK(view.value().stamp().settings!=legacy_view.value().stamp().settings&&view.value().stamp().cooker!=legacy_view.value().stamp().cooker);
  CHECK(!p::cook_mips(cooked.value())&&!p::cook_mips(legacy.value())&&!p::cook(cooked.value()));
  std::vector<std::byte> odd;
  for(unsigned y=0;y<5;++y)for(unsigned x=0;x<3;++x){
    odd.push_back(x==2?std::byte{255}:std::byte{});odd.push_back(y==4?std::byte{255}:std::byte{});
    odd.push_back((x+y)%2?std::byte{255}:std::byte{});odd.push_back(std::byte{255});
  }
  auto odd_import=imported_texture(3,5,odd);CHECK(odd_import);auto odd_product=p::cook_mips(odd_import.value());CHECK(odd_product);
  auto odd_view=s::validate_texture(odd_product.value());CHECK(odd_view&&odd_view.value().mip_count()==3);
  // Independent rational-area / Decimal oracle includes the entire last row and column.
  pixels_equal(odd_view.value().mip(1)->pixels,{156,0,182,255,156,170,182,255});
  pixels_equal(odd_view.value().mip(2)->pixels,{156,124,182,255});
  std::vector<std::byte> line(20,std::byte{});
  for(std::size_t at=3;at<line.size();at+=4)line[at]=std::byte{255};
  std::fill(line.end()-4,line.end(),std::byte{255});
  for(const auto dimensions:std::array<std::array<std::uint32_t,2>,2>{{{1,5},{5,1}}}){
    auto input=imported_texture(dimensions[0],dimensions[1],line);CHECK(input);auto product=p::cook_mips(input.value());CHECK(product);
    auto decoded=s::validate_texture(product.value());CHECK(decoded&&decoded.value().mip_count()==3);
    pixels_equal(decoded.value().mip(1)->pixels,{0,0,0,255,170,170,170,255});
    pixels_equal(decoded.value().mip(2)->pixels,{124,124,124,255});
  }
  // Every constant encoded byte must survive decode/average/encode exactly.
  for(unsigned code=0;code<256;++code){
    std::array<std::byte,8> solid{};solid.fill(static_cast<std::byte>(code));solid[3]=solid[7]=std::byte{255};
    auto input=imported_texture(1,2,solid);CHECK(input);auto product=p::cook_mips(input.value());CHECK(product);
    auto decoded=s::validate_texture(product.value());CHECK(decoded);pixels_equal(decoded.value().mip(1)->pixels,{code,code,code,255});
  }
  const std::array<std::byte,4> pixel{std::byte{17},std::byte{83},std::byte{241},std::byte{255}};
  auto one=imported_texture(1,1,pixel);CHECK(one);auto one_cooked=p::cook_mips(one.value());CHECK(one_cooked);
  auto one_view=s::validate_texture(one_cooked.value());CHECK(one_view&&one_view.value().schema()==2&&one_view.value().mip_count()==1);
  pixels_equal(one_view.value().texels(),{17,83,241,255});
  auto linear=imported_texture(2,2,checker,s::Encoding::LinearData);CHECK(linear);
  CHECK_ERROR(p::cook_mips(linear.value()),ErrorCode::IncompatibleVersion);
  auto alpha=checker;alpha[15]=std::byte{254};auto transparent=imported_texture(2,2,alpha);CHECK(transparent);
  CHECK_ERROR(p::cook_mips(transparent.value()),ErrorCode::Unsupported);
  for(std::size_t step=0;step<2;++step){
    failure=step;auto result=p::cook_mips(imported.value());failure=SIZE_MAX;
    CHECK_ERROR(result,ErrorCode::Exhausted);CHECK(p::cook_mips(imported.value()).value()==cooked.value());
  }
  failure=2;auto bounded=p::cook_mips(imported.value());const auto remaining=failure;failure=SIZE_MAX;
  CHECK(bounded&&remaining==0); // One chain scratch and one native output, no per-level allocations.
  std::vector<std::byte> maximum(1024U*1024U*4U,std::byte{255});
  auto max_import=imported_texture(1024,1024,maximum);CHECK(max_import);
  auto max_product=p::cook_mips(max_import.value());CHECK(max_product&&max_product.value().size()==5592692);
  auto max_view=s::validate_texture(max_product.value());CHECK(max_view&&max_view.value().mip_count()==11);
  pixels_equal(max_view.value().mip(10)->pixels,{255,255,255,255});
}
void scalar_mip_numerics(){
  const std::array<std::byte,16> checker{std::byte{},std::byte{},std::byte{},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255},
      std::byte{},std::byte{},std::byte{},std::byte{255},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
  auto imported=imported_texture(2,2,checker,s::Encoding::LinearData);CHECK(imported);auto cooked=p::cook_scalar_mips(imported.value());CHECK(cooked);
  auto view=s::validate_texture(cooked.value());CHECK(view&&view.value().schema()==3&&view.value().encoding()==s::Encoding::LinearData&&view.value().mip_count()==2);
  // Stored scalar values average to 128; the unchanged sRGB profile averages to 188.
  pixels_equal(view.value().mip(1)->pixels,{128,128,128,255});
  CHECK(std::equal(checker.begin(),checker.end(),view.value().pixels().begin(),view.value().pixels().end()));
  CHECK(p::cook_scalar_mips(imported.value()).value()==cooked.value());
  auto old=p::cook(imported.value());CHECK(old&&old.value()!=cooked.value());auto old_view=s::validate_texture(old.value());CHECK(old_view&&old_view.value().schema()==1);
  CHECK(view.value().stamp().source==old_view.value().stamp().source&&view.value().stamp().importer==old_view.value().stamp().importer);
  CHECK(view.value().stamp().settings==assets::digest(bytes("race-rgba8-scalar-mips-1:top-left;opaque-linear-data;independent-rgb;area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-half-up")));
  CHECK(view.value().stamp().cooker==assets::digest(bytes("race-rgba8-portable-cpu-scalar-mips-cooker-1")));
  CHECK(!p::cook_scalar_mips(cooked.value())&&!p::cook_scalar_mips(old.value())&&!p::cook(cooked.value()));
  CHECK_ERROR(p::cook_mips(imported.value()),ErrorCode::IncompatibleVersion);
  auto color=imported_texture(2,2,checker);CHECK(color);CHECK_ERROR(p::cook_scalar_mips(color.value()),ErrorCode::IncompatibleVersion);
  auto color_mips=p::cook_mips(color.value());CHECK(color_mips);pixels_equal(s::validate_texture(color_mips.value()).value().mip(1)->pixels,{188,188,188,255});
  std::vector<std::byte> odd;
  for(unsigned y=0;y<5;++y)for(unsigned x=0;x<3;++x){
    odd.push_back(x==2?std::byte{255}:std::byte{});odd.push_back(y==4?std::byte{255}:std::byte{});
    odd.push_back((x+y)%2?std::byte{255}:std::byte{});odd.push_back(std::byte{255});
  }
  auto odd_input=imported_texture(3,5,odd,s::Encoding::LinearData);CHECK(odd_input);auto odd_product=p::cook_scalar_mips(odd_input.value());CHECK(odd_product);
  auto odd_view=s::validate_texture(odd_product.value());CHECK(odd_view&&odd_view.value().mip_count()==3);
  // Independent Python Fraction pixel-box intersections; each scalar channel has different edge coverage.
  pixels_equal(odd_view.value().mip(1)->pixels,{85,0,119,255,85,102,119,255});
  pixels_equal(odd_view.value().mip(2)->pixels,{85,51,119,255});
  std::vector<std::byte> line(20,std::byte{});for(std::size_t at=3;at<line.size();at+=4)line[at]=std::byte{255};std::fill(line.end()-4,line.end(),std::byte{255});
  for(const auto dimensions:std::array<std::array<std::uint32_t,2>,2>{{{1,5},{5,1}}}){
    auto input=imported_texture(dimensions[0],dimensions[1],line,s::Encoding::LinearData);CHECK(input);auto product=p::cook_scalar_mips(input.value());CHECK(product);
    auto decoded=s::validate_texture(product.value());CHECK(decoded&&decoded.value().mip_count()==3);
    pixels_equal(decoded.value().mip(1)->pixels,{0,0,0,255,102,102,102,255});pixels_equal(decoded.value().mip(2)->pixels,{51,51,51,255});
  }
  const std::array<std::byte,8> halves{std::byte{},std::byte{2},std::byte{254},std::byte{255},std::byte{1},std::byte{3},std::byte{255},std::byte{255}};
  auto half_input=imported_texture(2,1,halves,s::Encoding::LinearData);CHECK(half_input);auto half_product=p::cook_scalar_mips(half_input.value());CHECK(half_product);
  pixels_equal(s::validate_texture(half_product.value()).value().mip(1)->pixels,{1,3,255,255});
  // Per-level quantization is recursive: averaging the original five red values directly would give zero.
  std::array<std::byte,20> recursive{};const std::array<unsigned,5> red{0,0,1,0,1};
  for(std::size_t i=0;i<red.size();++i){recursive[i*4]=std::byte(red[i]);recursive[i*4+1]=std::byte(255-red[i]);recursive[i*4+2]=std::byte{73};recursive[i*4+3]=std::byte{255};}
  auto recursive_input=imported_texture(5,1,recursive,s::Encoding::LinearData);CHECK(recursive_input);auto recursive_product=p::cook_scalar_mips(recursive_input.value());CHECK(recursive_product);
  auto recursive_view=s::validate_texture(recursive_product.value());CHECK(recursive_view);
  pixels_equal(recursive_view.value().mip(1)->pixels,{0,255,73,255,1,254,73,255});pixels_equal(recursive_view.value().mip(2)->pixels,{1,255,73,255});
  // Exercise every code without allowing RGB swizzles or cross-channel averaging.
  for(unsigned code=0;code<256;++code){
    std::array<std::byte,8> solid{};for(unsigned i=0;i<2;++i){solid[i*4]=std::byte(code);solid[i*4+1]=std::byte(255-code);solid[i*4+2]=std::byte((code+73)%256);solid[i*4+3]=std::byte{255};}
    auto input=imported_texture(1,2,solid,s::Encoding::LinearData);CHECK(input);auto product=p::cook_scalar_mips(input.value());CHECK(product);
    pixels_equal(s::validate_texture(product.value()).value().mip(1)->pixels,{code,255-code,(code+73)%256,255});
  }
  const std::array<std::byte,4> pixel{std::byte{17},std::byte{83},std::byte{241},std::byte{255}};
  auto one=imported_texture(1,1,pixel,s::Encoding::LinearData);CHECK(one);auto one_cooked=p::cook_scalar_mips(one.value());CHECK(one_cooked);
  auto one_view=s::validate_texture(one_cooked.value());CHECK(one_view&&one_view.value().schema()==3&&one_view.value().mip_count()==1);pixels_equal(one_view.value().texels(),{17,83,241,255});
  auto alpha=checker;alpha[15]=std::byte{254};auto transparent=imported_texture(2,2,alpha,s::Encoding::LinearData);CHECK(transparent);
  CHECK_ERROR(p::cook_scalar_mips(transparent.value()),ErrorCode::Unsupported);
  for(std::size_t step=0;step<2;++step){failure=step;auto result=p::cook_scalar_mips(imported.value());failure=SIZE_MAX;CHECK_ERROR(result,ErrorCode::Exhausted);CHECK(p::cook_scalar_mips(imported.value()).value()==cooked.value());}
  failure=2;auto bounded=p::cook_scalar_mips(imported.value());const auto remaining=failure;failure=SIZE_MAX;CHECK(bounded&&remaining==0);
  std::vector<std::byte> maximum(1024U*1024U*4U,std::byte{255});auto max_input=imported_texture(1024,1024,maximum,s::Encoding::LinearData);CHECK(max_input);
  auto max_product=p::cook_scalar_mips(max_input.value());CHECK(max_product&&max_product.value().size()==5592692);auto max_view=s::validate_texture(max_product.value());
  CHECK(max_view&&max_view.value().mip_count()==11);pixels_equal(max_view.value().mip(10)->pixels,{255,255,255,255});
}
struct CorruptReadbackFiles:FakeFiles{
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{
    auto result=FakeFiles::read(leaf,limit);if(result&&!result.value().empty())result.value().back()^=std::byte{1};return result;
  }
};
void scalar_material_authoring(){
  const std::array<std::byte,4> pixel{std::byte{20},std::byte{40},std::byte{80},std::byte{255}};
  auto texture_import=imported_texture(1,1,pixel,s::Encoding::LinearData);CHECK(texture_import);
  auto texture=p::cook_scalar_mips(texture_import.value());CHECK(texture);auto tv=s::validate_texture(texture.value());CHECK(tv);
  FakeFiles files;CHECK(p::publish(files,texture.value()));
  const auto reference=std::string("{\"asset\":\"")+assets::id_text(tv.value().id())+"\",\"revision\":\""+assets::hex(tv.value().revision())+"\"}";
  const auto source=std::string("{\"schema\":3,\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,1e-1],\"base_color_texture\":")+
      reference+",\"sampling\":\"linear-clamp-mips\",\"texture_role\":\"diffuse-reflectance-r\"}";
  auto imported=p::import_material({5,9},bytes(source));CHECK(imported);auto iv=s::validate_material(imported.value());CHECK(iv);
  CHECK(iv.value().schema()==3&&iv.value().material().texture_role==s::TextureRole::DiffuseReflectanceR);
  const auto info=p::profile_info(p::Profile::MaterialScalarMips);CHECK(info&&info.value().material&&info.value().mips);
  CHECK(info.value().encoding==s::Encoding::LinearData&&info.value().imported_schema==3&&info.value().cooked_schema==3);
  CHECK(iv.value().stamp().importer==assets::digest(bytes("race-material-json-importer-3")));
  CHECK(iv.value().stamp().settings==assets::digest(bytes("race-opaque-diffuse-3:linear-rgb;linear-clamp-mips;complete-cooked-linear-scalar-texture;diffuse-reflectance-r")));
  const auto retained=files.data;const auto writes=files.writes;auto cooked=p::cook(imported.value(),&files);CHECK(cooked);
  CHECK(files.data==retained&&files.writes==writes);auto cv=s::validate_material(cooked.value());CHECK(cv&&cv.value().schema()==3);
  CHECK(cv.value().material()==iv.value().material()&&s::validate_binding(cv.value(),tv.value()));
  CHECK(cv.value().stamp().cooker==assets::digest(bytes("race-opaque-diffuse-portable-cpu-cooker-3:diffuse-reflectance-r;linear-clamp-mips")));
  CHECK(p::cook(imported.value(),&files).value()==cooked.value());CHECK(!p::cook(cooked.value(),&files));
  CHECK_ERROR(p::cook(imported.value()),ErrorCode::InvalidInput);FakeFiles absent;CHECK_ERROR(p::cook(imported.value(),&absent),ErrorCode::NotFound);
  const auto changed=[&](std::string_view from,std::string_view to){auto value=source;const auto at=value.find(from);CHECK(at!=value.npos);value.replace(at,from.size(),to);return value;};
  for(const auto& invalid:std::vector<std::string>{changed("\"schema\":3","\"schema\":2"),changed("\"schema\":3","\"schema\":1"),
      changed(",\"texture_role\":\"diffuse-reflectance-r\"",""),changed(",\"sampling\":\"linear-clamp-mips\"",""),
      changed("\"diffuse-reflectance-r\"","\"color\""),changed("\"diffuse-reflectance-r\"","\"diffuse-reflectance-g\""),
      changed("\"diffuse-reflectance-r\"","2"),changed(reference,"null"),changed("linear-clamp-mips","linear-clamp-mip0"),
      source.substr(0,source.size()-1)+",\"texture_role\":\"diffuse-reflectance-r\"}",source+"x"})
    CHECK(!p::import_material({5,9},bytes(invalid)));
  const auto reordered=std::string("{\"texture_role\":\"diffuse-reflectance-r\",\"sampling\":\"linear-clamp-mips\",\"base_color_texture\":")+
      reference+",\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,0.1],\"schema\":3}";
  CHECK(p::import_material({5,9},bytes(reordered)));
  auto old=p::cook(texture_import.value());CHECK(old);CHECK(p::publish(files,old.value()));
  auto wrong=p::import_material({5,9},bytes(changed(assets::hex(tv.value().revision()),assets::hex(assets::digest(old.value())))));CHECK(wrong);
  const auto before=files.data;CHECK_ERROR(p::cook(wrong.value(),&files),ErrorCode::IncompatibleVersion);CHECK(files.data==before);
  failure=0;auto failed=p::import_material({5,9},bytes(source));failure=SIZE_MAX;CHECK_ERROR(failed,ErrorCode::Exhausted);
  std::size_t failures{};bool recovered=false;
  for(std::size_t step=0;step<12&&!recovered;++step){
    failure=step;auto result=p::cook(imported.value(),&files);failure=SIZE_MAX;
    if(result){recovered=true;CHECK(result.value()==cooked.value());}else{CHECK_ERROR(result,ErrorCode::Exhausted);++failures;CHECK(p::cook(imported.value(),&files));}
    CHECK(files.data==before);
  }
  CHECK(recovered&&failures>=3);
}
void mip_material_authoring(){
  const std::array<std::byte,4> pixel{std::byte{20},std::byte{40},std::byte{80},std::byte{255}};
  auto texture_import=imported_texture(1,1,pixel);CHECK(texture_import);
  auto texture=p::cook_mips(texture_import.value());CHECK(texture);auto texture_view=s::validate_texture(texture.value());CHECK(texture_view);
  FakeFiles files;auto texture_leaf=p::publish(files,texture.value());CHECK(texture_leaf);
  CHECK(p::publish(files,texture.value()).value()==texture_leaf.value());
  const auto reference=std::string("{\"asset\":\"")+assets::id_text(texture_view.value().id())+"\",\"revision\":\""+assets::hex(texture_view.value().revision())+"\"}";
  const auto definition=std::string("{\"schema\":2,\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,1e-1],\"base_color_texture\":")+reference+",\"sampling\":\"linear-clamp-mips\"}";
  auto imported=p::import_material({5,9},bytes(definition));CHECK(imported);
  auto imported_view=s::validate_material(imported.value());CHECK(imported_view&&imported_view.value().schema()==2);
  CHECK(imported_view.value().material().sampling==s::Sampling::LinearClampMips);
  CHECK(p::import_material({5,9},bytes(definition)).value()==imported.value());
  CHECK(!p::cook_mips(imported.value())&&!p::cook(imported.value()));
  auto product=p::cook(imported.value(),&files);CHECK(product);auto view=s::validate_material(product.value());CHECK(view&&view.value().schema()==2);
  CHECK(s::validate_binding(view.value(),texture_view.value()));
  CHECK(p::cook(imported.value(),&files).value()==product.value());CHECK(p::publish(files,product.value()));
  const auto reordered=std::string("{\"sampling\":\"linear-clamp-mips\",\"base_color_texture\":")+reference+",\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,0.1],\"schema\":2}";
  CHECK(p::import_material({5,9},bytes(reordered)));
  const auto legacy_definition=std::string("{\"schema\":1,\"model\":\"opaque-diffuse\",\"base_color\":[1,0.25,1e-1],\"base_color_texture\":")+reference+"}";
  auto legacy=p::import_material({5,9},bytes(legacy_definition));CHECK(legacy&&p::cook(legacy.value(),&files));
  auto legacy_view=s::validate_material(legacy.value());CHECK(legacy_view);
  CHECK(imported_view.value().stamp().settings!=legacy_view.value().stamp().settings);
  CHECK(imported_view.value().stamp().importer!=legacy_view.value().stamp().importer);
  CHECK(product.value()!=p::cook(legacy.value(),&files).value());
  const auto substitute=[&](std::string_view from,std::string_view to){
    auto changed=definition;const auto at=changed.find(from);CHECK(at!=std::string::npos);changed.replace(at,from.size(),to);return changed;
  };
  for(const auto& invalid:std::vector<std::string>{
      substitute("\"schema\":2","\"schema\":1"),substitute("\"schema\":2","\"schema\":3"),
      substitute(",\"sampling\":\"linear-clamp-mips\"",""),
      substitute("\"linear-clamp-mips\"","\"linear-clamp-mip0\""),
      substitute("\"linear-clamp-mips\"","\"nearest\""),
      substitute("\"linear-clamp-mips\"","2"),
      substitute("\"sampling\"","\"filter\""),
      substitute(reference,"null"),
      definition.substr(0,definition.size()-1)+",\"sampling\":\"linear-clamp-mips\"}",
      definition.substr(0,definition.size()-1)+",\"schema\":2}",
      definition+"x"}){
    CHECK(!p::import_material({5,9},bytes(invalid)));
  }
  auto old_texture=p::cook(texture_import.value());CHECK(old_texture&&p::publish(files,old_texture.value()));
  const auto old_revision=assets::hex(assets::digest(old_texture.value()));
  auto wrong_definition=definition;const auto pin_at=wrong_definition.find(assets::hex(texture_view.value().revision()));CHECK(pin_at!=std::string::npos);
  wrong_definition.replace(pin_at,64,old_revision);
  auto wrong=p::import_material({5,9},bytes(wrong_definition));CHECK(wrong);
  CHECK_ERROR(p::cook(wrong.value(),&files),ErrorCode::IncompatibleVersion); // Exact schema-1 pin cannot be upgraded implicitly.
  FakeFiles absent;CHECK(!p::cook(imported.value(),&absent));
  ReservedFiles oversized;oversized.data=files.data;CHECK_ERROR(p::cook(imported.value(),&oversized),ErrorCode::Exhausted);
  CHECK_ERROR(p::publish(oversized,texture.value()),ErrorCode::Exhausted);
  CorruptReadbackFiles corrupt;CHECK_ERROR(p::publish(corrupt,texture.value()),ErrorCode::InvalidInput);
  FakeFiles interrupted;interrupted.fail_write=0;CHECK_ERROR(p::publish(interrupted,texture.value()),ErrorCode::IoFailure);
  CHECK(p::publish(interrupted,texture.value()).value()==texture_leaf.value());
  FakeFiles allocation;allocation.throw_read=true;CHECK_ERROR(p::publish(allocation,texture.value()),ErrorCode::Exhausted);
  allocation.throw_read=false;CHECK(p::publish(allocation,texture.value()).value()==texture_leaf.value());
  failure=0;auto failed_import=p::import_material({5,9},bytes(definition));failure=SIZE_MAX;CHECK_ERROR(failed_import,ErrorCode::Exhausted);
  CHECK(p::import_material({5,9},bytes(definition)));
  std::size_t failures{};bool recovered=false;
  for(std::size_t step=0;step<12&&!recovered;++step){
    failure=step;auto result=p::cook(imported.value(),&files);failure=SIZE_MAX;
    if(result)recovered=true;else{CHECK_ERROR(result,ErrorCode::Exhausted);++failures;CHECK(p::cook(imported.value(),&files));}
  }
  CHECK(recovered&&failures>=3);
}
}
int main(){try{run();texture_dispatch_compatibility();bmp_import();bmp_rejections();mip_numerics();scalar_mip_numerics();mip_material_authoring();scalar_material_authoring();std::puts("surface pipeline: legacy goldens, BMP24 import/provenance, opaque-sRGB mip numerics, strict profiles, immutable publication and allocation recovery passed");return 0;}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
