//! Concrete trait contracts through the developer CLI, including relocation.
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
            "axeon-traits-workflow-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn run(&self, arguments: &[&str]) -> Output {
        support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "45s", "2s")
            .args(arguments)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
    fn success(&self, arguments: &[&str]) -> Output {
        let output = self.run(arguments);
        assert!(output.status.success(), "{arguments:?}: {output:?}");
        assert!(output.stderr.is_empty(), "{output:?}");
        output
    }
    fn project(&self) {
        self.success(&["new", "project"]);
        for (name, source) in SOURCES {
            fs::write(self.0.join("project").join(name), source).unwrap();
        }
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

const CONTRACT: &str = "// A public contract with private implementation functions.\nexport trait Summarize { fn summarize(value: Self, scale: i64) -> i64; fn mark(value: Self) -> (); }\n";
const TYPES: &str = r#"import contracts;
export record Packet { value: i64, stamp: () }
fn summarize_packet(value: Packet, scale: i64) -> i64 {
    print(value.value);
    return value.value * scale;
}
fn mark_packet(value: Packet) -> () { print(value.value); return value.stamp; }
impl contracts::Summarize for Packet {
    summarize = summarize_packet;
    mark = mark_packet;
}
"#;
const MAIN: &str = r#"import contracts; import types;
fn main() -> i64 {
    let mark: types::Packet = types::Packet { value: 5, stamp: () };
    let done: () = contracts::Summarize::mark(mark);
    let packet: types::Packet = types::Packet { value: 21, stamp: done };
    print(contracts::Summarize::summarize(packet, 2));
    return 0;
}
"#;
const TESTS: &str = r#"import contracts; import types;
fn main() -> i64 { return 99; }
fn test_traits() -> i64 {
    let packet: types::Packet = types::Packet { value: 21, stamp: () };
    return contracts::Summarize::summarize(packet, 2) - 42;
}
"#;
const SOURCES: &[(&str, &str)] = &[
    ("contracts.ax", CONTRACT),
    ("types.ax", TYPES),
    ("main.ax", MAIN),
    ("tests.ax", TESTS),
];
const EXPECTED: &[u8] = b"5\n21\n42\nresult: 0\n";

#[test]
fn static_traits_integrate_with_create_format_tests_native_and_distribution() {
    let fixture = Fixture::new();
    fixture.project();
    fixture.success(&["check", "project/main.ax"]);
    assert_eq!(
        fixture.success(&["interpret", "project/main.ax"]).stdout,
        EXPECTED
    );
    let tested = fixture.success(&["test", "project/tests.ax"]);
    assert!(String::from_utf8_lossy(&tested.stdout).contains("PASS test_traits"));
    for (name, _) in SOURCES {
        let path = format!("project/{name}");
        let formatted = fixture.success(&["format", &path]).stdout;
        fs::write(fixture.0.join(&path), &formatted).unwrap();
        fixture.success(&["format", &path, "--check"]);
        assert_eq!(fixture.success(&["format", &path]).stdout, formatted);
    }
    assert_eq!(
        fixture.success(&["test", "project/tests.ax"]).stdout,
        tested.stdout
    );
    for opt in ["0", "2", "3"] {
        assert_eq!(
            fixture
                .success(&["run", "project/main.ax", "--opt", opt])
                .stdout,
            EXPECTED
        );
    }
    fixture.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    let standalone = support::bounded_with_kill(fixture.0.join("app"), "10s", "2s")
        .output()
        .unwrap();
    assert!(standalone.status.success());
    assert_eq!(standalone.stdout, EXPECTED);
    assert!(standalone.stderr.is_empty());
}

#[test]
fn trait_builds_relocate_and_invalid_contracts_preserve_existing_outputs() {
    let first = Fixture::new();
    let second = Fixture::new();
    for fixture in [&first, &second] {
        fixture.project();
        fixture.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    }
    for name in ["app", "app.json"] {
        assert_eq!(
            fs::read(first.0.join(name)).unwrap(),
            fs::read(second.0.join(name)).unwrap()
        );
    }
    let original = fs::read(first.0.join("app")).unwrap();
    let receipt = fs::read(first.0.join("app.json")).unwrap();
    // The contract stays i64; a private binding with a changed signature is invalid.
    fs::write(
        first.0.join("project/types.ax"),
        TYPES
            .replace("scale: i64", "scale: bool")
            .replace("value.value * scale", "value.value"),
    )
    .unwrap();
    let invalid = first.run(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    assert!(!invalid.status.success());
    assert!(invalid.stdout.is_empty());
    let diagnostic = String::from_utf8_lossy(&invalid.stderr);
    assert!(diagnostic.contains("types.ax"), "{diagnostic}");
    assert!(diagnostic.contains("signature"), "{diagnostic}");
    assert_eq!(fs::read(first.0.join("app")).unwrap(), original);
    assert_eq!(fs::read(first.0.join("app.json")).unwrap(), receipt);
}

#[test]
fn unresolved_trait_formatting_preserves_contract_comments_and_tokens() {
    let fixture = Fixture::new();
    let source = "// direct dependency need not exist to format\nimport api;impl api::Contract for i64{method=local;}fn local(v:i64)->i64{return v;}fn main()->i64{return 0;}";
    fs::write(fixture.0.join("source.ax"), source).unwrap();
    let formatted = fixture.success(&["format", "source.ax"]).stdout;
    assert!(String::from_utf8_lossy(&formatted)
        .contains("// direct dependency need not exist to format"));
    fs::write(fixture.0.join("source.ax"), &formatted).unwrap();
    assert_eq!(fixture.success(&["format", "source.ax"]).stdout, formatted);
}
