#include "race/core/config.hpp"
#include "test.hpp"
#include <atomic>
#include <string>
#include <thread>
#include <utility>

namespace {
void config_name_case_tests() {
  using namespace race; using namespace race::core;
  ConfigSchema schema;
  CHECK(schema.add({"SimulationHz", "Core.Config", std::int64_t{60}, 1, 1000, true}));
  CHECK(schema.add({"simulationHz", "core.config", std::int64_t{30}, 1, 1000, true}));
  CHECK(schema.add({"Schema", "schema", std::int64_t{1}, 0, 2, true}));
  CHECK(schema.add({"Enabled", "Core.Config", false, 0, 0, true}));
  CHECK(schema.fields()[0].name == "SimulationHz");
  CHECK(schema.fields()[0].owner == "Core.Config");
  CHECK_ERROR(schema.add({"SimulationHz", "Other", std::int64_t{1}, 0, 2, true}), ErrorCode::Duplicate);
  CHECK_ERROR(schema.add({"schema", "Core", std::int64_t{1}, 0, 2, true}), ErrorCode::InvalidInput);
  CHECK(schema.size() == 4);

  ConfigPatch patch;
  CHECK(patch.set("SimulationHz", std::int64_t{90}));
  CHECK(patch.set("simulationHz", std::int64_t{45}));
  CHECK(patch.set("Schema", std::int64_t{2}));
  CHECK(patch.set("Enabled", true));
  CHECK_ERROR(patch.set("SimulationHz", std::int64_t{100}), ErrorCode::Duplicate);
  CHECK_ERROR(patch.set("schema", std::int64_t{1}), ErrorCode::InvalidInput);
  CHECK(patch.size() == 4);
  auto parsed = parse_config("schema=1\nSimulationHz=90\nsimulationHz=45\nSchema=2\nEnabled=true\n");
  CHECK(parsed); CHECK(parsed.value().size() == 4);
  CHECK(parsed.value().entries()[0].name == "SimulationHz");
  CHECK_ERROR(parse_config("schema=1\nSimulationHz=90\nSimulationHz=45\n"), ErrorCode::Duplicate);
  CHECK_ERROR(parse_config("Schema=1\nSimulationHz=90\n"), ErrorCode::IncompatibleVersion);
  CHECK_ERROR(parse_config("schema=1\nEnabled=True\n"), ErrorCode::InvalidInput);
  CHECK_ERROR(parse_config("schema=1\nEnabled=FALSE\n"), ErrorCode::InvalidInput);

  auto effective = resolve_config(schema, patch); CHECK(effective);
  auto text_effective = resolve_config(schema, parsed.value()); CHECK(text_effective);
  CHECK(effective.value()->identity() == text_effective.value()->identity());
  CHECK(effective.value()->identity() ==
        "race-config-schema=1\nEnabled=b:true\nSchema=i:2\nSimulationHz=i:90\nsimulationHz=i:45\n");
  CHECK(std::get<std::int64_t>(effective.value()->get("SimulationHz").value()) == 90);
  CHECK(std::get<std::int64_t>(effective.value()->get("simulationHz").value()) == 45);
  CHECK(effective.value()->origin("SimulationHz").value() == ConfigOrigin::Project);
  CHECK(effective.value()->origin("simulationHz").value() == ConfigOrigin::Project);
  CHECK_ERROR(effective.value()->get("SIMULATIONHZ"), ErrorCode::NotFound);
  CHECK_ERROR(effective.value()->origin("SIMULATIONHZ"), ErrorCode::NotFound);
  auto unregistered = parse_config("schema=1\nsimulationhz=90\n"); CHECK(unregistered);
  CHECK_ERROR(resolve_config(schema, unregistered.value()), ErrorCode::InvalidInput);
  auto alias = parse_config("schema=1\nSimulationHz=90\n"); CHECK(alias);
  CHECK_ERROR(resolve_config(headless_schema(), alias.value()), ErrorCode::InvalidInput);

  // Owners are stored exactly too; a case-only owner change is a schema change.
  ConfigSchema changed_owner;
  for (std::size_t i = 0; i < schema.size(); ++i) {
    auto field = schema.fields()[i];
    if (i == 0) field.owner = "core.Config";
    CHECK(changed_owner.add(std::move(field)));
  }
  auto changed = resolve_config(changed_owner, patch); CHECK(changed);
  CHECK(changed.value()->identity() == effective.value()->identity());
  ConfigStore store(effective.value());
  CHECK_ERROR(store.stage(changed.value()), ErrorCode::IncompatibleVersion);
  CHECK(store.snapshot() == effective.value());

  const auto lower = resolve_config(headless_schema()); CHECK(lower);
  CHECK(lower.value()->identity() ==
        "race-config-schema=1\nauthoritative=b:false\ndetailed_capture=b:false\n"
        "fixed_quality=b:false\nmax_backlog=i:8\nmax_catch_up=i:4\n"
        "quality_percent=i:100\nsimulation_hz=i:60\n");
}

void config_name_boundary_tests() {
  using namespace race; using namespace race::core;
  const std::string name48(48, 'A'), owner48(48, 'Z');
  ConfigSchema schema;
  CHECK(schema.add({name48, owner48, std::int64_t{1}, 0, 2, true}));
  CHECK(schema.add({"ABCDEFGHIJKLMNOPQRSTUVWXYZ", "AZaz09_.", std::int64_t{1}, 0, 2, true}));
  CHECK(schema.fields()[0].name == name48); CHECK(schema.fields()[0].owner == owner48);
  ConfigPatch patch; CHECK(patch.set(name48, std::int64_t{2}));
  auto parsed = parse_config("schema=1\n" + name48 + "=2\n"); CHECK(parsed);
  auto effective = resolve_config(schema, parsed.value()); CHECK(effective);
  CHECK(std::get<std::int64_t>(effective.value()->get(name48).value()) == 2);

  for (const auto& invalid : {std::string{}, std::string(49, 'A'), std::string{"Bad-Name"},
                             std::string{"Bad Name"}, std::string{"\xC3\x89"},
                             std::string{"A\0Z", 3}}) {
    ConfigSchema invalid_schema;
    CHECK_ERROR(invalid_schema.add({invalid, "Core", std::int64_t{1}, 0, 2, true}), ErrorCode::InvalidInput);
    CHECK_ERROR(invalid_schema.add({"Valid", invalid, std::int64_t{1}, 0, 2, true}), ErrorCode::InvalidInput);
    CHECK(invalid_schema.size() == 0);
    ConfigPatch invalid_patch;
    CHECK_ERROR(invalid_patch.set(invalid, std::int64_t{1}), ErrorCode::InvalidInput);
    CHECK(invalid_patch.size() == 0);
    CHECK_ERROR(parse_config("schema=1\n" + invalid + "=1\n"), ErrorCode::InvalidInput);
  }
}
}

