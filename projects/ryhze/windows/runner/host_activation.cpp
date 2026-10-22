#include "host_activation.h"

#include <bcrypt.h>
#include <process.h>
#include <sddl.h>

#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
constexpr DWORD kIoTimeout = 2000;
constexpr DWORD kShutdownTimeout = 3000;
constexpr size_t kMaxUri = 4096;
constexpr size_t kHeaderSize = 24;
constexpr size_t kReplayLimit = 64;
constexpr ULONGLONG kReplayLifetime = 30000;
constexpr uint8_t kAccepted = 1;
constexpr uint8_t kRejected = 2;
constexpr uint8_t kBusy = 3;
constexpr uint8_t kMagic[] = {'R', 'Y', 'A', 1};

class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() { reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }
  HANDLE get() const { return value_; }
  explicit operator bool() const {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  HANDLE release() {
    HANDLE value = value_;
    value_ = nullptr;
    return value;
  }
  void reset(HANDLE value = nullptr) {
    if (*this) CloseHandle(value_);
    value_ = value;
  }

 private:
  HANDLE value_;
};

struct SecurityDescriptor {
  PSECURITY_DESCRIPTOR value = nullptr;
  ~SecurityDescriptor() { if (value) LocalFree(value); }
  SecurityDescriptor() = default;
  SecurityDescriptor(const SecurityDescriptor&) = delete;
  SecurityDescriptor& operator=(const SecurityDescriptor&) = delete;
};

struct Identity {
  std::wstring user;
  std::wstring logon;
  DWORD session = 0;
  std::wstring image_path;
  FILE_ID_INFO image_id{};
  // Deny writes/deletion for the lifetime of the endpoint. A path match alone
  // would not identify the executable file from which this process was loaded.
  Handle image;
};

bool TokenBytes(HANDLE token, TOKEN_INFORMATION_CLASS kind,
                std::vector<uint8_t>& bytes) {
  DWORD needed = 0;
  GetTokenInformation(token, kind, nullptr, 0, &needed);
  if (needed == 0 || needed > 65536) return false;
  bytes.resize(needed);
  return GetTokenInformation(token, kind, bytes.data(), needed, &needed) != 0;
}

bool SidText(PSID sid, std::wstring& result) {
  LPWSTR text = nullptr;
  if (!ConvertSidToStringSidW(sid, &text)) return false;
  result = text;
  LocalFree(text);
  return true;
}

bool ReadToken(HANDLE token, Identity& identity) {
  std::vector<uint8_t> bytes;
  if (!TokenBytes(token, TokenUser, bytes) ||
      !SidText(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,
               identity.user)) return false;
  if (!TokenBytes(token, TokenGroups, bytes)) return false;
  const auto* groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
  for (DWORD i = 0; i < groups->GroupCount; ++i) {
    if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
      if (!SidText(groups->Groups[i].Sid, identity.logon)) return false;
      break;
    }
  }
  DWORD size = 0;
  return !identity.logon.empty() &&
         GetTokenInformation(token, TokenSessionId, &identity.session,
                             sizeof(identity.session), &size) != 0;
}

bool ReadIdentity(HANDLE process, Identity& identity) {
  HANDLE raw_token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) return false;
  Handle token(raw_token);
  if (!ReadToken(token.get(), identity)) return false;
  std::vector<wchar_t> path(32768);
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size) || size == 0)
    return false;
  identity.image.reset(CreateFileW(path.data(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!identity.image ||
      !GetFileInformationByHandleEx(identity.image.get(), FileIdInfo,
                                   &identity.image_id,
                                   sizeof(identity.image_id))) return false;
  const DWORD count = GetFinalPathNameByHandleW(
      identity.image.get(), path.data(), static_cast<DWORD>(path.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (count == 0 || count >= path.size()) return false;
  // Invariant folding makes spelling/case aliases converge on one endpoint.
  const int length = static_cast<int>(count);
  std::vector<wchar_t> folded(static_cast<size_t>(length));
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), length,
                    folded.data(), length, nullptr, nullptr, 0) != length)
    return false;
  identity.image_path.assign(folded.begin(), folded.end());
  return true;
}

