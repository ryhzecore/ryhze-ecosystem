//! Exercise private union results through project, formatting and test commands.
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

#[test]
fn unions_integrate_with_formatter_reference_tests_and_distributable_builds() {
    let f =
        Fixture(std::env::temp_dir().join(format!("axeon-union-workflow-{}", std::process::id())));
    fs::create_dir(&f.0).unwrap();
    f.success(&["new", "project"]);
    let module = r#"// Private result type, public scalar boundary.
union Reading { Value { number: f64, valid: bool }, Missing {}, Error { code: i64 } }
fn read(n:i64)->Reading {
  if n<0 { return Reading::Error { code:0-n }; }
  if n==0 { return Reading::Missing {}; }
  return Reading::Value { valid:true, number:f64_from_i64(n) };
}
fn forward(n:i64)->Reading { return read(n); }
export fn score(n:i64)->i64 {
 let reading:Reading=forward(n);
 match reading {
  Reading::Value { number:value, valid:ok } => {
   if ok && value==3.0 { return 3; }
   return 1;
  },
  Reading::Missing {} => { return 0; },
  Reading::Error { code:error } => { return error; },
 }
}
"#;
    fs::write(f.0.join("project/math.ax"), module).unwrap();
    fs::write(
        f.0.join("project/main.ax"),
        "import math; fn main()->i64 { print(math::score(3)); return math::score(-7)+math::score(3)+math::score(0); }",
    )
    .unwrap();
    fs::write(
        f.0.join("project/tests.ax"),
        "import math; fn test_value()->i64{return math::score(3)-3;} fn test_missing()->i64{return math::score(0);} fn test_error()->i64{return math::score(-7)-7;} fn main()->i64{return 99;}",
    )
    .unwrap();
    f.success(&["check", "project/main.ax"]);
    let expected = b"3\nresult: 10\n";
    assert_eq!(
        f.success(&["interpret", "project/main.ax"]).stdout,
        expected
    );
    let tests = f.success(&["test", "project/tests.ax"]);
    assert_eq!(
        tests.stdout,
        b"running 3 tests (reference interpreter; fuel 1000000 per test)\ntest test_error ...\nPASS test_error\ntest test_missing ...\nPASS test_missing\ntest test_value ...\nPASS test_value\ntest result: 3 passed; 0 failed\n"
    );
    for name in ["math", "main", "tests"] {
        let path = format!("project/{name}.ax");
        let formatted = f.success(&["format", &path]);
        fs::write(f.0.join(&path), &formatted.stdout).unwrap();
        f.success(&["format", &path, "--check"]);
    }
    assert_eq!(
        f.success(&["test", "project/tests.ax"]).stdout,
        tests.stdout
    );
    for opt in ["0", "2", "3"] {
        assert_eq!(
            f.success(&["run", "project/main.ax", "--opt", opt]).stdout,
            expected
        );
    }
    f.success(&[
        "build",
        "project/main.ax",
        "--opt",
        "3",
        "-o",
        "application",
    ]);
    let standalone = support::bounded_with_kill(f.0.join("application"), "5s", "1s")
        .output()
        .unwrap();
    assert!(standalone.status.success());
    assert!(standalone.stderr.is_empty());
    assert_eq!(standalone.stdout, expected);

    // A valid ordinary private union producer is not an i64 test entry point.
    fs::write(
        f.0.join("project/bad-tests.ax"),
        "union Outcome{Done{}} fn test_invalid()->Outcome{return Outcome::Done{};} fn main()->i64{return 0;}",
    )
    .unwrap();
    let invalid_test = f.run(&["test", "project/bad-tests.ax"]);
    assert_eq!(invalid_test.status.code(), Some(2));
    assert!(invalid_test.stdout.is_empty());
    assert!(String::from_utf8_lossy(&invalid_test.stderr).contains("E_TEST_SIGNATURE"));
}
