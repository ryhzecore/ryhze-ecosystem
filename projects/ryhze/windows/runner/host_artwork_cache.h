#ifndef RUNNER_HOST_ARTWORK_CACHE_H_
#define RUNNER_HOST_ARTWORK_CACHE_H_
#include <windows.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include "artwork_cache.h"

class HostArtworkCache {
 public:
  static constexpr UINT kMessage = WM_APP + 0x52c;
  explicit HostArtworkCache(std::optional<std::wstring> private_parent = std::nullopt);
  ~HostArtworkCache();
  void Attach(flutter::BinaryMessenger* messenger, HWND window);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
 private:
  using Result = flutter::MethodResult<flutter::EncodableValue>;
  struct Request;
  struct WorkerState;
  std::optional<std::wstring> parent_;
  HWND window_ = nullptr;
  HANDLE worker_ = nullptr;
  ULONGLONG deadline_ = 0;
  bool attached_ = false;
  size_t queued_bytes_ = 0;
  std::shared_ptr<ryhze::ArtworkCache> cache_;
  std::shared_ptr<WorkerState> state_;
  std::unique_ptr<Result> pending_;
  std::deque<std::unique_ptr<Request>> queue_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  void StartNext();
  void Poll();
  static unsigned __stdcall Run(void* context);
};
#endif
