//! Local project/test workflow. Subprocesses are bounded and native starter
//! execution requires the supported Clang, as do the existing CLI tests.
mod support;
#[cfg(unix)]
use std::os::unix::fs::{symlink, PermissionsExt};
#[cfg(unix)]
use std::path::Path;
use std::{
    fs,
    path::PathBuf,
    process::{Command, Output},
    sync::atomic::{AtomicU64, Ordering},
};

static NEXT: AtomicU64 = AtomicU64::new(0);

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-workflow-{}-{}",
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
    fn run(&self, args: &[&str]) -> Output {
        self.command(args).output().unwrap()
    }
    fn command(&self, args: &[&str]) -> Command {
        let mut command = support::bounded(env!("CARGO_BIN_EXE_axeon"), "45s");
        command.args(args).current_dir(&self.0);
        command
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
        text(&output.stdout),
        text(&output.stderr)
    );
}
fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).unwrap()
}

#[test]
fn new_project_checks_runs_tests_and_builds_a_distributable_program() {
    let fixture = Fixture::new();
    status(&fixture.run(&["new", "sample project"]), 0);
    let root = fixture.0.join("sample project");
    let mut files: Vec<_> = fs::read_dir(&root)
        .unwrap()
        .map(|entry| entry.unwrap().file_name())
        .collect();
    files.sort();
    assert_eq!(
        files,
        ["main.ax", "math.ax", "tests.ax"].map(std::ffi::OsString::from)
    );
    for source in [
        "sample project/main.ax",
        "sample project/math.ax",
        "sample project/tests.ax",
    ] {
        let formatted = fixture.run(&["format", source, "--check"]);
        status(&formatted, 0);
        assert!(formatted.stderr.is_empty());
    }
    for source in ["sample project/main.ax", "sample project/tests.ax"] {
        let checked = fixture.run(&["check", source]);
        status(&checked, 0);
        assert!(checked.stderr.is_empty());
    }
    for mode in ["interpret", "run"] {
        let output = fixture.run(&[mode, "sample project/main.ax"]);
        status(&output, 0);
        assert_eq!(text(&output.stdout), "42\nresult: 0\n");
        assert!(output.stderr.is_empty());
    }
    let tested = fixture.run(&[
        "test",
        "sample project/tests.ax",
        "--project-root",
        "sample project",
    ]);
    status(&tested, 0);
    assert!(tested.stderr.is_empty());
    assert!(text(&tested.stdout).contains("reference interpreter"));
    assert!(text(&tested.stdout).ends_with("test result: 2 passed; 0 failed\n"));
    let built = fixture.run(&["build", "sample project/main.ax", "-o", "sample-program"]);
    status(&built, 0);
    assert!(fixture.0.join("sample-program.json").is_file());
    let run = support::bounded(fixture.0.join("sample-program"), "5s")
        .output()
        .unwrap();
    status(&run, 0);
    assert_eq!(text(&run.stdout), "42\nresult: 0\n");
}

#[test]
fn project_creation_never_alters_an_existing_file_directory_or_symlink() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join("existing")).unwrap();
    fixture.file("existing/main.ax", "keep this existing source\n");
    fixture.file("regular", "keep this file\n");
    fs::create_dir(fixture.0.join("empty")).unwrap();
    #[cfg(unix)]
    symlink("existing", fixture.0.join("linked")).unwrap();
    for destination in [
        "existing",
        "regular",
        "empty",
        #[cfg(unix)]
        "linked",
        ".",
    ] {
        status(&fixture.run(&["new", destination]), 2);
    }
    assert_eq!(
        fs::read_to_string(fixture.0.join("existing/main.ax")).unwrap(),
        "keep this existing source\n"
    );
    assert_eq!(
        fs::read_to_string(fixture.0.join("regular")).unwrap(),
        "keep this file\n"
    );
    assert_eq!(fs::read_dir(fixture.0.join("existing")).unwrap().count(), 1);
    assert_eq!(fs::read_dir(fixture.0.join("empty")).unwrap().count(), 0);
    #[cfg(unix)]
    assert_eq!(
        fs::read_link(fixture.0.join("linked")).unwrap(),
        Path::new("existing")
    );
    status(&fixture.run(&["new", "missing/child"]), 2);
    assert!(!fixture.0.join("missing").exists());
}

