#include "internal.hpp"
#include <functional>
#include <unordered_set>
namespace race::scene {
namespace {
Result<void> dependencies(const std::vector<SectionInfo> &sections, bool require_present) {
  std::unordered_map<PersistentId, std::size_t, IdHash> index;
  for (std::size_t i = 0; i < sections.size(); ++i) {
    auto &d = sections[i].definition;
    if (!d.id.valid() || d.required.size() > core::Scheduler::max_dependencies)
      return fail(ErrorCode::InvalidInput, "invalid section definition");
    if (!index.emplace(d.id, i).second)
      return fail(ErrorCode::Duplicate, "duplicate section identity");
    std::unordered_set<PersistentId, IdHash> seen;
    for (auto id : d.required) {
      if (!id.valid())
        return fail(ErrorCode::InvalidInput, "zero section dependency");
      if (!seen.insert(id).second)
        return fail(ErrorCode::Duplicate, "duplicate section dependency");
    }
  }
  std::vector<unsigned char> visited(sections.size());
  std::function<Result<void>(std::size_t)> visit = [&](std::size_t i) -> Result<void> {
    if (visited[i] == 1)
      return fail(ErrorCode::Cycle, "section dependency cycle");
    if (visited[i] == 2)
      return {};
    visited[i] = 1;
    for (auto id : sections[i].definition.required) {
      auto at = index.find(id);
      if (at == index.end()) {
        if (require_present)
          return fail(ErrorCode::InvalidDependency, "section dependency absent");
        continue;
      }
      auto r = visit(at->second);
      if (!r)
        return r;
    }
    visited[i] = 2;
    return {};
  };
  for (std::size_t i = 0; i < sections.size(); ++i) {
    auto r = visit(i);
    if (!r)
      return r;
  }
  return {};
}
Result<Document> snapshot_document(const WorldState &s, bool require_idle) {
  try {
    Document d;
    d.origin = s.origin;
    d.objects.reserve(s.active.size());
    for (auto h : s.active) {
      auto o = s.snapshot(h.slot);
      if (!o)
        return o.error();
      d.objects.push_back(std::move(o).value());
    }
    for (auto &section : s.sections) {
      if (require_idle &&
          (section.preparing || section.ready || section.info.state == SectionState::Unloading))
        return fail(ErrorCode::Busy, "section transition cannot be saved");
      d.sections.push_back(section.info);
    }
    return d;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "document snapshot allocation");
  }
}
Transaction replace_objects(const WorldState &s, const std::vector<Object> &objects,
                            PersistentId section, bool all) {
  Transaction t;
  t.mode = EditMode::Simulation;
  t.commands.reserve(s.active.size() + objects.size());
  std::unordered_set<PersistentId, IdHash> wanted;
  for (auto &o : objects)
    wanted.insert(o.id);
  for (auto h : s.active)
    if ((all || s.nodes[h.slot].section == section) && !wanted.contains(s.nodes[h.slot].id))
      t.commands.push_back(Command::destroy(s.nodes[h.slot].id));
  for (auto &o : objects) {
    Command c = Command::create(o);
    if (s.find(o.id) != none)
      c.kind = CommandKind::Replace;
    t.commands.push_back(std::move(c));
  }
  return t;
}
} // namespace
Result<void> validate_document(const Document &d, const Schemas &schemas, Limits limits,
                               bool activation_dependencies) {
  if (d.schema != 1)
    return fail(ErrorCode::IncompatibleVersion, "unsupported Scene schema");
  if (!d.origin.revision || !finite(d.origin.position))
    return fail(ErrorCode::InvalidInput, "invalid document origin");
  if (d.objects.size() > limits.objects || d.sections.size() > limits.sections)
    return fail(ErrorCode::Exhausted, "document object or section limit");
  try {
    auto dep = dependencies(d.sections, true);
    if (!dep)
      return dep;
    std::unordered_map<PersistentId, std::size_t, IdHash> ids;
    std::unordered_map<PersistentId, const SectionInfo *, IdHash> sections;
    std::unordered_set<PersistentId, IdHash> manifests;
    std::size_t bytes = sizeof(Document), count = 0;
    for (auto &s : d.sections) {
      if (s.state == SectionState::Preparing || s.state == SectionState::Prepared ||
          s.state == SectionState::Unloading)
        return fail(ErrorCode::InvalidInput, "transient section state in document");
      if (static_cast<unsigned>(s.state) > static_cast<unsigned>(SectionState::Cancelled))
        return fail(ErrorCode::InvalidInput, "invalid section state");
      if (s.manifest.size() > limits.objects)
        return fail(ErrorCode::Exhausted, "section manifest capacity");
      sections.emplace(s.definition.id, &s);
      bytes += sizeof(SectionInfo) + s.manifest.capacity() * sizeof(PersistentId) +
               s.definition.required.capacity() * sizeof(PersistentId);
      for (auto id : s.manifest) {
        if (!id.valid())
          return fail(ErrorCode::InvalidInput, "zero manifest identity");
        if (!manifests.insert(id).second)
          return fail(ErrorCode::Duplicate, "duplicate section manifest identity");
      }
    }
    if (bytes > limits.document_bytes)
      return fail(ErrorCode::Exhausted, "document byte capacity");
    if (manifests.size() > limits.objects)
      return fail(ErrorCode::Exhausted, "total manifest capacity");
    for (std::size_t i = 0; i < d.objects.size(); ++i) {
      const auto &o = d.objects[i];
      Object normalized = o;
      auto n = normalize(normalized, schemas, limits);
      if (!n)
        return n;
      if (normalized != o)
        return fail(ErrorCode::InvalidInput, "document component values must be normalized");
      if (!ids.emplace(o.id, i).second)
        return fail(ErrorCode::Duplicate, "duplicate authored identity");
      bytes += object_bytes(o);
      count += o.components.size();
      if (bytes > limits.document_bytes || count > limits.components)
        return fail(ErrorCode::Exhausted, "document byte or component capacity");
      if (o.section.valid()) {
        auto at = sections.find(o.section);
        if (at == sections.end() || at->second->state != SectionState::Active ||
            std::find(at->second->manifest.begin(), at->second->manifest.end(), o.id) ==
                at->second->manifest.end())
          return fail(ErrorCode::InvalidDependency, "active object section manifest mismatch");
      } else if (manifests.contains(o.id))
        return fail(ErrorCode::Duplicate, "unsectioned object duplicates reserved identity");
    }
    for (auto &s : d.sections) {
      if (s.state == SectionState::Active) {
        for (auto id : s.manifest) {
          auto at = ids.find(id);
          if (at == ids.end() || d.objects[at->second].section != s.definition.id)
            return fail(ErrorCode::InvalidDependency, "active manifest object absent");
        }
        for (auto id : s.definition.required)
          if (activation_dependencies && sections.at(id)->state != SectionState::Active)
            return fail(ErrorCode::InvalidDependency, "active section dependency unavailable");
      } else
        for (auto id : s.manifest)
          if (ids.contains(id))
            return fail(ErrorCode::Duplicate, "unloaded manifest object is active");
    }
    std::vector<unsigned char> color(d.objects.size());
    std::vector<std::size_t> depths(d.objects.size());
    std::vector<Affine> matrices(d.objects.size());
    std::function<Result<void>(std::size_t, std::size_t)> visit =
        [&](std::size_t i, std::size_t recursion) -> Result<void> {
      if (color[i] == 1)
        return fail(ErrorCode::Cycle, "document hierarchy cycle");
      if (color[i] == 2)
        return {};
      if (recursion >= limits.hierarchy_depth)
        return fail(ErrorCode::Exhausted, "document hierarchy depth");
      color[i] = 1;
      const auto &o = d.objects[i];
      auto *c = component(o, transform_type);
      if (!c) {
        color[i] = 2;
        return {};
      }
      auto m = affine(local_transform(*c));
      if (!m)
        return m.error();
      matrices[i] = m.value();
      depths[i] = 1;
      auto p = parent_reference(*c);
      if (p.object.valid()) {
        auto at = ids.find(p.object);
        if (at == ids.end())
          return fail(ErrorCode::InvalidDependency, "document parent absent");
        const auto &parent = d.objects[at->second];
        if (!component(parent, transform_type) ||
            parent.section != (p.section.valid() ? p.section : o.section))
          return fail(ErrorCode::InvalidDependency, "document parent mismatch");
        if (parent.section != o.section) {
          auto s = sections.find(o.section);
          if (s == sections.end() ||
              std::find(s->second->definition.required.begin(),
                        s->second->definition.required.end(),
                        parent.section) == s->second->definition.required.end())
            return fail(ErrorCode::InvalidDependency, "document parent retention missing");
        }
        auto r = visit(at->second, recursion + 1);
        if (!r)
          return r;
        depths[i] = depths[at->second] + 1;
        if (depths[i] > limits.hierarchy_depth)
          return fail(ErrorCode::Exhausted, "document hierarchy depth");
        auto combined = compose(matrices[at->second], m.value());
        if (!combined)
          return combined.error();
        matrices[i] = combined.value();
      }
      color[i] = 2;
      return {};
    };
    for (std::size_t i = 0; i < d.objects.size(); ++i) {
      auto r = visit(i, 0);
      if (!r)
        return r;
      for (auto &c : d.objects[i].components)
        for (auto &f : c.fields)
          if (auto *ref = std::get_if<Reference>(&f.value); ref && ref->object.valid()) {
            if (ref->section.valid() && !sections.contains(ref->section))
              return fail(ErrorCode::InvalidDependency, "reference names undeclared section");
            auto target = ids.find(ref->object);
            auto expected = ref->section;
            if (c.type == transform_type && f.id == parent_field && !expected.valid())
              expected = d.objects[i].section;
            if (target != ids.end() && d.objects[target->second].section != expected)
              return fail(ErrorCode::InvalidDependency,
                          "document reference target section mismatch");
          }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "document validation allocation");
  }
}
Result<void> World::declare_section(SectionDefinition definition) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  if (state_->section(definition.id))
    return fail(ErrorCode::Duplicate, "section already declared");
  if (state_->sections.size() == state_->limits.sections)
    return fail(ErrorCode::Exhausted, "section limit");
  try {
    std::vector<SectionInfo> candidate;
    for (auto &s : state_->sections)
      candidate.push_back(s.info);
    candidate.push_back({std::move(definition), SectionState::Declared, {}, {}});
    auto r = dependencies(candidate, false);
    if (!r)
      return r;
    SectionRecord record;
    record.info = std::move(candidate.back());
    record.info.definition.required = std::vector<PersistentId>(
        record.info.definition.required.begin(), record.info.definition.required.end());
    state_->sections.push_back(std::move(record));
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "section declaration allocation");
  }
}
Result<Preparation> World::begin_prepare(PersistentId id) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  auto *s = state_->section(id);
  if (!s)
    return fail(ErrorCode::NotFound, "section absent");
  if (s->preparing || s->ready || s->info.state == SectionState::Unloading)
    return fail(ErrorCode::Busy, "section transition pending");
  if (state_->next_preparation == UINT64_MAX)
    return fail(ErrorCode::Overflow, "preparation epoch exhausted");
  s->ticket = state_->next_preparation++;
  s->preparing = true;
  s->cancelled = false;
  s->info.last_failure.reset();
  if (s->info.state != SectionState::Active)
    s->info.state = SectionState::Preparing;
  return Preparation{id, s->ticket, state_->identity};
}
Result<void> World::cancel_prepare(Preparation p) {
  std::lock_guard lock(state_->mutex);
  auto *s = state_->section(p.section);
  if (p.world != state_->identity || !s || s->ticket != p.ticket || (!s->preparing && !s->ready))
    return fail(ErrorCode::StaleHandle, "preparation ticket stale");
  s->cancelled = true;
  s->preparing = s->ready = false;
  state_->prepared_bytes -= s->bytes;
  s->bytes = 0;
  std::vector<Object>{}.swap(s->prepared);
  if (s->info.state != SectionState::Active)
    s->info.state = SectionState::Cancelled;
  s->info.last_failure = ErrorCode::Cancelled;
  return {};
}
Result<void> World::finish_prepare(Preparation p, std::vector<Object> objects) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  auto *s = state_->section(p.section);
  if (p.world != state_->identity || !s || s->ticket != p.ticket)
    return fail(ErrorCode::StaleHandle, "preparation ticket stale");
  if (s->cancelled)
    return fail(ErrorCode::Cancelled, "preparation cancelled");
  if (!s->preparing)
    return fail(ErrorCode::StaleHandle, "preparation no longer pending");
  auto reject = [&](Error e) -> Result<void> {
    s->preparing = false;
    if (s->info.state != SectionState::Active)
      s->info.state = SectionState::Failed;
    s->info.last_failure = e.code;
    return e;
  };
  try {
    if (objects.size() > state_->limits.objects)
      return reject(fail(ErrorCode::Exhausted, "prepared object limit"));
    std::size_t bytes = objects.capacity() * sizeof(Object);
    if (bytes > state_->limits.prepared_bytes - state_->prepared_bytes)
      return reject(fail(ErrorCode::Exhausted, "prepared payload budget"));
    std::vector<PersistentId> manifest;
    manifest.reserve(objects.size());
    for (auto &o : objects) {
      if (o.section != p.section)
        return reject(fail(ErrorCode::InvalidInput, "prepared object section mismatch"));
      auto n = normalize(o, *state_->schemas, state_->limits);
      if (!n)
        return reject(n.error());
      bytes += object_bytes(o);
      manifest.push_back(o.id);
      if (bytes > state_->limits.prepared_bytes - state_->prepared_bytes)
        return reject(fail(ErrorCode::Exhausted, "prepared payload budget"));
    }
    auto document = snapshot_document(*state_, false);
    if (!document)
      return reject(document.error());
    auto &d = document.value();
    d.objects.erase(std::remove_if(d.objects.begin(), d.objects.end(),
                                   [&](auto &o) { return o.section == p.section; }),
                    d.objects.end());
    d.objects.insert(d.objects.end(), objects.begin(), objects.end());
    for (auto &section : d.sections) {
      if (section.definition.id == p.section) {
        section.state = SectionState::Active;
        section.manifest = manifest;
      } else if (section.state == SectionState::Preparing ||
                 section.state == SectionState::Prepared)
        section.state = SectionState::Unloaded;
    }
    auto valid = validate_document(d, *state_->schemas, state_->limits, false);
    if (!valid)
      return reject(valid.error());
    s->prepared = std::move(objects);
    s->bytes = bytes;
    state_->prepared_bytes += bytes;
    s->preparing = false;
    s->ready = true;
    if (s->info.state != SectionState::Active)
      s->info.state = SectionState::Prepared;
    return {};
  } catch (const std::bad_alloc &) {
    return reject(fail(ErrorCode::Exhausted, "preparation allocation"));
  }
}
Result<void> World::activate(PersistentId id) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  auto *s = state_->section(id);
  if (!s)
    return fail(ErrorCode::NotFound, "section absent");
  if (!s->ready)
    return fail(ErrorCode::Busy, "section not prepared");
  if (state_->readers || state_->view_gate_closed())
    return fail(ErrorCode::Busy, "activation safe point unavailable");
  for (auto dep : s->info.definition.required) {
    auto *d = state_->section(dep);
    if (!d || d->info.state != SectionState::Active)
      return fail(ErrorCode::InvalidDependency, "section dependency unavailable");
  }
  try {
    std::vector<PersistentId> manifest;
    for (auto &o : s->prepared) {
      auto slot = state_->find(o.id);
      if (slot != none && state_->nodes[slot].section != id)
        return fail(ErrorCode::Duplicate, "prepared identity acquired by another owner");
      manifest.push_back(o.id);
    }
    auto snapshot = snapshot_document(*state_, false);
    if (!snapshot)
      return snapshot.error();
    auto &candidate = snapshot.value();
    candidate.objects.erase(std::remove_if(candidate.objects.begin(), candidate.objects.end(),
                                           [&](auto &o) { return o.section == id; }),
                            candidate.objects.end());
    candidate.objects.insert(candidate.objects.end(), s->prepared.begin(), s->prepared.end());
    for (auto &section : candidate.sections) {
      if (section.definition.id == id) {
        section.state = SectionState::Active;
        section.manifest = manifest;
      } else if (section.state == SectionState::Preparing ||
                 section.state == SectionState::Prepared)
        section.state = SectionState::Unloaded;
    }
    auto checked = validate_document(candidate, *state_->schemas, state_->limits);
    if (!checked)
      return checked;
    auto t = replace_objects(*state_, s->prepared, id, false);
    auto result = state_->apply_locked(t, false, false, true, true);
    if (!result) {
      s->info.last_failure = result.error().code;
      return result;
    }
    s->info.manifest.swap(manifest);
    s->info.state = SectionState::Active;
    s->info.last_failure.reset();
    s->ready = false;
    state_->prepared_bytes -= s->bytes;
    s->bytes = 0;
    std::vector<Object>{}.swap(s->prepared);
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "activation planning allocation");
  }
}
Result<void> World::unload(PersistentId id, std::chrono::milliseconds timeout) {
  if (timeout.count() < 0)
    return fail(ErrorCode::InvalidInput, "negative unload timeout");
  std::unique_lock lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  auto *s = state_->section(id);
  if (!s)
    return fail(ErrorCode::NotFound, "section absent");
  if (s->preparing || s->ready)
    return fail(ErrorCode::Busy, "section preparation owns payload");
  if (s->info.state == SectionState::Unloaded)
    return {};
  if (s->info.state != SectionState::Active && s->info.state != SectionState::Unloading)
    return fail(ErrorCode::Busy, "section is not active");
  for (auto &dependent : state_->sections)
    if ((dependent.info.state == SectionState::Active ||
         dependent.info.state == SectionState::Unloading) &&
        std::find(dependent.info.definition.required.begin(),
                  dependent.info.definition.required.end(),
                  id) != dependent.info.definition.required.end())
      return fail(ErrorCode::Busy, "dependent section retains provider");
  if (timeout == std::chrono::milliseconds::max() && state_->frame_inflight)
    return fail(ErrorCode::Busy, "drain Scene execution before indefinite unload");
  s->info.state = SectionState::Unloading;
  bool ready;
  if (timeout == std::chrono::milliseconds::max()) {
    state_->readers_done.wait(lock, [&] { return !state_->readers; });
    ready = true;
  } else
    ready = state_->readers_done.wait_for(lock, timeout, [&] { return !state_->readers; });
  if (!ready)
    return fail(ErrorCode::Timeout, "readers retain unloading section");
  try {
    auto t = replace_objects(*state_, {}, id, false);
    auto result = state_->apply_locked(t, false, false, true, true);
    if (!result) {
      s->info.last_failure = result.error().code;
      return result;
    }
    s->info.state = SectionState::Unloaded;
    s->info.last_failure.reset();
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "unload planning allocation");
  }
}
Result<SectionInfo> World::section(PersistentId id) const {
  std::lock_guard lock(state_->mutex);
  auto *s = state_->section(id);
  if (!s)
    return fail(ErrorCode::NotFound, "section absent");
  try {
    return s->info;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "section snapshot allocation");
  }
}
ResolvedReference World::resolve(Reference reference) const {
  std::lock_guard lock(state_->mutex);
  if (!reference.object.valid())
    return {ReferenceState::Invalid, {}};
  auto slot = state_->find(reference.object);
  if (slot != none) {
    if (state_->nodes[slot].section != reference.section)
      return {ReferenceState::Invalid, {}};
    return {ReferenceState::Available, state_->nodes[slot].handle};
  }
  if (reference.section.valid()) {
    auto *s = state_->section(reference.section);
    if (!s)
      return {ReferenceState::Invalid, {}};
    if (std::find(s->info.manifest.begin(), s->info.manifest.end(), reference.object) !=
            s->info.manifest.end() &&
        s->info.state != SectionState::Active)
      return {ReferenceState::Unloaded, {}};
  }
  return {ReferenceState::Missing, {}};
}
Result<Document> World::document() const {
  std::lock_guard lock(state_->mutex);
  return snapshot_document(*state_, true);
}
Result<void> World::load(const Document &input) {
  std::lock_guard lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  if (state_->stopping)
    return fail(ErrorCode::Stopping, "world stopping");
  if (state_->readers || state_->view_gate_closed())
    return fail(ErrorCode::Busy, "load safe point unavailable");
  for (auto &s : state_->sections)
    if (s.preparing || s.ready)
      return fail(ErrorCode::Busy, "preparation pending");
  if (state_->origin.revision == UINT64_MAX || state_->generation == UINT64_MAX)
    return fail(ErrorCode::Overflow, "load epoch exhausted");
  auto valid = validate_document(input, *state_->schemas, state_->limits);
  if (!valid)
    return valid;
  try {
    std::vector<SectionRecord> candidate;
    candidate.reserve(state_->limits.sections);
    for (auto &s : input.sections) {
      SectionRecord record;
      record.info = s;
      candidate.push_back(std::move(record));
    }
    auto t = replace_objects(*state_, input.objects, {}, true);
    auto generation = state_->generation;
    auto r = state_->apply_locked(t, false, false, true, true, &candidate);
    if (!r)
      return r;
    state_->sections.swap(candidate);
    state_->origin = {input.origin.position, state_->origin.revision + 1};
    if (state_->generation == generation)
      ++state_->generation;
    state_->history.clear();
    state_->history_bytes = state_->cursor = 0;
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "load candidate allocation");
  }
}
Result<void> World::stop(std::chrono::milliseconds timeout) {
  if (timeout.count() < 0)
    return fail(ErrorCode::InvalidInput, "negative stop timeout");
  std::unique_lock lock(state_->mutex);
  auto own = state_->owner_check();
  if (!own)
    return own.error();
  state_->stopping = true;
  state_->commands->close();
  if (timeout == std::chrono::milliseconds::max() && state_->frame_inflight)
    return fail(ErrorCode::Busy, "drain Scene execution before indefinite stop");
  bool ready;
  if (timeout == std::chrono::milliseconds::max()) {
    state_->readers_done.wait(lock, [&] { return !state_->readers; });
    ready = true;
  } else
    ready = state_->readers_done.wait_for(lock, timeout, [&] { return !state_->readers; });
  if (!ready)
    return fail(ErrorCode::Timeout, "world readers retain live storage");
  try {
    std::vector<PersistentId> order;
    order.reserve(state_->sections.size());
    std::unordered_set<PersistentId, IdHash> visited;
    std::function<void(const SectionRecord &)> visit = [&](const SectionRecord &s) {
      if (!visited.insert(s.info.definition.id).second)
        return;
      for (auto dep : s.info.definition.required)
        if (auto *d = state_->section(dep))
          visit(*d);
      order.push_back(s.info.definition.id);
    };
    for (auto &s : state_->sections)
      visit(s);
    std::reverse(order.begin(), order.end());
    auto erased = state_->active;
    auto r = state_->handles.replace_batch(erased, {});
    if (!r)
      return r.error();
    for (auto id : order) {
      auto *s = state_->section(id);
      if (s->info.state == SectionState::Active || s->info.state == SectionState::Unloading) {
        state_->stopped_sections.push_back(id);
        s->info.state = SectionState::Unloaded;
      }
      if (s->info.state == SectionState::Preparing || s->info.state == SectionState::Prepared)
        s->info.state = SectionState::Cancelled;
      s->preparing = s->ready = false;
      s->cancelled = true;
      s->bytes = 0;
      std::vector<Object>{}.swap(s->prepared);
    }
    for (auto h : state_->active) {
      state_->index_erase(state_->nodes[h.slot].id);
      state_->nodes[h.slot] = Node{};
    }
    state_->active.clear();
    for (auto &p : state_->pools) {
      p.count = 0;
      std::fill_n(p.sparse.data(), state_->limits.objects, none);
    }
    state_->components = state_->prepared_bytes = 0;
    state_->history.clear();
    state_->cursor = state_->history_bytes = 0;
    while (state_->queued) {
      auto next = state_->commands->receive();
      auto p = std::move(*next.value());
      state_->pending_bytes -= p.bytes;
      --state_->queued;
      state_->completions.push_back({p.ticket, fail(ErrorCode::Cancelled, "world stopped")});
    }
    state_->publication_pending = false;
    return {};
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "stop planning allocation");
  }
}
std::vector<PersistentId> World::teardown_order() const {
  std::lock_guard lock(state_->mutex);
  return state_->stopped_sections;
}
} // namespace race::scene
