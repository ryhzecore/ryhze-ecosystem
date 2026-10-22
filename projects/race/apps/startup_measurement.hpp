#pragma once
// Private opt-in desktop measurement witness. No renderer or engine API changes.
#include "race/build_identity.hpp"
#include <Windows.h>
// Do not leak the Windows SDK's legacy qualifier into scene::near declarations.
#ifdef near
#undef near
#endif
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace race::desktop::startup {
inline std::int64_t ticks() {
  LARGE_INTEGER value{};
  if (!QueryPerformanceCounter(&value)) throw std::runtime_error("startup QPC unavailable");
  return value.QuadPart;
}
struct EntryStages {
  std::int64_t main_qpc{ticks()};
  std::int64_t qapplication_qpc{}, style_qpc{}, arguments_qpc{};
  void qapplication_ready() { qapplication_qpc = ticks(); }
  void style_ready() { style_qpc = ticks(); }
  void arguments_ready() { arguments_qpc = ticks(); }
};
inline std::string quoted(std::string_view text) {
  std::string result{"\""};
  for (unsigned char c : text) {
    if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
    else if (c < 32) throw std::runtime_error("startup metadata contains control character");
    else result += static_cast<char>(c);
  }
  return result + '"';
}
class Witness {
  HANDLE file_{INVALID_HANDLE_VALUE};
  bool presented_{}, responsive_{}, settled_{}, ready_{};
  bool visible_{};
  std::uint32_t width_{}, height_{};
  std::uint64_t epoch_{};
  unsigned stable_{};
  void write(std::string_view line) {
    DWORD written{};
    if (line.size() > 8192 || !WriteFile(file_, line.data(), static_cast<DWORD>(line.size()),
                                        &written, nullptr) || written != line.size())
      throw std::runtime_error("startup witness write failed");
  }
public:
  explicit Witness(const std::filesystem::path& directory, std::string_view lane,
                   const EntryStages* entry = nullptr) {
    const auto path = directory / "ready-events.jsonl";
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) throw std::runtime_error("startup witness needs fresh output");
    try {
      LARGE_INTEGER frequency{};
      if (!QueryPerformanceFrequency(&frequency)) throw std::runtime_error("startup QPC frequency unavailable");
      auto fields = ",\"schema\":1,\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"frequency\":" + std::to_string(frequency.QuadPart) + ",\"lane\":" + quoted(lane) +
        ",\"source_commit\":" + quoted(build::source_commit) +
        ",\"authored_input_sha256\":" + quoted(build::authored_input_sha256);
      if (entry) {
        const auto ready_qpc = ticks();
        fields += ",\"qapplication_qpc\":" + std::to_string(entry->qapplication_qpc) +
          ",\"style_qpc\":" + std::to_string(entry->style_qpc) +
          ",\"arguments_qpc\":" + std::to_string(entry->arguments_qpc) +
          ",\"witness_ready_qpc\":" + std::to_string(ready_qpc);
        event_at("application_entry", entry->main_qpc, fields);
      } else event("application_entry", fields);
    } catch (...) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; throw; }
  }
  ~Witness() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
  Witness(const Witness&) = delete;
  Witness& operator=(const Witness&) = delete;
  void event_at(std::string_view name, std::int64_t qpc, std::string_view fields = {}) {
    write("{\"event\":" + quoted(name) + ",\"qpc\":" + std::to_string(qpc) +
          std::string(fields) + "}\n");
  }
  void event(std::string_view name, std::string_view fields = {}) { event_at(name, ticks(), fields); }
  void turn() {
    if (presented_ && !responsive_) { responsive_ = true; event("responsive_event_turn"); }
  }
  void adopted(std::size_t objects) {
    if (objects) throw std::runtime_error("startup Scene is not empty");
    event("scene_adopted", ",\"objects\":0");
  }
  void visible(HWND window) {
    if (!IsWindowVisible(window) || IsIconic(window))
      throw std::runtime_error("startup window is hidden or minimized");
    if (!visible_) { visible_ = true; event("window_visible"); }
  }
  void frame(std::size_t objects, std::uint32_t width, std::uint32_t height,
             std::uint64_t epoch, bool hardware, std::uint64_t errors,
             std::uint64_t warnings, std::string_view adapter) {
    if (objects || width != 1280 || height != 720 || !hardware || errors || warnings)
      throw std::runtime_error("startup empty/hardware/frame contract failed");
    if (ready_ && (width != width_ || height != height_ || epoch != epoch_))
      throw std::runtime_error("startup surface changed after ready");
    if (!presented_) {
      event("first_present_api_return", ",\"objects\":0,\"width\":" + std::to_string(width) +
        ",\"height\":" + std::to_string(height) + ",\"epoch\":" + std::to_string(epoch) +
        ",\"adapter\":" + quoted(adapter));
      presented_ = true;
    }
    stable_ = width == width_ && height == height_ && epoch == epoch_ ? stable_ + 1 : 1;
    width_ = width; height_ = height; epoch_ = epoch;
    if (stable_ >= 2 && !settled_) {
      settled_ = true;
      event("geometry_settled", ",\"stable_frames\":" + std::to_string(stable_) +
        ",\"width\":" + std::to_string(width_) + ",\"height\":" + std::to_string(height_) +
        ",\"epoch\":" + std::to_string(epoch_));
    }
    if (settled_ && responsive_ && !ready_) { ready_ = true; event("ready"); }
  }
};
} // namespace race::desktop::startup