#[test]
fn workflow_arguments_fail_before_creating_or_executing_anything() {
    let fixture = Fixture::new();
    fixture.file(
        "tests.ax",
        "fn main() -> i64 { return 0; } fn test_ok() -> i64 { print(999); return 0; }",
    );
    for args in [
        vec!["new"],
        vec!["new", ""],
        vec!["new", "--force"],
        vec!["new", "absent", "extra"],
        vec!["test"],
        vec!["test", ""],
        vec!["test", "--fuel", "1"],
        vec!["test", "tests.ax", "--fuel"],
        vec!["test", "tests.ax", "--fuel", ""],
        vec!["test", "tests.ax", "--fuel", "-1"],
        vec!["test", "tests.ax", "--fuel", "+1"],
        vec!["test", "tests.ax", "--fuel", "1.5"],
        vec!["test", "tests.ax", "--fuel", "18446744073709551616"],
        vec!["test", "tests.ax", "--fuel", "1", "--fuel", "2"],
        vec!["test", "tests.ax", "--project-root"],
        vec!["test", "tests.ax", "--project-root", ""],
        vec![
            "test",
            "tests.ax",
            "--project-root",
            ".",
            "--project-root",
            ".",
        ],
        vec!["test", "tests.ax", "--native"],
        vec!["test", "tests.ax", "--opt", "3"],
        vec!["test", "tests.ax", "extra.ax"],
    ] {
        let result = fixture.run(&args);
        status(&result, 2);
        assert!(result.stdout.is_empty());
        assert!(text(&result.stderr).contains("error[D001]"));
    }
    assert!(!fixture.0.join("absent").exists());
    status(&fixture.run(&["test", "missing.ax"]), 2);
}

#[test]
fn entry_tests_are_sorted_imported_candidates_and_main_do_not_run() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "import dep;\nfn main() -> i64 { print(999); return 1 / 0; }\nfn test_z() -> i64 { print(22); return dep::answer() - 42; }\nfn test_a() -> i64 { print(11); return 0; }\nfn testing() -> i64 { print(888); return 7; }\n");
    fixture.file("dep.ax", "export fn answer() -> i64 { return 42; }\nfn test_imported(x: bool) -> bool { return x; }\nfn main() -> i64 { print(777); return 1 / 0; }\n");
    let output = fixture.run(&["test", "tests.ax"]);
    status(&output, 0);
    assert!(output.stderr.is_empty());
    assert_eq!(text(&output.stdout), "running 2 tests (reference interpreter; fuel 1000000 per test)\ntest test_a ...\n11\nPASS test_a\ntest test_z ...\n22\nPASS test_z\ntest result: 2 passed; 0 failed\n");
}

#[test]
fn invalid_test_signatures_reject_the_entire_suite_before_execution() {
    let fixture = Fixture::new();
    for invalid in [
        "fn test_bad(x: i64) -> i64 { return x; }",
        "fn test_bad() -> bool { return true; }",
        "fn test_bad() -> f64 { return 0.0; }",
    ] {
        fixture.file("tests.ax", format!("fn main() -> i64 {{ return 0; }}\nfn test_a() -> i64 {{ print(999); return 0; }}\n{invalid}\n"));
        let output = fixture.run(&["test", "tests.ax"]);
        status(&output, 2);
        assert!(output.stdout.is_empty());
        assert!(text(&output.stderr).contains("tests.ax:3:1: error[E_TEST_SIGNATURE]"));
    }
}

#[test]
fn suites_require_main_and_at_least_one_entry_test_and_limit_sixty_four() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "fn test_a() -> i64 { return 0; }");
    let missing_main = fixture.run(&["test", "tests.ax"]);
    status(&missing_main, 1);
    assert!(text(&missing_main.stderr).contains("error[T002]"));
    fixture.file("tests.ax", "import dep; fn main() -> i64 { return 0; }");
    fixture.file("dep.ax", "export fn test_hidden() -> i64 { return 0; }");
    let empty = fixture.run(&["test", "tests.ax"]);
    status(&empty, 2);
    assert!(text(&empty.stderr).contains("error[E_TEST_EMPTY]"));
    let mut source = "fn main() -> i64 { return 0; }\n".to_owned();
    for i in 0..64 {
        source.push_str(&format!("fn test_{i:02}() -> i64 {{ return 0; }}\n"));
    }
    fixture.file("tests.ax", &source);
    let accepted = fixture.run(&["test", "tests.ax"]);
    status(&accepted, 0);
    assert!(text(&accepted.stdout).ends_with("test result: 64 passed; 0 failed\n"));
    source.push_str("fn test_64() -> i64 { return 0; }\n");
    fixture.file("tests.ax", source);
    let excess = fixture.run(&["test", "tests.ax"]);
    status(&excess, 2);
    assert!(excess.stdout.is_empty());
    assert!(text(&excess.stderr).contains("error[E_TEST_LIMIT]"));
}

