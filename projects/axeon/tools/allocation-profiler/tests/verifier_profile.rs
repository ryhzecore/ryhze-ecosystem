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
            let path = std::env::temp_dir().join(format!(
                "axeon-verifier-profile-test-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create fixture: {error}"),
            }
        }
        panic!("fixture naming budget exhausted");
    }
    fn run(&self, args: &[&str]) -> Output {
        let stdout = self.0.join("child.stdout");
        let stderr = self.0.join("child.stderr");
        let mut child = Command::new(env!("CARGO_BIN_EXE_verify-ir-profile"))
            .args(args)
            .current_dir(&self.0)
            .stdin(Stdio::null())
            .stdout(fs::File::create(&stdout).unwrap())
            .stderr(fs::File::create(&stderr).unwrap())
            .spawn()
            .unwrap();
        let deadline = Instant::now() + Duration::from_secs(35);
        let status = loop {
            match child.try_wait() {
                Ok(Some(status)) => break status,
                Ok(None) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(10));
                }
                other => {
                    let _ = child.kill();
                    let _ = child.wait();
                    panic!("verification profiler child failed or timed out: {other:?}");
                }
            }
        };
        Output {
            status,
            stdout: fs::read(stdout).unwrap(),
            stderr: fs::read(stderr).unwrap(),
        }
    }
    fn report(&self, directory: &str) -> String {
        fs::read_to_string(self.0.join(directory).join("verification.json")).unwrap()
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn complete(report: &str, samples: usize) {
    for text in [
        "\"status\":\"complete\"",
        "\"outcome\":\"success\"",
        "\"active_phase\":null",
        "\"failure\":null",
        "\"counting_allocator\":false",
        "\"loads_and_checks_once\":true",
        "\"check_includes_verification\":true",
        "\"explicit_samples_repeat_check_verification\":true",
        "\"source_executed\":false",
        "\"end_to_end_compile_timing\":false",
    ] {
        assert!(report.contains(text), "{text}: {report}");
    }
    assert!(report.contains(&format!("\"requested_samples\":{samples},")));
    assert!(report.contains(&format!(
        "\"observed_successful_explicit_calls\":{}",
        samples + 1
    )));
    assert_eq!(report.matches("\"kind\":\"warmup\"").count(), 1);
    assert_eq!(report.matches("\"kind\":\"measured\"").count(), samples);
    assert_eq!(report.matches("\"verified\":true").count(), samples + 1);
    assert!(!report.contains("\"verified\":false"));
    let raw = report
        .split("\"samples\":[")
        .nth(1)
        .unwrap()
        .split("],\"observed_successful_explicit_calls\"")
        .next()
        .unwrap();
    for (position, item) in raw.split("},{").enumerate() {
        let duration = item
            .split("\"nanoseconds\":")
            .nth(1)
            .unwrap()
            .split(',')
            .next()
            .unwrap()
            .parse::<u128>()
            .unwrap();
        assert!(
            duration <= 30_000_000_000,
            "completed duration exceeds deadline"
        );
        let index = item
            .split("\"index\":")
            .nth(1)
            .unwrap()
            .split(',')
            .next()
            .unwrap()
            .parse::<usize>()
            .unwrap();
        assert_eq!(index, position.saturating_sub(1));
    }
}

#[test]
fn defaults_record_seven_warm_ir_samples_without_executing_source() {
    let temp = Temp::new();
    let source = "fn main()->i64{print(99);return 1/0;}";
    fs::write(temp.0.join("main.ax"), source).unwrap();
    let output = temp.run(&["main.ax", "--output-dir", "sample"]);
    assert!(output.status.success(), "{output:?}");
    assert!(output.stdout.is_empty() && output.stderr.is_empty());
    complete(&temp.report("sample"), 7);
    assert_eq!(fs::read_to_string(temp.0.join("main.ax")).unwrap(), source);
}

#[test]
fn module_inputs_and_sample_bounds_are_supported() {
    let temp = Temp::new();
    fs::write(temp.0.join("data.ax"), "export fn done()->(){return ();}").unwrap();
    fs::write(
        temp.0.join("main.ax"),
        "import data;fn main()->i64{data::done();return 42;}",
    )
    .unwrap();
    for (samples, directory) in [("1", "one"), ("101", "maximum")] {
        let output = temp.run(&[
            "main.ax",
            "--output-dir",
            directory,
            "--project-root",
            ".",
            "--samples",
            samples,
        ]);
        assert!(output.status.success(), "{output:?}");
        let report = temp.report(directory);
        complete(&report, samples.parse().unwrap());
        assert!(report.contains("\"logical_path\":\"data.ax\""));
        assert!(report.contains("\"logical_path\":\"main.ax\""));
    }
}

#[test]
fn invalid_sources_preserve_failed_receipts_without_samples() {
    let temp = Temp::new();
    for (source, directory, outcome) in [
        ("fn main()->i64{return ;}", "parse", "load_error"),
        ("fn main()->i64{return ();}", "check", "check_error"),
    ] {
        fs::write(temp.0.join("main.ax"), source).unwrap();
        let output = temp.run(&["main.ax", "--output-dir", directory]);
        assert_eq!(output.status.code(), Some(1), "{output:?}");
        let report = temp.report(directory);
        assert!(report.contains("\"status\":\"failed\""));
        assert!(report.contains(&format!("\"outcome\":\"{outcome}\"")));
        assert!(report.contains("\"samples\":[]"));
        assert!(report.contains("\"observed_successful_explicit_calls\":0"));
    }
}

#[test]
fn invalid_options_and_existing_output_never_overwrite() {
    let temp = Temp::new();
    fs::write(temp.0.join("main.ax"), "fn main()->i64{return 0;}").unwrap();
    fs::create_dir(temp.0.join("existing")).unwrap();
    fs::write(temp.0.join("existing/keep"), "untouched").unwrap();
    assert_eq!(
        temp.run(&["main.ax", "--output-dir", "existing"])
            .status
            .code(),
        Some(2)
    );
    assert_eq!(
        fs::read(temp.0.join("existing/keep")).unwrap(),
        b"untouched"
    );
    assert!(!temp.0.join("existing/verification.json").exists());
    for invalid in ["0", "102", "+2", "-1", "1.0", "99999999999999999999999999"] {
        assert_eq!(
            temp.run(&["main.ax", "--output-dir", "invalid", "--samples", invalid])
                .status
                .code(),
            Some(2)
        );
        assert!(!temp.0.join("invalid").exists());
    }
    for args in [
        vec!["main.ax"],
        vec!["main.ax", "--output-dir", "invalid", "--samples"],
        vec![
            "main.ax",
            "--output-dir",
            "invalid",
            "--samples",
            "1",
            "--samples",
            "2",
        ],
        vec!["main.ax", "--output-dir", "invalid", "--unknown", "x"],
    ] {
        assert_eq!(temp.run(&args).status.code(), Some(2));
        assert!(!temp.0.join("invalid").exists());
    }
}