void config_tests() {
  using namespace race; using namespace race::core;
  config_name_case_tests(); config_name_boundary_tests();
  const auto schema = headless_schema();
  auto base = resolve_config(schema); CHECK(base);
  CHECK(std::get<std::int64_t>(base.value()->get("simulation_hz").value()) == 60);
  CHECK(base.value()->origin("simulation_hz").value() == ConfigOrigin::EngineDefault);
  CHECK(!base.value()->get("unknown")); CHECK(!base.value()->origin("unknown"));
  auto project = parse_config("schema=1\nquality_percent=90\n"); CHECK(project);
  auto device = parse_config("schema=1\nquality_percent=80\n"); CHECK(device);
  auto launch = parse_config("schema=1\nquality_percent=70\n"); CHECK(launch);
  auto merged = resolve_config(schema, project.value(), device.value(), launch.value()); CHECK(merged);
  CHECK(std::get<std::int64_t>(merged.value()->get("quality_percent").value()) == 70);
  CHECK(merged.value()->origin("quality_percent").value() == ConfigOrigin::Launch);
  auto project_only = resolve_config(schema, project.value()).value();
  CHECK(project_only->origin("quality_percent").value() == ConfigOrigin::Project);
  auto device_only = resolve_config(schema, {}, device.value()).value();
  CHECK(device_only->origin("quality_percent").value() == ConfigOrigin::Device);
  CHECK(!parse_config("quality_percent=90\n")); CHECK(!parse_config("schema=2\n"));
  CHECK(!parse_config("schema=1\nschema=1\n")); CHECK(!parse_config("schema=1\nno assignment\n"));
  CHECK(!parse_config("schema=1\nx=1x\n")); CHECK(!parse_config("schema=1\nx=\n"));
  CHECK(!parse_config("schema=1\nx=9223372036854775808\n"));
  CHECK(!parse_config("schema=1\nx=1\nx=2\n"));
  CHECK(!parse_config(std::string(8193, 'x')));
  CHECK(!parse_config("schema=1\n" + std::string(257, 'x')));
  CHECK(parse_config(" # fixture\r\n schema = 1 \r\n detailed_capture = true \r\n"));
  auto unknown = parse_config("schema=1\nunknown=4\n"); CHECK(unknown); CHECK(!resolve_config(schema, unknown.value()));
  auto type = parse_config("schema=1\nquality_percent=true\n"); CHECK(type); CHECK(!resolve_config(schema, type.value()));
  auto range = parse_config("schema=1\nquality_percent=101\n"); CHECK(range); CHECK(!resolve_config(schema, range.value()));
  auto cross = parse_config("schema=1\nmax_backlog=2\n"); CHECK(cross); CHECK(!resolve_config(schema, cross.value()));
  ConfigPatch wrong_version; wrong_version.schema_version = 2; CHECK(!resolve_config(schema, wrong_version));
  auto fixed = parse_config("schema=1\nfixed_quality=true\n").value();
  CHECK(resolve_config(schema, fixed)); CHECK(!resolve_config(schema, fixed, device.value()));
  CHECK(!resolve_config(schema, {}, fixed)); CHECK(!resolve_config(schema, {}, {}, fixed));
  auto authority = parse_config("schema=1\nauthoritative=true\n").value();
  auto rate = parse_config("schema=1\nsimulation_hz=30\n").value();
  CHECK(resolve_config(schema, authority)); CHECK(!resolve_config(schema, authority, rate));
  CHECK(!resolve_config(schema, authority, {}, rate)); CHECK(!resolve_config(schema, authority, device.value()));
  auto reordered_a = parse_config("schema=1\nquality_percent=90\ndetailed_capture=true\n").value();
  auto reordered_b = parse_config("schema=1\ndetailed_capture=true\nquality_percent=90\n").value();
  auto same_a = resolve_config(schema, reordered_a).value();
  auto same_b = resolve_config(schema, {}, reordered_b).value();
  CHECK(same_a->identity() == same_b->identity()); CHECK(same_a->identity() != base.value()->identity());
  ConfigStore store(base.value()); auto retained = store.snapshot();
  CHECK(store.stage(same_a)); CHECK(store.snapshot()->identity() == base.value()->identity());
  auto restart = resolve_config(schema, rate).value(); CHECK(!store.stage(restart));
  CHECK(store.snapshot()->identity() == base.value()->identity());
  CHECK(store.apply_at_safe_point()); CHECK(store.snapshot()->identity() == same_a->identity());
  CHECK(retained->identity() == base.value()->identity()); CHECK(!store.apply_at_safe_point());
  std::thread wrong_owner([&] { CHECK(!store.stage(base.value())); CHECK(!store.apply_at_safe_point()); CHECK(store.snapshot()); });
  wrong_owner.join();
  std::atomic<bool> stop = false;
  std::thread reader([&] { while (!stop.load()) { auto snapshot = store.snapshot(); CHECK(snapshot); CHECK(snapshot->get("quality_percent")); } });
  for (int i = 0; i < 1000; ++i) { CHECK(store.stage(i % 2 == 0 ? base.value() : same_a)); CHECK(store.apply_at_safe_point()); }
  stop = true; reader.join();
  ConfigSchema other; CHECK(other.add({"only", "test", std::int64_t{1}, 0, 2, true}));
  CHECK(!store.stage(resolve_config(other).value()));
  CHECK(!other.add({"only", "test", std::int64_t{1}, 0, 2, true}));
  CHECK(!other.add({"bad name", "test", std::int64_t{1}, 0, 2, true}));
  CHECK(!other.add({"invalid", "test", std::int64_t{3}, 0, 2, true}));
  CHECK(!other.add_rule(nullptr));
  ConfigPatch full;
  for (std::size_t i = 0; i < max_config_fields; ++i) CHECK(full.set("f" + std::to_string(i), std::int64_t{0}));
  CHECK(!full.set("overflow", std::int64_t{0}));
}