bool SameToken(const Identity& a, const Identity& b) {
  return a.user == b.user && a.logon == b.logon && a.session == b.session;
}

bool VerifyProcess(ULONG pid, const Identity& own) {
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                             FALSE, pid));
  if (!process || WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT)
    return false;
  Identity peer;
  return ReadIdentity(process.get(), peer) && SameToken(own, peer) &&
         own.image_path == peer.image_path &&
         own.image_id.VolumeSerialNumber == peer.image_id.VolumeSerialNumber &&
         std::memcmp(own.image_id.FileId.Identifier,
                     peer.image_id.FileId.Identifier,
                     sizeof(own.image_id.FileId.Identifier)) == 0;
}

bool VerifyClient(HANDLE pipe, const Identity& own) {
  ULONG pid = 0;
  if (!GetNamedPipeClientProcessId(pipe, &pid) || !VerifyProcess(pid, own))
    return false;
  // Bind authentication to the pipe's actual client token as well as its PID.
  // Clients request identification-only QOS, so this grants no impersonation
  // authority to the server beyond inspecting identity.
  if (!ImpersonateNamedPipeClient(pipe)) return false;
  HANDLE raw_token = nullptr;
  const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE,
                                      &raw_token) != 0;
  Handle token(raw_token);
  Identity peer;
  const bool valid = opened && ReadToken(token.get(), peer) && SameToken(own, peer);
  if (!RevertToSelf()) std::terminate();
  return valid;
}

std::optional<std::wstring> ScopeHash(const Identity& identity) {
  const auto hex = [](const uint8_t* bytes, size_t size) {
    std::wstring result;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
      result.push_back(digits[bytes[i] >> 4]);
      result.push_back(digits[bytes[i] & 15]);
    }
    return result;
  };
  const std::wstring key = identity.user + L"|" + identity.logon + L"|" +
      std::to_wstring(identity.session) + L"|" + identity.image_path + L"|" +
      std::to_wstring(identity.image_id.VolumeSerialNumber) + L"|" +
      hex(identity.image_id.FileId.Identifier,
          sizeof(identity.image_id.FileId.Identifier));
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0) return std::nullopt;
  std::array<uint8_t, 32> digest{};
  const NTSTATUS status = BCryptHash(
      algorithm, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(key.data())),
      static_cast<ULONG>(key.size() * sizeof(wchar_t)), digest.data(),
      static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) return std::nullopt;
  return hex(digest.data(), digest.size());
}

DWORD Remaining(ULONGLONG deadline) {
  const ULONGLONG now = GetTickCount64();
  return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
}

// Every operation is overlapped, cancellable by stop, and has a fixed deadline.
// After cancellation, reap completion before the OVERLAPPED storage is reused.
bool FinishIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop,
              ULONGLONG deadline, DWORD& transferred) {
  const HANDLE events[] = {stop, operation.hEvent};
  const DWORD wait = WaitForMultipleObjects(2, events, FALSE, Remaining(deadline));
  if (wait != WAIT_OBJECT_0 + 1) {
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
  }
  return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != 0;
}

bool Transfer(HANDLE pipe, HANDLE stop, uint8_t* data, DWORD size, bool write,
              ULONGLONG deadline) {
  Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!event) return false;
  DWORD offset = 0;
  while (offset < size) {
    if (Remaining(deadline) == 0 || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
      return false;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    ResetEvent(event.get());
    DWORD transferred = 0;
    const BOOL immediate = write
        ? WriteFile(pipe, data + offset, size - offset, &transferred, &operation)
        : ReadFile(pipe, data + offset, size - offset, &transferred, &operation);
    if (!immediate) {
      if (GetLastError() != ERROR_IO_PENDING ||
          !FinishIo(pipe, operation, stop, deadline, transferred)) return false;
    }
    if (transferred == 0 || transferred > size - offset) return false;
    offset += transferred;
  }
  return true;
}

