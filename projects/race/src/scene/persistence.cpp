#include "internal.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_set>
namespace race::scene {
namespace {
// Parser errors are local control flow; none escape the public Result boundary.
struct ParseFailure {
  Error error;
};
[[noreturn]] void invalid(const char *message) {
  throw ParseFailure{fail(ErrorCode::InvalidInput, message)};
}
[[noreturn]] void exhausted(const char *message) {
  throw ParseFailure{fail(ErrorCode::Exhausted, message)};
}
struct Json {
  enum Kind { Null, Boolean, Number, String, Array, Object } kind{Null};
  bool boolean{};
  std::string text, key;
  std::vector<Json> array, object;
};
class Parser {
  std::string_view input_;
  std::size_t at_{}, charged_{}, limit_;
  void charge(std::size_t n) {
    if (n > limit_ - charged_)
      exhausted("JSON parser storage bound");
    charged_ += n;
  }
  void ws() {
    while (at_ < input_.size() && (input_[at_] == ' ' || input_[at_] == '\n' ||
                                   input_[at_] == '\r' || input_[at_] == '\t'))
      ++at_;
  }
  bool take(char c) {
    ws();
    if (at_ < input_.size() && input_[at_] == c) {
      ++at_;
      return true;
    }
    return false;
  }
  std::string string() {
    if (!take('"'))
      invalid("JSON string expected");
    std::string out;
    while (at_ < input_.size()) {
      unsigned char c = input_[at_++];
      if (c == '"') {
        charge(out.capacity() + 1);
        return out;
      }
      if (c < 32)
        invalid("control character in JSON string");
      if (c == '\\') {
        if (at_ == input_.size())
          invalid("truncated JSON escape");
        char e = input_[at_++];
        if (e == '"' || e == '\\' || e == '/')
          c = static_cast<unsigned char>(e);
        else if (e == 'b')
          c = '\b';
        else if (e == 'f')
          c = '\f';
        else if (e == 'n')
          c = '\n';
        else if (e == 'r')
          c = '\r';
        else if (e == 't')
          c = '\t';
        else if (e == 'u') {
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            if (at_ == input_.size())
              invalid("truncated unicode escape");
            char h = input_[at_++];
            unsigned v = h >= '0' && h <= '9'   ? h - '0'
                         : h >= 'a' && h <= 'f' ? h - 'a' + 10
                         : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                : 16;
            if (v == 16)
              invalid("invalid unicode escape");
            code = code * 16 + v;
          }
          if (code > 127)
            invalid("Scene JSON keys and identities are ASCII");
          c = static_cast<unsigned char>(code);
        } else
          invalid("invalid JSON escape");
      }
      if (c > 127)
        invalid("Scene JSON strings are ASCII");
      if (out.size() == 256)
        exhausted("JSON string length limit");
      out.push_back(static_cast<char>(c));
    }
    invalid("unterminated JSON string");
  }
  Json value(std::size_t depth) {
    ws();
    if (depth > 32)
      exhausted("JSON nesting limit");
    if (at_ == input_.size())
      invalid("truncated JSON value");
    charge(2 * sizeof(Json) + 2 * sizeof(std::string));
    Json out;
    char c = input_[at_];
    if (c == '{') {
      ++at_;
      out.kind = Json::Object;
      if (take('}'))
        return out;
      for (;;) {
        auto key = string();
        if (std::find_if(out.object.begin(), out.object.end(),
                         [&](auto &pair) { return pair.key == key; }) != out.object.end())
          invalid("duplicate JSON key");
        if (!take(':'))
          invalid("JSON colon expected");
        auto child = value(depth + 1);
        child.key = std::move(key);
        out.object.push_back(std::move(child));
        if (take('}'))
          return out;
        if (!take(','))
          invalid("JSON object separator expected");
      }
    }
    if (c == '[') {
      ++at_;
      out.kind = Json::Array;
      if (take(']'))
        return out;
      for (;;) {
        out.array.push_back(value(depth + 1));
        if (take(']'))
          return out;
        if (!take(','))
          invalid("JSON array separator expected");
      }
    }
    if (c == '"') {
      out.kind = Json::String;
      out.text = string();
      return out;
    }
    for (auto literal :
         {std::string_view("null"), std::string_view("true"), std::string_view("false")})
      if (input_.substr(at_, literal.size()) == literal) {
        at_ += literal.size();
        if (literal != "null") {
          out.kind = Json::Boolean;
          out.boolean = literal == "true";
        }
        return out;
      }
    auto first = at_;
    if (input_[at_] == '-')
      ++at_;
    if (at_ == input_.size())
      invalid("invalid JSON number");
    if (input_[at_] == '0')
      ++at_;
    else {
      if (input_[at_] < '1' || input_[at_] > '9')
        invalid("invalid JSON value");
      while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9')
        ++at_;
    }
    if (at_ < input_.size() && input_[at_] == '.') {
      ++at_;
      auto begin = at_;
      while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9')
        ++at_;
      if (at_ == begin)
        invalid("invalid number fraction");
    }
    if (at_ < input_.size() && (input_[at_] == 'e' || input_[at_] == 'E')) {
      ++at_;
      if (at_ < input_.size() && (input_[at_] == '+' || input_[at_] == '-'))
        ++at_;
      auto begin = at_;
      while (at_ < input_.size() && input_[at_] >= '0' && input_[at_] <= '9')
        ++at_;
      if (at_ == begin)
        invalid("invalid number exponent");
    }
    if (at_ - first > 64)
      invalid("JSON number too long");
    out.kind = Json::Number;
    out.text = input_.substr(first, at_ - first);
    charge(out.text.capacity());
    return out;
  }

public:
  Parser(std::string_view input, std::size_t limit) : input_(input), limit_(limit) {}
  Json parse() {
    auto v = value(0);
    ws();
    if (at_ != input_.size())
      invalid("trailing JSON data");
    return v;
  }
};
const Json &member(const Json &j, std::string_view key) {
  if (j.kind != Json::Object)
    invalid("JSON object expected");
  for (auto &v : j.object)
    if (v.key == key)
      return v;
  invalid("required JSON member missing");
}
void keys(const Json &j, std::initializer_list<std::string_view> allowed) {
  if (j.kind != Json::Object)
    invalid("JSON object expected");
  for (auto &v : j.object) {
    if (std::find(allowed.begin(), allowed.end(), v.key) == allowed.end())
      invalid("unknown JSON member");
  }
}
const std::vector<Json> &array(const Json &j, std::size_t maximum) {
  if (j.kind != Json::Array)
    invalid("JSON array expected");
  if (j.array.size() > maximum)
    exhausted("JSON array capacity");
  return j.array;
}
template <class T> T integer(const Json &j) {
  if (j.kind != Json::Number)
    invalid("integer required");
  T result{};
  auto r = std::from_chars(j.text.data(), j.text.data() + j.text.size(), result);
  if (r.ec != std::errc{} || r.ptr != j.text.data() + j.text.size())
    invalid("invalid or overflowing integer");
  return result;
}
double scalar(const Json &j) {
  if (j.kind != Json::Number)
    invalid("number required");
  double result{};
  auto r = std::from_chars(j.text.data(), j.text.data() + j.text.size(), result);
  if (r.ec != std::errc{} || r.ptr != j.text.data() + j.text.size() || !std::isfinite(result))
    invalid("non-finite or overflowing number");
  return result;
}
std::string id_text(PersistentId id) {
  char out[33]{};
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 16; ++i) {
    out[i] = hex[(id.high >> ((15 - i) * 4)) & 15];
    out[16 + i] = hex[(id.low >> ((15 - i) * 4)) & 15];
  }
  return std::string(out, 32);
}
PersistentId persistent(const Json &j) {
  if (j.kind != Json::String || j.text.size() != 32)
    invalid("128-bit hexadecimal identity required");
  PersistentId id;
  for (unsigned i = 0; i < 32; ++i) {
    char c = j.text[i];
    unsigned v = c >= '0' && c <= '9'   ? c - '0'
                 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                        : 16;
    if (v == 16)
      invalid("invalid hexadecimal identity");
    auto &half = i < 16 ? id.high : id.low;
    half = (half << 4) | v;
  }
  return id;
}
Vec3 vector(const Json &j) {
  auto &a = array(j, 3);
  if (a.size() != 3)
    invalid("three vector coordinates required");
  return {scalar(a[0]), scalar(a[1]), scalar(a[2])};
}
Value json_value(const Json &j, std::size_t kind) {
  switch (kind) {
  case 0:
    return integer<std::int64_t>(j);
  case 1:
    return scalar(j);
  case 2:
    if (j.kind != Json::Boolean)
      invalid("boolean required");
    return j.boolean;
  case 3:
    return vector(j);
  case 4: {
    auto &a = array(j, 4);
    if (a.size() != 4)
      invalid("four quaternion coordinates required");
    return Quat{scalar(a[0]), scalar(a[1]), scalar(a[2]), scalar(a[3])};
  }
  case 5:
    keys(j, {"section", "object"});
    return Reference{persistent(member(j, "section")), persistent(member(j, "object"))};
  case 6:
    return persistent(j);
  default:
    invalid("unknown field kind");
  }
}
const char *const kinds[]{"integer",    "scalar",    "boolean", "vector",
                          "quaternion", "reference", "asset"};