#[test]
fn failed_returns_and_imported_numeric_traps_preserve_output_and_continue() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "import dep;\nfn main() -> i64 { return 0; }\nfn test_a_return() -> i64 { print(11); return -7; }\nfn test_b_divide() -> i64 { print(22); return dep::boom(); }\nfn test_c_overflow() -> i64 { print(44); return 9223372036854775807 + 1; }\nfn test_d_pass() -> i64 { print(55); return 0; }\n");
    fixture.file(
        "dep.ax",
        "export fn boom() -> i64 {\n print(33);\n return 1 / 0;\n}\n",
    );
    let output = fixture.run(&["test", "tests.ax"]);
    status(&output, 1);
    let stdout = text(&output.stdout);
    assert!(stdout.contains("11\nFAIL test_a_return at tests.ax:3:1: returned -7\n"));
    assert!(stdout.contains("22\n33\nFAIL test_b_divide\n"));
    assert!(stdout.contains("44\nFAIL test_c_overflow\n"));
    assert!(stdout.contains("55\nPASS test_d_pass\n"));
    assert!(stdout.ends_with("test result: 1 passed; 3 failed\n"));
    assert_eq!(text(&output.stderr), "test test_b_divide: trap[division_by_zero] at dep.ax:3:9\ntest test_c_overflow: trap[overflow] at tests.ax:5:49\n");
}

#[test]
fn each_test_receives_fresh_fuel_and_fuel_traps_are_failures() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "fn main() -> i64 { return 0; }\nfn test_a() -> i64 { return 0; }\nfn test_b() -> i64 { return 0; }\n");
    let fresh = fixture.run(&["test", "tests.ax", "--fuel", "3"]);
    status(&fresh, 0);
    assert!(text(&fresh.stdout).ends_with("test result: 2 passed; 0 failed\n"));
    for fuel in ["0", "2"] {
        let exhausted = fixture.run(&["test", "tests.ax", "--fuel", fuel]);
        status(&exhausted, 1);
        assert_eq!(
            text(&exhausted.stderr)
                .matches("trap[fuel_exhausted]")
                .count(),
            2
        );
        assert!(text(&exhausted.stdout).ends_with("test result: 0 passed; 2 failed\n"));
    }
    let maximum = fixture.run(&["test", "tests.ax", "--fuel", "18446744073709551615"]);
    status(&maximum, 0);
}

#[test]
fn tests_keep_private_imports_private_and_check_all_frozen_sources() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "import dep;\nfn main() -> i64 { return 0; }\nfn test_private() -> i64 { return dep::hidden(); }\n");
    fixture.file("dep.ax", "fn hidden() -> i64 { return 0; }");
    let private = fixture.run(&["test", "tests.ax"]);
    status(&private, 1);
    assert!(private.stdout.is_empty());
    assert!(text(&private.stderr).contains("tests.ax:3:"));
    fixture.file(
        "tests.ax",
        "import dep; fn main() -> i64 { return 0; } fn test_pass() -> i64 { return 0; }",
    );
    fixture.file("dep.ax", "fn broken() -> i64 { return true; }");
    let broken_dependency = fixture.run(&["test", "tests.ax"]);
    status(&broken_dependency, 1);
    assert!(broken_dependency.stdout.is_empty());
    assert!(text(&broken_dependency.stderr).contains("dep.ax:1:"));
}