int HexDigit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  return -1;
}

bool IdCharacter(char value) {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z') ||
         (value >= '0' && value <= '9');
}

std::optional<std::string> DecodeId(const std::string& text) {
  std::string result;
  for (size_t i = 0; i < text.size(); ++i) {
    char value = text[i];
    if (value == '%') {
      if (i + 2 >= text.size()) return std::nullopt;
      const int hi = HexDigit(text[i + 1]), lo = HexDigit(text[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      value = static_cast<char>((hi << 4) | lo);
      i += 2;
    }
    if (!IdCharacter(value) &&
        (result.empty() || (value != '.' && value != '_' && value != '-')))
      return std::nullopt;
    result.push_back(value);
    if (result.size() > 128) return std::nullopt;
  }
  if (result.empty()) return std::nullopt;
  return result;
}

struct Activation {
  int64_t sequence;
  std::optional<std::string> uri;
  EncodableValue Encode() const {
    return EncodableValue(EncodableMap{
        {EncodableValue("sequence"), EncodableValue(sequence)},
        {EncodableValue("uri"), uri ? EncodableValue(*uri) : EncodableValue()}});
  }
};
}  // namespace

struct HostActivation::State {
  Identity identity;
  SecurityDescriptor security;
  Handle mutex_handle;
  Handle stop;
  Handle first_pipe;
  std::wstring name;
  std::mutex lock;
  HWND window = nullptr;
  bool closing = false;
  bool focus_pending = false;
  int64_t sequence = 0;
  std::deque<Activation> pending;
  struct Seen {
    std::array<uint8_t, 16> nonce;
    std::optional<std::string> uri;
    ULONGLONG expires;
  };
  std::deque<Seen> seen;

  Handle NewPipe() {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.value,
                                    FALSE};
    return Handle(CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                          FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 8192, 8192, kIoTimeout, &attributes));
  }

  uint8_t Accept(const std::array<uint8_t, 16>& nonce,
                 const std::optional<std::string>& uri) {
    std::lock_guard<std::mutex> guard(lock);
    if (closing) return kRejected;
    const ULONGLONG now = GetTickCount64();
    while (!seen.empty() && seen.front().expires <= now) seen.pop_front();
    for (const Seen& value : seen) {
      if (value.nonce == nonce) return value.uri == uri ? kAccepted : kRejected;
    }
    if (pending.size() >= HostActivation::kQueueLimit ||
        sequence == std::numeric_limits<int64_t>::max()) return kBusy;
    pending.push_back({++sequence, uri});
    seen.push_back({nonce, uri, now + kReplayLifetime});
    if (seen.size() > kReplayLimit) seen.pop_front();
    focus_pending = true;
    if (window) PostMessageW(window, HostActivation::kMessage, 0, 0);
    return kAccepted;
  }
};

HostActivation::HostActivation() = default;
HostActivation::~HostActivation() {
  Detach();
  Stop();
}

std::optional<std::string> HostActivation::CanonicalizeUri(const std::string& uri) {
  if (uri.size() > kMaxUri || uri.size() < 12) return std::nullopt;
  for (const unsigned char value : uri) {
    if (value <= 32 || value >= 127 || value == '\\' || value == '#')
      return std::nullopt;
  }
  std::string authority = uri.substr(0, 11);
  std::transform(authority.begin(), authority.end(), authority.begin(),
                 [](char c) {
                   return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                 });
  if (authority != "ryhze://app") return std::nullopt;
  const size_t question = uri.find('?');
  const std::string path = uri.substr(11, question == std::string::npos
                                             ? std::string::npos : question - 11);
  if (path == "/activity") {
    return question == std::string::npos ? std::optional<std::string>("ryhze://app/activity")
                                        : std::nullopt;
  }
  if (path != "/games/store" && path != "/games/library" && path != "/films/store")
    return std::nullopt;
  std::optional<std::string> item, provider;
  if (question != std::string::npos) {
    size_t position = question + 1;
    if (position == uri.size()) return std::nullopt;
    while (position < uri.size()) {
      const size_t end = uri.find('&', position);
      const auto entry = uri.substr(position, end == std::string::npos
                                                 ? std::string::npos : end - position);
      const size_t equals = entry.find('=');
      if (equals == std::string::npos) return std::nullopt;
      const auto value = DecodeId(entry.substr(equals + 1));
      if (!value) return std::nullopt;
      const auto key = entry.substr(0, equals);
      if (key == "item" && !item) item = value;
      else if (key == "provider" && !provider) provider = value;
      else return std::nullopt;
      if (end == std::string::npos) break;
      position = end + 1;
      if (position == uri.size()) return std::nullopt;
    }
  }
  if (path == "/games/library" ? item.has_value() != provider.has_value()
                                : provider.has_value()) return std::nullopt;
  std::string result = "ryhze://app" + path;
  if (item) result += "?item=" + *item;
  if (provider) result += "&provider=" + *provider;
  return result;
}

