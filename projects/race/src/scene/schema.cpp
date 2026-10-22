#include "internal.hpp"
#include <cmath>
namespace race::scene {
namespace {
bool name_valid(std::string_view s) {
  if (s.empty() || s.size() > 48)
    return false;
  for (char c : s)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '.'))
      return false;
  return true;
}
} // namespace
Result<std::shared_ptr<const Schemas>> Schemas::create(std::vector<ComponentSchema> types) {
  try {
    if (types.size() >= max_types)
      return fail(ErrorCode::Exhausted, "component schema capacity");
    types.push_back({transform_type,
                     1,
                     "Transform",
                     {{translation_field, "translation", Vec3{}},
                      {rotation_field, "rotation", Quat{}},
                      {scale_field, "scale", Vec3{1, 1, 1}},
                      {parent_field, "parent", Reference{}}}});
    for (auto &f : types.back().fields)
      f.exposed = true;
    std::sort(types.begin(), types.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    for (std::size_t i = 0; i < types.size(); ++i) {
      auto &t = types[i];
      if (!t.id || !t.version || !name_valid(t.name) || t.fields.empty() ||
          t.fields.size() > max_fields)
        return fail(ErrorCode::InvalidInput, "invalid component schema");
      if (i && types[i - 1].id == t.id)
        return fail(ErrorCode::Duplicate, "duplicate component type");
      std::sort(t.fields.begin(), t.fields.end(), [](auto &a, auto &b) { return a.id < b.id; });
      for (std::size_t j = 0; j < t.fields.size(); ++j) {
        auto &f = t.fields[j];
        if (!f.id || !name_valid(f.name) || !std::isfinite(f.minimum) ||
            !std::isfinite(f.maximum) || f.minimum > f.maximum)
          return fail(ErrorCode::InvalidInput, "invalid field schema");
        if (j && t.fields[j - 1].id == f.id)
          return fail(ErrorCode::Duplicate, "duplicate field ID");
      }
    }
    std::vector<ComponentSchema> compact;
    compact.reserve(types.size());
    for (auto &t : types) {
      ComponentSchema c{t.id, t.version, std::string(t.name.data(), t.name.size()), {}};
      c.fields.reserve(t.fields.size());
      for (auto &f : t.fields)
        c.fields.push_back({f.id, std::string(f.name.data(), f.name.size()), f.default_value,
                            f.minimum, f.maximum, f.exposed});
      compact.push_back(std::move(c));
    }
    auto result = std::shared_ptr<const Schemas>(new Schemas(std::move(compact)));
    for (auto &t : result->types())
      for (auto &f : t.fields) {
        auto v = result->validate(t.id, f.id, f.default_value);
        if (!v)
          return v.error();
      }
    return result;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "schema allocation failed");
  }
}
const ComponentSchema *Schemas::find(TypeId id) const noexcept {
  for (auto &t : types_)
    if (t.id == id)
      return &t;
  return nullptr;
}
Result<std::size_t> Schemas::index(TypeId id) const noexcept {
  for (std::size_t i = 0; i < types_.size(); ++i)
    if (types_[i].id == id)
      return i;
  return fail(ErrorCode::Unsupported, "unknown component type");
}
Result<std::size_t> Schemas::field_index(TypeId t, FieldId f) const noexcept {
  auto *s = find(t);
  if (!s)
    return fail(ErrorCode::Unsupported, "unknown component type");
  for (std::size_t i = 0; i < s->fields.size(); ++i)
    if (s->fields[i].id == f)
      return i;
  return fail(ErrorCode::Unsupported, "unknown component field");
}
Result<void> Schemas::validate(TypeId type, FieldId id, const Value &value) const noexcept {
  auto i = field_index(type, id);
  if (!i)
    return i.error();
  const auto &f = find(type)->fields[i.value()];
  if (value.index() != f.default_value.index())
    return fail(ErrorCode::InvalidInput, "component field type mismatch");
  bool valid = true;
  if (auto *p = std::get_if<std::int64_t>(&value))
    valid = static_cast<long double>(*p) >= f.minimum && static_cast<long double>(*p) <= f.maximum;
  if (auto *p = std::get_if<double>(&value))
    valid = std::isfinite(*p) && *p >= f.minimum && *p <= f.maximum;
  if (auto *p = std::get_if<Vec3>(&value))
    valid = finite(*p) && p->x >= f.minimum && p->x <= f.maximum && p->y >= f.minimum &&
            p->y <= f.maximum && p->z >= f.minimum && p->z <= f.maximum;
  if (auto *p = std::get_if<Quat>(&value)) {
    auto q = *p;
    auto n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    valid = std::isfinite(n) && std::abs(n - 1) <= 1e-10;
  }
  if (auto *p = std::get_if<Reference>(&value))
    valid = p->object.valid() || !p->section.valid();
  return valid ? Result<void>{}
               : Result<void>{fail(ErrorCode::InvalidInput, "component field validation failed")};
}
Component transform_component(LocalTransform t, Reference parent) {
  return {transform_type,
          1,
          {{translation_field, t.translation},
           {rotation_field, t.rotation},
           {scale_field, t.scale},
           {parent_field, parent}}};
}
const Component *component(const Object &o, TypeId t) noexcept {
  for (auto &c : o.components)
    if (c.type == t)
      return &c;
  return nullptr;
}
Component *component(Object &o, TypeId t) noexcept {
  for (auto &c : o.components)
    if (c.type == t)
      return &c;
  return nullptr;
}
const Value *field(const Component &c, FieldId id) noexcept {
  for (auto &f : c.fields)
    if (f.id == id)
      return &f.value;
  return nullptr;
}
LocalTransform local_transform(const Component &c) {
  return {std::get<Vec3>(*field(c, translation_field)), std::get<Quat>(*field(c, rotation_field)),
          std::get<Vec3>(*field(c, scale_field))};
}
Reference parent_reference(const Component &c) {
  return std::get<Reference>(*field(c, parent_field));
}
Result<void> validate_limits(Limits l) {
  if (!l.objects || l.objects > 65536 || !l.components || l.components > l.objects * max_types ||
      !l.commands_per_transaction || l.commands_per_transaction > 131072 || !l.transaction_bytes ||
      !l.pending_transactions || l.pending_transactions > 4096 || !l.pending_bytes ||
      !l.history_groups || l.history_groups > 1024 || !l.history_bytes || !l.views ||
      l.views > 4096 || !l.sections || l.sections > 1024 || !l.prepared_bytes ||
      !l.document_bytes || l.document_bytes > 256 * 1024 * 1024 || l.json_scratch_bytes < 65536 ||
      l.json_scratch_bytes > 1024 * 1024 * 1024 || !l.hierarchy_depth || l.hierarchy_depth > 1024 ||
      !l.template_depth || l.template_depth > max_template_depth || !l.template_objects ||
      l.template_objects > l.objects || !l.template_revisions || l.template_revisions > 1024)
    return fail(ErrorCode::InvalidInput, "invalid Scene limits");
  return {};
}
Result<void> normalize(Object &o, const Schemas &s, Limits limits) {
  if (!o.id.valid() || o.components.size() > s.types().size())
    return fail(ErrorCode::InvalidInput, "invalid authored object");
  std::sort(o.components.begin(), o.components.end(),
            [](auto &a, auto &b) { return a.type < b.type; });
  for (std::size_t i = 0; i < o.components.size(); ++i) {
    auto &c = o.components[i];
    auto *schema = s.find(c.type);
    if (!schema)
      return fail(ErrorCode::Unsupported, "unknown component type");
    if (c.version != schema->version)
      return fail(ErrorCode::IncompatibleVersion, "component schema version");
    if (i && o.components[i - 1].type == c.type)
      return fail(ErrorCode::Duplicate, "duplicate component");
    if (c.fields.size() > schema->fields.size())
      return fail(ErrorCode::InvalidInput, "too many component fields");
    std::sort(c.fields.begin(), c.fields.end(), [](auto &a, auto &b) { return a.id < b.id; });
    for (std::size_t j = 0; j < c.fields.size(); ++j) {
      if (j && c.fields[j - 1].id == c.fields[j].id)
        return fail(ErrorCode::Duplicate, "duplicate component field");
      auto v = s.validate(c.type, c.fields[j].id, c.fields[j].value);
      if (!v)
        return v.error();
    }
    for (auto &f : schema->fields)
      if (!field(c, f.id))
        c.fields.push_back({f.id, f.default_value});
    std::sort(c.fields.begin(), c.fields.end(), [](auto &a, auto &b) { return a.id < b.id; });
  }
  if (auto *c = component(o, transform_type)) {
    auto v = validate(local_transform(*c));
    if (!v)
      return v;
  }
  if (o.binding) {
    auto &b = *o.binding;
    if (!b.instance.valid() || !b.pinned.id.valid() || !b.pinned.revision || !b.path.length ||
        b.path.length > limits.template_depth + 1 ||
        b.overrides.size() > max_types * (max_fields + 1))
      return fail(ErrorCode::InvalidInput, "invalid template binding");
    for (std::size_t i = 0; i < b.path.length; ++i)
      if (!b.path.nodes[i].valid())
        return fail(ErrorCode::InvalidInput, "invalid template path");
    for (std::size_t i = b.path.length; i < b.path.nodes.size(); ++i)
      if (b.path.nodes[i].valid())
        return fail(ErrorCode::InvalidInput, "noncanonical template path");
    std::sort(b.overrides.begin(), b.overrides.end(), [](auto &a, auto &z) {
      return a.type < z.type || (a.type == z.type && a.field < z.field);
    });
    for (std::size_t i = 0; i < b.overrides.size(); ++i) {
      auto v = b.overrides[i];
      if (!s.find(v.type))
        return fail(ErrorCode::Unsupported, "override component type");
      if (v.field && !s.field_index(v.type, v.field))
        return fail(ErrorCode::Unsupported, "override field");
      if (i && b.overrides[i - 1] == v)
        return fail(ErrorCode::Duplicate, "duplicate override");
    }
  }
  return {};
}
std::size_t object_bytes(const Object &o) noexcept {
  std::size_t n = sizeof(Object) + o.components.capacity() * sizeof(Component);
  for (auto &c : o.components)
    n += c.fields.capacity() * sizeof(FieldValue);
  if (o.binding)
    n += o.binding->overrides.capacity() * sizeof(Override);
  return n;
}
Result<std::size_t> transaction_bytes(const Transaction &t, Limits l) noexcept {
  if (t.mode != EditMode::Authoring && t.mode != EditMode::Simulation)
    return fail(ErrorCode::InvalidInput, "unknown edit mode");
  if (t.commands.size() > l.commands_per_transaction)
    return fail(ErrorCode::QueueFull, "transaction command limit");
  auto b = checked_mul(t.commands.capacity(), sizeof(Command));
  if (!b)
    return b.error();
  std::size_t bytes = b.value();
  if (bytes > l.transaction_bytes)
    return fail(ErrorCode::Exhausted, "transaction byte limit");
  for (auto &c : t.commands) {
    if (c.object.components.size() > max_types || c.component.fields.size() > max_fields ||
        (c.object.binding && c.object.binding->overrides.size() > max_types * (max_fields + 1)))
      return fail(ErrorCode::Exhausted, "command payload limit");
    for (auto &x : c.object.components)
      if (x.fields.size() > max_fields)
        return fail(ErrorCode::Exhausted, "command field limit");
    auto n = checked_add(bytes, object_bytes(c.object) +
                                    c.component.fields.capacity() * sizeof(FieldValue));
    if (!n)
      return n.error();
    bytes = n.value();
    if (bytes > l.transaction_bytes)
      return fail(ErrorCode::Exhausted, "transaction byte limit");
  }
  return bytes;
}
Result<PackedBinding> pack_binding(const std::optional<Binding> &b, const Schemas &s) {
  PackedBinding p;
  if (!b)
    return p;
  p.bound = true;
  p.instance = b->instance;
  p.pinned = b->pinned;
  p.path = b->path;
  for (auto &o : b->overrides) {
    auto t = s.index(o.type);
    if (!t)
      return t.error();
    std::size_t f = max_fields;
    if (o.field) {
      auto fi = s.field_index(o.type, o.field);
      if (!fi)
        return fi.error();
      f = fi.value();
    }
    p.overrides[t.value()] |= static_cast<std::uint16_t>(1U << f);
  }
  return p;
}
std::optional<Binding> unpack_binding(const PackedBinding &p, const Schemas &s) {
  if (!p.bound)
    return {};
  Binding b{p.instance, p.pinned, p.path, {}};
  for (std::size_t t = 0; t < s.types().size(); ++t) {
    if (p.overrides[t] & (1U << max_fields))
      b.overrides.push_back({s.types()[t].id, 0});
    for (std::size_t f = 0; f < s.types()[t].fields.size(); ++f)
      if (p.overrides[t] & (1U << f))
        b.overrides.push_back({s.types()[t].id, s.types()[t].fields[f].id});
  }
  return b;
}
Command Command::create(Object o) {
  Command c;
  c.kind = CommandKind::Create;
  c.target = o.id;
  c.object = std::move(o);
  return c;
}
Command Command::destroy(PersistentId id, std::optional<ObjectHandle> h) {
  Command c;
  c.kind = CommandKind::Destroy;
  c.target = id;
  c.expected_handle = h;
  return c;
}
Command Command::set(PersistentId id, TypeId t, FieldId f, Value v, std::optional<ObjectHandle> h) {
  Command c;
  c.kind = CommandKind::SetField;
  c.target = id;
  c.type = t;
  c.field = f;
  c.value = v;
  c.expected_handle = h;
  return c;
}
Command Command::parent_to(PersistentId id, Reference p, ReparentMode m) {
  Command c;
  c.kind = CommandKind::Reparent;
  c.target = id;
  c.parent = p;
  c.reparent = m;
  return c;
}
} // namespace race::scene
