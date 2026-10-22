//! Module driver contracts, including independently parsed/rehashed artifact
//! receipts. All children are bounded; supported native tooling is required.
mod support;
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Output;
use std::sync::atomic::{AtomicU64, Ordering};
use support::bounded;

static NEXT: AtomicU64 = AtomicU64::new(0);
const ENTRY: &str = "import math;\nfn main() -> i64 { print(math::twice(21)); return 7; }\n";
const MATH: &str = "export fn twice(x: i64) -> i64 { return x + x; }\n";

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-module-cli-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn file(&self, name: &str, data: impl AsRef<[u8]>) -> PathBuf {
        let path = self.0.join(name);
        fs::write(&path, data).unwrap();
        path
    }
    fn simple(&self) {
        self.file("main.ax", ENTRY);
        self.file("math.ax", MATH);
    }
    fn run(&self, args: &[&str]) -> Output {
        self.run_clang(args, None)
    }
    fn run_clang(&self, args: &[&str], clang: Option<&Path>) -> Output {
        let mut command = bounded(env!("CARGO_BIN_EXE_axeon"), "45s");
        command.current_dir(&self.0).args(args);
        if let Some(clang) = clang {
            command.env("AXEON_CLANG", clang);
        }
        command.output().unwrap()
    }
    fn receipt(&self, artifact: &str) -> String {
        fs::read_to_string(self.0.join(format!("{artifact}.json"))).unwrap()
    }
    fn python(&self, script: &str, args: &[&str]) -> Output {
        bounded(support::python(), "10s")
            .current_dir(&self.0)
            .args(["-B", "-X", "utf8", "-c", script])
            .args(args)
            .output()
            .unwrap()
    }
    fn build(&self, artifact: &str) {
        status(&self.run(&["build", "main.ax", "-o", artifact]), 0);
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn status(output: &Output, expected: i32) {
    assert_eq!(
        output.status.code(),
        Some(expected),
        "stdout: {}\nstderr: {}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}
fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).unwrap()
}

#[test]
fn module_source_commands_and_explicit_root_share_the_supported_pipeline() {
    let fixture = Fixture::new();
    fixture.simple();
    for command in [
        "check",
        "interpret",
        "emit-ir",
        "emit-object",
        "build",
        "run",
    ] {
        let mut args = vec![command, "main.ax", "--project-root", "."];
        if matches!(command, "emit-ir" | "emit-object" | "build") {
            args.extend(["-o", "output"]);
        }
        let output = fixture.run(&args);
        status(&output, 0);
        assert!(output.stderr.is_empty());
        assert_eq!(
            text(&output.stdout),
            if matches!(command, "interpret" | "run") {
                "42\nresult: 7\n"
            } else {
                ""
            }
        );
        if command == "build" {
            let receipt = fixture.receipt("output");
            for expected in [
                "\"schema_version\": 3",
                "\"abi\": \"axeon-hosted-2\"",
                "\"runtime\": \"hosted-c-4\"",
                "\"entry\": \"main.ax\"",
            ] {
                assert!(receipt.contains(expected), "missing {expected}: {receipt}");
            }
            let executed = bounded(fixture.0.join("output"), "5s").output().unwrap();
            status(&executed, 0);
            assert_eq!(text(&executed.stdout), "42\nresult: 7\n");
        }
    }
    for opt in ["0", "2", "3"] {
        let output = fixture.run(&["run", "main.ax", "--opt", opt]);
        status(&output, 0);
        assert_eq!(text(&output.stdout), "42\nresult: 7\n");
        assert!(output.stderr.is_empty());
    }
}

#[test]
fn project_root_is_a_source_option_and_export_only_programs_use_module_metadata() {
    let fixture = Fixture::new();
    fixture.file(
        "main.ax",
        "export fn identity(x: i64) -> i64 { return x; } fn main() -> i64 { return identity(7); }",
    );
    for args in [
        vec!["--version", "--project-root", "."],
        vec!["--help", "--project-root", "."],
        vec!["check", "main.ax", "--project-root"],
        vec!["check", "main.ax", "--project-root", ""],
        vec![
            "check",
            "main.ax",
            "--project-root",
            ".",
            "--project-root",
            ".",
        ],
    ] {
        status(&fixture.run(&args), 2);
    }
    fixture.build("export-only");
    assert!(fixture
        .receipt("export-only")
        .contains("\"schema_version\": 3"));
    fixture.file("legacy.ax", "fn main() -> i64 { return 7; }");
    #[cfg(unix)]
    symlink(
        fixture.0.join("legacy.ax"),
        fixture.0.join("legacy-alias.ax"),
    )
    .unwrap();
    #[cfg(windows)]
    fs::hard_link(
        fixture.0.join("legacy.ax"),
        fixture.0.join("legacy-alias.ax"),
    )
    .unwrap();
    let legacy = fixture.run(&["interpret", "legacy-alias.ax", "--project-root", "."]);
    status(&legacy, 0);
    assert_eq!(text(&legacy.stdout), "result: 7\n");
    status(
        &fixture.run(&["build", "legacy-alias.ax", "-o", "legacy"]),
        0,
    );
    let receipt = fixture.receipt("legacy");
    assert!(receipt.contains("\"schema_version\": 2"));
    assert!(receipt.contains("\"abi\": \"axeon-hosted-1\""));
    assert!(receipt.contains("\"runtime\": \"hosted-c-4\""));
}

#[test]
fn imported_traps_match_reference_file_and_unicode_locations_at_all_optimizations() {
    let fixture = Fixture::new();
    fixture.file(
        "main.ax",
        "import math; fn main() -> i64 { print(1); return math::boom(); }",
    );
    fixture.file(
        "math.ax",
        "// 雪🙂\nexport fn boom() -> i64 {\n  print(9);\n\u{2003} return 1 / 0;\n}\n",
    );
    let interpreted = fixture.run(&["interpret", "main.ax"]);
    status(&interpreted, 70);
    assert_eq!(text(&interpreted.stdout), "1\n9\n");
    assert_eq!(
        text(&interpreted.stderr),
        "trap[division_by_zero] at math.ax:4:10\n"
    );
    for opt in ["0", "2", "3"] {
        let native = fixture.run(&["run", "main.ax", "--opt", opt]);
        status(&native, 70);
        assert_eq!(native.stdout, interpreted.stdout);
        assert_eq!(native.stderr, interpreted.stderr);
    }
}

#[test]
fn module_json_diagnostics_retain_primary_and_related_source_locations() {
    let fixture = Fixture::new();
    fixture.file(
        "main.ax",
        "import math;\nfn main() -> i64 { return math::twice(true); }\n",
    );
    fixture.file(
        "math.ax",
        "// 雪🙂\nexport fn twice(x: i64) -> i64 { return x + x; }\n",
    );
    let output = fixture.run(&["check", "main.ax", "--diagnostic-format", "json"]);
    status(&output, 1);
    fixture.file("diagnostics.json", &output.stderr);
    let parsed = fixture.python(
        r#"
import json
from pathlib import Path
d = json.loads(Path('diagnostics.json').read_text())
assert d['schema_version'] == 2
errors = d['diagnostics']
assert errors and any(e['path'] == 'main.ax' and e['line'] == 2 for e in errors)
related = [r for e in errors for r in e['related']]
assert any(r['path'] == 'math.ax' and r['line'] == 2 for r in related), related
for loc in errors + related:
    source = Path(loc['path']).read_bytes()
    assert 0 <= loc['start'] <= loc['end'] <= len(source)
    prefix = source[:loc['start']].decode()
    assert loc['line'] == prefix.count('\n') + 1
    assert loc['column'] == len(prefix.rsplit('\n', 1)[-1]) + 1
    assert isinstance(loc['source_id'], int)
"#,
        &[],
    );
    status(&parsed, 0);
    let textual = fixture.run(&["check", "main.ax"]);
    status(&textual, 1);
    assert!(text(&textual.stderr).contains("main.ax:2:"));
    assert!(text(&textual.stderr).contains("related at math.ax:2:"));
    assert!(!text(&textual.stderr).contains(&fixture.0.display().to_string()));
    fixture.file("main.ax", "import missing; fn main() -> i64 { return 0; }");
    let missing = fixture.run(&["check", "main.ax", "--diagnostic-format", "json"]);
    status(&missing, 1);
    assert!(text(&missing.stderr).starts_with("{\"schema_version\":2,"));
}

#[test]
fn all_loaded_sources_are_protected_from_artifact_and_sidecar_aliases() {
    let fixture = Fixture::new();
    fixture.simple();
    fs::create_dir(fixture.0.join("directory")).unwrap();
    #[cfg(unix)]
    symlink(fixture.0.join("math.ax"), fixture.0.join("symbolic.ax")).unwrap();
    fs::hard_link(fixture.0.join("math.ax"), fixture.0.join("hard.ax")).unwrap();
    for command in ["emit-ir", "emit-object", "build"] {
        for destination in [
            "main.ax",
            "math.ax",
            "directory/../math.ax",
            #[cfg(unix)]
            "symbolic.ax",
            "hard.ax",
            #[cfg(windows)]
            "MATH.ax",
        ] {
            let output = fixture.run(&[command, "main.ax", "-o", destination]);
            status(&output, 2);
            assert!(text(&output.stderr).contains("output cannot overwrite source"));
            assert_eq!(
                fs::read_to_string(fixture.0.join("main.ax")).unwrap(),
                ENTRY
            );
            assert_eq!(fs::read_to_string(fixture.0.join("math.ax")).unwrap(), MATH);
        }
    }
    for (name, hardlink) in [
        #[cfg(unix)]
        ("out-symbolic", false),
        ("out-hard", true),
    ] {
        let output_path = fixture.file(name, "old artifact");
        let sidecar = fixture.0.join(format!("{name}.json"));
        if hardlink {
            fs::hard_link(fixture.0.join("math.ax"), &sidecar).unwrap();
        } else {
            #[cfg(unix)]
            symlink(fixture.0.join("math.ax"), &sidecar).unwrap();
        }
        for command in ["emit-object", "build"] {
            let output = fixture.run(&[command, "main.ax", "-o", name]);
            status(&output, 2);
            assert!(text(&output.stderr).contains("output cannot overwrite source"));
            assert_eq!(fs::read_to_string(&output_path).unwrap(), "old artifact");
            assert_eq!(fs::read_to_string(&sidecar).unwrap(), MATH);
        }
    }
    #[cfg(unix)]
    assert!(fs::symlink_metadata(fixture.0.join("symbolic.ax"))
        .unwrap()
        .file_type()
        .is_symlink());
}

#[test]
fn graph_failures_preserve_both_previous_artifacts() {
    let fixture = Fixture::new();
    fixture.file("main.ax", "import math; fn main() -> i64 { return 0; }");
    fixture.file(
        "math.ax",
        "import main; export fn value() -> i64 { return 1; }",
    );
    fixture.file("program", "old artifact");
    fixture.file("program.json", "old sidecar");
    for command in ["emit-ir", "emit-object", "build"] {
        status(&fixture.run(&[command, "main.ax", "-o", "program"]), 1);
        assert_eq!(
            fs::read_to_string(fixture.0.join("program")).unwrap(),
            "old artifact"
        );
        assert_eq!(fixture.receipt("program"), "old sidecar");
    }
}

const VERIFY_RECEIPT: &str = r#"
import hashlib, json, sys
from pathlib import Path
artifact = Path(sys.argv[1])
d = json.loads(Path(str(artifact) + '.json').read_text())
canonical = lambda value: json.dumps(value, separators=(',', ':'), ensure_ascii=False)
digest = lambda value: hashlib.sha256(value.encode()).hexdigest()
tagged = lambda domain, value: digest(domain + '\n' + value)
assert d['schema_version'] == 3
assert d['compiler'] == 'axeon-0.13.0' and d['language_subset'] == 13
assert d['artifact_sha256'] == hashlib.sha256(artifact.read_bytes()).hexdigest()
assert d['source_sha256'] == hashlib.sha256(Path(d['entry']).read_bytes()).hexdigest()
modules = d['modules']
assert [m['path'] for m in modules] == sorted(m['path'] for m in modules)
by_path = {m['path']: m for m in modules}
assert len(by_path) == len(modules)
definitions = {}
for m in modules:
    assert m['interface_schema'] in (1, 2)
    types = m['type_exports']
    assert [t['name'] for t in types] == sorted(t['name'] for t in types)
    for t in types:
        shape = t['definition']
        assert shape['module'] == m['path'] and shape['name'] == t['name']
        assert shape['kind'] in ('record', 'union')
        if shape['kind'] == 'record':
            assert set(shape) == {'kind', 'module', 'name', 'fields'}
            field_sets = [shape['fields']]
        else:
            assert set(shape) == {'kind', 'module', 'name', 'variants'}
            assert all(set(v) == {'name', 'fields'} for v in shape['variants'])
            field_sets = [v['fields'] for v in shape['variants']]
        for fields in field_sets:
            assert all(set(f) == {'name', 'type'} for f in fields)
        expected = tagged('axeon-export-type-v2', canonical(shape))
        assert t['definition_sha256'] == expected
        key = (shape['kind'], shape['module'], shape['name'])
        assert key not in definitions
        definitions[key] = expected
def validate_type(ty):
    if isinstance(ty, str):
        assert ty in ('i64', 'f64', 'bool', '()')
    elif ty['kind'] == 'array':
        assert set(ty) == {'kind', 'element', 'length'}
        assert ty['element'] in ('i64', 'f64') and type(ty['length']) is int and 1 <= ty['length'] <= 32
    else:
        assert set(ty) == {'kind', 'module', 'name', 'definition_sha256'}
        assert ty['definition_sha256'] == definitions[(ty['kind'], ty['module'], ty['name'])]
for m in modules:
    for t in m['type_exports']:
        shape = t['definition']
        field_sets = [shape['fields']] if shape['kind'] == 'record' else [v['fields'] for v in shape['variants']]
        for fields in field_sets:
            for field in fields:
                validate_type(field['type'])
    assert m['content_sha256'] == hashlib.sha256(Path(m['path']).read_bytes()).hexdigest()
    exports = m['exports']
    assert [e['name'] for e in exports] == sorted(e['name'] for e in exports)
    if m['interface_schema'] == 1:
        assert m['type_exports'] == []
        for e in exports:
            assert isinstance(e['signature'], str)
            assert e['signature_sha256'] == tagged('axeon-export-signature-v1', e['signature'])
        expected = tagged('axeon-module-interface-v1', canonical([e['signature'] for e in exports]))
    else:
        for e in exports:
            signature = e['signature']
            assert set(signature) == {'name', 'params', 'result'} and signature['name'] == e['name']
            for ty in signature['params'] + [signature['result']]:
                validate_type(ty)
            assert e['signature_sha256'] == tagged('axeon-export-signature-v2', canonical(signature))
        interface = {'module': m['path'], 'types': [t['definition'] for t in m['type_exports']],
                     'functions': [e['signature'] for e in exports]}
        expected = tagged('axeon-module-interface-v2', canonical(interface))
    assert m['interface_sha256'] == expected
    imports = m['imports']
    assert [i['name'] for i in imports] == sorted(i['name'] for i in imports)
    for i in imports:
        target = by_path[i['path']]
        assert i['interface_sha256'] == target['interface_sha256']
        assert i['dependency_sha256'] == target['dependency_sha256']
    inputs = {key: m[key] for key in ('path', 'content_sha256', 'interface_sha256', 'imports')}
    assert m['dependency_sha256'] == tagged('axeon-module-dependency-v1', canonical(inputs))
assert d['entry_dependency_sha256'] == by_path[d['entry']]['dependency_sha256']
assert d['module_graph_sha256'] == tagged('axeon-module-graph-v1', canonical(modules))
inputs = d['build_inputs']
assert inputs['module_graph_sha256'] == d['module_graph_sha256']
assert inputs['entry_dependency_sha256'] == d['entry_dependency_sha256']
assert inputs['optimization'] == d['optimization']
assert inputs['artifact_kind'] == d['artifact_kind']
assert inputs['ir_sha256'] == d['ir_sha256']
assert d['complete_build_sha256'] == tagged('axeon-complete-build-v1', canonical(inputs))
assert str(Path.cwd()) not in canonical(d)
"#;

fn diamond(fixture: &Fixture) {
    fixture.file(
        "main.ax",
        "import right; import left; fn main() -> i64 { return left::value() + right::value(); }",
    );
    fixture.file(
        "left.ax",
        "import math; export fn value() -> i64 { return math::twice(2); }",
    );
    fixture.file(
        "right.ax",
        "import math; export fn value() -> i64 { return math::twice(3); }",
    );
    fixture.file(
        "math.ax",
        "export fn z() -> i64 { return 0; } export fn twice(x: i64) -> i64 { return x+x; }",
    );
}

#[test]
fn module_receipts_rehash_independently_and_relocate_reproducibly() {
    let first = Fixture::new();
    let second = Fixture::new();
    diamond(&first);
    diamond(&second);
    for command in ["emit-object", "build"] {
        for fixture in [&first, &second] {
            status(
                &fixture.run(&[command, "main.ax", "-o", "artifact", "--opt", "2"]),
                0,
            );
            status(&fixture.python(VERIFY_RECEIPT, &["artifact"]), 0);
            #[cfg(windows)]
            if command == "emit-object" {
                let bytes = fs::read(fixture.0.join("artifact")).unwrap();
                assert_eq!(
                    &bytes[4..8],
                    &[0; 4],
                    "COFF timestamp must be zero even when both emissions share a second"
                );
            }
        }
        assert_eq!(
            fs::read(first.0.join("artifact")).unwrap(),
            fs::read(second.0.join("artifact")).unwrap()
        );
        assert_eq!(first.receipt("artifact"), second.receipt("artifact"));
    }
}

#[test]
fn body_interface_transitive_dependencies_and_options_have_distinct_hash_effects() {
    let fixture = Fixture::new();
    diamond(&fixture);
    fixture.build("before");
    fixture.file(
        "math.ax",
        "export fn z() -> i64 { return 0; } export fn twice(x: i64) -> i64 { return x+x+1; }",
    );
    fixture.build("body");
    status(&fixture.python(VERIFY_RECEIPT, &["body"]), 0);
    fixture.file(
        "math.ax",
        "export fn z() -> bool { return true; } export fn twice(x: i64) -> i64 { return x+x+1; }",
    );
    fixture.build("interface");
    status(&fixture.python(VERIFY_RECEIPT, &["interface"]), 0);
    status(
        &fixture.run(&["build", "main.ax", "-o", "opt", "--opt", "0"]),
        0,
    );
    let checks = fixture.python(
        r#"
import json
from pathlib import Path
load = lambda name: json.loads(Path(name + '.json').read_text())
a, b, c, o = map(load, ['before', 'body', 'interface', 'opt'])
modules = lambda d: {m['path']:m for m in d['modules']}
ma, mb, mc = map(modules, [a,b,c])
assert ma['math.ax']['content_sha256'] != mb['math.ax']['content_sha256']
assert ma['math.ax']['interface_sha256'] == mb['math.ax']['interface_sha256']
assert mb['math.ax']['interface_sha256'] != mc['math.ax']['interface_sha256']
for path in ('main.ax','left.ax','right.ax'):
    assert ma[path]['content_sha256'] == mb[path]['content_sha256'] == mc[path]['content_sha256']
    assert ma[path]['dependency_sha256'] != mb[path]['dependency_sha256']
    assert mb[path]['dependency_sha256'] != mc[path]['dependency_sha256']
assert len({d['complete_build_sha256'] for d in (a,b,c,o)}) == 4
assert c['module_graph_sha256'] == o['module_graph_sha256']
assert c['ir_sha256'] == o['ir_sha256']
"#,
        &[],
    );
    status(&checks, 0);
}

const PUBLIC_TYPES: &str = r#"
export record Pair { value:i64, valid:bool }
export union Choice { Value { number:f64 }, Missing {} }
export fn make()->Pair { return Pair { valid:true, value:42 }; }
export fn passthrough(value:Pair)->Pair { return value; }
export fn choose(flag:bool)->Choice {
    if flag { return Choice::Value { number:-0.0 }; }
    return Choice::Missing {};
}
export fn vector(values:[i64;2])->[i64;2] { return values; }
"#;
const PUBLIC_BRIDGE: &str = r#"
import types;
export fn relay(value:types::Pair)->types::Pair { return types::passthrough(value); }
export fn carry(value:types::Choice)->types::Choice { return value; }
"#;
const PUBLIC_ENTRY: &str = r#"
import types; import bridge; import other;
fn main()->i64 {
    let first:types::Pair=types::make();
    let second:types::Pair=bridge::relay(first);
    return second.value;
}
"#;

fn aggregate_modules(fixture: &Fixture) {
    fixture.file("main.ax", PUBLIC_ENTRY);
    fixture.file("types.ax", PUBLIC_TYPES);
    fixture.file("bridge.ax", PUBLIC_BRIDGE);
    // The same exported spelling and shape in another module is distinct.
    fixture.file("other.ax", "export record Pair { value:i64, valid:bool }");
}

fn nested_modules(fixture: &Fixture) {
    fixture.file("leaf.ax", "export record Leaf { number:i64, scale:f64 }");
    fixture.file("types.ax", "import leaf; export record Boxed { values:[i64;2], leaf:leaf::Leaf } export union Packet { Data { value:Boxed }, Empty {} } export record Shared { left:Boxed, right:Boxed }");
    fixture.file(
        "bridge.ax",
        "import types; export fn relay(value:types::Packet)->types::Packet { return value; }",
    );
    fixture.file(
        "main.ax",
        "import types; import bridge; fn main()->i64 { return 0; }",
    );
}

#[test]
fn nested_public_definitions_rehash_and_relocate_without_compiler_local_ids() {
    let first = Fixture::new();
    let second = Fixture::new();
    for fixture in [&first, &second] {
        nested_modules(fixture);
        fixture.build("nested");
        status(&fixture.python(VERIFY_RECEIPT, &["nested"]), 0);
        status(&fixture.python(r#"
import json
from pathlib import Path
d = json.loads(Path('nested.json').read_text())
m = {m['path']: m for m in d['modules']}
types = {t['name']: t for t in m['types.ax']['type_exports']}
boxed = types['Boxed']
fields = boxed['definition']['fields']
assert fields[0]['type'] == {'kind':'array','element':'i64','length':2}
assert fields[1]['type']['definition_sha256'] == m['leaf.ax']['type_exports'][0]['definition_sha256']
packet = types['Packet']
assert packet['definition']['variants'][0]['fields'][0]['type']['definition_sha256'] == boxed['definition_sha256']
shared = types['Shared']['definition']['fields']
assert shared[0]['type'] == shared[1]['type']
assert shared[0]['type']['definition_sha256'] == boxed['definition_sha256']
signature = m['bridge.ax']['exports'][0]['signature']
assert signature['params'] == [signature['result']]
assert signature['result']['definition_sha256'] == packet['definition_sha256']
assert 'record@' not in str(d) and 'union@' not in str(d)
"#, &[]), 0);
    }
    assert_eq!(first.receipt("nested"), second.receipt("nested"));
    assert_eq!(
        fs::read(first.0.join("nested")).unwrap(),
        fs::read(second.0.join("nested")).unwrap()
    );
}

#[test]
fn nested_definition_changes_propagate_transitively_but_private_edits_do_not() {
    let fixture = Fixture::new();
    nested_modules(&fixture);
    fixture.build("original");
    status(&fixture.python(VERIFY_RECEIPT, &["original"]), 0);
    fixture.file("leaf.ax", "// shifted offsets\nrecord Private { v:i64 } export record Leaf { number:i64, scale:f64 } fn private_body()->i64{return 9;}");
    fixture.file("aaa.ax", "fn unused()->i64{return 0;}");
    fixture.file(
        "main.ax",
        "import aaa; import bridge; import types; fn main()->i64{return 0;}",
    );
    fixture.build("private-edit");
    status(&fixture.python(VERIFY_RECEIPT, &["private-edit"]), 0);
    fixture.file("leaf.ax", "export record Leaf { scale:f64, number:i64 }");
    fixture.build("reordered-leaf");
    status(&fixture.python(VERIFY_RECEIPT, &["reordered-leaf"]), 0);
    fixture.file("types.ax", "import leaf; export record Boxed { values:[i64;3], leaf:leaf::Leaf } export union Packet { Data { value:Boxed }, Empty {} } export record Shared { left:Boxed, right:Boxed }");
    fixture.build("resized-array");
    status(&fixture.python(VERIFY_RECEIPT, &["resized-array"]), 0);
    status(&fixture.python(r#"
import json
from pathlib import Path
load = lambda name: {m['path']: m for m in json.loads(Path(name+'.json').read_text())['modules']}
a,b,c,d = map(load, ['original','private-edit','reordered-leaf','resized-array'])
for path in ('leaf.ax','types.ax','bridge.ax','main.ax'):
    assert a[path]['interface_sha256'] == b[path]['interface_sha256'], path
    assert a[path]['type_exports'] == b[path]['type_exports'], path
    assert a[path]['exports'] == b[path]['exports'], path
for before, after in [(b,c),(c,d)]:
    for path in ('types.ax','bridge.ax'):
        assert before[path]['interface_sha256'] != after[path]['interface_sha256'], path
    before_types = {t['name']:t['definition_sha256'] for t in before['types.ax']['type_exports']}
    after_types = {t['name']:t['definition_sha256'] for t in after['types.ax']['type_exports']}
    assert all(before_types[n] != after_types[n] for n in ('Boxed','Packet','Shared'))
    assert before['bridge.ax']['exports'][0]['signature_sha256'] != after['bridge.ax']['exports'][0]['signature_sha256']
assert c['leaf.ax']['interface_sha256'] == d['leaf.ax']['interface_sha256']
assert a['main.ax']['dependency_sha256'] != b['main.ax']['dependency_sha256']
"#, &[]), 0);
}

#[test]
fn aggregate_interfaces_rehash_independently_and_relocate_with_qualified_types() {
    let first = Fixture::new();
    let second = Fixture::new();
    for fixture in [&first, &second] {
        aggregate_modules(fixture);
        fixture.build("artifact");
        status(&fixture.python(VERIFY_RECEIPT, &["artifact"]), 0);
        let executed = bounded(fixture.0.join("artifact"), "5s").output().unwrap();
        status(&executed, 0);
        assert_eq!(executed.stdout, b"result: 42\n");
        assert!(executed.stderr.is_empty());
        status(
            &fixture.python(
                r#"
import json
from pathlib import Path
d = json.loads(Path('artifact.json').read_text())
m = {m['path']: m for m in d['modules']}
assert m['main.ax']['interface_schema'] == 1
assert all(m[p]['interface_schema'] == 2 for p in ('types.ax', 'bridge.ax', 'other.ax'))
types = {t['name']: t for t in m['types.ax']['type_exports']}
assert list(types) == ['Choice', 'Pair']
assert [f['name'] for f in types['Pair']['definition']['fields']] == ['value', 'valid']
assert [v['name'] for v in types['Choice']['definition']['variants']] == ['Value', 'Missing']
assert types['Pair']['definition_sha256'] != m['other.ax']['type_exports'][0]['definition_sha256']
assert m['bridge.ax']['type_exports'] == []
functions = {e['name']: e['signature'] for e in m['bridge.ax']['exports']}
for name, nominal in [('relay', 'Pair'), ('carry', 'Choice')]:
    assert functions[name]['params'] == [functions[name]['result']]
    ty = functions[name]['result']
    assert ty['module'] == 'types.ax' and ty['name'] == nominal
    assert ty['definition_sha256'] == types[nominal]['definition_sha256']
vector = next(e['signature'] for e in m['types.ax']['exports'] if e['name'] == 'vector')
assert vector['params'] == [{'kind': 'array', 'element': 'i64', 'length': 2}]
assert vector['result'] == vector['params'][0]
assert all('record@' not in str(e) and 'union@' not in str(e) for module in m.values() for e in module['exports'])
"#,
                &[],
            ),
            0,
        );
    }
    assert_eq!(first.receipt("artifact"), second.receipt("artifact"));
    assert_eq!(
        fs::read(first.0.join("artifact")).unwrap(),
        fs::read(second.0.join("artifact")).unwrap()
    );
}

#[test]
fn public_type_interfaces_ignore_comments_bodies_offsets_and_source_id_order() {
    let fixture = Fixture::new();
    aggregate_modules(&fixture);
    let original = axeon::project::Project::load(&fixture.0.join("main.ax"), None).unwrap();
    let original_source = original
        .modules
        .iter()
        .find(|module| original.sources.get(module.source).unwrap().logical_path == "types.ax")
        .unwrap()
        .source;
    fixture.build("before");
    let changed = format!(
        "// Moving declaration offsets does not change public identity.\nrecord Private {{ field:i64 }}\n{}",
        PUBLIC_TYPES.replace("value:42", "value:43")
    );
    fixture.file("types.ax", changed);
    fixture.file(
        "bridge.ax",
        PUBLIC_BRIDGE.replace("value", "renamed_parameter"),
    );
    // The loader sorts direct imports. A new earlier dependency, rather than
    // mere textual reordering, shifts the SourceIds of existing modules.
    fixture.file("aaa.ax", "fn private_helper()->i64{return 0;}");
    fixture.file(
        "main.ax",
        PUBLIC_ENTRY.replace(
            "import types; import bridge; import other;",
            "import aaa; import other; import bridge; import types;",
        ),
    );
    let shifted = axeon::project::Project::load(&fixture.0.join("main.ax"), None).unwrap();
    let shifted_source = shifted
        .modules
        .iter()
        .find(|module| shifted.sources.get(module.source).unwrap().logical_path == "types.ax")
        .unwrap()
        .source;
    assert_ne!(
        original_source, shifted_source,
        "fixture must actually shift source IDs"
    );
    fixture.build("shifted");
    status(&fixture.python(VERIFY_RECEIPT, &["shifted"]), 0);
    status(
        &fixture.python(
            r#"
import json
from pathlib import Path
load = lambda n: json.loads(Path(n+'.json').read_text())
a, b = map(load, ['before', 'shifted'])
modules = lambda d: {m['path']:m for m in d['modules']}
ma, mb = map(modules, (a,b))
assert len(mb) == len(ma) + 1 and 'aaa.ax' in mb
for path in ma:
    assert ma[path]['interface_sha256'] == mb[path]['interface_sha256'], path
    assert ma[path]['exports'] == mb[path]['exports'], path
    assert ma[path]['type_exports'] == mb[path]['type_exports'], path
for path in ('types.ax','bridge.ax','main.ax'):
    assert ma[path]['content_sha256'] != mb[path]['content_sha256']
    assert ma[path]['dependency_sha256'] != mb[path]['dependency_sha256']
assert a['entry_dependency_sha256'] != b['entry_dependency_sha256']
assert a['complete_build_sha256'] != b['complete_build_sha256']
"#,
            &[],
        ),
        0,
    );
    let executed = bounded(fixture.0.join("shifted"), "5s").output().unwrap();
    status(&executed, 0);
    assert_eq!(executed.stdout, b"result: 43\n");
}

#[test]
fn a_standalone_type_export_uses_module_interface_schema_two() {
    let fixture = Fixture::new();
    fixture.file(
        "main.ax",
        "export record Item { number:f64 } fn main()->i64{return 0;}",
    );
    fixture.build("type-only");
    status(&fixture.python(VERIFY_RECEIPT, &["type-only"]), 0);
    status(
        &fixture.python(
            r#"
import json
from pathlib import Path
d = json.loads(Path('type-only.json').read_text())
assert len(d['modules']) == 1
m = d['modules'][0]
assert m['interface_schema'] == 2 and m['exports'] == []
assert m['type_exports'][0]['name'] == 'Item'
"#,
            &[],
        ),
        0,
    );
}

#[test]
fn unit_interfaces_use_schema_two_and_rehash_across_modules() {
    let fixture = Fixture::new();
    for (name, source) in [
        (
            "types.ax",
            include_str!("../examples/unit-modules/types.ax"),
        ),
        (
            "actions.ax",
            include_str!("../examples/unit-modules/actions.ax"),
        ),
        ("main.ax", include_str!("../examples/unit-modules/main.ax")),
    ] {
        fixture.file(name, source);
    }
    fixture.file(
        "plain.ax",
        "export fn identity(value:())->(){return value;}",
    );
    let entry = format!(
        "import plain;\n{}",
        include_str!("../examples/unit-modules/main.ax")
    );
    fixture.file("main.ax", entry);
    fixture.build("unit-program");
    status(&fixture.python(VERIFY_RECEIPT, &["unit-program"]), 0);
    status(
        &fixture.python(
            r#"
import json
from pathlib import Path
d = json.loads(Path('unit-program.json').read_text())
modules = {m['path']: m for m in d['modules']}
plain = modules['plain.ax']
assert plain['interface_schema'] == 2 and plain['type_exports'] == []
assert plain['exports'][0]['signature'] == {'name':'identity','params':['()'],'result':'()'}
types = modules['types.ax']['type_exports']
completion = next(t['definition'] for t in types if t['name'] == 'Completion')
assert completion['fields'] == [{'name':'value','type':'i64'},{'name':'done','type':'()'}]
assert modules['actions.ax']['interface_schema'] == 2
assert modules['main.ax']['interface_schema'] == 1
"#,
            &[],
        ),
        0,
    );
}

#[test]
fn exported_shapes_invalidate_imported_signatures_and_full_dependencies() {
    let fixture = Fixture::new();
    aggregate_modules(&fixture);
    fixture.build("before");
    let reordered = PUBLIC_TYPES.replace("value:i64, valid:bool", "valid:bool, value:i64");
    fixture.file("types.ax", &reordered);
    fixture.build("record-order");
    status(&fixture.python(VERIFY_RECEIPT, &["record-order"]), 0);
    let union = reordered.replace(
        "Value { number:f64 }, Missing {}",
        "Missing {}, Value { number:f64 }",
    );
    fixture.file("types.ax", &union);
    fixture.build("variant-order");
    status(&fixture.python(VERIFY_RECEIPT, &["variant-order"]), 0);
    let array = union.replace("[i64;2]", "[f64;3]");
    fixture.file("types.ax", &array);
    fixture.build("array-type");
    status(&fixture.python(VERIFY_RECEIPT, &["array-type"]), 0);
    fixture.file(
        "types.ax",
        format!("{array}\nexport record Unused {{ number:f64 }}\n"),
    );
    fixture.build("unused-export");
    status(&fixture.python(VERIFY_RECEIPT, &["unused-export"]), 0);
    status(
        &fixture.python(
            r#"
import json
from pathlib import Path
load = lambda n: json.loads(Path(n+'.json').read_text())
docs = list(map(load, ['before','record-order','variant-order','array-type','unused-export']))
modules = [{m['path']:m for m in d['modules']} for d in docs]
def signature(module, name):
    return next(e['signature_sha256'] for e in module['exports'] if e['name'] == name)
for before, after in zip(modules, modules[1:]):
    assert before['types.ax']['interface_sha256'] != after['types.ax']['interface_sha256']
    for path in ('types.ax','bridge.ax','main.ax'):
        assert before[path]['dependency_sha256'] != after[path]['dependency_sha256'], path
assert signature(modules[0]['bridge.ax'],'relay') != signature(modules[1]['bridge.ax'],'relay')
assert signature(modules[0]['bridge.ax'],'carry') == signature(modules[1]['bridge.ax'],'carry')
assert signature(modules[1]['bridge.ax'],'carry') != signature(modules[2]['bridge.ax'],'carry')
assert signature(modules[1]['bridge.ax'],'relay') == signature(modules[2]['bridge.ax'],'relay')
assert modules[0]['bridge.ax']['interface_sha256'] != modules[1]['bridge.ax']['interface_sha256']
assert modules[1]['bridge.ax']['interface_sha256'] != modules[2]['bridge.ax']['interface_sha256']
assert signature(modules[2]['types.ax'],'vector') != signature(modules[3]['types.ax'],'vector')
# Unrelated shapes and array-only signatures must not invalidate the bridge's public interface.
assert modules[2]['bridge.ax']['interface_sha256'] == modules[3]['bridge.ax']['interface_sha256'] == modules[4]['bridge.ax']['interface_sha256']
assert len({d['complete_build_sha256'] for d in docs}) == len(docs)
"#,
            &[],
        ),
        0,
    );
}

#[test]
#[cfg(unix)]
fn native_probe_source_mutation_does_not_change_the_frozen_build_inputs() {
    let fixture = Fixture::new();
    fixture.simple();
    let clang = std::env::var_os("AXEON_CLANG")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("clang-19"));
    let shell_quote =
        |value: &Path| format!("'{}'", value.to_str().unwrap().replace('\'', "'\\''"));
    let script = fixture.file("mutating-clang", format!(
        "#!/bin/sh\nif [ \"$1\" = \"--version\" ]; then\n  printf '%s\\n' 'export fn twice(x: i64) -> i64 {{ return 99; }}' > {}\nfi\nexec {} \"$@\"\n",
        shell_quote(&fixture.0.join("math.ax")), shell_quote(&clang)
    ));
    fs::set_permissions(&script, fs::Permissions::from_mode(0o755)).unwrap();
    status(
        &fixture.run_clang(&["build", "main.ax", "-o", "program"], Some(&script)),
        0,
    );
    let executed = bounded(fixture.0.join("program"), "5s").output().unwrap();
    status(&executed, 0);
    assert_eq!(text(&executed.stdout), "42\nresult: 7\n");
    fixture.file("expected-original.ax", MATH);
    status(
        &fixture.python(
            r#"
import hashlib, json
from pathlib import Path
d = json.loads(Path('program.json').read_text())
m = next(m for m in d['modules'] if m['path'] == 'math.ax')
assert m['content_sha256'] == hashlib.sha256(Path('expected-original.ax').read_bytes()).hexdigest()
assert m['content_sha256'] != hashlib.sha256(Path('math.ax').read_bytes()).hexdigest()
"#,
            &[],
        ),
        0,
    );
}