HostActivation::StartResult HostActivation::Start(
    const std::optional<std::string>& uri) {
  if (state_) return StartResult::unavailable;
  std::optional<std::string> canonical;
  if (uri) {
    canonical = CanonicalizeUri(*uri);
    if (!canonical) return StartResult::invalidArguments;
  }
  auto state = std::make_shared<State>();
  if (!ReadIdentity(GetCurrentProcess(), state->identity))
    return StartResult::unavailable;
  const auto hash = ScopeHash(state->identity);
  if (!hash) return StartResult::unavailable;
  state->name = L"\\\\.\\pipe\\Ryhze.Activation.v1." + *hash;
  const std::wstring mutex_name = L"Local\\Ryhze.Activation.v1." + *hash;
  // Protected DACL: only this logon SID, which is unique to this logon session.
  const std::wstring sddl = L"D:P(A;;GA;;;" + state->identity.logon + L")";
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &state->security.value, nullptr))
    return StartResult::unavailable;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),
                                  state->security.value, FALSE};
  state->stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!state->stop) return StartResult::unavailable;
  SetLastError(ERROR_SUCCESS);
  state->mutex_handle.reset(CreateMutexW(&attributes, FALSE, mutex_name.c_str()));
  const DWORD mutex_error = GetLastError();
  if (!state->mutex_handle) return StartResult::unavailable;
  if (mutex_error == ERROR_ALREADY_EXISTS) {
    Handle pipe;
    const ULONGLONG deadline = GetTickCount64() + kIoTimeout;
    do {
      pipe.reset(CreateFileW(state->name.c_str(), GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                                 SECURITY_IDENTIFICATION, nullptr));
      if (pipe) break;
      const DWORD error = GetLastError();
      if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
        return StartResult::unavailable;
      WaitNamedPipeW(state->name.c_str(), 25);
      // WaitNamedPipe returns immediately when the first instance is not ready.
      if (error == ERROR_FILE_NOT_FOUND) Sleep(10);
    } while (Remaining(deadline) != 0);
    ULONG server_pid = 0;
    if (!pipe || !GetNamedPipeServerProcessId(pipe.get(), &server_pid) ||
        !VerifyProcess(server_pid, state->identity)) return StartResult::unavailable;
    std::vector<uint8_t> request(kHeaderSize + (canonical ? canonical->size() : 0));
    std::copy(std::begin(kMagic), std::end(kMagic), request.begin());
    const uint32_t length = static_cast<uint32_t>(request.size() - kHeaderSize);
    std::memcpy(request.data() + 4, &length, sizeof(length));
    if (BCryptGenRandom(nullptr, request.data() + 8, 16,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
      return StartResult::unavailable;
    if (canonical) std::copy(canonical->begin(), canonical->end(),
                              request.begin() + kHeaderSize);
    AllowSetForegroundWindow(server_pid);
    const ULONGLONG io_deadline = GetTickCount64() + kIoTimeout;
    uint8_t reply = 0;
    if (!Transfer(pipe.get(), state->stop.get(), request.data(),
                  static_cast<DWORD>(request.size()), true, io_deadline) ||
        !Transfer(pipe.get(), state->stop.get(), &reply, 1, false, io_deadline) ||
        reply != kAccepted) return StartResult::unavailable;
    // Confirm the reply was consumed before the server disconnects. This is
    // best effort: acceptance already happened and remains queued until Dart's
    // separate acknowledgement, even when this final transport receipt fails.
    Transfer(pipe.get(), state->stop.get(), &reply, 1, true, io_deadline);
    return StartResult::forwarded;
  }
  state->first_pipe = state->NewPipe();
  if (!state->first_pipe) return StartResult::unavailable;
  if (canonical) state->pending.push_back({++state->sequence, canonical});
  auto* context = new std::shared_ptr<State>(state);
  const uintptr_t thread = _beginthreadex(nullptr, 0, Serve, context, 0, nullptr);
  if (thread == 0) {
    delete context;
    return StartResult::unavailable;
  }
  worker_ = reinterpret_cast<HANDLE>(thread);
  state_ = std::move(state);
  return StartResult::primary;
}

unsigned __stdcall HostActivation::Serve(void* context) {
  std::unique_ptr<std::shared_ptr<State>> owner(
      static_cast<std::shared_ptr<State>*>(context));
  const auto state = *owner;
  owner.reset();
  Handle pipe = std::move(state->first_pipe);
  while (WaitForSingleObject(state->stop.get(), 0) != WAIT_OBJECT_0) {
    if (!pipe) break;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) break;
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    bool connected = ConnectNamedPipe(pipe.get(), &operation) != 0;
    if (!connected) {
      const DWORD error = GetLastError();
      if (error == ERROR_PIPE_CONNECTED) connected = true;
      else if (error == ERROR_IO_PENDING) {
        DWORD transferred = 0;
        // A listener may wait indefinitely; shutdown always wakes its wait.
        const HANDLE events[] = {state->stop.get(), event.get()};
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1) {
          CancelIoEx(pipe.get(), &operation);
          GetOverlappedResult(pipe.get(), &operation, &transferred, TRUE);
          break;
        }
        connected = GetOverlappedResult(pipe.get(), &operation, &transferred,
                                         FALSE) != 0;
      }
    }
    if (connected) {
      const ULONGLONG deadline = GetTickCount64() + kIoTimeout;
      std::array<uint8_t, kHeaderSize> header{};
      uint8_t reply = kRejected;
      if (Transfer(pipe.get(), state->stop.get(), header.data(),
                    static_cast<DWORD>(header.size()), false, deadline) &&
          std::equal(std::begin(kMagic), std::end(kMagic), header.begin()) &&
          VerifyClient(pipe.get(), state->identity)) {
        uint32_t length = 0;
        std::memcpy(&length, header.data() + 4, sizeof(length));
        std::array<uint8_t, 16> nonce{};
        std::copy(header.begin() + 8, header.end(), nonce.begin());
        if (length <= kMaxUri &&
            std::any_of(nonce.begin(), nonce.end(), [](uint8_t v) { return v != 0; })) {
          std::vector<uint8_t> body(length);
          if (Transfer(pipe.get(), state->stop.get(), body.data(), length,
                        false, deadline)) {
            const std::optional<std::string> uri = length == 0 ? std::nullopt :
                HostActivation::CanonicalizeUri(std::string(body.begin(), body.end()));
            if (length == 0 || uri) reply = state->Accept(nonce, uri);
          }
        }
      }
      if (Transfer(pipe.get(), state->stop.get(), &reply, 1, true, deadline)) {
        uint8_t receipt = 0;
        Transfer(pipe.get(), state->stop.get(), &receipt, 1, false, deadline);
      }
      // Never FlushFileBuffers: it can wait indefinitely for a client to read.
      DisconnectNamedPipe(pipe.get());
    }
    if (WaitForSingleObject(state->stop.get(), 0) == WAIT_OBJECT_0) break;
    // Reuse the same first-instance handle. Closing/recreating would leave a
    // namespace race and can fail while a disconnected client still has its
    // handle open. DisconnectNamedPipe makes this instance listenable again.
    if (!connected) break;
  }
  return 0;
}

