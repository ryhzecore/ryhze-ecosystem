mod support;
use std::fs;
use std::path::PathBuf;
use std::process::Output;
use std::sync::atomic::{AtomicU64, Ordering};

static NEXT: AtomicU64 = AtomicU64::new(0);
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "axeon-tooling-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
    fn run(&self, args: &[&str]) -> Output {
        support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "40s", "2s")
            .args(args)
            .current_dir(&self.0)
            .output()
            .unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn cli_formats_without_mutating_source_and_checks_canonical_form() {
    let f = Fixture::new();
    let source =
        "// precise\r\nfn main()->i64{let a:[f64;2]=[0.1,-0.0];print(f64_bits(a[1]));return 42;}";
    fs::write(f.0.join("input.ax"), source).unwrap();
    let out = f.run(&["format", "input.ax"]);
    assert!(
        out.status.success(),
        "{}",
        String::from_utf8_lossy(&out.stderr)
    );
    assert_eq!(fs::read_to_string(f.0.join("input.ax")).unwrap(), source);
    fs::write(f.0.join("formatted.ax"), &out.stdout).unwrap();
    assert!(f
        .run(&["format", "formatted.ax", "--check"])
        .status
        .success());
    assert_eq!(
        f.run(&["format", "input.ax", "--check"]).status.code(),
        Some(1)
    );
    for mode in ["interpret", "run"] {
        let before = f.run(&[mode, "input.ax"]);
        let after = f.run(&[mode, "formatted.ax"]);
        assert!(
            before.status.success(),
            "{}",
            String::from_utf8_lossy(&before.stderr)
        );
        assert!(
            after.status.success(),
            "{}",
            String::from_utf8_lossy(&after.stderr)
        );
        assert_eq!(before.stdout, after.stdout);
    }
}

#[test]
fn formatter_rejects_bad_syntax_bytes_options_and_directories() {
    let f = Fixture::new();
    fs::write(f.0.join("bad.ax"), "fn main()->i64{return ;}").unwrap();
    let out = f.run(&["format", "bad.ax"]);
    assert_eq!(out.status.code(), Some(1));
    assert!(out.stdout.is_empty());
    fs::write(f.0.join("bytes.ax"), [0xff]).unwrap();
    assert_eq!(f.run(&["format", "bytes.ax"]).status.code(), Some(2));
    assert_eq!(f.run(&["format", "."]).status.code(), Some(2));
    assert_eq!(
        f.run(&["format", "bad.ax", "--write"]).status.code(),
        Some(2)
    );
}

#[cfg(unix)]
#[test]
fn formatter_rejects_fifo_without_waiting_for_a_writer() {
    let f = Fixture::new();
    assert!(support::bounded("mkfifo", "5s")
        .arg(f.0.join("pipe.ax"))
        .status()
        .unwrap()
        .success());
    assert_eq!(f.run(&["format", "pipe.ax"]).status.code(), Some(2));
}

#[test]
fn offline_explanations_cover_registered_production_codes() {
    let f = Fixture::new();
    for code in ["D001", "O007", "N006", "E_MODULE_CYCLE", "E_TEST_SIGNATURE"] {
        let out = f.run(&["explain", code]);
        assert!(out.status.success());
        assert!(String::from_utf8(out.stdout)
            .unwrap()
            .starts_with(&format!("{code}: ")));
    }
    for args in [
        vec!["explain"],
        vec!["explain", "unknown"],
        vec!["explain", "T009", "extra"],
    ] {
        assert_eq!(f.run(&args).status.code(), Some(2));
    }
    // Check the actual diagnostic constructors, not test-only expected strings.
    for file in [
        "lexer.rs",
        "parser.rs",
        "check.rs",
        "project.rs",
        "tooling.rs",
    ] {
        let text = fs::read_to_string(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("src")
                .join(file),
        )
        .unwrap();
        for tail in text.split("Diagnostic::new(").skip(1) {
            let tail = tail.trim_start();
            if let Some(tail) = tail.strip_prefix('"') {
                let code = tail.split('"').next().unwrap();
                assert!(
                    axeon::tooling::explain(code).is_some(),
                    "missing explanation for {file} {code}"
                );
            }
        }
    }
}

#[test]
fn shipped_parseable_examples_format_idempotently() {
    fn visit(path: PathBuf) {
        for item in fs::read_dir(path).unwrap() {
            let path = item.unwrap().path();
            if path.is_dir() {
                visit(path);
            } else if path.extension().is_some_and(|extension| extension == "ax") {
                let source = fs::read_to_string(&path).unwrap();
                if axeon::parser::parse(&source).is_ok() {
                    let formatted = axeon::tooling::format_source(&source)
                        .unwrap_or_else(|errors| panic!("{}: {errors:?}", path.display()));
                    assert_eq!(
                        axeon::tooling::format_source(&formatted).unwrap(),
                        formatted,
                        "{}",
                        path.display()
                    );
                }
            }
        }
    }
    visit(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("examples"));
}
