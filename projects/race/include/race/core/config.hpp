#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

namespace race::core {
constexpr std::size_t max_config_fields = 24;
using ConfigValue = std::variant<std::int64_t, bool>;
enum class ConfigOrigin : std::uint8_t { EngineDefault, Project, Device, Launch };
constexpr std::uint8_t override_permission(ConfigOrigin o) { return std::uint8_t{1} << static_cast<unsigned>(o); }
constexpr std::uint8_t all_config_overrides = 14;
struct ConfigField {
  std::string name, owner;
  ConfigValue default_value;
  std::int64_t minimum = 0, maximum = 0;
  bool runtime_mutable = false;
  std::uint8_t permitted_overrides = all_config_overrides;
};
struct ConfigEntry { std::string name; ConfigValue value; };
class ConfigPatch {
  std::array<ConfigEntry, max_config_fields> entries_{};
  std::size_t size_ = 0;
public:
  std::uint32_t schema_version = 1;
  Result<void> set(std::string_view name, ConfigValue);
  const auto& entries() const noexcept { return entries_; }
  std::size_t size() const noexcept { return size_; }
};
class ConfigSchema;
class EffectiveConfig {
  struct Entry { ConfigField field; ConfigValue value; ConfigOrigin origin; };
  std::array<Entry, max_config_fields> entries_{};
  std::size_t size_ = 0;
  std::string identity_;
  std::array<Result<void> (*)(const EffectiveConfig&), 8> rules_{};
  friend Result<std::shared_ptr<const EffectiveConfig>> resolve_config(
      const ConfigSchema&, const ConfigPatch&, const ConfigPatch&, const ConfigPatch&);
  friend class ConfigStore;
public:
  Result<ConfigValue> get(std::string_view) const;
  Result<ConfigOrigin> origin(std::string_view) const;
  const std::string& identity() const noexcept { return identity_; }
  std::size_t size() const noexcept { return size_; }
};
using ConfigRule = Result<void> (*)(const EffectiveConfig&);
class ConfigSchema {
  std::array<ConfigField, max_config_fields> fields_{};
  std::array<ConfigRule, 8> rules_{};
  std::size_t size_ = 0, rule_count_ = 0;
public:
  static constexpr std::uint32_t version = 1;
  Result<void> add(ConfigField);
  Result<void> add_rule(ConfigRule);
  const auto& fields() const noexcept { return fields_; }
  const auto& rules() const noexcept { return rules_; }
  std::size_t size() const noexcept { return size_; }
  Result<void> validate_rules(const EffectiveConfig&) const;
};
// Deliberately small text schema: schema=1, then named integer/bool fields.
// Unknowns, duplicates, unsupported versions and malformed input are rejected.
Result<ConfigPatch> parse_config(std::string_view);
Result<std::shared_ptr<const EffectiveConfig>> resolve_config(
    const ConfigSchema&, const ConfigPatch& project = {},
    const ConfigPatch& device = {}, const ConfigPatch& launch = {});
ConfigSchema headless_schema();
// Owner-thread staging and safe-point publication. Readers retain immutable
// shared snapshots; failed staging never changes either the active or pending one.
class ConfigStore {
  std::atomic<std::shared_ptr<const EffectiveConfig>> active_;
  std::shared_ptr<const EffectiveConfig> pending_;
  std::thread::id owner_ = std::this_thread::get_id();
public:
  explicit ConfigStore(std::shared_ptr<const EffectiveConfig> initial);
  Result<void> stage(std::shared_ptr<const EffectiveConfig>);
  Result<void> apply_at_safe_point();
  std::shared_ptr<const EffectiveConfig> snapshot() const;
};
}