void HostActivation::Attach(flutter::BinaryMessenger* messenger, HWND window,
                             std::function<void()> bring_forward) {
  Detach();
  if (!state_ || !window || !messenger) return;
  bring_forward_ = std::move(bring_forward);
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->window = window;
    if (!state_->pending.empty()) PostMessageW(window, kMessage, 0, 0);
  }
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      messenger, kChannel, &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
        if (call.method_name() == "connect") {
          if (call.arguments() && !call.arguments()->IsNull()) {
            result->Error("invalid-arguments", "connect takes no arguments");
            return;
          }
          EncodableList values;
          {
            std::lock_guard<std::mutex> guard(state_->lock);
            for (const auto& activation : state_->pending) {
              values.push_back(activation.Encode());
              last_sent_ = activation.sequence;
            }
          }
          connected_ = true;
          result->Success(EncodableValue(EncodableMap{
              {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
              {EncodableValue("singleInstance"), EncodableValue(true)},
              {EncodableValue("activations"), EncodableValue(values)}}));
          return;
        }
        if (call.method_name() == "ack") {
          const auto* map = call.arguments()
              ? std::get_if<EncodableMap>(call.arguments()) : nullptr;
          const auto entry = map ? map->find(EncodableValue("sequence"))
                                 : EncodableMap::const_iterator{};
          int64_t sequence = -1;
          if (map && map->size() == 1 && entry != map->end()) {
            if (const auto* value = std::get_if<int64_t>(&entry->second))
              sequence = *value;
            else if (const auto* small_value = std::get_if<int32_t>(&entry->second))
              sequence = *small_value;
          }
          if (!connected_ || sequence < 1 || sequence > last_sent_) {
            result->Error("invalid-arguments", "invalid activation acknowledgement");
            return;
          }
          {
            std::lock_guard<std::mutex> guard(state_->lock);
            while (!state_->pending.empty() &&
                   state_->pending.front().sequence <= sequence)
              state_->pending.pop_front();
          }
          result->Success();
          return;
        }
        result->NotImplemented();
      });
}

