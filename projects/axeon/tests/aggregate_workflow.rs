//! Public owned data through a realistic local project and installed-style CLI.
mod support;
use std::fs;
use std::path::PathBuf;
use std::process::Output;

struct Fixture(PathBuf);
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
impl Fixture {
    fn run(&self, args: &[&str]) -> Output {
        support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "45s", "2s")
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
    fn success(&self, args: &[&str]) -> Output {
        let out = self.run(args);
        assert!(
            out.status.success(),
            "{args:?}: {}",
            String::from_utf8_lossy(&out.stderr)
        );
        assert!(out.stderr.is_empty());
        out
    }
}

const TYPES: &str = r#"// Fields and variants are part of the explicit public contract.
export record Reading { base: f64, valid: bool }
export union Outcome { Value { total: f64, count: i64 }, Invalid { code: i64 } }
"#;
const STATISTICS: &str = r#"import types;
export fn samples() -> [f64; 3] { return [1.0, 2.0, 2.5]; }
export fn reading() -> types::Reading { return types::Reading { valid: true, base: 10.0 }; }
fn fold(values: [f64; 3], reading: types::Reading) -> types::Outcome {
    if !reading.valid { return types::Outcome::Invalid { code: 7 }; }
    return types::Outcome::Value { count: 3, total: reading.base + values[0] + values[1] + values[2] };
}
export fn summarize(values: [f64; 3], reading: types::Reading) -> types::Outcome {
    return fold(values, reading);
}
export fn count(outcome: types::Outcome) -> i64 {
    match outcome {
        types::Outcome::Value { total: number, count: count } => { return count; },
        types::Outcome::Invalid { code: error } => { return 0 - error; },
    }
}
export fn report(outcome: types::Outcome) -> i64 {
    match outcome {
        types::Outcome::Value { total: number, count: count } => { print(number); return count; },
        types::Outcome::Invalid { code: error } => { return 0 - error; },
    }
}
"#;
const MAIN: &str = r#"import statistics;
import types;
fn main() -> i64 {
    let samples: [f64; 3] = statistics::samples();
    let reading: types::Reading = statistics::reading();
    let result: types::Outcome = statistics::summarize(samples, reading);
    return statistics::report(result);
}
"#;
const TESTS: &str = r#"import statistics;
import types;
fn main() -> i64 { return 99; }
fn test_valid() -> i64 {
    let samples: [f64; 3] = statistics::samples();
    let reading: types::Reading = statistics::reading();
    let result: types::Outcome = statistics::summarize(samples, reading);
    return statistics::count(result) - 3;
}
fn test_invalid() -> i64 {
    let samples: [f64; 3] = statistics::samples();
    let reading: types::Reading = types::Reading { base: 0.0, valid: false };
    let result: types::Outcome = statistics::summarize(samples, reading);
    return statistics::count(result) + 7;
}
"#;

#[test]
fn exported_owners_integrate_with_format_tests_build_and_standalone_outputs() {
    let fixture = Fixture(
        std::env::temp_dir().join(format!("axeon-aggregate-workflow-{}", std::process::id())),
    );
    fs::create_dir(&fixture.0).unwrap();
    fixture.success(&["new", "project"]);
    for (name, source) in [
        ("types", TYPES),
        ("statistics", STATISTICS),
        ("main", MAIN),
        ("tests", TESTS),
    ] {
        fs::write(fixture.0.join(format!("project/{name}.ax")), source).unwrap();
    }
    fixture.success(&["check", "project/main.ax"]);
    let expected = b"f64:0x402f000000000000\nresult: 3\n";
    assert_eq!(
        fixture.success(&["interpret", "project/main.ax"]).stdout,
        expected
    );
    let tests = fixture.success(&["test", "project/tests.ax"]);
    assert_eq!(tests.stdout, b"running 2 tests (reference interpreter; fuel 1000000 per test)\ntest test_invalid ...\nPASS test_invalid\ntest test_valid ...\nPASS test_valid\ntest result: 2 passed; 0 failed\n");
    for name in ["types", "statistics", "main", "tests"] {
        let path = format!("project/{name}.ax");
        let formatted = fixture.success(&["format", &path]);
        fs::write(fixture.0.join(&path), &formatted.stdout).unwrap();
        fixture.success(&["format", &path, "--check"]);
    }
    assert_eq!(
        fixture.success(&["test", "project/tests.ax"]).stdout,
        tests.stdout
    );
    for opt in ["0", "2", "3"] {
        assert_eq!(
            fixture
                .success(&["run", "project/main.ax", "--opt", opt])
                .stdout,
            expected
        );
    }
    fixture.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    let native = support::bounded_with_kill(fixture.0.join("app"), "10s", "2s")
        .output()
        .unwrap();
    assert!(native.status.success());
    assert_eq!(native.stdout, expected);
    assert!(native.stderr.is_empty());
    // The test command retains its explicit no-argument i64 entry contract.
    fs::write(
        fixture.0.join("project/tests.ax"),
        format!("{TESTS}\nfn test_bad(owner: types::Reading) -> i64 {{ return 0; }}\n"),
    )
    .unwrap();
    let invalid = fixture.run(&["test", "project/tests.ax"]);
    assert_eq!(invalid.status.code(), Some(2));
    assert!(invalid.stdout.is_empty());
    assert!(String::from_utf8_lossy(&invalid.stderr).contains("E_TEST_SIGNATURE"));
}

