#include "race/core/config.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <new>

namespace race::core {
namespace {
bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= 48 && std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.';
  });
}
std::string_view trim(std::string_view input) {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t' || input.front() == '\r')) input.remove_prefix(1);
  while (!input.empty() && (input.back() == ' ' || input.back() == '\t' || input.back() == '\r')) input.remove_suffix(1);
  return input;
}
Result<void> valid_value(const ConfigField& field, const ConfigValue& value) {
  if (value.index() != field.default_value.index()) return fail(ErrorCode::InvalidInput, "configuration field type mismatch");
  if (const auto* integer = std::get_if<std::int64_t>(&value))
    if (*integer < field.minimum || *integer > field.maximum) return fail(ErrorCode::InvalidInput, "configuration field out of range");
  return {};
}
Result<void> headless_rules(const EffectiveConfig& config) {
  const auto authority = std::get<bool>(config.get("authoritative").value());
  const auto fixed_quality = std::get<bool>(config.get("fixed_quality").value());
  const auto quality = std::get<std::int64_t>(config.get("quality_percent").value());
  const auto catch_up = std::get<std::int64_t>(config.get("max_catch_up").value());
  const auto backlog = std::get<std::int64_t>(config.get("max_backlog").value());
  if (backlog < catch_up) return fail(ErrorCode::InvalidInput, "backlog limit must cover catch-up limit");
  if (fixed_quality && quality != 100) return fail(ErrorCode::InvalidInput, "fixed-quality request cannot reduce quality");
  if (authority) {
    for (const auto key : {"simulation_hz", "quality_percent"}) {
      const auto origin = config.origin(key).value();
      if (origin == ConfigOrigin::Device || origin == ConfigOrigin::Launch)
        return fail(ErrorCode::InvalidInput, "local override cannot change authority policy");
    }
  }
  return {};
}
}
Result<void> ConfigPatch::set(std::string_view name, ConfigValue value) {
  if (!valid_name(name) || name == "schema") return fail(ErrorCode::InvalidInput, "invalid configuration field name");
  for (std::size_t i = 0; i < size_; ++i)
    if (entries_[i].name == name) return fail(ErrorCode::Duplicate, "duplicate configuration field");
  if (size_ == max_config_fields) return fail(ErrorCode::Exhausted, "configuration field capacity exhausted");
  try { entries_[size_] = ConfigEntry{std::string(name), std::move(value)}; ++size_; }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "configuration allocation failed"); }
  return {};
}
Result<ConfigValue> EffectiveConfig::get(std::string_view name) const {
  for (std::size_t i = 0; i < size_; ++i) if (entries_[i].field.name == name) return entries_[i].value;
  return fail(ErrorCode::NotFound, "unknown configuration field");
}
Result<ConfigOrigin> EffectiveConfig::origin(std::string_view name) const {
  for (std::size_t i = 0; i < size_; ++i) if (entries_[i].field.name == name) return entries_[i].origin;
  return fail(ErrorCode::NotFound, "unknown configuration field");
}
Result<void> ConfigSchema::add(ConfigField field) {
  if (!valid_name(field.name) || field.name == "schema" || !valid_name(field.owner) ||
      field.minimum > field.maximum || (field.permitted_overrides & ~all_config_overrides) != 0)
    return fail(ErrorCode::InvalidInput, "invalid configuration field schema");
  auto value_check = valid_value(field, field.default_value);
  if (!value_check) return value_check.error();
  for (std::size_t i = 0; i < size_; ++i)
    if (fields_[i].name == field.name) return fail(ErrorCode::Duplicate, "duplicate configuration schema field");
  if (size_ == max_config_fields) return fail(ErrorCode::Exhausted, "configuration schema capacity exhausted");
  fields_[size_++] = std::move(field);
  return {};
}
Result<void> ConfigSchema::add_rule(ConfigRule rule) {
  if (!rule) return fail(ErrorCode::InvalidInput, "null configuration rule");
  if (rule_count_ == rules_.size()) return fail(ErrorCode::Exhausted, "configuration rule capacity exhausted");
  rules_[rule_count_++] = rule;
  return {};
}
Result<void> ConfigSchema::validate_rules(const EffectiveConfig& config) const {
  for (std::size_t i = 0; i < rule_count_; ++i) {
    auto result = rules_[i](config);
    if (!result) return result.error();
  }
  return {};
}
Result<ConfigPatch> parse_config(std::string_view text) {
  if (text.size() > 8192) return fail(ErrorCode::Exhausted, "configuration text limit exceeded");
  ConfigPatch result;
  bool version_seen = false;
  while (!text.empty()) {
    const auto end = text.find('\n');
    auto line = trim(text.substr(0, end));
    if (end == std::string_view::npos) text = {}; else text.remove_prefix(end + 1);
    if (line.size() > 256) return fail(ErrorCode::Exhausted, "configuration line limit exceeded");
    if (line.empty() || line.front() == '#') continue;
    const auto equal = line.find('=');
    if (equal == std::string_view::npos) return fail(ErrorCode::InvalidInput, "configuration assignment expected");
    const auto name = trim(line.substr(0, equal));
    const auto value = trim(line.substr(equal + 1));
    if (name == "schema") {
      if (version_seen) return fail(ErrorCode::Duplicate, "duplicate configuration schema declaration");
      version_seen = true;
      if (value != "1") return fail(ErrorCode::IncompatibleVersion, "unsupported configuration schema version");
      continue;
    }
    ConfigValue parsed;
    if (value == "true") parsed = true;
    else if (value == "false") parsed = false;
    else {
      std::int64_t integer = 0;
      const auto conversion = std::from_chars(value.data(), value.data() + value.size(), integer);
      if (conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size())
        return fail(ErrorCode::InvalidInput, "invalid configuration value");
      parsed = integer;
    }
    auto added = result.set(name, parsed);
    if (!added) return added.error();
  }
  if (!version_seen) return fail(ErrorCode::IncompatibleVersion, "configuration schema declaration required");
  return result;
}
Result<std::shared_ptr<const EffectiveConfig>> resolve_config(
    const ConfigSchema& schema, const ConfigPatch& project,
    const ConfigPatch& device, const ConfigPatch& launch) {
  try {
    auto effective = std::make_shared<EffectiveConfig>();
    effective->size_ = schema.size();
    effective->rules_ = schema.rules();
    for (std::size_t i = 0; i < schema.size(); ++i)
      effective->entries_[i] = {schema.fields()[i], schema.fields()[i].default_value, ConfigOrigin::EngineDefault};
    const std::array<const ConfigPatch*, 3> layers{&project, &device, &launch};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
      const auto& patch = *layers[layer];
      if (patch.schema_version != ConfigSchema::version) return fail(ErrorCode::IncompatibleVersion, "unsupported configuration patch version");
      const auto origin = static_cast<ConfigOrigin>(layer + 1);
      for (std::size_t j = 0; j < patch.size(); ++j) {
        const auto& input = patch.entries()[j];
        auto begin = effective->entries_.begin();
        auto end = begin + static_cast<std::ptrdiff_t>(effective->size_);
        auto found = std::find_if(begin, end, [&](const auto& entry) { return entry.field.name == input.name; });
        if (found == end) return fail(ErrorCode::InvalidInput, "unknown configuration field");
        if ((found->field.permitted_overrides & override_permission(origin)) == 0)
          return fail(ErrorCode::InvalidInput, "configuration override forbidden");
        auto checked = valid_value(found->field, input.value);
        if (!checked) return checked.error();
        found->value = input.value; found->origin = origin;
      }
    }
    auto checked = schema.validate_rules(*effective);
    if (!checked) return checked.error();
    // Identity is complete canonical value text, not a collision-prone hash.
    // Origins are recorded separately and do not change equivalent settings.
    std::sort(effective->entries_.begin(), effective->entries_.begin() + static_cast<std::ptrdiff_t>(effective->size_),
              [](const auto& a, const auto& b) { return a.field.name < b.field.name; });
    effective->identity_ = "race-config-schema=1\n";
    for (std::size_t i = 0; i < effective->size_; ++i) {
      const auto& entry = effective->entries_[i];
      effective->identity_ += entry.field.name + "=";
      if (const auto* integer = std::get_if<std::int64_t>(&entry.value)) effective->identity_ += "i:" + std::to_string(*integer);
      else effective->identity_ += std::get<bool>(entry.value) ? "b:true" : "b:false";
      effective->identity_ += '\n';
    }
    return std::shared_ptr<const EffectiveConfig>(std::move(effective));
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "configuration snapshot allocation failed"); }
  catch (...) { return fail(ErrorCode::InvalidInput, "configuration validator threw an exception"); }
}
ConfigSchema headless_schema() {
  ConfigSchema schema;
  const auto add = [&](ConfigField field) { if (!schema.add(std::move(field))) std::abort(); };
  // Demonstration defaults, configurable workload values; not performance budgets.
  add({"simulation_hz", "core.fixture", std::int64_t{60}, 1, 1000, false, all_config_overrides});
  add({"max_catch_up", "core.fixture", std::int64_t{4}, 1, 1024, true, all_config_overrides});
  add({"max_backlog", "core.fixture", std::int64_t{8}, 1, 4096, true, all_config_overrides});
  add({"quality_percent", "core.fixture", std::int64_t{100}, 1, 100, true, all_config_overrides});
  add({"authoritative", "core.fixture", false, 0, 0, false, override_permission(ConfigOrigin::Project)});
  add({"fixed_quality", "core.fixture", false, 0, 0, false, override_permission(ConfigOrigin::Project)});
  add({"detailed_capture", "core.fixture", false, 0, 0, true, all_config_overrides});
  if (!schema.add_rule(headless_rules)) std::abort();
  return schema;
}
ConfigStore::ConfigStore(std::shared_ptr<const EffectiveConfig> initial) : active_(std::move(initial)) {
  if (!active_.load()) std::abort();
}
Result<void> ConfigStore::stage(std::shared_ptr<const EffectiveConfig> next) {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "configuration staging requires owner thread");
  const auto current = active_.load(std::memory_order_acquire);
  if (!next || next->size_ != current->size_ || next->rules_ != current->rules_)
    return fail(ErrorCode::IncompatibleVersion, "configuration schema differs");
  for (std::size_t i = 0; i < current->size_; ++i) {
    const auto& a = current->entries_[i]; const auto& b = next->entries_[i];
    if (a.field.name != b.field.name || a.field.owner != b.field.owner || a.field.default_value != b.field.default_value ||
        a.field.minimum != b.field.minimum || a.field.maximum != b.field.maximum || a.field.runtime_mutable != b.field.runtime_mutable ||
        a.field.permitted_overrides != b.field.permitted_overrides)
      return fail(ErrorCode::IncompatibleVersion, "configuration field contract differs");
    if (!a.field.runtime_mutable && a.value != b.value) return fail(ErrorCode::InvalidInput, "configuration value requires restart");
  }
  pending_ = std::move(next);
  return {};
}
Result<void> ConfigStore::apply_at_safe_point() {
  if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread, "configuration publication requires owner thread");
  if (!pending_) return fail(ErrorCode::NotFound, "no configuration update staged");
  active_.store(std::move(pending_), std::memory_order_release);
  return {};
}
std::shared_ptr<const EffectiveConfig> ConfigStore::snapshot() const { return active_.load(std::memory_order_acquire); }
}