const char *const states[]{"declared",  "preparing", "prepared", "active",
                           "unloading", "unloaded",  "failed",   "cancelled"};
std::size_t named(const Json &j, const char *const *names, std::size_t count) {
  if (j.kind != Json::String)
    invalid("named value required");
  for (std::size_t i = 0; i < count; ++i)
    if (j.text == names[i])
      return i;
  invalid("unknown named value");
}
Document json_document(const Json &j, const Schemas &schemas, Limits limits) {
  keys(j, {"schema", "origin", "sections", "objects"});
  Document d;
  d.schema = integer<std::uint32_t>(member(j, "schema"));
  if (d.schema != 1)
    throw ParseFailure{fail(ErrorCode::IncompatibleVersion, "unsupported Scene schema")};
  d.origin = {vector(member(j, "origin")), 1};
  for (auto &value : array(member(j, "sections"), limits.sections)) {
    keys(value, {"id", "required", "state", "objects"});
    SectionInfo s;
    s.definition.id = persistent(member(value, "id"));
    s.state = static_cast<SectionState>(named(member(value, "state"), states, 8));
    for (auto &dep : array(member(value, "required"), core::Scheduler::max_dependencies))
      s.definition.required.push_back(persistent(dep));
    for (auto &id : array(member(value, "objects"), limits.objects))
      s.manifest.push_back(persistent(id));
    d.sections.push_back(std::move(s));
  }
  for (auto &value : array(member(j, "objects"), limits.objects)) {
    keys(value, {"id", "section", "components", "template"});
    Object o;
    o.id = persistent(member(value, "id"));
    o.section = persistent(member(value, "section"));
    for (auto &c : array(member(value, "components"), max_types)) {
      keys(c, {"type", "version", "fields"});
      Component component;
      component.type = integer<TypeId>(member(c, "type"));
      component.version = integer<std::uint32_t>(member(c, "version"));
      if (!schemas.find(component.type))
        throw ParseFailure{fail(ErrorCode::Unsupported, "unknown component type")};
      for (auto &f : array(member(c, "fields"), max_fields)) {
        keys(f, {"id", "kind", "value"});
        component.fields.push_back(
            {integer<FieldId>(member(f, "id")),
             json_value(member(f, "value"), named(member(f, "kind"), kinds, 7))});
      }
      o.components.push_back(std::move(component));
    }
    auto &binding = member(value, "template");
    if (binding.kind != Json::Null) {
      keys(binding, {"instance", "id", "revision", "path", "overrides"});
      Binding b;
      b.instance = persistent(member(binding, "instance"));
      b.pinned = {persistent(member(binding, "id")),
                  integer<std::uint64_t>(member(binding, "revision"))};
      for (auto &node : array(member(binding, "path"), limits.template_depth + 1))
        b.path.nodes[b.path.length++] = persistent(node);
      for (auto &v : array(member(binding, "overrides"), max_types * (max_fields + 1))) {
        auto &pair = array(v, 2);
        if (pair.size() != 2)
          invalid("override needs type and field identity");
        b.overrides.push_back({integer<TypeId>(pair[0]), integer<FieldId>(pair[1])});
      }
      o.binding = std::move(b);
    }
    auto n = normalize(o, schemas, limits);
    if (!n)
      throw ParseFailure{n.error()};
    d.objects.push_back(std::move(o));
  }
  return d;
}
void write_id(std::ostream &out, PersistentId id) { out << '"' << id_text(id) << '"'; }
void write_vec(std::ostream &out, Vec3 v) {
  out << '[' << v.x << ", " << v.y << ", " << v.z << ']';
}
void write_value(std::ostream &out, const Value &v) {
  switch (v.index()) {
  case 0:
    out << std::get<std::int64_t>(v);
    break;
  case 1:
    out << std::get<double>(v);
    break;
  case 2:
    out << (std::get<bool>(v) ? "true" : "false");
    break;
  case 3:
    write_vec(out, std::get<Vec3>(v));
    break;
  case 4: {
    auto q = std::get<Quat>(v);
    out << '[' << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ']';
    break;
  }
  case 5: {
    auto r = std::get<Reference>(v);
    out << "{\"section\": ";
    write_id(out, r.section);
    out << ", \"object\": ";
    write_id(out, r.object);
    out << '}';
    break;
  }
  case 6:
    write_id(out, std::get<PersistentId>(v));
    break;
  }
}
Document canonical(Document d) {
  std::sort(d.objects.begin(), d.objects.end(),
            [](auto &a, auto &b) { return id_less(a.id, b.id); });
  std::sort(d.sections.begin(), d.sections.end(),
            [](auto &a, auto &b) { return id_less(a.definition.id, b.definition.id); });
  for (auto &s : d.sections) {
    std::sort(s.definition.required.begin(), s.definition.required.end(), id_less);
    std::sort(s.manifest.begin(), s.manifest.end(), id_less);
    s.last_failure.reset();
  }
  return d;
}
struct BinaryWriter {
  std::vector<std::byte> bytes;
  std::size_t limit;
  explicit BinaryWriter(std::size_t l) : limit(l) {}
  void u(std::uint64_t value, unsigned width) {
    if (width > limit - bytes.size())
      exhausted("cooked byte limit");
    for (unsigned i = 0; i < width; ++i)
      bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
  }
  void real(double d) {
    static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
    u(std::bit_cast<std::uint64_t>(d), 8);
  }
  void id(PersistentId id) {
    u(id.high, 8);
    u(id.low, 8);
  }
  void vec(Vec3 v) {
    real(v.x);
    real(v.y);
    real(v.z);
  }
  void text(std::string_view v) {
    u(v.size(), 2);
    for (unsigned char c : v)
      u(c, 1);
  }
  void value(const Value &v) {
    u(v.index(), 1);
    switch (v.index()) {
    case 0:
      u(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(v)), 8);
      break;
    case 1:
      real(std::get<double>(v));
      break;
    case 2:
      u(std::get<bool>(v), 1);
      break;
    case 3:
      vec(std::get<Vec3>(v));
      break;
    case 4: {
      auto q = std::get<Quat>(v);
      real(q.x);
      real(q.y);
      real(q.z);
      real(q.w);
      break;
    }
    case 5: {
      auto r = std::get<Reference>(v);
      id(r.section);
      id(r.object);
      break;
    }
    case 6:
      id(std::get<PersistentId>(v));
      break;
    }
  }
};
struct BinaryReader {
  std::span<const std::byte> bytes;
  std::size_t at{};
  std::uint64_t u(unsigned width) {
    if (width > bytes.size() - at)
      invalid("truncated cooked field");
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i)
      n |= std::uint64_t(std::to_integer<unsigned>(bytes[at++])) << (8 * i);
    return n;
  }
  std::size_t count(unsigned width, std::size_t maximum) {
    auto n = u(width);
    if (n > maximum)
      exhausted("cooked count exceeds capacity");
    return static_cast<std::size_t>(n);
  }
  double real() {
    auto n = std::bit_cast<double>(u(8));
    if (!std::isfinite(n))
      invalid("non-finite cooked value");
    return n;
  }
  PersistentId id() {
    auto high = u(8);
    auto low = u(8);
    return {high, low};
  }
  Vec3 vec() {
    auto x = real(), y = real(), z = real();
    return {x, y, z};
  }
  Value value() {
    auto kind = u(1);
    switch (kind) {
    case 0:
      return std::bit_cast<std::int64_t>(u(8));
    case 1:
      return real();
    case 2: {
      auto b = u(1);
      if (b > 1)
        invalid("invalid cooked boolean");
      return b != 0;
    }
    case 3:
      return vec();
    case 4: {
      auto x = real(), y = real(), z = real(), w = real();
      return Quat{x, y, z, w};
    }
    case 5: {
      auto section = id();
      auto object = id();
      return Reference{section, object};
    }
    case 6:
      return id();
    default:
      invalid("unknown cooked value kind");
    }
  }
};
std::vector<std::byte> schema_descriptor(const Schemas &schemas) {
  BinaryWriter w(128 * 1024);
  w.u(schemas.types().size(), 2);
  for (auto &t : schemas.types()) {
    w.u(t.id, 8);
    w.u(t.version, 4);
    w.text(t.name);
    w.u(t.fields.size(), 2);
    for (auto &f : t.fields) {
      w.u(f.id, 8);
      w.text(f.name);
      w.value(f.default_value);
      w.real(f.minimum);
      w.real(f.maximum);
      w.u(f.exposed, 1);
    }
  }
  return std::move(w.bytes);
}
void binary_payload(BinaryWriter &w, const Document &d) {
  w.vec(d.origin.position);
  w.u(d.sections.size(), 4);
  w.u(d.objects.size(), 4);
  for (auto &s : d.sections) {
    w.id(s.definition.id);
    w.u(static_cast<unsigned>(s.state), 1);
    w.u(s.definition.required.size(), 2);
    for (auto id : s.definition.required)
      w.id(id);
    w.u(s.manifest.size(), 4);
    for (auto id : s.manifest)
      w.id(id);
  }
  for (auto &o : d.objects) {
    w.id(o.id);
    w.id(o.section);
    w.u(o.components.size(), 2);
    for (auto &c : o.components) {
      w.u(c.type, 8);
      w.u(c.version, 4);
      w.u(c.fields.size(), 2);
      for (auto &f : c.fields) {
        w.u(f.id, 8);
        w.value(f.value);
      }
    }
    w.u(o.binding.has_value(), 1);
    if (o.binding) {
      auto &b = *o.binding;
      w.id(b.instance);
      w.id(b.pinned.id);
      w.u(b.pinned.revision, 8);
      w.u(b.path.length, 1);
      for (std::size_t i = 0; i < b.path.length; ++i)
        w.id(b.path.nodes[i]);
      w.u(b.overrides.size(), 2);
      for (auto &v : b.overrides) {
        w.u(v.type, 8);
        w.u(v.field, 8);
      }
    }
  }
}
Document binary_document(BinaryReader &r, const Schemas &schemas, Limits limits) {
  Document d;
  d.origin = {r.vec(), 1};
  auto section_count = r.count(4, limits.sections), object_count = r.count(4, limits.objects);
  for (std::size_t i = 0; i < section_count; ++i) {
    SectionInfo s;
    s.definition.id = r.id();
    auto state = r.u(1);
    if (state > 7)
      invalid("unknown cooked section state");
    s.state = static_cast<SectionState>(state);
    auto dependencies = r.count(2, core::Scheduler::max_dependencies);
    for (std::size_t j = 0; j < dependencies; ++j)
      s.definition.required.push_back(r.id());
    auto count = r.count(4, limits.objects);
    for (std::size_t j = 0; j < count; ++j)
      s.manifest.push_back(r.id());
    d.sections.push_back(std::move(s));
  }
  for (std::size_t i = 0; i < object_count; ++i) {
    Object o;
    o.id = r.id();
    o.section = r.id();
    auto types = r.count(2, max_types);
    for (std::size_t j = 0; j < types; ++j) {
      Component c;
      c.type = r.u(8);
      c.version = static_cast<std::uint32_t>(r.u(4));
      if (!schemas.find(c.type))
        throw ParseFailure{fail(ErrorCode::Unsupported, "unknown component type")};
      auto fields = r.count(2, max_fields);
      for (std::size_t k = 0; k < fields; ++k) {
        auto id = r.u(8);
        c.fields.push_back({id, r.value()});
      }
      o.components.push_back(std::move(c));
    }
    auto bound = r.u(1);
    if (bound > 1)
      invalid("invalid cooked binding flag");
    if (bound) {
      Binding b;
      b.instance = r.id();
      b.pinned.id = r.id();
      b.pinned.revision = r.u(8);
      b.path.length = static_cast<std::uint8_t>(r.count(1, limits.template_depth + 1));
      for (std::size_t j = 0; j < b.path.length; ++j)
        b.path.nodes[j] = r.id();
      auto overrides = r.count(2, max_types * (max_fields + 1));
      for (std::size_t j = 0; j < overrides; ++j) {
        auto type = r.u(8), field = r.u(8);
        b.overrides.push_back({type, field});
      }
      o.binding = std::move(b);
    }
    auto n = normalize(o, schemas, limits);
    if (!n)
      throw ParseFailure{n.error()};
    d.objects.push_back(std::move(o));
  }
  if (r.at != r.bytes.size())
    invalid("trailing cooked payload");
  return d;
}
bool source_valid(std::string_view s) {
  if (s.empty() || s.size() > 128)
    return false;
  for (unsigned char c : s)
    if (c < 33 || c > 126)
      return false;
  return true;
}
} // namespace
Result<std::string> encode_json(const Document &input, const Schemas &schemas, Limits limits) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  valid = validate_document(input, schemas, limits);
  if (!valid)
    return valid.error();
  try {
    auto d = canonical(input);
    std::ostringstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << "{\n  \"schema\": 1,\n  \"origin\": ";
    write_vec(out, d.origin.position);
    out << ",\n  \"sections\": [";
    bool comma = false;
    for (auto &s : d.sections) {
      if (comma)
        out << ',';
      comma = true;
      out << "\n    {\"id\": ";
      write_id(out, s.definition.id);
      out << ", \"required\": [";
      for (std::size_t i = 0; i < s.definition.required.size(); ++i) {
        if (i)
          out << ", ";
        write_id(out, s.definition.required[i]);
      }
      out << "], \"state\": \"" << states[static_cast<unsigned>(s.state)] << "\", \"objects\": [";
      for (std::size_t i = 0; i < s.manifest.size(); ++i) {
        if (i)
          out << ", ";
        write_id(out, s.manifest[i]);
      }
      out << "]}";
    }
    out << "\n  ],\n  \"objects\": [";
    comma = false;
    for (auto &o : d.objects) {
      if (comma)
        out << ',';
      comma = true;
      out << "\n    {\"id\": ";
      write_id(out, o.id);
      out << ", \"section\": ";
      write_id(out, o.section);
      out << ", \"components\": [";
      for (std::size_t i = 0; i < o.components.size(); ++i) {
        auto &c = o.components[i];
        if (i)
          out << ',';
        out << "\n      {\"type\": " << c.type << ", \"version\": " << c.version
            << ", \"fields\": [";
        for (std::size_t j = 0; j < c.fields.size(); ++j) {
          auto &f = c.fields[j];
          if (j)
            out << ", ";
          out << "{\"id\": " << f.id << ", \"kind\": \"" << kinds[f.value.index()]
              << "\", \"value\": ";
          write_value(out, f.value);
          out << '}';
        }
        out << "]}";
      }
      out << "\n    ], \"template\": ";
      if (!o.binding)
        out << "null";
      else {
        auto &b = *o.binding;
        out << "{\"instance\": ";
        write_id(out, b.instance);
        out << ", \"id\": ";
        write_id(out, b.pinned.id);
        out << ", \"revision\": " << b.pinned.revision << ", \"path\": [";
        for (std::size_t i = 0; i < b.path.length; ++i) {
          if (i)
            out << ", ";
          write_id(out, b.path.nodes[i]);
        }
        out << "], \"overrides\": [";
        for (std::size_t i = 0; i < b.overrides.size(); ++i) {
          if (i)
            out << ", ";
          out << '[' << b.overrides[i].type << ", " << b.overrides[i].field << ']';
        }
        out << "]}";
      }
      out << '}';
      if (static_cast<std::size_t>(out.tellp()) > limits.document_bytes)
        exhausted("JSON output byte limit");
    }
    out << "\n  ]\n}\n";
    auto result = out.str();
    if (result.size() > limits.document_bytes)
      exhausted("JSON output byte limit");
    return result;
  } catch (const ParseFailure &e) {
    return e.error;
  } catch (const std::ios_base::failure &) {
    return fail(ErrorCode::Exhausted, "JSON output stream failed");
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "JSON encode allocation");
  }
}
Result<Document> decode_json(std::string_view input, const Schemas &schemas, Limits limits) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  if (input.size() > limits.document_bytes)
    return fail(ErrorCode::Exhausted, "JSON input byte limit");
  try {
    Parser parser(input, limits.json_scratch_bytes);
    auto d = json_document(parser.parse(), schemas, limits);
    auto r = validate_document(d, schemas, limits);
    if (!r)
      return r.error();
    return d;
  } catch (const ParseFailure &e) {
    return e.error;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "JSON decode allocation");
  }
}
Result<Cooked> cook(const Document &input, const Schemas &schemas, Limits limits,
                    std::string_view source) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  valid = validate_document(input, schemas, limits);
  if (!valid)
    return valid.error();
  if (!source_valid(source))
    return fail(ErrorCode::InvalidInput, "explicit bounded source identity required");
  try {
    auto d = canonical(input);
    auto descriptor = schema_descriptor(schemas);
    BinaryWriter payload(limits.document_bytes);
    binary_payload(payload, d);
    BinaryWriter out(limits.document_bytes);
    for (unsigned char c : std::string_view("RACESCN1"))
      out.u(c, 1);
    out.u(1, 4);
    out.u(1, 4);
    out.u(40 + source.size() + descriptor.size(), 8);
    out.u(payload.bytes.size(), 8);
    out.u(source.size(), 4);
    out.u(descriptor.size(), 4);
    for (unsigned char c : source)
      out.u(c, 1);
    for (auto b : descriptor)
      out.u(std::to_integer<unsigned>(b), 1);
    for (auto b : payload.bytes)
      out.u(std::to_integer<unsigned>(b), 1);
    return Cooked{std::move(out.bytes)};
  } catch (const ParseFailure &e) {
    return e.error;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "cooked encode allocation");
  }
}
Result<Document> decode_cooked(std::span<const std::byte> input, const Schemas &schemas,
                               Limits limits, std::string_view expected_source) {
  auto valid = validate_limits(limits);
  if (!valid)
    return valid.error();
  if (input.size() > limits.document_bytes)
    return fail(ErrorCode::Exhausted, "cooked input byte limit");
  try {
    BinaryReader r{input};
    for (unsigned char c : std::string_view("RACESCN1"))
      if (r.u(1) != c)
        invalid("cooked magic mismatch");
    if (r.u(4) != 1 || r.u(4) != 1)
      return fail(ErrorCode::IncompatibleVersion, "unsupported cooked or Scene schema");
    auto offset = r.u(8), size = r.u(8);
    auto source_size = r.count(4, 128), descriptor_size = r.count(4, 128 * 1024);
    if (offset != 40 + source_size + descriptor_size || offset > input.size() ||
        size != input.size() - offset)
      invalid("cooked offsets or sizes invalid");
    std::string source;
    for (std::size_t i = 0; i < source_size; ++i)
      source.push_back(static_cast<char>(r.u(1)));
    if (!source_valid(source) || (!expected_source.empty() && source != expected_source))
      invalid("cooked source identity mismatch");
    auto descriptor = schema_descriptor(schemas);
    if (descriptor.size() != descriptor_size)
      return fail(ErrorCode::IncompatibleVersion, "cooked schema descriptor mismatch");
    for (auto b : descriptor)
      if (r.u(1) != std::to_integer<unsigned>(b))
        return fail(ErrorCode::IncompatibleVersion, "cooked schema descriptor mismatch");
    BinaryReader payload{input.subspan(static_cast<std::size_t>(offset))};
    auto d = binary_document(payload, schemas, limits);
    auto checked = validate_document(d, schemas, limits);
    if (!checked)
      return checked.error();
    return d;
  } catch (const ParseFailure &e) {
    return e.error;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "cooked decode allocation");
  }
}
Result<void> save_json(platform::Files &files, std::string_view path, const World &world,
                       const Schemas &schemas, Limits limits, platform::Durability durability) {
  auto document = world.document();
  if (!document)
    return document.error();
  auto encoded = encode_json(document.value(), schemas, limits);
  if (!encoded)
    return encoded.error();
  auto bytes = std::as_bytes(std::span(encoded.value().data(), encoded.value().size()));
  return files.replace(path, bytes, durability);
}
} // namespace race::scene
