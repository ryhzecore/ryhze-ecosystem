#include "runtime_common.hpp"

namespace race_runtime {
fs::path from_utf8(std::string_view value) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}
std::string utf8(const fs::path &value) {
  const auto bytes = value.u8string();
  return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}
std::string json_string(std::string_view value) {
  std::ostringstream stream;
  stream << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      stream << '\\' << static_cast<char>(c);
    else if (c < 32)
      stream << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(c)
             << std::dec;
    else
      stream << static_cast<char>(c);
  }
  stream << '"';
  return stream.str();
}
race::render::RenderFrame copy_frame(race::desktop::Document &document, const race::desktop::MeshSet *meshes,
                                     const Camera &camera, std::uint64_t tick) {
  return take(race::desktop::scene_frame(document, meshes, camera.view(), true, tick));
}

} // namespace race_runtime