#[test]
fn qualified_formatting_requires_only_syntax_and_preserves_tokens_and_comments() {
    let source = "// dependency need not be installed to format\nimport types; fn consume(x:types::Reading)->types::Outcome{return types::Outcome::Invalid{code:7};}fn main()->i64{return 0;}";
    let formatted = axeon::tooling::format_source(source).unwrap();
    assert_eq!(
        axeon::tooling::format_source(&formatted).unwrap(),
        formatted
    );
    assert!(formatted.contains("// dependency need not be installed to format"));
    for invalid in [
        "import types; fn main()->i64{let value:types::Reading=;return 0;}",
        "import types; fn main()->i64{let value:types::Reading=types::Reading{base:0.0;return 0;}",
    ] {
        assert!(axeon::tooling::format_source(invalid).is_err());
    }
    let unit = "fn done(value:(// type\n))->(){return (// value\n);}";
    let formatted = axeon::tooling::format_source(unit).unwrap();
    assert_eq!(
        axeon::tooling::format_source(&formatted).unwrap(),
        formatted
    );
    assert!(formatted.contains("// type"));
    assert!(formatted.contains("// value"));
}

#[test]
fn nested_owners_cross_modules_and_integrate_with_format_tests_and_distribution() {
    let fixture =
        Fixture(std::env::temp_dir().join(format!("axeon-nested-workflow-{}", std::process::id())));
    fs::create_dir(&fixture.0).unwrap();
    for (name, source) in [
        ("types", include_str!("../examples/nested-modules/types.ax")),
        (
            "producer",
            include_str!("../examples/nested-modules/producer.ax"),
        ),
        ("main", include_str!("../examples/nested-modules/main.ax")),
        ("tests", include_str!("../examples/nested-modules/tests.ax")),
    ] {
        fs::write(fixture.0.join(format!("{name}.ax")), source).unwrap();
    }
    fixture.success(&["check", "main.ax"]);
    let expected = b"f64:0x402f000000000000\nf64:0x8000000000000000\nresult: 3\n";
    assert_eq!(fixture.success(&["interpret", "main.ax"]).stdout, expected);
    let tests = fixture.success(&["test", "tests.ax"]);
    assert_eq!(tests.stdout, b"running 1 tests (reference interpreter; fuel 1000000 per test)\ntest test_nested_transfer ...\nPASS test_nested_transfer\ntest result: 1 passed; 0 failed\n");
    for name in ["types", "producer", "main", "tests"] {
        let path = format!("{name}.ax");
        let formatted = fixture.success(&["format", &path]);
        fs::write(fixture.0.join(&path), &formatted.stdout).unwrap();
        fixture.success(&["format", &path, "--check"]);
    }
    assert_eq!(fixture.success(&["test", "tests.ax"]).stdout, tests.stdout);
    for opt in ["0", "2", "3"] {
        assert_eq!(
            fixture.success(&["run", "main.ax", "--opt", opt]).stdout,
            expected
        );
    }
    fixture.success(&["build", "main.ax", "--opt", "3", "-o", "app"]);
    let native = support::bounded_with_kill(fixture.0.join("app"), "10s", "2s")
        .output()
        .unwrap();
    assert!(native.status.success());
    assert_eq!(native.stdout, expected);
    assert!(native.stderr.is_empty());
}

#[test]
fn unit_values_integrate_with_owned_modules_and_developer_workflow() {
    let fixture =
        Fixture(std::env::temp_dir().join(format!("axeon-unit-workflow-{}", std::process::id())));
    fs::create_dir(&fixture.0).unwrap();
    fixture.success(&["new", "project"]);
    for (name, source) in [
        ("types", include_str!("../examples/unit-modules/types.ax")),
        (
            "actions",
            include_str!("../examples/unit-modules/actions.ax"),
        ),
        ("main", include_str!("../examples/unit-modules/main.ax")),
        ("tests", include_str!("../examples/unit-modules/tests.ax")),
    ] {
        fs::write(fixture.0.join(format!("project/{name}.ax")), source).unwrap();
    }
    fixture.success(&["check", "project/main.ax"]);
    let expected = b"1\n2\n3\n42\nresult: 0\n";
    assert_eq!(
        fixture.success(&["interpret", "project/main.ax"]).stdout,
        expected
    );
    let tests = fixture.success(&["test", "project/tests.ax"]);
    assert_eq!(tests.stdout, b"running 1 tests (reference interpreter; fuel 1000000 per test)\ntest test_unit_flow ...\nPASS test_unit_flow\ntest result: 1 passed; 0 failed\n");
    for name in ["types", "actions", "main", "tests"] {
        let path = format!("project/{name}.ax");
        let formatted = fixture.success(&["format", &path]);
        fs::write(fixture.0.join(&path), &formatted.stdout).unwrap();
        fixture.success(&["format", &path, "--check"]);
    }
    assert_eq!(
        fixture.success(&["test", "project/tests.ax"]).stdout,
        tests.stdout
    );
    for opt in ["0", "2", "3"] {
        assert_eq!(
            fixture
                .success(&["run", "project/main.ax", "--opt", opt])
                .stdout,
            expected
        );
    }
    fixture.success(&["build", "project/main.ax", "--opt", "3", "-o", "app"]);
    let native = support::bounded_with_kill(fixture.0.join("app"), "10s", "2s")
        .output()
        .unwrap();
    assert!(native.status.success());
    assert_eq!(native.stdout, expected);
    assert!(native.stderr.is_empty());
    fs::write(
        fixture.0.join("project/tests.ax"),
        "fn main()->i64{return 0;} fn test_unit()->(){return ();}",
    )
    .unwrap();
    let invalid = fixture.run(&["test", "project/tests.ax"]);
    assert_eq!(invalid.status.code(), Some(2));
    assert!(invalid.stdout.is_empty());
    assert!(String::from_utf8_lossy(&invalid.stderr).contains("E_TEST_SIGNATURE"));
}