#[test]
fn test_project_roots_and_symlink_confinement_match_the_compiler() {
    let fixture = Fixture::new();
    status(&fixture.run(&["new", "inside"]), 0);
    fs::create_dir(fixture.0.join("outside")).unwrap();
    let outside_entry = fixture.run(&["test", "inside/tests.ax", "--project-root", "outside"]);
    status(&outside_entry, 1);
    assert!(outside_entry.stdout.is_empty());
    assert!(text(&outside_entry.stderr).contains("E_MODULE_PATH"));
    #[cfg(unix)]
    {
        fixture.file("outside/other.ax", "export fn value() -> i64 { return 0; }");
        symlink("../outside/other.ax", fixture.0.join("inside/other.ax")).unwrap();
        fixture.file("inside/tests.ax", "import other; fn main() -> i64 { return 0; } fn test_a() -> i64 { return other::value(); }");
        let alias = fixture.run(&["test", "inside/tests.ax", "--project-root", "inside"]);
        status(&alias, 1);
        assert!(alias.stdout.is_empty());
        assert!(text(&alias.stderr).contains("E_MODULE_PATH"));
    }
    #[cfg(windows)]
    {
        fixture.file(
            "inside/tests.ax",
            "import other; fn main() -> i64 { return 0; } fn test_a() -> i64 { return 0; }",
        );
        fs::hard_link(
            fixture.0.join("inside/tests.ax"),
            fixture.0.join("inside/other.ax"),
        )
        .unwrap();
        let alias = fixture.run(&["test", "inside/tests.ax", "--project-root", "inside"]);
        status(&alias, 1);
        assert!(alias.stdout.is_empty());
        assert!(text(&alias.stderr).contains("E_MODULE_ALIAS"));
    }
}

#[test]
#[cfg(unix)]
fn test_and_new_do_not_invoke_compilers_dependency_hooks_or_ambient_scripts() {
    let fixture = Fixture::new();
    let hook = fixture.file("hook", "#!/bin/sh\ntouch HOOK_RAN\nexit 1\n");
    fs::set_permissions(&hook, fs::Permissions::from_mode(0o755)).unwrap();
    fixture.file("build.sh", "touch HOOK_RAN\n");
    fixture.file("axeon.toml", "[hooks]\ntest = './hook'\nnew = './hook'\n");
    let created = fixture
        .command(&["new", "sample"])
        .env("AXEON_CLANG", &hook)
        .env("AXEON_TENSOR_RUNTIME", &hook)
        .env("BASH_ENV", &hook)
        .output()
        .unwrap();
    status(&created, 0);
    let tested = fixture
        .command(&["test", "sample/tests.ax"])
        .env("AXEON_CLANG", &hook)
        .env("AXEON_TENSOR_RUNTIME", &hook)
        .env("BASH_ENV", &hook)
        .output()
        .unwrap();
    status(&tested, 0);
    assert!(!fixture.0.join("HOOK_RAN").exists());
    assert!(!fixture.0.join("sample/HOOK_RAN").exists());
}

#[cfg(windows)]
#[test]
fn windows_test_and_new_ignore_compiler_settings_and_package_hooks() {
    let fixture = Fixture::new();
    let hook = fixture.file(
        "hook.cmd",
        "@echo off\r\necho hook>HOOK_RAN\r\nexit /b 1\r\n",
    );
    fixture.file("build.cmd", "@echo hook>HOOK_RAN\r\n");
    fixture.file(
        "axeon.toml",
        "[hooks]\ntest = 'hook.cmd'\nnew = 'hook.cmd'\n",
    );
    for args in [vec!["new", "sample"], vec!["test", "sample/tests.ax"]] {
        let output = fixture
            .command(&args)
            .env("AXEON_CLANG", &hook)
            .env("AXEON_TENSOR_RUNTIME", &hook)
            .env("COMSPEC", &hook)
            .output()
            .unwrap();
        status(&output, 0);
    }
    assert!(!fixture.0.join("HOOK_RAN").exists());
    assert!(!fixture.0.join("sample/HOOK_RAN").exists());
}

#[test]
fn output_budget_is_enforced_per_test_and_following_tests_still_run() {
    let fixture = Fixture::new();
    fixture.file("tests.ax", "fn main() -> i64 { return 0; }\nfn test_a_output() -> i64 {\n while true { print(9223372036854775807); }\n return 0;\n}\nfn test_b_output() -> i64 { print(42); return 0; }\n");
    let output = fixture.run(&["test", "tests.ax"]);
    status(&output, 1);
    assert!(output.stdout.len() <= (1 << 20) + 1024);
    assert!(text(&output.stderr).contains("test test_a_output: trap[output_limit]"));
    assert!(text(&output.stdout).contains("42\nPASS test_b_output\n"));
    assert!(text(&output.stdout).ends_with("test result: 1 passed; 1 failed\n"));
}
