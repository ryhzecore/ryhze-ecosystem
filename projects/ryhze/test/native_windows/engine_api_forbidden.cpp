// Link the real Flutter wrapper/codec while forbidding engine-backed calls.
// Tests supply their own BinaryMessenger; any accidental engine use fails hard.
#include <flutter_messenger.h>
#include <flutter_texture_registrar.h>

#include <cstdlib>

bool FlutterDesktopMessengerSend(FlutterDesktopMessengerRef, const char*,
                                 const uint8_t*, size_t) {
  std::abort();
}
bool FlutterDesktopMessengerSendWithReply(
    FlutterDesktopMessengerRef, const char*, const uint8_t*, size_t,
    FlutterDesktopBinaryReply, void*) {
  std::abort();
}
void FlutterDesktopMessengerSendResponse(
    FlutterDesktopMessengerRef, const FlutterDesktopMessageResponseHandle*,
    const uint8_t*, size_t) {
  std::abort();
}
void FlutterDesktopMessengerSetCallback(FlutterDesktopMessengerRef, const char*,
                                        FlutterDesktopMessageCallback, void*) {
  std::abort();
}
FlutterDesktopMessengerRef FlutterDesktopMessengerAddRef(
    FlutterDesktopMessengerRef) {
  std::abort();
}
void FlutterDesktopMessengerRelease(FlutterDesktopMessengerRef) {
  std::abort();
}
bool FlutterDesktopMessengerIsAvailable(FlutterDesktopMessengerRef) {
  std::abort();
}
FlutterDesktopMessengerRef FlutterDesktopMessengerLock(
    FlutterDesktopMessengerRef) {
  std::abort();
}
void FlutterDesktopMessengerUnlock(FlutterDesktopMessengerRef) {
  std::abort();
}
int64_t FlutterDesktopTextureRegistrarRegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef, const FlutterDesktopTextureInfo*) {
  std::abort();
}
void FlutterDesktopTextureRegistrarUnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef, int64_t, void (*)(void*), void*) {
  std::abort();
}
bool FlutterDesktopTextureRegistrarMarkExternalTextureFrameAvailable(
    FlutterDesktopTextureRegistrarRef, int64_t) {
  std::abort();
}
