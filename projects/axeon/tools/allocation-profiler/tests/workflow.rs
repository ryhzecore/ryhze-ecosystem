#![forbid(unsafe_code)]
use std::fs;
use std::path::PathBuf;
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::{Duration, Instant};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        for _ in 0..1000 {
            let id = NEXT.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("axeon-allocation-test-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => panic!("cannot create fixture: {e}"),
            }
        }
        panic!("fixture naming budget exhausted")
    }
    fn write(&self, name: &str, value: &str) {
        fs::write(self.0.join(name), value).unwrap();
    }
    fn run(&self, args: &[&str]) -> Output {
        let stdout_path = self.0.join("child.stdout");
        let stderr_path = self.0.join("child.stderr");
        let mut child = Command::new(env!("CARGO_BIN_EXE_axeon-allocation-profiler"))
            .args(args)
            .current_dir(&self.0)
            .stdin(Stdio::null())
            .stdout(fs::File::create(&stdout_path).unwrap())
            .stderr(fs::File::create(&stderr_path).unwrap())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(30);
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break status,
                Ok(None) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(10))
                }
                result => {
                    let _ = child.kill();
                    let _ = child.wait();
                    panic!("profiler child failed or timed out: {result:?}");
                }
            }
        };
        Output {
            status,
            stdout: fs::read(stdout_path).unwrap(),
            stderr: fs::read(stderr_path).unwrap(),
        }
    }
    fn report(&self, directory: &str) -> String {
        fs::read_to_string(self.0.join(directory).join("report.json")).unwrap()
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn complete(report: &str, outcome: &str) {
    assert!(report.contains("\"status\":\"complete\""), "{report}");
    assert!(
        report.contains(&format!("\"outcome\":\"{outcome}\"")),
        "{report}"
    );
    assert!(!report.contains("\"accounting_valid\":false"), "{report}");
    assert!(report.contains("\"active_phase\":null"));
}

#[test]
fn project_uses_real_compiler_emission_and_reference_execution() {
    let temp = Temp::new();
    let source = "fn main()->i64{print(2);return 42;}";
    temp.write("main.ax", source);
    let child = temp.run(&["project", "main.ax", "--output-dir", "sample"]);
    assert!(child.status.success(), "{:?}", child);
    let report = temp.report("sample");
    complete(&report, "success");
    assert!(report.contains("\"result\":42"));
    assert_eq!(fs::read(temp.0.join("sample/stdout.txt")).unwrap(), b"2\n");
    let expected = axeon::llvm::emit(&axeon::compile(source).unwrap(), source);
    assert_eq!(
        fs::read_to_string(temp.0.join("sample/emitted.ll")).unwrap(),
        expected
    );
    for phase in [
        "load",
        "check",
        "verify-ir",
        "emit-ir",
        "drop-ir-output",
        "interpret",
        "drop-execution",
        "drop-program",
        "drop-project",
    ] {
        assert!(report.contains(&format!("\"name\":\"{phase}\"")), "{phase}");
    }
    assert!(report.find("\"name\":\"check\"") < report.find("\"name\":\"verify-ir\""));
    assert!(report.find("\"name\":\"verify-ir\"") < report.find("\"name\":\"emit-ir\""));
    assert!(report.contains("\"check_includes_verification\":true"));
    assert!(report.contains("\"verify_ir_repeats_check_verification\":true"));
    assert!(report.contains("\"phases_are_disjoint_pipeline\":false"));
    assert_eq!(fs::read_to_string(temp.0.join("main.ax")).unwrap(), source);
}

#[test]
fn module_aggregate_inputs_use_identical_module_ir_and_exact_float_bits() {
    let temp = Temp::new();
    temp.write(
        "main.ax",
        "import data;fn main()->i64{let r:data::R=data::make();print(r.x);return 42;}",
    );
    temp.write(
        "data.ax",
        "export record R{x:f64}export fn make()->R{return R{x:-0.0};}",
    );
    let child = temp.run(&[
        "project",
        "main.ax",
        "--output-dir",
        "sample",
        "--project-root",
        ".",
    ]);
    assert!(child.status.success(), "{:?}", child);
    complete(&temp.report("sample"), "success");
    let project = axeon::project::Project::load(&temp.0.join("main.ax"), Some(&temp.0)).unwrap();
    let expected = axeon::llvm::emit_modules(&project.check().unwrap(), &project.sources);
    assert_eq!(
        fs::read_to_string(temp.0.join("sample/emitted.ll")).unwrap(),
        expected
    );
    assert_eq!(
        fs::read(temp.0.join("sample/stdout.txt")).unwrap(),
        b"f64:0x8000000000000000\n"
    );
}

#[test]
fn load_and_check_failures_preserve_diagnostics_and_record_cleanup() {
    let temp = Temp::new();
    temp.write("parse.ax", "fn main()->i64{return ;}");
    assert_eq!(
        temp.run(&["project", "parse.ax", "--output-dir", "parse"])
            .status
            .code(),
        Some(1)
    );
    let report = temp.report("parse");
    complete(&report, "load_error");
    assert!(report.contains("\"name\":\"drop-load-error\""));
    assert!(report.contains("\"skipped_phases\":[\"check\",\"verify-ir\""));
    assert!(!report.contains("\"name\":\"verify-ir\""));
    assert!(!temp.0.join("parse/emitted.ll").exists());
    temp.write("check.ax", "fn main()->i64{return true;}");
    assert_eq!(
        temp.run(&["project", "check.ax", "--output-dir", "check"])
            .status
            .code(),
        Some(1)
    );
    let report = temp.report("check");
    complete(&report, "check_error");
    assert!(report.contains("\"T009\""));
    assert!(report.contains("\"name\":\"drop-check-error\""));
    assert!(report.contains("\"name\":\"drop-project\""));
    assert!(report.contains("\"skipped_phases\":[\"verify-ir\""));
    assert!(!report.contains("\"name\":\"verify-ir\""));
    assert!(!temp.0.join("check/stdout.txt").exists());
}

#[test]
fn traps_preserve_prior_output_and_fuel_is_bounded() {
    let temp = Temp::new();
    temp.write("main.ax", "fn main()->i64{print(7);return 1/0;}");
    assert_eq!(
        temp.run(&["project", "main.ax", "--output-dir", "trap"])
            .status
            .code(),
        Some(1)
    );
    let report = temp.report("trap");
    complete(&report, "trap");
    assert!(report.contains("\"name\":\"division_by_zero\""));
    assert_eq!(fs::read(temp.0.join("trap/stdout.txt")).unwrap(), b"7\n");
    temp.write("loop.ax", "fn main()->i64{while true{}return 0;}");
    assert_eq!(
        temp.run(&["project", "loop.ax", "--output-dir", "fuel", "--fuel", "20"])
            .status
            .code(),
        Some(1)
    );
    let report = temp.report("fuel");
    complete(&report, "trap");
    assert!(report.contains("\"name\":\"fuel_exhausted\""));
}

#[test]
fn formatter_success_and_budget_rejection_both_keep_complete_accounting() {
    let temp = Temp::new();
    let source = "fn main()->i64{return 42;}";
    temp.write("main.ax", source);
    assert!(temp
        .run(&["format", "main.ax", "--output-dir", "format"])
        .status
        .success());
    complete(&temp.report("format"), "success");
    assert_eq!(
        fs::read_to_string(temp.0.join("format/formatted.ax")).unwrap(),
        axeon::tooling::format_source(source).unwrap()
    );
    let rejection = format!(
        "fn main()->i64{{{}{}{}return 0;}}",
        "{".repeat(50),
        "//\n".repeat(300_000),
        "}".repeat(50)
    );
    temp.write("budget.ax", &rejection);
    assert_eq!(
        temp.run(&["format", "budget.ax", "--output-dir", "reject"])
            .status
            .code(),
        Some(1)
    );
    let report = temp.report("reject");
    complete(&report, "format_error");
    assert!(report.contains("\"E_FORMAT_LIMIT\""));
    assert!(report.contains("\"name\":\"drop-source\""));
    assert!(!temp.0.join("reject/formatted.ax").exists());
}

#[test]
fn existing_outputs_invalid_options_and_nonregular_sources_are_preserved() {
    let temp = Temp::new();
    temp.write("main.ax", "fn main()->i64{return 0;}");
    fs::create_dir(temp.0.join("existing")).unwrap();
    temp.write("existing/keep", "untouched");
    assert_eq!(
        temp.run(&["project", "main.ax", "--output-dir", "existing"])
            .status
            .code(),
        Some(2)
    );
    assert_eq!(
        fs::read_to_string(temp.0.join("existing/keep")).unwrap(),
        "untouched"
    );
    assert!(!temp.0.join("existing/report.json").exists());
    assert_eq!(
        temp.run(&[
            "project",
            "main.ax",
            "--output-dir",
            "invalid",
            "--fuel",
            "10000001"
        ])
        .status
        .code(),
        Some(2)
    );
    assert!(!temp.0.join("invalid").exists());
    assert_eq!(
        temp.run(&["format", "existing", "--output-dir", "directory"])
            .status
            .code(),
        Some(1)
    );
    complete(&temp.report("directory"), "read_error");
}
