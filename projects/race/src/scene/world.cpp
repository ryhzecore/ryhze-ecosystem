#include "internal.hpp"
#include <functional>
#include <unordered_set>

namespace race::scene {
Result<void> WorldState::initialize(TrackedAllocator &allocator) {
  auto a = nodes.initialize(allocator, limits.objects);
  if (!a)
    return a;
  id_capacity = std::bit_ceil(limits.objects * 2);
  a = ids.initialize(allocator, id_capacity);
  if (!a)
    return a;
  pools.resize(schemas->types().size());
  for (auto &p : pools) {
    a = p.rows.initialize(allocator, limits.objects);
    if (!a)
      return a;
    a = p.sparse.initialize(allocator, limits.objects);
    if (!a)
      return a;
    std::fill_n(p.sparse.data(), limits.objects, none);
    pool_bytes += p.rows.bytes() + p.sparse.bytes();
  }
  pool_bytes += nodes.bytes() + ids.bytes();
  active.reserve(limits.objects);
  history.reserve(limits.history_groups);
  sections.reserve(limits.sections);
  stopped_sections.reserve(limits.sections);
  completions.reserve(limits.pending_transactions);
  commands = std::make_unique<BoundedChannel<Pending>>(limits.pending_transactions);
  return {};
}
std::uint32_t WorldState::find(PersistentId id) const noexcept {
  if (!id.valid())
    return none;
  auto bucket = IdHash{}(id) & (id_capacity - 1);
  for (std::size_t i = 0; i < id_capacity; ++i) {
    const auto &e = ids[(bucket + i) & (id_capacity - 1)];
    if (e.occupied && e.id == id)
      return e.slot;
    if (!e.occupied && !e.tombstone)
      return none;
  }
  return none;
}
void WorldState::index_insert(PersistentId id, std::uint32_t slot) noexcept {
  auto bucket = IdHash{}(id) & (id_capacity - 1);
  for (std::size_t i = 0; i < id_capacity; ++i) {
    auto &e = ids[(bucket + i) & (id_capacity - 1)];
    if (!e.occupied) {
      e = {id, slot, true, false};
      return;
    }
  }
  std::abort();
}
void WorldState::index_erase(PersistentId id) noexcept {
  auto bucket = IdHash{}(id) & (id_capacity - 1);
  for (std::size_t i = 0; i < id_capacity; ++i) {
    auto &e = ids[(bucket + i) & (id_capacity - 1)];
    if (e.occupied && e.id == id) {
      e.occupied = false;
      e.tombstone = true;
      return;
    }
  }
  std::abort();
}
Result<std::uint32_t> WorldState::resolve(ObjectHandle h) const {
  auto r = handles.resolve(h);
  if (!r)
    return r.error();
  if (h.slot >= limits.objects || !nodes[h.slot].alive || nodes[h.slot].handle != h)
    return fail(ErrorCode::StaleHandle, "stale world handle");
  return h.slot;
}
const ComponentRow *WorldState::row(std::uint32_t slot, TypeId type) const noexcept {
  auto i = schemas->index(type);
  if (!i)
    return nullptr;
  const auto &p = pools[i.value()];
  auto at = p.sparse[slot];
  return at == none ? nullptr : &p.rows[at];
}
Result<Object> WorldState::snapshot(std::uint32_t slot) const {
  try {
    const auto &n = nodes[slot];
    Object o{n.id, n.section, {}, unpack_binding(n.binding, *schemas)};
    for (std::size_t t = 0; t < pools.size(); ++t) {
      const auto &p = pools[t];
      auto at = p.sparse[slot];
      if (at == none)
        continue;
      const auto &schema = schemas->types()[t];
      Component c{schema.id, schema.version, {}};
      c.fields.reserve(schema.fields.size());
      for (std::size_t f = 0; f < schema.fields.size(); ++f)
        c.fields.push_back({schema.fields[f].id, p.rows[at].values[f]});
      o.components.push_back(std::move(c));
    }
    return o;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "object snapshot allocation");
  }
}
SectionRecord *WorldState::section(PersistentId id) noexcept {
  for (auto &s : sections)
    if (s.info.definition.id == id)
      return &s;
  return nullptr;
}
const SectionRecord *WorldState::section(PersistentId id) const noexcept {
  for (auto &s : sections)
    if (s.info.definition.id == id)
      return &s;
  return nullptr;
}
bool WorldState::view_gate_closed() const noexcept {
  if (stopping || publication_pending || execution_pending)
    return true;
  for (auto &s : sections)
    if (s.info.state == SectionState::Unloading)
      return true;
  return false;
}
Result<void> WorldState::owner_check() const noexcept {
  if (owner != std::this_thread::get_id())
    return fail(ErrorCode::WrongThread, "world mutation requires owner thread");
  return {};
}
namespace {
struct PlannedTransform {
  Affine matrix;
  std::uint32_t depth{};
};
void mark_override(Object &o, TypeId t, FieldId f) {
  if (!o.binding)
    return;
  auto &v = o.binding->overrides;
  if (!f)
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto &x) { return x.type == t; }), v.end());
  else if (std::find(v.begin(), v.end(), Override{t, 0}) != v.end())
    return;
  if (std::find(v.begin(), v.end(), Override{t, f}) == v.end())
    v.push_back({t, f});
}
Command replacement(Object o) {
  Command c;
  c.kind = CommandKind::Replace;
  c.target = o.id;
  c.object = std::move(o);
  return c;
}
} // namespace
Result<void> WorldState::apply_locked(const Transaction &transaction, bool record_history,
                                      bool overrides, bool bypass_gate, bool administrative,
                                      const std::vector<SectionRecord> *candidate_sections) {
  auto own = owner_check();
  if (!own)
    return own;
  if (stopping && !administrative)
    return fail(ErrorCode::Stopping, "world stopping");
  if (readers)
    return fail(ErrorCode::Busy, "protected readers retain current generation");
  for (auto &s : sections)
    if (!administrative && s.info.state == SectionState::Unloading)
      return fail(ErrorCode::Busy, "section unloading blocks publication");
  if (!bypass_gate && view_gate_closed())
    return fail(ErrorCode::Busy, "publication already pending");
  auto admitted = transaction_bytes(transaction, limits);
  if (!admitted)
    return admitted.error();
  if (transaction.commands.empty())
    return {};
  if (generation == UINT64_MAX)
    return fail(ErrorCode::Overflow, "world generation exhausted");
  try {
    std::vector<Delta> delta;
    delta.reserve(std::min(transaction.commands.size(), limits.objects * 2));
    std::unordered_map<PersistentId, std::size_t, IdHash> index;
    index.reserve(delta.capacity());
    auto edit = [&](PersistentId id) -> Result<std::size_t> {
      if (!id.valid())
        return fail(ErrorCode::InvalidInput, "zero object identity");
      if (auto at = index.find(id); at != index.end())
        return at->second;
      if (delta.size() >= limits.objects * 2)
        return fail(ErrorCode::Exhausted, "transaction identity limit");
      std::optional<Object> before;
      auto slot = find(id);
      if (slot != none) {
        auto o = snapshot(slot);
        if (!o)
          return o.error();
        before = std::move(o).value();
      }
      auto at = delta.size();
      delta.push_back({id, before, std::move(before)});
      index.emplace(id, at);
      return at;
    };
    // Candidate lookup is owned and only used during planning. Published arrays
    // are never modified by validation or by a failed transaction.
    auto candidate = [&](PersistentId id) -> Result<Object> {
      if (auto at = index.find(id); at != index.end()) {
        auto &o = delta[at->second].after;
        if (!o)
          return fail(ErrorCode::NotFound, "object absent in candidate");
        return *o;
      }
      auto slot = find(id);
      if (slot == none)
        return fail(ErrorCode::NotFound, "object missing");
      return snapshot(slot);
    };
    auto matrix_before_command = [&](PersistentId id) -> Result<Affine> {
      std::vector<Object> chain;
      chain.reserve(std::min(limits.hierarchy_depth, std::size_t{32}));
      std::unordered_set<PersistentId, IdHash> seen;
      while (id.valid()) {
        if (chain.size() >= limits.hierarchy_depth)
          return fail(ErrorCode::Exhausted, "hierarchy depth limit");
        if (!seen.insert(id).second)
          return fail(ErrorCode::Cycle, "hierarchy cycle");
        auto o = candidate(id);
        if (!o)
          return o.error();
        auto *c = component(o.value(), transform_type);
        if (!c)
          return fail(ErrorCode::InvalidDependency, "parent needs Transform");
        auto p = parent_reference(*c);
        if (p.object.valid()) {
          auto po = candidate(p.object);
          if (!po)
            return po.error();
          if (po.value().section != (p.section.valid() ? p.section : o.value().section))
            return fail(ErrorCode::InvalidDependency, "parent section mismatch");
        }
        chain.push_back(std::move(o).value());
        id = p.object;
      }
      Affine result;
      for (auto i = chain.rbegin(); i != chain.rend(); ++i) {
        auto local = affine(local_transform(*component(*i, transform_type)));
        if (!local)
          return local.error();
        auto combined = compose(result, local.value());
        if (!combined)
          return combined.error();
        result = combined.value();
      }
      return result;
    };
    for (const auto &cmd : transaction.commands) {
      auto at = edit(cmd.target);
      if (!at)
        return at.error();
      auto &d = delta[at.value()];
      if (cmd.expected_handle) {
        auto r = resolve(*cmd.expected_handle);
        if (!r)
          return r.error();
        if (nodes[r.value()].id != cmd.target)
          return fail(ErrorCode::StaleHandle, "command handle identity mismatch");
      }
      if (cmd.kind == CommandKind::Create) {
        if (d.after || d.before)
          return fail(ErrorCode::Duplicate,
                      "create requires a fresh absent identity in this transaction");
        if (cmd.object.id != cmd.target)
          return fail(ErrorCode::InvalidInput, "create identity mismatch");
        d.after = cmd.object;
      } else {
        if (!d.after)
          return fail(ErrorCode::NotFound, "edit target absent");
        auto &o = *d.after;
        if (cmd.kind == CommandKind::Destroy) {
          d.after.reset();
          continue;
        }
        if (cmd.kind == CommandKind::Replace) {
          if (cmd.object.id != cmd.target)
            return fail(ErrorCode::InvalidInput, "replace identity mismatch");
          o = cmd.object;
        } else if (cmd.kind == CommandKind::PutComponent) {
          auto *c = component(o, cmd.component.type);
          if (c)
            *c = cmd.component;
          else
            o.components.push_back(cmd.component);
          if (overrides)
            mark_override(o, cmd.component.type, 0);
        } else if (cmd.kind == CommandKind::RemoveComponent) {
          auto it = std::find_if(o.components.begin(), o.components.end(),
                                 [&](auto &c) { return c.type == cmd.type; });
          if (it == o.components.end())
            return fail(ErrorCode::NotFound, "component missing");
          o.components.erase(it);
          if (overrides)
            mark_override(o, cmd.type, 0);
        } else if (cmd.kind == CommandKind::SetField) {
          auto *c = component(o, cmd.type);
          if (!c)
            return fail(ErrorCode::NotFound, "component missing");
          auto v = schemas->validate(cmd.type, cmd.field, cmd.value);
          if (!v)
            return v;
          auto f = std::find_if(c->fields.begin(), c->fields.end(),
                                [&](auto &x) { return x.id == cmd.field; });
          if (f == c->fields.end())
            c->fields.push_back({cmd.field, cmd.value});
          else
            f->value = cmd.value;
          if (overrides)
            mark_override(o, cmd.type, cmd.field);
        } else if (cmd.kind == CommandKind::Reparent) {
          if (cmd.reparent != ReparentMode::PreserveLocal &&
              cmd.reparent != ReparentMode::PreserveWorld)
            return fail(ErrorCode::InvalidInput, "unknown reparent mode");
          auto *c = component(o, transform_type);
          if (!c)
            return fail(ErrorCode::NotFound, "Transform missing");
          LocalTransform local = local_transform(*c);
          if (cmd.reparent == ReparentMode::PreserveWorld) {
            auto previous = matrix_before_command(o.id);
            if (!previous)
              return previous.error();
            Affine parent;
            if (cmd.parent.object.valid()) {
              auto p = matrix_before_command(cmd.parent.object);
              if (!p)
                return p.error();
              parent = p.value();
            }
            auto inv = inverse(parent);
            if (!inv)
              return inv.error();
            auto relative = compose(inv.value(), previous.value());
            if (!relative)
              return relative.error();
            auto trs = decompose(relative.value());
            if (!trs)
              return trs.error();
            local = trs.value();
          }
          *c = transform_component(local, cmd.parent);
          if (overrides) {
            mark_override(o, transform_type, parent_field);
            if (cmd.reparent == ReparentMode::PreserveWorld) {
              mark_override(o, transform_type, translation_field);
              mark_override(o, transform_type, rotation_field);
              mark_override(o, transform_type, scale_field);
            }
          }
        } else
          return fail(ErrorCode::InvalidInput, "unknown command");
      }
      if (d.after) {
        auto n = normalize(*d.after, *schemas, limits);
        if (!n)
          return n;
      }
    }
    delta.erase(
        std::remove_if(delta.begin(), delta.end(), [](auto &d) { return d.before == d.after; }),
        delta.end());
    if (delta.empty())
      return {};
    index.clear();
    for (std::size_t i = 0; i < delta.size(); ++i)
      index.emplace(delta[i].id, i);
    std::size_t object_count = active.size(), component_count = components;
    std::vector<ObjectHandle> erased;
    std::vector<std::shared_ptr<ObjectIdentity>> owners;
    std::vector<std::size_t> inserted;
    std::vector<PackedBinding> bindings;
    bindings.reserve(delta.size());
    std::unordered_set<PersistentId, IdHash> dirty;
    dirty.reserve(std::min(limits.objects, delta.size() * 2));
    for (std::size_t i = 0; i < delta.size(); ++i) {
      auto &d = delta[i];
      if (d.before) {
        --object_count;
        component_count -= d.before->components.size();
      }
      if (d.after) {
        ++object_count;
        component_count += d.after->components.size();
        if (d.after->section.valid() && !administrative) {
          auto *s = section(d.after->section);
          if (!s || s->info.state != SectionState::Active)
            return fail(ErrorCode::InvalidDependency, "object section is not active");
        }
      }
      if (d.before && !d.after)
        erased.push_back(nodes[find(d.id)].handle);
      if (!d.before && d.after) {
        owners.push_back(std::make_shared<ObjectIdentity>(ObjectIdentity{d.id}));
        inserted.push_back(i);
      }
      auto b = pack_binding(d.after ? d.after->binding : std::optional<Binding>{}, *schemas);
      if (!b)
        return b.error();
      bindings.push_back(b.value());
      auto *before = d.before ? component(*d.before, transform_type) : nullptr;
      auto *after = d.after ? component(*d.after, transform_type) : nullptr;
      if ((d.before && d.after && d.before->section != d.after->section) ||
          (before == nullptr) != (after == nullptr) || (before && after && *before != *after)) {
        dirty.insert(d.id);
        auto slot = find(d.id);
        if (slot != none) {
          std::vector<std::uint32_t> stack;
          for (auto c = nodes[slot].first_child; c != none; c = nodes[c].next)
            stack.push_back(c);
          while (!stack.empty()) {
            auto c = stack.back();
            stack.pop_back();
            if (!dirty.insert(nodes[c].id).second)
              continue;
            for (auto child = nodes[c].first_child; child != none; child = nodes[child].next)
              stack.push_back(child);
          }
        }
      }
    }
    if (object_count > limits.objects || component_count > limits.components)
      return fail(ErrorCode::Exhausted, "world capacity exceeded");
    std::unordered_map<PersistentId, PlannedTransform, IdHash> matrices;
    matrices.reserve(dirty.size());
    std::unordered_set<PersistentId, IdHash> visiting;
    std::function<Result<PlannedTransform>(PersistentId, std::size_t)> derive =
        [&](PersistentId id, std::size_t recursion) -> Result<PlannedTransform> {
      if (recursion >= limits.hierarchy_depth)
        return fail(ErrorCode::Exhausted, "hierarchy depth limit");
      if (auto at = matrices.find(id); at != matrices.end())
        return at->second;
      auto slot = find(id);
      if (!dirty.contains(id) && slot != none) {
        if (!row(slot, transform_type))
          return fail(ErrorCode::InvalidDependency, "parent needs Transform");
        return PlannedTransform{nodes[slot].derived, nodes[slot].depth};
      }
      if (!visiting.insert(id).second)
        return fail(ErrorCode::Cycle, "hierarchy cycle");
      auto o = candidate(id);
      if (!o)
        return fail(ErrorCode::InvalidDependency, "hierarchy references missing object");
      auto *c = component(o.value(), transform_type);
      if (!c)
        return fail(ErrorCode::InvalidDependency, "parent needs Transform");
      auto local = affine(local_transform(*c));
      if (!local)
        return local.error();
      PlannedTransform out{local.value(), 1};
      auto p = parent_reference(*c);
      if (p.object.valid()) {
        auto po = candidate(p.object);
        if (!po)
          return fail(ErrorCode::InvalidDependency, "parent missing");
        if (po.value().section != (p.section.valid() ? p.section : o.value().section))
          return fail(ErrorCode::InvalidDependency, "parent section mismatch");
        if (po.value().section != o.value().section) {
          const SectionRecord *s = nullptr;
          if (candidate_sections) {
            for (auto &section : *candidate_sections)
              if (section.info.definition.id == o.value().section)
                s = &section;
          } else
            s = section(o.value().section);
          if (!s ||
              std::find(s->info.definition.required.begin(), s->info.definition.required.end(),
                        po.value().section) == s->info.definition.required.end())
            return fail(ErrorCode::InvalidDependency,
                        "cross-section parent requires retention dependency");
        }
        auto parent = derive(p.object, recursion + 1);
        if (!parent)
          return parent.error();
        out.depth = parent.value().depth + 1;
        if (out.depth > limits.hierarchy_depth)
          return fail(ErrorCode::Exhausted, "hierarchy depth limit");
        auto composed = compose(parent.value().matrix, local.value());
        if (!composed)
          return composed.error();
        out.matrix = composed.value();
      }
      visiting.erase(id);
      matrices.emplace(id, out);
      return out;
    };
    for (auto id : dirty) {
      auto o = candidate(id);
      if (!o) {
        if (o.error().code == ErrorCode::NotFound)
          continue;
        return o.error();
      }
      if (component(o.value(), transform_type)) {
        auto m = derive(id, 0);
        if (!m)
          return m.error();
      }
    }
    std::size_t planned_bytes = delta.capacity() * sizeof(Delta);
    for (auto &d : delta) {
      if (d.before)
        planned_bytes += object_bytes(*d.before);
      if (d.after)
        planned_bytes += object_bytes(*d.after);
    }
    if (planned_bytes > limits.transaction_bytes)
      return fail(ErrorCode::Exhausted, "normalized transaction byte limit");
    std::vector<std::pair<std::size_t, std::vector<PersistentId>>> manifests;
    if (!administrative) {
      for (std::size_t si = 0; si < sections.size(); ++si) {
        auto &s = sections[si];
        if (s.info.state != SectionState::Active)
          continue;
        bool affected = false;
        for (auto &d : delta)
          if ((d.before && d.before->section == s.info.definition.id) ||
              (d.after && d.after->section == s.info.definition.id)) {
            affected = true;
            break;
          }
        if (!affected)
          continue;
        auto manifest_ids = s.info.manifest;
        for (auto &d : delta) {
          if (d.before && d.before->section == s.info.definition.id)
            manifest_ids.erase(std::remove(manifest_ids.begin(), manifest_ids.end(), d.id), manifest_ids.end());
          if (d.after && d.after->section == s.info.definition.id)
            manifest_ids.push_back(d.id);
        }
        manifests.emplace_back(si, std::move(manifest_ids));
      }
      for (auto &d : delta)
        if (!d.before && d.after)
          for (auto &s : sections)
            if (s.info.state != SectionState::Active &&
                std::find(s.info.manifest.begin(), s.info.manifest.end(), d.id) !=
                    s.info.manifest.end())
              return fail(ErrorCode::Duplicate, "identity reserved by unloaded section");
    }
    if (!administrative) {
      std::size_t manifest_total = 0;
      for (std::size_t i = 0; i < sections.size(); ++i) {
        auto at =
            std::find_if(manifests.begin(), manifests.end(), [&](auto &p) { return p.first == i; });
        auto count = at == manifests.end() ? sections[i].info.manifest.size() : at->second.size();
        if (count > limits.objects - manifest_total)
          return fail(ErrorCode::Exhausted, "total section manifest capacity");
        manifest_total += count;
      }
    }
    auto bindings_valid = validate_candidate_bindings(*this, delta);
    if (!bindings_valid)
      return bindings_valid;
    auto section_exists = [&](PersistentId id) {
      if (candidate_sections) {
        for (auto &s : *candidate_sections)
          if (s.info.definition.id == id)
            return true;
        return false;
      }
      return section(id) != nullptr;
    };
    auto reference_valid = [&](PersistentId source_section, TypeId type, FieldId field_id,
                               const Reference &r) -> Result<void> {
      if (!r.object.valid())
        return {};
      if (r.section.valid() && !section_exists(r.section))
        return fail(ErrorCode::InvalidDependency, "reference names undeclared section");
      std::optional<PersistentId> target_section;
      if (auto at = index.find(r.object); at != index.end()) {
        if (delta[at->second].after)
          target_section = delta[at->second].after->section;
      } else {
        auto slot = find(r.object);
        if (slot != none)
          target_section = nodes[slot].section;
      }
      if (!target_section)
        return {};
      auto expected = r.section;
      if (type == transform_type && field_id == parent_field && !expected.valid())
        expected = source_section;
      if (*target_section != expected)
        return fail(ErrorCode::InvalidDependency, "reference target section mismatch");
      return {};
    };
    bool targets_changed = false;
    for (auto &d : delta) {
      if (d.after) {
        for (auto &c : d.after->components)
          for (auto &f : c.fields)
            if (auto *r = std::get_if<Reference>(&f.value)) {
              auto checked = reference_valid(d.after->section, c.type, f.id, *r);
              if (!checked)
                return checked;
            }
        if (!d.before || d.before->section != d.after->section)
          targets_changed = true;
      }
    }
    // Incoming links to newly available or moved identities also need checking.
    // Scan compact reference columns without copying unchanged authored objects.
    if (targets_changed)
      for (std::size_t t = 0; t < pools.size(); ++t) {
        const auto &schema = schemas->types()[t];
        const auto &p = pools[t];
        for (std::size_t f = 0; f < schema.fields.size(); ++f)
          if (std::holds_alternative<Reference>(schema.fields[f].default_value))
            for (std::size_t row = 0; row < p.count; ++row) {
              const auto &n = nodes[p.rows[row].object.slot];
              if (index.contains(n.id))
                continue;
              const auto &reference = std::get<Reference>(p.rows[row].values[f]);
              if (!index.contains(reference.object))
                continue;
              auto checked = reference_valid(n.section, schema.id, schema.fields[f].id, reference);
              if (!checked)
                return checked;
            }
      }
    History entry;
    bool keep = record_history && transaction.mode == EditMode::Authoring;
    if (keep) {
      entry.delta = delta;
      entry.bytes = entry.delta.capacity() * sizeof(Delta);
      for (auto &d : entry.delta) {
        if (d.before)
          entry.bytes += object_bytes(*d.before);
        if (d.after)
          entry.bytes += object_bytes(*d.after);
      }
      std::size_t retained = 0;
      for (std::size_t i = 0; i < cursor; ++i)
        retained += history[i].bytes;
      if (cursor >= limits.history_groups || entry.bytes > limits.history_bytes - retained)
        return fail(ErrorCode::Exhausted, "bounded undo history full");
    }
    // All allocations and candidate checks precede the atomic handle change.
    // ObjectIdentity is a trivial marker; it never invokes user destruction.
    Result<std::vector<ObjectHandle>> replaced =
        erased.empty() && owners.empty()
            ? Result<std::vector<ObjectHandle>>(std::vector<ObjectHandle>{})
            : handles.replace_batch(erased, owners);
    if (!replaced)
      return replaced.error();
    auto unlink = [&](std::uint32_t slot) {
      auto &n = nodes[slot];
      if (n.parent != none) {
        if (n.previous == none)
          nodes[n.parent].first_child = n.next;
        else
          nodes[n.previous].next = n.next;
        if (n.next != none)
          nodes[n.next].previous = n.previous;
      }
      n.parent = n.previous = n.next = none;
    };
    for (auto &d : delta)
      if (d.before) {
        auto slot = find(d.id);
        unlink(slot);
        for (auto &p : pools) {
          auto at = p.sparse[slot];
          if (at != none) {
            --p.count;
            if (at != p.count) {
              p.rows[at] = p.rows[p.count];
              p.sparse[p.rows[at].object.slot] = at;
            }
            p.sparse[slot] = none;
          }
        }
        if (!d.after) {
          auto &n = nodes[slot];
          auto at = n.active_index;
          active[at] = active.back();
          nodes[active[at].slot].active_index = at;
          active.pop_back();
          index_erase(d.id);
          n.alive = false;
        }
      }
    for (std::size_t i = 0; i < inserted.size(); ++i) {
      auto &d = delta[inserted[i]];
      auto h = replaced.value()[i];
      nodes[h.slot] = Node{};
      auto &n = nodes[h.slot];
      n.alive = true;
      n.handle = h;
      n.id = d.id;
      n.active_index = static_cast<std::uint32_t>(active.size());
      active.push_back(h);
      index_insert(d.id, h.slot);
    }
    for (std::size_t i = 0; i < delta.size(); ++i)
      if (auto &o = delta[i].after) {
        auto slot = find(o->id);
        auto &n = nodes[slot];
        n.section = o->section;
        n.binding = bindings[i];
        for (auto &c : o->components) {
          auto t = schemas->index(c.type).value();
          auto &p = pools[t];
          auto &row = p.rows[p.count];
          row.object = n.handle;
          for (std::size_t f = 0; f < c.fields.size(); ++f)
            row.values[f] = c.fields[f].value;
          p.sparse[slot] = static_cast<std::uint32_t>(p.count++);
        }
      }
    for (auto &d : delta)
      if (d.after) {
        auto slot = find(d.id);
        auto &n = nodes[slot];
        if (auto *c = component(*d.after, transform_type)) {
          auto p = parent_reference(*c);
          if (p.object.valid()) {
            n.parent = find(p.object);
            n.next = nodes[n.parent].first_child;
            if (n.next != none)
              nodes[n.next].previous = slot;
            nodes[n.parent].first_child = slot;
          }
        }
      }
    for (auto &[id, m] : matrices) {
      auto slot = find(id);
      nodes[slot].derived = m.matrix;
      nodes[slot].depth = m.depth;
    }
    for (auto &[si, manifest] : manifests)
      sections[si].info.manifest.swap(manifest);
    components = component_count;
    ++generation;
    ++publications;
    recomputations += matrices.size();
    touched += delta.size();
    if (keep) {
      while (history.size() > cursor) {
        history_bytes -= history.back().bytes;
        history.pop_back();
      }
      history_bytes += entry.bytes;
      history.push_back(std::move(entry));
      cursor = history.size();
    }
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "transaction planning allocation");
  }
}
World::World(std::shared_ptr<WorldState> state) : state_(std::move(state)) {}
Result<std::unique_ptr<World>> World::create(TrackedAllocator &allocator,
                                             std::shared_ptr<const Schemas> schemas, Limits limits,
                                             std::shared_ptr<const Templates> templates) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  if (!schemas)
    return fail(ErrorCode::InvalidInput, "schemas required");
  try {
    auto state = std::make_shared<WorldState>(std::move(schemas), limits, std::move(templates));
    auto ready = state->initialize(allocator);
    if (!ready)
      return ready.error();
    return std::unique_ptr<World>(new World(std::move(state)));
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "world allocation");
  }
}
World::~World() {
  std::lock_guard lock(state_->mutex);
  state_->stopping = true;
  state_->commands->close();
}
Result<ReadView> World::read() const {
  std::lock_guard lock(state_->mutex);
  if (state_->view_gate_closed())
    return fail(state_->stopping ? ErrorCode::Stopping : ErrorCode::Busy,
                "world reader admission closed");
  if (state_->readers >= state_->limits.views)
    return fail(ErrorCode::Exhausted, "view limit");
  ++state_->readers;
  return ReadView(state_);
}
ReadView::ReadView(std::shared_ptr<WorldState> s) : state_(std::move(s)) {}
ReadView::~ReadView() { release(); }
ReadView::ReadView(ReadView &&o) noexcept : state_(std::move(o.state_)) {}
ReadView &ReadView::operator=(ReadView &&o) noexcept {
  if (this != &o) {
    release();
    state_ = std::move(o.state_);
  }
  return *this;
}
void ReadView::release() noexcept {
  if (!state_)
    return;
  auto retained = std::move(state_);
  {
    std::lock_guard lock(retained->mutex);
    --retained->readers;
  }
  retained->readers_done.notify_all();
}
std::uint64_t ReadView::generation() const noexcept { return state_ ? state_->generation : 0; }
Result<ObjectHandle> ReadView::find(PersistentId id) const {
  if (!state_)
    return fail(ErrorCode::StaleHandle, "released view");
  auto s = state_->find(id);
  if (s == none)
    return fail(ErrorCode::NotFound, "object missing");
  return state_->nodes[s].handle;
}
Result<Object> ReadView::object(ObjectHandle h) const {
  if (!state_)
    return fail(ErrorCode::StaleHandle, "released view");
  auto s = state_->resolve(h);
  if (!s)
    return s.error();
  return state_->snapshot(s.value());
}
Result<Value> ReadView::value(ObjectHandle h, TypeId t, FieldId f) const {
  if (!state_)
    return fail(ErrorCode::StaleHandle, "released view");
  auto s = state_->resolve(h);
  if (!s)
    return s.error();
  auto fi = state_->schemas->field_index(t, f);
  if (!fi)
    return fi.error();
  auto *r = state_->row(s.value(), t);
  if (!r)
    return fail(ErrorCode::NotFound, "component missing");
  return r->values[fi.value()];
}
Result<Affine> ReadView::world_transform(ObjectHandle h) const {
  if (!state_)
    return fail(ErrorCode::StaleHandle, "released view");
  auto s = state_->resolve(h);
  if (!s)
    return s.error();
  if (!state_->row(s.value(), transform_type))
    return fail(ErrorCode::NotFound, "Transform missing");
  return state_->nodes[s.value()].derived;
}
Result<std::span<const ComponentRow>> ReadView::query(TypeId t) const {
  if (!state_)
    return fail(ErrorCode::StaleHandle, "released view");
  auto i = state_->schemas->index(t);
  if (!i)
    return i.error();
  const auto &p = state_->pools[i.value()];
  return std::span<const ComponentRow>(p.rows.data(), p.count);
}
std::span<const ObjectHandle> ReadView::objects() const noexcept {
  return state_ ? std::span<const ObjectHandle>(state_->active) : std::span<const ObjectHandle>{};
}
LocalOrigin ReadView::origin() const noexcept {
  return state_ ? state_->origin : LocalOrigin{{}, 0};
}
Result<void> World::apply(const Transaction &t) {
  std::lock_guard lock(state_->mutex);
  return state_->apply_locked(t);
}
Result<std::uint64_t> World::enqueue(Transaction t) {
  auto size = transaction_bytes(t, state_->limits);
  if (!size)
    return size.error();
  std::lock_guard lock(state_->mutex);
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  if (state_->outstanding >= state_->limits.pending_transactions ||
      size.value() > state_->limits.pending_bytes - state_->pending_bytes)
    return fail(ErrorCode::QueueFull, "bounded command queue full");
  if (state_->next_ticket == UINT64_MAX)
    return fail(ErrorCode::Overflow, "command ticket exhausted");
  auto ticket = state_->next_ticket;
  Pending p{ticket, std::move(t), size.value()};
  auto result = state_->commands->send(std::move(p));
  if (!result)
    return result.error();
  ++state_->next_ticket;
  ++state_->outstanding;
  ++state_->queued;
  state_->pending_bytes += size.value();
  state_->publication_pending = true;
  return ticket;
}
Result<void> World::publish_pending() {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own;
  if (state_->readers)
    return fail(ErrorCode::Busy, "readers retain current generation");
  while (state_->queued) {
    auto next = state_->commands->receive();
    if (!next || !next.value())
      return fail(ErrorCode::Invariant, "command queue accounting");
    auto p = std::move(*next.value());
    --state_->queued;
    state_->pending_bytes -= p.bytes;
    Result<void> r = state_->stopping ? Result<void>(fail(ErrorCode::Cancelled, "world stopping"))
                                      : state_->apply_locked(p.transaction, true, true, true);
    state_->completions.push_back({p.ticket, r});
  }
  state_->publication_pending = false;
  return {};
}
Result<CommandCompletion> World::take_completion(std::uint64_t ticket) {
  std::lock_guard lock(state_->mutex);
  auto &v = state_->completions;
  auto it = std::find_if(v.begin(), v.end(), [&](auto &c) { return c.ticket == ticket; });
  if (it == v.end())
    return fail(ErrorCode::NotFound, "completion not available");
  auto c = *it;
  v.erase(it);
  --state_->outstanding;
  return c;
}
namespace {
Result<void> history_step(WorldState &s, bool redo) {
  auto own = s.owner_check();
  if (!own)
    return own;
  if ((redo && s.cursor == s.history.size()) || (!redo && !s.cursor))
    return fail(ErrorCode::NotFound, "no history entry");
  try {
    const auto &entry = s.history[redo ? s.cursor : s.cursor - 1];
    Transaction t;
    t.commands.reserve(entry.delta.size());
    for (auto &d : entry.delta) {
      const auto &expected = redo ? d.before : d.after;
      const auto &desired = redo ? d.after : d.before;
      auto slot = s.find(d.id);
      if (expected) {
        if (slot == none)
          return fail(ErrorCode::Busy, "history conflicts with current object");
        auto current = s.snapshot(slot);
        if (!current)
          return current.error();
        if (current.value() != *expected)
          return fail(ErrorCode::Busy, "history conflicts with current values");
      } else if (slot != none)
        return fail(ErrorCode::Busy, "history conflicts with current identity");
      if (desired)
        t.commands.push_back(expected ? replacement(*desired) : Command::create(*desired));
      else
        t.commands.push_back(Command::destroy(d.id));
    }
    auto r = s.apply_locked(t, false, false);
    if (r) {
      if (redo)
        ++s.cursor;
      else
        --s.cursor;
    }
    return r;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "history planning allocation");
  }
}
} // namespace
Result<void> World::undo() {
  std::lock_guard lock(state_->mutex);
  return history_step(*state_, false);
}
Result<void> World::redo() {
  std::lock_guard lock(state_->mutex);
  return history_step(*state_, true);
}
Result<void> World::clear_history() {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own;
  state_->history.clear();
  state_->history_bytes = state_->cursor = 0;
  return {};
}
Result<void> World::set_origin(Vec3 position) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own;
  if (!finite(position))
    return fail(ErrorCode::InvalidInput, "origin must be finite");
  if (state_->readers || state_->view_gate_closed())
    return fail(ErrorCode::Busy, "origin publication blocked");
  if (state_->origin.revision == UINT64_MAX || state_->generation == UINT64_MAX)
    return fail(ErrorCode::Overflow, "origin revision exhausted");
  state_->origin.position = position;
  ++state_->origin.revision;
  ++state_->generation;
  return {};
}
WorldStats World::stats() const {
  std::lock_guard lock(state_->mutex);
  const auto &s = *state_;
  std::size_t metadata =
      sizeof(WorldState) + s.active.capacity() * sizeof(ObjectHandle) +
      s.pools.capacity() * sizeof(Pool) + s.history.capacity() * sizeof(History) +
      s.sections.capacity() * sizeof(SectionRecord) +
      s.limits.pending_transactions * (sizeof(Pending) + sizeof(CommandCompletion)) +
      s.limits.objects * (sizeof(std::shared_ptr<ObjectIdentity>) + sizeof(ObjectIdentity) + 32);
  return {s.active.size(), s.components,     s.readers,        s.readers ? 1U : 0U, s.queued,
          s.pending_bytes, s.history.size(), s.history_bytes,  s.prepared_bytes,    s.pool_bytes,
          metadata,        s.generation,     s.recomputations, s.touched,           s.publications};
}
} // namespace race::scene
