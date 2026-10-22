#ifndef RUNNER_HOST_SECURITY_H_
#define RUNNER_HOST_SECURITY_H_
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <memory>
#include <optional>
#include <string>
#include "credential_vault.h"

// In-process, versioned contracts. The caller never selects the vault path.
class HostSecurity {
 public:
  explicit HostSecurity(std::optional<std::wstring> private_root = std::nullopt);
  ~HostSecurity();
  void Attach(flutter::BinaryMessenger* messenger);
  void Detach();
 private:
  std::optional<std::wstring> private_root_;
  std::unique_ptr<ryhze::CredentialVault> vault_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
};
#endif
