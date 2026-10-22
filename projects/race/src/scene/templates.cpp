#include "internal.hpp"
#include <functional>
#include <unordered_set>
namespace race::scene {
const TemplateRevision *Templates::find(TemplateKey key) const noexcept {
  for (auto &r : revisions_)
    if (r.key == key)
      return &r;
  return nullptr;
}
Result<std::vector<Expanded>> expand_template(const Templates &catalog, TemplateKey key,
                                              const Schemas &schemas, Limits limits) {
  try {
    std::vector<Expanded> output;
    std::vector<TemplateKey> stack;
    std::size_t bytes = 0;
    // Temporary linkage IDs never collide with any authored identity/reference.
    // No range of the public PersistentId space is reserved.
    std::unordered_set<PersistentId, IdHash> used;
    for (auto &r : catalog.revisions())
      for (auto &o : r.objects) {
        used.insert(o.id);
        for (auto &c : o.components)
          for (auto &f : c.fields)
            if (auto *ref = std::get_if<Reference>(&f.value))
              used.insert(ref->object);
      }
    std::uint64_t temporary = 1;
    auto local_id = [&]() {
      PersistentId candidate;
      do {
        candidate = {UINT64_MAX, temporary++};
      } while (used.contains(candidate));
      used.insert(candidate);
      return candidate;
    };
    std::function<Result<void>(TemplateKey, TemplatePath, PersistentId)> expand =
        [&](TemplateKey current, TemplatePath prefix, PersistentId anchor) -> Result<void> {
      if (prefix.length > limits.template_depth)
        return fail(ErrorCode::Exhausted, "nested template depth");
      if (std::find(stack.begin(), stack.end(), current) != stack.end())
        return fail(ErrorCode::Cycle, "nested template cycle");
      auto *revision = catalog.find(current);
      if (!revision)
        return fail(ErrorCode::InvalidDependency, "template revision dependency missing");
      stack.push_back(current);
      if (revision->objects.size() > limits.template_objects - output.size())
        return fail(ErrorCode::Exhausted, "expanded template object limit");
      std::unordered_map<PersistentId, PersistentId, IdHash> remap;
      for (auto &o : revision->objects)
        remap.emplace(o.id, local_id());
      for (auto object : revision->objects) {
        auto original_id = object.id;
        object.id = remap.at(original_id);
        object.binding.reset();
        for (auto &c : object.components)
          for (auto &f : c.fields)
            if (auto *p = std::get_if<Reference>(&f.value);
                p && !p->section.valid() && p->object.valid()) {
              if (auto it = remap.find(p->object); it != remap.end())
                p->object = it->second;
            }
        if (anchor.valid())
          if (auto *c = component(object, transform_type)) {
            auto parent = parent_reference(*c);
            if (!parent.object.valid())
              for (auto &f : c->fields)
                if (f.id == parent_field)
                  f.value = Reference{{}, anchor};
          }
        auto path = prefix;
        path.nodes[path.length++] = original_id;
        bytes += object_bytes(object) + sizeof(Expanded);
        if (bytes > limits.document_bytes)
          return fail(ErrorCode::Exhausted, "expanded template byte limit");
        output.push_back({std::move(object), path});
      }
      for (auto &nested : revision->nested) {
        auto next = prefix;
        if (next.length >= limits.template_depth)
          return fail(ErrorCode::Exhausted, "nested template depth");
        next.nodes[next.length++] = nested.anchor;
        auto result = expand(nested.dependency, next, remap.at(nested.anchor));
        if (!result)
          return result;
      }
      stack.pop_back();
      return {};
    };
    auto r = expand(key, {}, {});
    if (!r)
      return r.error();
    for (auto &e : output) {
      auto valid = normalize(e.object, schemas, limits);
      if (!valid)
        return valid.error();
    }
    return output;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "template expansion allocation");
  }
}
Result<std::shared_ptr<const Templates>> Templates::create(std::vector<TemplateRevision> revisions,
                                                           const Schemas &schemas, Limits limits) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  if (revisions.size() > limits.template_revisions)
    return fail(ErrorCode::Exhausted, "template revision capacity");
  try {
    std::size_t bytes = revisions.capacity() * sizeof(TemplateRevision);
    if (bytes > limits.document_bytes)
      return fail(ErrorCode::Exhausted, "template catalog byte budget");
    for (std::size_t i = 0; i < revisions.size(); ++i) {
      auto &r = revisions[i];
      if (!r.key.id.valid() || !r.key.revision || r.objects.empty())
        return fail(ErrorCode::InvalidInput, "invalid template revision");
      for (std::size_t j = 0; j < i; ++j)
        if (revisions[j].key == r.key)
          return fail(ErrorCode::Duplicate, "duplicate template revision");
      if (r.objects.size() > limits.template_objects || r.nested.size() > limits.template_objects)
        return fail(ErrorCode::Exhausted, "template definition capacity");
      bytes += r.objects.capacity() * sizeof(Object) + r.nested.capacity() * sizeof(NestedTemplate);
      if (bytes > limits.document_bytes)
        return fail(ErrorCode::Exhausted, "template catalog byte budget");
      std::unordered_set<PersistentId, IdHash> ids, nested;
      for (auto &o : r.objects) {
        if (o.section.valid() || o.binding)
          return fail(ErrorCode::InvalidInput, "template definitions contain local objects only");
        auto n = normalize(o, schemas, limits);
        if (!n)
          return n.error();
        if (!ids.insert(o.id).second)
          return fail(ErrorCode::Duplicate, "duplicate template object identity");
        bytes += object_bytes(o);
        if (bytes > limits.document_bytes)
          return fail(ErrorCode::Exhausted, "template catalog byte budget");
      }
      for (auto &n : r.nested) {
        if (!ids.contains(n.anchor) || !n.dependency.id.valid() || !n.dependency.revision)
          return fail(ErrorCode::InvalidDependency, "invalid nested template dependency");
        if (!nested.insert(n.anchor).second)
          return fail(ErrorCode::Duplicate, "nested template anchor reused");
        auto it = std::find_if(r.objects.begin(), r.objects.end(),
                               [&](auto &o) { return o.id == n.anchor; });
        if (!component(*it, transform_type))
          return fail(ErrorCode::InvalidDependency, "nested anchor needs Transform");
      }
    }
    auto result = std::shared_ptr<const Templates>(new Templates(std::move(revisions)));
    for (auto &r : result->revisions()) {
      auto expanded = expand_template(*result, r.key, schemas, limits);
      if (!expanded)
        return expanded.error();
      Document d;
      for (auto &e : expanded.value())
        d.objects.push_back(std::move(e.object));
      auto checked = validate_document(d, schemas, limits);
      if (!checked)
        return checked.error();
    }
    return result;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "template catalog allocation");
  }
}
namespace {
struct PathHash {
  std::size_t operator()(const TemplatePath &p) const noexcept {
    std::size_t h = p.length;
    for (std::size_t i = 0; i < p.length; ++i)
      h ^= IdHash{}(p.nodes[i]) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};
Result<void> remap_expansion(std::vector<Expanded> &objects, std::span<const PersistentId> ids,
                             PersistentId instance, TemplateKey key, PersistentId section) {
  if (ids.size() != objects.size())
    return fail(ErrorCode::InvalidInput, "fresh identity count mismatch");
  std::unordered_map<PersistentId, PersistentId, IdHash> mapping;
  std::unordered_set<PersistentId, IdHash> unique;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (!ids[i].valid() || !unique.insert(ids[i]).second)
      return fail(ErrorCode::Duplicate, "fresh identities must be nonzero and unique");
    mapping.emplace(objects[i].object.id, ids[i]);
  }
  for (std::size_t i = 0; i < objects.size(); ++i) {
    auto &o = objects[i].object;
    o.id = ids[i];
    o.section = section;
    o.binding = Binding{instance, key, objects[i].path, {}};
    for (auto &c : o.components)
      for (auto &f : c.fields)
        if (auto *r = std::get_if<Reference>(&f.value); r && !r->section.valid()) {
          if (auto at = mapping.find(r->object); at != mapping.end()) {
            r->object = at->second;
            r->section = section;
          }
        }
  }
  return {};
}
Result<void> catalog_ready(const WorldState &s) {
  if (!s.templates)
    return fail(ErrorCode::NotFound, "world has no template catalog");
  return s.owner_check();
}

} // namespace
Result<void> World::instantiate(TemplateKey key, PersistentId instance,
                                std::span<const PersistentId> fresh, PersistentId section) {
  std::lock_guard lock(state_->mutex);
  auto ready = catalog_ready(*state_);
  if (!ready)
    return ready;
  if (!instance.valid())
    return fail(ErrorCode::InvalidInput, "zero instance identity");
  for (auto h : state_->active)
    if (state_->nodes[h.slot].binding.bound && state_->nodes[h.slot].binding.instance == instance)
      return fail(ErrorCode::Duplicate, "instance identity already active");
  try {
    auto expanded = expand_template(*state_->templates, key, *state_->schemas, state_->limits);
    if (!expanded)
      return expanded.error();
    for (auto id : fresh) {
      if (state_->find(id) != none)
        return fail(ErrorCode::Duplicate, "instance ID already active");
      for (auto &r : state_->templates->revisions())
        for (auto &o : r.objects)
          if (o.id == id)
            return fail(ErrorCode::Duplicate,
                        "instance IDs must differ from template-local identities");
    }
    auto remapped = remap_expansion(expanded.value(), fresh, instance, key, section);
    if (!remapped)
      return remapped;
    Transaction t;
    for (auto &e : expanded.value())
      t.commands.push_back(Command::create(std::move(e.object)));
    return state_->apply_locked(t, true, false);
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "instance planning allocation");
  }
}
Result<void> World::update_instance(PersistentId instance, TemplateKey next,
                                    std::span<const PersistentId> added) {
  std::lock_guard lock(state_->mutex);
  auto ready = catalog_ready(*state_);
  if (!ready)
    return ready;
  try {
    std::vector<Object> old;
    std::unordered_map<TemplatePath, std::size_t, PathHash> index;
    for (auto h : state_->active)
      if (state_->nodes[h.slot].binding.bound &&
          state_->nodes[h.slot].binding.instance == instance) {
        auto o = state_->snapshot(h.slot);
        if (!o)
          return o.error();
        if (!index.emplace(o.value().binding->path, old.size()).second)
          return fail(ErrorCode::InvalidInput, "duplicate instance path");
        old.push_back(std::move(o).value());
      }
    if (old.empty())
      return fail(ErrorCode::NotFound, "template instance missing");
    auto pin = old.front().binding->pinned;
    auto section = old.front().section;
    if (next.id != pin.id)
      return fail(ErrorCode::InvalidInput, "revision update must retain template identity");
    for (auto &o : old)
      if (o.binding->pinned != pin || o.section != section)
        return fail(ErrorCode::InvalidInput, "inconsistent instance binding");
    auto expanded = expand_template(*state_->templates, next, *state_->schemas, state_->limits);
    if (!expanded)
      return expanded.error();
    std::unordered_set<TemplatePath, PathHash> paths;
    std::vector<PersistentId> ids;
    std::size_t fresh = 0;
    for (auto &e : expanded.value()) {
      paths.insert(e.path);
      if (auto at = index.find(e.path); at != index.end())
        ids.push_back(old[at->second].id);
      else {
        if (fresh == added.size())
          return fail(ErrorCode::InvalidInput, "new template nodes need fresh IDs");
        if (state_->find(added[fresh]) != none)
          return fail(ErrorCode::Duplicate, "new node ID already active");
        for (auto &r : state_->templates->revisions())
          for (auto &o : r.objects)
            if (o.id == added[fresh])
              return fail(ErrorCode::Duplicate, "new node ID equals template-local identity");
        ids.push_back(added[fresh++]);
      }
    }
    if (fresh != added.size())
      return fail(ErrorCode::InvalidInput, "unused added IDs");
    auto remapped = remap_expansion(expanded.value(), ids, instance, next, section);
    if (!remapped)
      return remapped;
    auto old_base = expand_template(*state_->templates, pin, *state_->schemas, state_->limits);
    if (!old_base)
      return old_base.error();
    Transaction t;
    for (auto &o : old)
      if (!paths.contains(o.binding->path)) {
        if (!o.binding->overrides.empty())
          return fail(ErrorCode::Busy, "update removes customised template node");
        t.commands.push_back(Command::destroy(o.id));
      }
    for (auto &e : expanded.value()) {
      auto at = index.find(e.path);
      if (at == index.end()) {
        t.commands.push_back(Command::create(std::move(e.object)));
        continue;
      }
      auto &previous = old[at->second];
      auto &candidate = e.object;
      candidate.binding->overrides = previous.binding->overrides;
      for (auto override : previous.binding->overrides) {
        auto *prior = component(previous, override.type);
        auto *target = component(candidate, override.type);
        if (!override.field) {
          auto base = std::find_if(old_base.value().begin(), old_base.value().end(),
                                   [&](auto &e) { return e.path == previous.binding->path; });
          bool was_present =
              base != old_base.value().end() && component(base->object, override.type);
          if (!was_present) {
            if (target)
              return fail(ErrorCode::Busy,
                          "template introduces a component colliding with an additive override");
            if (prior)
              candidate.components.push_back(*prior);
            continue;
          }
          if (!target)
            return fail(ErrorCode::Busy, "update removes customised component");
          if (prior)
            *target = *prior;
          else
            candidate.components.erase(
                std::remove_if(candidate.components.begin(), candidate.components.end(),
                               [&](auto &c) { return c.type == override.type; }),
                candidate.components.end());
        } else {
          if (!prior || !target || !field(*prior, override.field) ||
              !field(*target, override.field))
            return fail(ErrorCode::Busy, "update removes customised field");
          for (auto &f : target->fields)
            if (f.id == override.field)
              f.value = *field(*prior, override.field);
        }
      }
      Command command = Command::create(std::move(candidate));
      command.kind = CommandKind::Replace;
      t.commands.push_back(std::move(command));
    }
    return state_->apply_locked(t, true, false);
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "template update planning allocation");
  }
}
Result<void> World::reset_override(PersistentId id, TypeId type, FieldId field_id) {
  std::lock_guard lock(state_->mutex);
  auto ready = catalog_ready(*state_);
  if (!ready)
    return ready;
  auto slot = state_->find(id);
  if (slot == none)
    return fail(ErrorCode::NotFound, "object missing");
  auto current = state_->snapshot(slot);
  if (!current)
    return current.error();
  if (!current.value().binding)
    return fail(ErrorCode::NotFound, "object has no template binding");
  try {
    auto &object = current.value();
    auto &binding = *object.binding;
    auto found =
        std::find(binding.overrides.begin(), binding.overrides.end(), Override{type, field_id});
    if (found == binding.overrides.end())
      return fail(ErrorCode::NotFound, "override absent");
    auto expanded =
        expand_template(*state_->templates, binding.pinned, *state_->schemas, state_->limits);
    if (!expanded)
      return expanded.error();
    std::unordered_map<TemplatePath, PersistentId, PathHash> ids;
    for (auto h : state_->active) {
      const auto &n = state_->nodes[h.slot];
      if (n.binding.bound && n.binding.instance == binding.instance)
        ids.emplace(n.binding.path, n.id);
    }
    std::vector<PersistentId> remap;
    for (auto &e : expanded.value()) {
      auto at = ids.find(e.path);
      if (at == ids.end())
        return fail(ErrorCode::Busy, "instance structure diverged");
      remap.push_back(at->second);
    }
    auto r =
        remap_expansion(expanded.value(), remap, binding.instance, binding.pinned, object.section);
    if (!r)
      return r;
    auto target = std::find_if(expanded.value().begin(), expanded.value().end(),
                               [&](auto &e) { return e.path == binding.path; });
    if (target == expanded.value().end())
      return fail(ErrorCode::InvalidDependency, "template path missing");
    auto *original = component(target->object, type);
    auto *actual = component(object, type);
    if (!field_id) {
      if (original) {
        if (actual)
          *actual = *original;
        else
          object.components.push_back(*original);
      } else
        object.components.erase(std::remove_if(object.components.begin(), object.components.end(),
                                               [&](auto &c) { return c.type == type; }),
                                object.components.end());
    } else {
      if (!original || !actual || !field(*original, field_id))
        return fail(ErrorCode::InvalidDependency, "template field missing");
      for (auto &f : actual->fields)
        if (f.id == field_id)
          f.value = *field(*original, field_id);
    }
    binding.overrides.erase(found);
    Command c = Command::create(std::move(object));
    c.kind = CommandKind::Replace;
    return state_->apply_locked({{std::move(c)}}, true, false);
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "override reset planning allocation");
  }
}
Result<void> World::detach(PersistentId instance) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own;
  try {
    Transaction t;
    for (auto h : state_->active)
      if (state_->nodes[h.slot].binding.bound &&
          state_->nodes[h.slot].binding.instance == instance) {
        auto o = state_->snapshot(h.slot);
        if (!o)
          return o.error();
        o.value().binding.reset();
        Command c = Command::create(std::move(o).value());
        c.kind = CommandKind::Replace;
        t.commands.push_back(std::move(c));
      }
    if (t.commands.empty())
      return fail(ErrorCode::NotFound, "instance missing");
    return state_->apply_locked(t, true, false);
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "detach planning allocation");
  }
}
} // namespace race::scene