void HostActivation::Detach() {
  connected_ = false;
  last_sent_ = 0;
  bring_forward_ = nullptr;
  if (state_) {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->window = nullptr;
  }
  if (channel_) {
    channel_->SetMethodCallHandler(nullptr);
    channel_.reset();
  }
}

std::optional<LRESULT> HostActivation::HandleMessage(UINT message, WPARAM wparam,
                                                    LPARAM lparam) {
  (void)wparam;
  (void)lparam;
  if (message != kMessage) return std::nullopt;
  if (!state_) return 0;
  bool focus = false;
  std::vector<Activation> delivery;
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    focus = std::exchange(state_->focus_pending, false);
    if (connected_) {
      for (const auto& activation : state_->pending) {
        if (activation.sequence > last_sent_) delivery.push_back(activation);
      }
    }
  }
  if (focus && bring_forward_) bring_forward_();
  for (const auto& activation : delivery) {
    if (!connected_ || !channel_) break;
    last_sent_ = activation.sequence;
    channel_->InvokeMethod("activate", std::make_unique<EncodableValue>(activation.Encode()));
  }
  return 0;
}

void HostActivation::Stop() {
  if (!state_) return;
  {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->closing = true;
    state_->window = nullptr;
  }
  SetEvent(state_->stop.get());
  if (worker_) {
    // The worker owns State independently, including all outstanding I/O
    // storage. Even a delayed OS cancellation cannot hold window teardown or
    // access a destroyed HostActivation/Flutter messenger.
    WaitForSingleObject(worker_, kShutdownTimeout);
    CloseHandle(worker_);
    worker_ = nullptr;
  }
  state_.reset();
}

std::wstring HostActivation::pipe_name() const {
  return state_ ? state_->name : std::wstring();
}
