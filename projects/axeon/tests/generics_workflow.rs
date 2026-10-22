//! End-to-end explicit generic source and CLI workflow acceptance.
mod support;
use std::fs;
use std::path::PathBuf;
use std::process::Output;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-generics-workflow-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn run(&self, args: &[&str]) -> Output {
        support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "45s", "2s")
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
    fn success(&self, args: &[&str]) -> Output {
        let output = self.run(args);
        assert!(output.status.success(), "{args:?}: {output:?}");
        assert!(output.stderr.is_empty(), "{args:?}: {output:?}");
        output
    }
    fn project(&self) {
        self.success(&["new", "project"]);
        for (name, source) in SOURCES {
            fs::write(self.0.join("project").join(name), source).unwrap();
        }
    }
    fn python(&self, script: &str) {
        let output = support::bounded_with_kill(support::python(), "10s", "2s")
            .args(["-B", "-c", script])
            .current_dir(&self.0)
            .output()
            .unwrap();
        assert!(output.status.success(), "{output:?}");
        assert!(output.stderr.is_empty(), "{output:?}");
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
const CONTRACTS: &str = r#"export trait Forward { fn forward(value: Self) -> Self; }
export trait Summarize { fn summarize(value: Self) -> i64; }
"#;
const TYPES: &str = r#"import contracts;
export record Packet { samples: [i64; 3], stamp: () }
fn forward_packet(value: Packet) -> Packet { return value; }
fn summarize_packet(value: Packet) -> i64 {
    let samples: [i64; 3] = value.samples;
    return samples[0] + samples[1] + samples[2];
}
impl contracts::Forward for Packet { forward = forward_packet; }
impl contracts::Summarize for Packet { summarize = summarize_packet; }
"#;
const ALGORITHMS: &str = r#"import contracts;
fn finish(value: i64) -> i64 { return value + 1; }
export fn pipeline<T: contracts::Forward + contracts::Summarize>(value: T) -> i64 {
    let forwarded: T = contracts::Forward::forward(value);
    return finish(contracts::Summarize::summarize(forwarded));
}
"#;
const MAIN: &str = r#"import types; import algorithms;
fn main() -> i64 {
    let packet: types::Packet = types::Packet { samples: [10, 20, 11], stamp: () };
    print(algorithms::pipeline::<types::Packet>(packet));
    return 0;
}
"#;
const TESTS: &str = r#"import types; import algorithms;
fn main() -> i64 { return 99; }
fn test_generic_pipeline() -> i64 {
    let packet: types::Packet = types::Packet { samples: [10, 20, 11], stamp: () };
    return algorithms::pipeline::<types::Packet>(packet) - 42;
}
"#;
const SOURCES: &[(&str, &str)] = &[
    ("contracts.ax", CONTRACTS),
    ("types.ax", TYPES),
    ("algorithms.ax", ALGORITHMS),
    ("main.ax", MAIN),
    ("tests.ax", TESTS),
];
const EXPECTED: &[u8] = b"42\nresult: 0\n";

#[test]
fn explicit_generics_integrate_with_create_format_test_and_native_distribution() {
    let f = Fixture::new();
    f.project();
    f.success(&["check", "project/main.ax"]);
    assert_eq!(
        f.success(&["interpret", "project/main.ax"]).stdout,
        EXPECTED
    );
    let tested = f.success(&["test", "project/tests.ax"]);
    assert!(String::from_utf8_lossy(&tested.stdout).contains("PASS test_generic_pipeline"));
    for (name, _) in SOURCES {
        let path = format!("project/{name}");
        let formatted = f.success(&["format", &path]).stdout;
        fs::write(f.0.join(&path), &formatted).unwrap();
        f.success(&["format", &path, "--check"]);
        assert_eq!(f.success(&["format", &path]).stdout, formatted);
    }
    assert_eq!(
        f.success(&["test", "project/tests.ax"]).stdout,
        tested.stdout
    );
    for opt in ["0", "2", "3"] {
        assert_eq!(
            f.success(&["run", "project/main.ax", "--opt", opt]).stdout,
            EXPECTED
        );
    }
    f.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    let standalone = support::bounded_with_kill(f.0.join("app"), "10s", "2s")
        .output()
        .unwrap();
    assert!(standalone.status.success());
    assert_eq!(standalone.stdout, EXPECTED);
    assert!(standalone.stderr.is_empty());
}

#[test]
fn generic_builds_relocate_and_unused_invalid_templates_preserve_outputs() {
    let first = Fixture::new();
    let second = Fixture::new();
    for f in [&first, &second] {
        f.project();
        f.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    }
    for path in ["app", "app.json"] {
        assert_eq!(
            fs::read(first.0.join(path)).unwrap(),
            fs::read(second.0.join(path)).unwrap()
        );
    }
    let binary = fs::read(first.0.join("app")).unwrap();
    let metadata = fs::read(first.0.join("app.json")).unwrap();
    assert!(String::from_utf8_lossy(&metadata).contains("\"interface_schema\":4"));
    fs::write(first.0.join("project/algorithms.ax"), format!(
        "{ALGORITHMS}\nfn unused_invalid<T>(value: T) -> T {{ let moved: T = value; return value; }}\n"
    )).unwrap();
    let bad = first.run(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    assert_eq!(bad.status.code(), Some(1), "{bad:?}");
    assert!(bad.stdout.is_empty());
    let diagnostic = String::from_utf8_lossy(&bad.stderr);
    assert!(diagnostic.contains("algorithms.ax"), "{diagnostic}");
    assert!(diagnostic.contains("moved"), "{diagnostic}");
    assert_eq!(fs::read(first.0.join("app")).unwrap(), binary);
    assert_eq!(fs::read(first.0.join("app.json")).unwrap(), metadata);
}

#[test]
fn generic_metadata_rehashes_canonically_and_tracks_body_helpers_and_implementations() {
    let f = Fixture::new();
    f.project();
    f.success(&["build", "project/main.ax", "-o", "baseline"]);
    f.python(r#"
import hashlib, json
from pathlib import Path
d=json.loads(Path('baseline.json').read_text())
modules={m['path']:m for m in d['modules']}
compact=lambda v:json.dumps(v,separators=(',',':'),ensure_ascii=False)
digest=lambda tag,v:hashlib.sha256((tag+'\n'+compact(v)).encode()).hexdigest()
generic=modules['algorithms.ax']
assert generic['interface_schema']==4
assert modules['main.ax']['interface_schema']==1
assert modules['contracts.ax']['interface_schema']==modules['types.ax']['interface_schema']==3
assert all('generic_exports' not in m for name,m in modules.items() if name!='algorithms.ax')
record=generic['generic_exports'][0]
definition=record['definition']
contracts={t['name']:t for t in modules['contracts.ax']['trait_exports']}
bounds=[{'module':'contracts.ax','name':name,'definition_sha256':contracts[name]['definition_sha256']} for name in ['Forward','Summarize']]
text=Path('project/algorithms.ax').read_text()
body=text[text.index('{',text.index('export fn pipeline')):text.rfind('}')+1]
body_sha=hashlib.sha256(('axeon-generic-body-v1\n'+body).encode()).hexdigest()
assert definition=={'module':'algorithms.ax','name':'pipeline','parameters':[{'name':'T','bounds':bounds}],'params':[{'parameter':0}],'result':'i64','body_sha256':body_sha}
assert record['definition_sha256']==digest('axeon-export-generic-v1',definition)
shape={'module':generic['path'],'types':[t['definition'] for t in generic['type_exports']], 'functions':[e['signature'] for e in generic['exports']], 'traits':[t['definition'] for t in generic['trait_exports']], 'implementations':generic['implementations'], 'generics':[g['definition'] for g in generic['generic_exports']]}
assert generic['interface_sha256']==digest('axeon-module-interface-v4',shape)
for m in d['modules']:
    data={key:m[key] for key in ['path','content_sha256','interface_sha256','imports']}
    assert m['dependency_sha256']==digest('axeon-module-dependency-v1',data)
    for edge in m['imports']:
        assert edge['interface_sha256']==modules[edge['path']]['interface_sha256']
        assert edge['dependency_sha256']==modules[edge['path']]['dependency_sha256']
assert d['module_graph_sha256']==digest('axeon-module-graph-v1',d['modules'])
assert d['complete_build_sha256']==digest('axeon-complete-build-v1',d['build_inputs'])
assert str(Path.cwd()) not in Path('baseline.json').read_text()
assert 'source_id' not in compact(definition) and 'offset' not in compact(definition)
"#);
    fs::write(
        f.0.join("project/algorithms.ax"),
        ALGORITHMS.replace(
            "return finish(contracts::Summarize::summarize(forwarded));",
            "return finish(contracts::Summarize::summarize(forwarded)) + 0;",
        ),
    )
    .unwrap();
    f.success(&["build", "project/main.ax", "-o", "body"]);
    assert_eq!(
        f.success(&["interpret", "project/main.ax"]).stdout,
        EXPECTED
    );
    fs::write(
        f.0.join("project/algorithms.ax"),
        ALGORITHMS.replace("return value + 1;", "return value + 2;"),
    )
    .unwrap();
    f.success(&["build", "project/main.ax", "-o", "helper"]);
    assert_eq!(
        f.success(&["interpret", "project/main.ax"]).stdout,
        b"43\nresult: 0\n"
    );
    fs::write(
        f.0.join("project/types.ax"),
        TYPES.replace(
            "return samples[0] + samples[1] + samples[2];",
            "return samples[0] + samples[1] + samples[2] + 1;",
        ),
    )
    .unwrap();
    f.success(&["build", "project/main.ax", "-o", "implementation"]);
    assert_eq!(
        f.success(&["interpret", "project/main.ax"]).stdout,
        b"44\nresult: 0\n"
    );
    f.python(r#"
import json
from pathlib import Path
load=lambda n:json.loads(Path(n+'.json').read_text())
a,b,c,d=[load(n) for n in ['baseline','body','helper','implementation']]
ma,mb,mc,md=[{m['path']:m for m in x['modules']} for x in [a,b,c,d]]
assert ma['algorithms.ax']['interface_sha256']!=mb['algorithms.ax']['interface_sha256']
assert ma['algorithms.ax']['interface_sha256']==mc['algorithms.ax']['interface_sha256']==md['algorithms.ax']['interface_sha256']
assert ma['algorithms.ax']['dependency_sha256']!=mc['algorithms.ax']['dependency_sha256']
assert mc['types.ax']['interface_sha256']==md['types.ax']['interface_sha256']
assert mc['types.ax']['dependency_sha256']!=md['types.ax']['dependency_sha256']
assert len({x['entry_dependency_sha256'] for x in [a,b,c,d]})==4
assert len({x['complete_build_sha256'] for x in [a,b,c,d]})==4
"#);
}