namespace race::scene {
Result<void> validate_candidate_bindings(const WorldState &s, const std::vector<Delta> &delta) {
  struct Cache {
    TemplateKey key;
    std::unordered_set<TemplatePath, PathHash> paths;
  };
  struct Instance {
    TemplateKey key;
    PersistentId section;
    std::unordered_set<TemplatePath, PathHash> paths;
  };
  try {
    std::unordered_set<PersistentId, IdHash> affected;
    std::unordered_map<PersistentId, const Delta *, IdHash> changes;
    for (auto &d : delta) {
      changes.emplace(d.id, &d);
      if (d.before && d.before->binding)
        affected.insert(d.before->binding->instance);
      if (d.after && d.after->binding)
        affected.insert(d.after->binding->instance);
    }
    if (affected.empty())
      return {};
    std::vector<Cache> cache;
    std::unordered_map<PersistentId, Instance, IdHash> instances;
    auto check = [&](const Object &o) -> Result<void> {
      if (!o.binding)
        return {};
      const auto &b = *o.binding;
      if (!s.templates)
        return fail(ErrorCode::InvalidDependency, "bound object needs template catalog");
      auto at =
          std::find_if(cache.begin(), cache.end(), [&](auto &c) { return c.key == b.pinned; });
      if (at == cache.end()) {
        auto expanded = expand_template(*s.templates, b.pinned, *s.schemas, s.limits);
        if (!expanded)
          return expanded.error();
        Cache c{b.pinned, {}};
        for (auto &e : expanded.value())
          c.paths.insert(e.path);
        cache.push_back(std::move(c));
        at = std::prev(cache.end());
      }
      if (!at->paths.contains(b.path))
        return fail(ErrorCode::InvalidDependency, "template binding path missing");
      auto [instance, inserted] =
          instances.try_emplace(b.instance, Instance{b.pinned, o.section, {}});
      (void)inserted;
      if (instance->second.key != b.pinned || instance->second.section != o.section)
        return fail(ErrorCode::InvalidInput, "instance pin or section mismatch");
      if (!instance->second.paths.insert(b.path).second)
        return fail(ErrorCode::Duplicate, "duplicate template instance path");
      for (auto &override : b.overrides)
        if (override.field) {
          if (std::find(b.overrides.begin(), b.overrides.end(), Override{override.type, 0}) !=
              b.overrides.end())
            return fail(ErrorCode::InvalidInput, "overlapping component and field overrides");
          auto *c = component(o, override.type);
          if (!c || !field(*c, override.field))
            return fail(ErrorCode::InvalidInput, "override field value absent");
        }
      return {};
    };
    for (auto &d : delta)
      if (d.after && d.after->binding) {
        auto r = check(*d.after);
        if (!r)
          return r;
      }
    for (auto h : s.active) {
      const auto &n = s.nodes[h.slot];
      if (!n.binding.bound || !affected.contains(n.binding.instance) || changes.contains(n.id))
        continue;
      auto o = s.snapshot(h.slot);
      if (!o)
        return o.error();
      auto r = check(o.value());
      if (!r)
        return r;
    }
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "binding validation allocation");
  }
}
} // namespace race::scene
