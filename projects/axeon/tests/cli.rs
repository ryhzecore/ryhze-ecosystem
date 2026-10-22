//! Driver integration contracts. Every subprocess uses the host's bounded runner;
//! native cases deliberately require the supported Clang instead of skipping.
mod support;
use std::ffi::{OsStr, OsString};
use std::fs;
#[cfg(unix)]
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::Output;
use std::sync::atomic::{AtomicU64, Ordering};
use support::bounded;

static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(0);
const VALID: &str = "fn main() -> i64 { print(42); print(true); return -7; }\n";
const EMPTY_TENSOR_SCOPE: &str = "fn main() -> i64 { tensor_scope(0) {} catch (err: tensor_error) { print(tensor_error_code(err)); } return 7; }\n";

struct Fixture(PathBuf);

impl Fixture {
    fn new() -> Self {
        let id = NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed);
        let path =
            std::env::temp_dir().join(format!("axeon-cli-tests-{}-{id}", std::process::id()));
        fs::create_dir(&path).expect("unique test directory");
        Self(path)
    }

    fn file(&self, name: &str, bytes: impl AsRef<[u8]>) -> PathBuf {
        let path = self.0.join(name);
        fs::write(&path, bytes).unwrap();
        path
    }

    #[cfg(unix)]
    fn script(&self, name: &str, body: &str) -> PathBuf {
        let path = self.file(name, format!("#!/bin/sh\n{body}\n"));
        fs::set_permissions(&path, fs::Permissions::from_mode(0o755)).unwrap();
        path
    }

    fn run<I, S>(&self, args: I) -> Output
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        self.run_with_clang(args, None)
    }

    fn run_with_clang<I, S>(&self, args: I, clang: Option<&Path>) -> Output
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        self.run_with_runtime(args, clang, None)
    }

    fn run_with_runtime<I, S>(
        &self,
        args: I,
        clang: Option<&Path>,
        runtime: Option<&Path>,
    ) -> Output
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        let mut command = bounded(env!("CARGO_BIN_EXE_axeon"), "45s");
        command
            .current_dir(&self.0)
            .args(args)
            .env_remove("AXEON_TENSOR_RUNTIME");
        #[cfg(unix)]
        command.env_remove("AXEON_CLANG");
        if let Some(clang) = clang {
            command.env("AXEON_CLANG", clang);
        }
        if let Some(runtime) = runtime {
            command.env("AXEON_TENSOR_RUNTIME", runtime);
        }
        command.output().expect("run CLI with bounded timeout")
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn native_clang() -> PathBuf {
    let mut candidates: Vec<OsString> = std::env::var_os("AXEON_CLANG").into_iter().collect();
    #[cfg(not(windows))]
    candidates.extend([
        OsString::from("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang"),
        OsString::from("clang-19"),
        OsString::from("clang"),
    ]);
    #[cfg(windows)]
    candidates.push(OsString::from("clang.exe"));
    for candidate in candidates {
        let Ok(output) = bounded(&candidate, "5s").arg("--version").output() else {
            continue;
        };
        if output.status.success()
            && String::from_utf8_lossy(&output.stdout)
                .lines()
                .next()
                .is_some_and(|line| {
                    line.split_whitespace()
                        .collect::<Vec<_>>()
                        .windows(3)
                        .any(|words| words == ["clang", "version", "19.1.7"])
                })
        {
            return PathBuf::from(candidate);
        }
    }
    panic!("CLI native integration tests require Clang 19.1.7; set AXEON_CLANG");
}

#[cfg(windows)]
fn synthetic_windows_clang(fixture: &Fixture) -> PathBuf {
    // Compile with the real tool before assigning the synthetic AXEON_CLANG.
    // This fixture checks driver failure handling, not SDK/linker availability.
    let source = fixture.file("mock clang.c", include_bytes!("native/mock_clang.c"));
    let executable = fixture.0.join("mock clang.exe");
    let compiled = bounded(native_clang(), "30s")
        .args([
            "--target=x86_64-pc-windows-msvc",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-O0",
        ])
        .args(support::clang_compile_flags())
        .args(support::clang_link_flags())
        .arg(&source)
        .arg("-o")
        .arg(&executable)
        .output()
        .unwrap();
    expect_status(&compiled, 0);
    assert!(executable.is_file());
    executable
}

#[cfg(windows)]
fn synthetic_windows_clang_command(
    fixture: &Fixture,
    clang: &Path,
    mode: &str,
    marker: &Path,
) -> std::process::Command {
    let mut command = bounded(env!("CARGO_BIN_EXE_axeon"), "45s");
    command
        .current_dir(&fixture.0)
        .env_remove("AXEON_TENSOR_RUNTIME")
        .env("AXEON_CLANG", clang)
        .env("AXEON_TEST_MOCK_CLANG_MODE", mode)
        .env("AXEON_TEST_MOCK_CLANG_MARKER", marker);
    command
}

fn archive_tool(clang: &Path) -> PathBuf {
    #[cfg(windows)]
    {
        std::env::var_os("AXEON_AR")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                clang
                    .parent()
                    .unwrap_or_else(|| Path::new(""))
                    .join("llvm-ar.exe")
            })
    }
    #[cfg(not(windows))]
    {
        let _ = clang;
        PathBuf::from("ar")
    }
}

fn archive_file(name: &str) -> String {
    format!("{name}.{}", if cfg!(windows) { "lib" } else { "a" })
}

fn expect_status(output: &Output, expected: i32) {
    assert_eq!(
        output.status.code(),
        Some(expected),
        "stdout: {}\nstderr: {}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
}

fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).expect("CLI output must be UTF-8")
}

fn sha256(path: &Path) -> String {
    #[cfg(not(windows))]
    let output = bounded("sha256sum", "5s").arg(path).output().unwrap();
    #[cfg(windows)]
    let output = bounded(support::python(), "5s")
        .args(["-B", "-X", "utf8", "-c", "import hashlib,sys; from pathlib import Path; sys.stdout.write(hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest())"])
        .arg(path)
        .output()
        .unwrap();
    expect_status(&output, 0);
    let digest = text(&output.stdout).split_whitespace().next().unwrap();
    assert_eq!(digest.len(), 64);
    assert!(digest.bytes().all(|byte| byte.is_ascii_hexdigit()));
    digest.to_owned()
}

fn metadata(path: &Path) -> String {
    fs::read_to_string(PathBuf::from(format!("{}.json", path.display()))).unwrap()
}

#[test]
fn help_version_and_command_errors_are_stable() {
    let fixture = Fixture::new();
    for args in [Vec::<&str>::new(), vec!["--help"]] {
        let output = fixture.run(args);
        expect_status(&output, 0);
        assert!(text(&output.stdout).contains("check|interpret|emit-ir|emit-object|build|run"));
        assert!(output.stderr.is_empty());
    }
    let output = fixture.run(["--version"]);
    expect_status(&output, 0);
    assert_eq!(
        text(&output.stdout),
        "axeon 0.13.0; subset 13; LLVM/Clang 19.1.7; hosted ABI 1 (single-file), 2 (modules)\n"
    );
    for args in [vec!["unknown"], vec!["check"]] {
        let output = fixture.run(args);
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("error[D001]"));
    }
}

#[test]
fn check_and_interpret_have_distinct_success_output() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let output = fixture.run([OsStr::new("check"), source.as_os_str()]);
    expect_status(&output, 0);
    assert!(output.stdout.is_empty());
    assert!(output.stderr.is_empty());
    let output = fixture.run([OsStr::new("interpret"), source.as_os_str()]);
    expect_status(&output, 0);
    assert_eq!(text(&output.stdout), "42\ntrue\nresult: -7\n");
    assert!(output.stderr.is_empty());
}

#[test]
fn interpretation_preserves_prints_before_traps_and_honors_fuel() {
    let fixture = Fixture::new();
    let source = fixture.file("trap.ax", "fn main() -> i64 { print(9); return 1 / 0; }");
    let output = fixture.run([OsStr::new("interpret"), source.as_os_str()]);
    expect_status(&output, 70);
    assert_eq!(text(&output.stdout), "9\n");
    assert_eq!(text(&output.stderr), "trap[division_by_zero] at 1:37\n");
    let source = fixture.file("loop.ax", "fn main() -> i64 { while true {} return 0; }");
    let output = fixture.run([
        OsStr::new("interpret"),
        source.as_os_str(),
        OsStr::new("--fuel"),
        OsStr::new("20"),
    ]);
    expect_status(&output, 70);
    assert!(text(&output.stderr).starts_with("trap[fuel_exhausted] at 1:"));
    let output = fixture.run([
        OsStr::new("interpret"),
        source.as_os_str(),
        OsStr::new("--fuel"),
        OsStr::new("0"),
    ]);
    expect_status(&output, 70);
    assert_eq!(text(&output.stderr), "trap[fuel_exhausted] at 1:1\n");
}

#[test]
fn options_reject_invalid_values_duplicates_and_inapplicable_combinations() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    for invalid in ["1", "02", "+2", "-O2", "3x", "", "99"] {
        let output = fixture.run([
            OsStr::new("run"),
            source.as_os_str(),
            OsStr::new("--opt"),
            OsStr::new(invalid),
        ]);
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("error[D001]"));
    }
    for (command, options) in [
        ("check", vec!["-o", "out"]),
        ("interpret", vec!["-o", "out"]),
        ("check", vec!["--opt", "0"]),
        ("interpret", vec!["--opt", "2"]),
        ("emit-ir", vec!["-o", "out", "--opt", "3"]),
        ("run", vec!["--fuel", "10"]),
        ("check", vec!["--diagnostic-format", "yaml"]),
        (
            "check",
            vec!["--diagnostic-format", "text", "--diagnostic-format", "json"],
        ),
        ("run", vec!["--opt", "0", "--opt", "2"]),
        ("interpret", vec!["--fuel", "-1"]),
        ("interpret", vec!["--fuel", "18446744073709551616"]),
        ("check", vec!["--unknown", "value"]),
        ("check", vec!["--diagnostic-format"]),
        ("emit-ir", vec![]),
        ("emit-object", vec![]),
        ("build", vec![]),
    ] {
        let mut args = vec![OsString::from(command), source.as_os_str().to_owned()];
        args.extend(options.into_iter().map(OsString::from));
        let output = fixture.run(args);
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("error[D001]"));
    }
}

#[test]
fn frontend_json_diagnostics_preserve_byte_spans_unicode_columns_and_related_locations() {
    let fixture = Fixture::new();
    let contents = "// λ\nfn main() -> i64 {\n\u{2003}let x: i64 = 1;\n\u{2003}let x: i64 = 2;\n return x;\n}\n";
    #[cfg(not(windows))]
    let source = fixture.file("source\"quoted\nname.ax", contents);
    #[cfg(windows)]
    let source = fixture.file("source 雪 λ.ax", contents);
    let output = fixture.run([
        OsStr::new("check"),
        source.as_os_str(),
        OsStr::new("--diagnostic-format"),
        OsStr::new("json"),
    ]);
    expect_status(&output, 1);
    assert!(output.stdout.is_empty());
    let json = text(&output.stderr);
    assert!(json.starts_with("{\"schema_version\":1,\"diagnostics\":[{"));
    assert!(json.ends_with("]}\n"));
    assert_eq!(
        json.lines().count(),
        1,
        "a path newline must be JSON-escaped"
    );
    assert!(json.contains("\"code\":\"T004\""));
    let first = contents.find("x:").unwrap();
    let second = contents.rfind("x:").unwrap();
    assert!(
        json.contains(&format!(
            "\"start\":{second},\"end\":{},\"line\":4,\"column\":6",
            second + 1
        )),
        "{json}"
    );
    assert!(json.contains(&format!("\"related\":[{{\"start\":{first},\"end\":{},\"message\":\"previous binding declaration\"}}]", first + 1)), "{json}");
    #[cfg(not(windows))]
    assert!(json.contains("source\\\"quoted\\nname.ax"));
    #[cfg(windows)]
    assert!(json.contains("source 雪 λ.ax"));
    assert!(!json.contains("error[D001]"));
}

#[test]
fn frontend_text_diagnostics_have_path_and_related_declaration() {
    let fixture = Fixture::new();
    let source = fixture.file(
        "bad.ax",
        "fn main() -> i64 { let x: i64 = 1; x = 2; return x; }",
    );
    let output = fixture.run([
        OsStr::new("check"),
        source.as_os_str(),
        OsStr::new("--diagnostic-format"),
        OsStr::new("text"),
    ]);
    expect_status(&output, 1);
    let stderr = text(&output.stderr);
    assert!(stderr.contains("error[T006]"));
    assert!(stderr.contains("related at"));
    assert!(stderr.contains(&source.display().to_string()));
    assert!(stderr.contains("bytes "));
}

#[test]
fn missing_non_utf8_and_oversized_source_files_fail_without_artifacts() {
    let fixture = Fixture::new();
    let invalid = fixture.file("invalid.ax", [0xff, 0xfe, b'\n']);
    let oversized = fixture.file("oversized.ax", vec![b' '; (1 << 20) + 1]);
    let missing = fixture.0.join("missing.ax");
    for (source, message) in [
        (&invalid, "source must be valid UTF-8"),
        (&oversized, "source exceeds 1048576-byte limit"),
        (
            &missing,
            if cfg!(windows) {
                "cannot inspect source file"
            } else {
                "cannot open"
            },
        ),
    ] {
        let destination = fixture.0.join("out.ll");
        let output = fixture.run([
            OsStr::new("emit-ir"),
            source.as_os_str(),
            OsStr::new("-o"),
            destination.as_os_str(),
        ]);
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains(message));
        assert!(!destination.exists());
    }
}

#[test]
fn emit_ir_is_deterministic_and_does_not_need_native_compiler() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let unavailable = fixture.0.join("no-such-clang");
    let a = fixture.0.join("first.ll");
    let b = fixture.0.join("second.ll");
    for destination in [&a, &b] {
        let output = fixture.run_with_clang(
            [
                OsStr::new("emit-ir"),
                source.as_os_str(),
                OsStr::new("-o"),
                destination.as_os_str(),
            ],
            Some(&unavailable),
        );
        expect_status(&output, 0);
        assert!(output.stdout.is_empty());
        assert!(output.stderr.is_empty());
    }
    let bytes = fs::read(&a).unwrap();
    assert_eq!(bytes, fs::read(&b).unwrap());
    let ir = text(&bytes);
    assert!(ir.contains(&format!("target triple = \"{}\"", support::target_triple())));
    assert!(ir.contains("define internal i64"));
}

#[test]
fn front_end_failures_preserve_existing_outputs_and_metadata() {
    let fixture = Fixture::new();
    let source = fixture.file("bad.ax", "fn main() -> i64 { return true; }");
    let artifact = fixture.file("out", b"old artifact");
    let sidecar = fixture.file("out.json", b"old metadata");
    let unavailable = fixture.0.join("no-such-clang");
    for command in ["emit-ir", "emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&unavailable),
        );
        expect_status(&output, 1);
        assert!(text(&output.stderr).contains("error[T009]"));
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
        assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
    }
}

#[test]
#[cfg(unix)]
fn rejects_unsupported_or_unsuccessful_clang_version_probes() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out", b"old artifact");
    let sidecar = fixture.file("out.json", b"old metadata");
    for (index, body) in [
        "printf 'clang version 20.0.0\\n'\nexit 0",
        "printf 'clang version 19.1.70\\n'\nexit 0",
        "printf 'not a supported compiler\\n'\nexit 0",
        "printf 'clang version 20.0.0 (compatibility 19.1.7 )\\n'\nexit 0",
        "printf 'clang version 19.1.7\\n'\nexit 1",
    ]
    .iter()
    .enumerate()
    {
        let clang = fixture.script(&format!("bad-clang-{index}"), body);
        let output = fixture.run_with_clang(
            [
                OsStr::new("build"),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert!(
            text(&output.stderr).contains("Clang") || text(&output.stderr).contains("compiler"),
            "{}",
            text(&output.stderr)
        );
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
        assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
    }
}

#[test]
#[cfg(unix)]
fn native_compiler_failure_preserves_existing_outputs_and_metadata() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out", b"old artifact");
    let sidecar = fixture.file("out.json", b"old metadata");
    let clang = fixture.script("failing-clang", "if [ \"$1\" = \"--version\" ]; then\n  printf 'clang version 19.1.7\\n'\n  exit 0\nfi\nprintf 'deliberate native compiler failure\\n' >&2\nexit 9");
    for command in ["emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("native compilation failed"));
        assert!(text(&output.stderr).contains("deliberate native compiler failure"));
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
        assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
    }
}

#[cfg(windows)]
#[test]
fn synthetic_windows_clang_rejects_bad_probes_before_compilation() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out.exe", b"old artifact");
    let sidecar = fixture.file("out.exe.json", b"old metadata");
    let clang = synthetic_windows_clang(&fixture);
    for (mode, version, code, stderr) in [
        ("wrong-major", "clang version 20.0.0\n", 0, ""),
        ("version-suffix", "clang version 19.1.70\n", 0, ""),
        ("malformed-version", "not a supported compiler\n", 0, ""),
        (
            "embedded-version",
            "clang version 20.0.0 (compatibility 19.1.7 )\n",
            0,
            "",
        ),
        (
            "probe-failure",
            "clang version 19.1.7\n",
            1,
            "synthetic probe failure\n",
        ),
    ] {
        let marker = fixture.0.join(format!("{mode}-compiled"));
        let probe = bounded(&clang, "5s")
            .arg("--version")
            .env("AXEON_TEST_MOCK_CLANG_MODE", mode)
            .env("AXEON_TEST_MOCK_CLANG_MARKER", &marker)
            .output()
            .unwrap();
        expect_status(&probe, code);
        assert_eq!(probe.stdout, version.as_bytes());
        assert_eq!(probe.stderr, stderr.as_bytes());
        for command in ["emit-object", "build"] {
            let output = synthetic_windows_clang_command(&fixture, &clang, mode, &marker)
                .arg(command)
                .arg(&source)
                .arg("-o")
                .arg(&artifact)
                .output()
                .unwrap();
            expect_status(&output, 2);
            assert!(output.stdout.is_empty());
            let diagnostic = text(&output.stderr);
            assert!(diagnostic.starts_with("error[D001]: native backend requires Clang 19.1.7"));
            assert!(diagnostic.contains(version.trim_end()));
            assert!(diagnostic.contains(&format!("(probe {})", probe.status)));
            if !stderr.is_empty() {
                assert!(diagnostic.contains(stderr));
            }
            assert!(!marker.exists(), "rejected probe must not compile: {mode}");
            assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
            assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
        }
    }
}

#[cfg(windows)]
#[test]
fn synthetic_windows_compiler_failure_preserves_outputs_status_and_diagnostics() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out.exe", b"old artifact");
    let sidecar = fixture.file("out.exe.json", b"old metadata");
    let clang = synthetic_windows_clang(&fixture);
    let direct_marker = fixture.0.join("direct-compiled");
    let failure = bounded(&clang, "5s")
        .arg("synthetic-compilation")
        .env("AXEON_TEST_MOCK_CLANG_MODE", "compiler-failure")
        .env("AXEON_TEST_MOCK_CLANG_MARKER", &direct_marker)
        .output()
        .unwrap();
    expect_status(&failure, 9);
    assert_eq!(failure.stdout, b"synthetic compiler stdout\n");
    assert_eq!(failure.stderr, b"synthetic compiler stderr\n");
    assert_eq!(fs::read(&direct_marker).unwrap(), b"compile\n");
    for command in ["emit-object", "build"] {
        let marker = fixture.0.join(format!("{command}-compiled"));
        let output = synthetic_windows_clang_command(&fixture, &clang, "compiler-failure", &marker)
            .arg(command)
            .arg(&source)
            .arg("-o")
            .arg(&artifact)
            .output()
            .unwrap();
        expect_status(&output, 2);
        // Compiler stdout is captured, not confused with Axeon program output.
        assert!(output.stdout.is_empty());
        assert_eq!(
            text(&output.stderr),
            format!(
                "error[D001]: native compilation failed ({}): synthetic compiler stderr\n\n",
                failure.status
            )
        );
        assert_eq!(fs::read(&marker).unwrap(), b"compile\n");
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
        assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
    }
}

#[cfg(windows)]
#[test]
fn missing_windows_runtime_libraries_report_real_linker_failure_and_preserve_outputs() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out.exe", b"old artifact");
    let sidecar = fixture.file("out.exe.json", b"old metadata");
    let empty_libraries = fixture.0.join("empty native library search");
    fs::create_dir(&empty_libraries).unwrap();
    let clang = native_clang();
    // LLVM 19's MSVC.cpp skips automatic VC/SDK library search when LIB is
    // present. Keep real Clang, lld-link and INCLUDE intact; replace LIB only
    // in this child with a nonempty path to an empty fixture directory.
    // This qualifies missing runtime libraries, not missing SDK headers/linker.
    let output = bounded(env!("CARGO_BIN_EXE_axeon"), "45s")
        .current_dir(&fixture.0)
        .arg("build")
        .arg(&source)
        .arg("-o")
        .arg(&artifact)
        .env("AXEON_CLANG", &clang)
        .env_remove("AXEON_TENSOR_RUNTIME")
        .env("LIB", &empty_libraries)
        .output()
        .unwrap();
    expect_status(&output, 2);
    assert!(output.stdout.is_empty());
    let diagnostic = text(&output.stderr).to_ascii_lowercase();
    assert!(diagnostic.starts_with("error[d001]: native compilation failed"));
    assert!(diagnostic.contains("lld-link: error:"), "{diagnostic}");
    assert!(
        diagnostic.contains("could not open 'libcmt.lib'"),
        "{diagnostic}"
    );
    assert!(
        diagnostic.contains("could not open 'oldnames.lib'"),
        "{diagnostic}"
    );
    assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
    assert_eq!(fs::read(&sidecar).unwrap(), b"old metadata");
    assert_eq!(fs::read_dir(&empty_libraries).unwrap().count(), 0);
}

#[test]
fn artifact_paths_and_symlinks_cannot_overwrite_source() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let clang = native_clang();
    for command in ["emit-ir", "emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                source.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("output cannot overwrite source"));
        assert_eq!(fs::read_to_string(&source).unwrap(), VALID);
    }
    let hard = fixture.0.join("hard.ax");
    fs::hard_link(&source, &hard).unwrap();
    for command in ["emit-ir", "emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                hard.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("output cannot overwrite source"));
        assert_eq!(fs::read_to_string(&source).unwrap(), VALID);
        assert_eq!(fs::read_to_string(&hard).unwrap(), VALID);
    }
    #[cfg(unix)]
    {
        let alias = fixture.0.join("alias.ax");
        symlink(&source, &alias).unwrap();
        let output = fixture.run([
            OsStr::new("emit-ir"),
            source.as_os_str(),
            OsStr::new("-o"),
            alias.as_os_str(),
        ]);
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("output cannot overwrite source"));
        assert_eq!(fs::read_to_string(&source).unwrap(), VALID);
        assert!(fs::symlink_metadata(&alias)
            .unwrap()
            .file_type()
            .is_symlink());
    }
}

#[test]
fn metadata_sidecar_cannot_overwrite_source_or_replace_existing_artifact() {
    let fixture = Fixture::new();
    let source = fixture.file("out.json", VALID);
    let artifact = fixture.file("out", b"old artifact");
    let clang = native_clang();
    for command in ["emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("output cannot overwrite source"));
        assert_eq!(fs::read_to_string(&source).unwrap(), VALID);
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
    }
}

#[test]
fn metadata_destination_directory_failure_preserves_existing_artifact() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out", b"old artifact");
    let sidecar = fixture.0.join("out.json");
    fs::create_dir(&sidecar).unwrap();
    fs::write(sidecar.join("keep"), b"existing directory content").unwrap();
    let clang = native_clang();
    for command in ["emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&clang),
        );
        expect_status(&output, 2);
        assert_eq!(fs::read(&artifact).unwrap(), b"old artifact");
        assert!(sidecar.is_dir());
        assert_eq!(
            fs::read(sidecar.join("keep")).unwrap(),
            b"existing directory content"
        );
    }
}

#[test]
fn all_native_commands_and_optimization_levels_execute_successfully() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let clang = native_clang();
    for optimization in ["0", "2", "3"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new("run"),
                source.as_os_str(),
                OsStr::new("--opt"),
                OsStr::new(optimization),
            ],
            Some(&clang),
        );
        expect_status(&output, 0);
        assert_eq!(text(&output.stdout), "42\ntrue\nresult: -7\n");
        assert!(output.stderr.is_empty());
    }
    let artifact = fixture.0.join("executable");
    let output = fixture.run_with_clang(
        [
            OsStr::new("build"),
            source.as_os_str(),
            OsStr::new("-o"),
            artifact.as_os_str(),
            OsStr::new("--opt"),
            OsStr::new("2"),
        ],
        Some(&clang),
    );
    expect_status(&output, 0);
    assert!(output.stdout.is_empty());
    #[cfg(unix)]
    assert_ne!(
        fs::metadata(&artifact).unwrap().permissions().mode() & 0o111,
        0
    );
    let executed = bounded(&artifact, "5s").output().unwrap();
    expect_status(&executed, 0);
    assert_eq!(text(&executed.stdout), "42\ntrue\nresult: -7\n");
    #[cfg(windows)]
    {
        let executable = fs::read(&artifact).unwrap();
        assert_eq!(&executable[..2], b"MZ");
        let offset = u32::from_le_bytes(executable[60..64].try_into().unwrap()) as usize;
        assert_eq!(&executable[offset..offset + 4], b"PE\0\0");
        assert_eq!(&executable[offset + 4..offset + 6], &[0x64, 0x86]);
    }
    let object = fixture.0.join("module.o");
    let output = fixture.run_with_clang(
        [
            OsStr::new("emit-object"),
            source.as_os_str(),
            OsStr::new("-o"),
            object.as_os_str(),
            OsStr::new("--opt"),
            OsStr::new("3"),
        ],
        Some(&clang),
    );
    expect_status(&output, 0);
    let bytes = fs::read(&object).unwrap();
    #[cfg(not(windows))]
    {
        assert_eq!(&bytes[..4], b"\x7fELF");
        assert_eq!(&bytes[16..18], &[1, 0], "ELF must be a relocatable object");
    }
    #[cfg(windows)]
    {
        assert!(bytes.len() >= 20, "complete COFF header required");
        assert_eq!(&bytes[..2], &[0x64, 0x86], "COFF machine must be AMD64");
        assert_eq!(
            &bytes[4..8],
            &[0; 4],
            "COFF timestamp must be deterministic"
        );
        assert_ne!(
            u16::from_le_bytes([bytes[2], bytes[3]]),
            0,
            "COFF requires sections"
        );
        assert_eq!(
            &bytes[16..18],
            &[0, 0],
            "relocatable COFF has no PE optional header"
        );
    }
    assert!(metadata(&object).contains("\"artifact_kind\": \"object\""));
}

#[test]
fn native_runtime_traps_match_interpreter_output_and_exit_status() {
    let fixture = Fixture::new();
    let source = fixture.file(
        "trap.ax",
        "// λ\nfn main() -> i64 { print(false); return 9223372036854775807 + 1; }\n",
    );
    let interpreted = fixture.run([OsStr::new("interpret"), source.as_os_str()]);
    expect_status(&interpreted, 70);
    let clang = native_clang();
    for optimization in ["0", "2", "3"] {
        let native = fixture.run_with_clang(
            [
                OsStr::new("run"),
                source.as_os_str(),
                OsStr::new("--opt"),
                OsStr::new(optimization),
            ],
            Some(&clang),
        );
        expect_status(&native, 70);
        assert_eq!(native.stdout, interpreted.stdout);
        assert_eq!(native.stderr, interpreted.stderr);
    }
}

#[test]
fn artifact_metadata_hashes_match_files_and_repeat_builds_are_reproducible() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let ir = fixture.0.join("program.ll");
    expect_status(
        &fixture.run([
            OsStr::new("emit-ir"),
            source.as_os_str(),
            OsStr::new("-o"),
            ir.as_os_str(),
        ]),
        0,
    );
    let clang = native_clang();
    for (command, kind) in [("emit-object", "object"), ("build", "executable")] {
        let first = fixture.0.join(format!("{command}-first"));
        let second = fixture.0.join(format!("{command}-second"));
        for artifact in [&first, &second] {
            let output = fixture.run_with_clang(
                [
                    OsStr::new(command),
                    source.as_os_str(),
                    OsStr::new("-o"),
                    artifact.as_os_str(),
                    OsStr::new("--opt"),
                    OsStr::new("2"),
                ],
                Some(&clang),
            );
            expect_status(&output, 0);
            let metadata = metadata(artifact);
            for expected in [
                "\"schema_version\": 2".to_owned(),
                "\"compiler\": \"axeon-0.13.0\"".to_owned(),
                "\"language_subset\": 13".to_owned(),
                "\"bootstrap_rust\": \"1.85.0\"".to_owned(),
                "\"llvm_clang\": \"19.1.7\"".to_owned(),
                format!("\"target\": \"{}\"", support::target_triple()),
                "\"abi\": \"axeon-hosted-1\"".to_owned(),
                "\"runtime\": \"hosted-c-4\"".to_owned(),
                "\"precision\": \"checked-i64-strict-f64-observer-nan-v1\"".to_owned(),
                "\"optimization\": 2".to_owned(),
                format!("\"artifact_kind\": \"{kind}\""),
                format!("\"source_sha256\": \"{}\"", sha256(&source)),
                format!("\"ir_sha256\": \"{}\"", sha256(&ir)),
                format!("\"artifact_sha256\": \"{}\"", sha256(artifact)),
            ] {
                assert!(
                    metadata.contains(&expected),
                    "missing {expected} in {metadata}"
                );
            }
            assert!(!metadata.contains(&fixture.0.display().to_string()));
        }
        assert_eq!(fs::read(&first).unwrap(), fs::read(&second).unwrap());
        assert_eq!(metadata(&first), metadata(&second));
    }
}

#[test]
fn default_native_optimization_is_two_without_relaxed_float_options() {
    let fixture = Fixture::new();
    let source = fixture.file("strict.ax", "fn main()->i64{let x:f64=(1.0000000000000002*0.9999999999999998)-1.0;print(x);print(-0.0);return 0;}");
    let default = fixture.0.join("default");
    let explicit = fixture.0.join("explicit");
    let clang = native_clang();
    let output = fixture.run_with_clang(
        [
            OsStr::new("build"),
            source.as_os_str(),
            OsStr::new("-o"),
            default.as_os_str(),
        ],
        Some(&clang),
    );
    expect_status(&output, 0);
    assert!(metadata(&default).contains("\"optimization\": 2"));
    let output = fixture.run_with_clang(
        [
            OsStr::new("build"),
            source.as_os_str(),
            OsStr::new("-o"),
            explicit.as_os_str(),
            OsStr::new("--opt"),
            OsStr::new("2"),
        ],
        Some(&clang),
    );
    expect_status(&output, 0);
    assert_eq!(fs::read(&default).unwrap(), fs::read(&explicit).unwrap());
    assert_eq!(metadata(&default), metadata(&explicit));
    let execution = bounded(&default, "5s").output().unwrap();
    expect_status(&execution, 0);
    assert_eq!(
        text(&execution.stdout),
        "f64:0x0000000000000000\nf64:0x8000000000000000\nresult: 0\n"
    );
    assert!(execution.stderr.is_empty());
    let rejected = fixture.run([
        OsStr::new("build"),
        source.as_os_str(),
        OsStr::new("-o"),
        default.as_os_str(),
        OsStr::new("--fast-math"),
        OsStr::new("true"),
    ]);
    expect_status(&rejected, 2);
    assert!(
        text(&rejected.stderr).contains("unknown option or unsupported value: --fast-math true")
    );
}

#[test]
fn tensor_frontend_interpreter_and_object_emission_do_not_require_runtime_archive() {
    let fixture = Fixture::new();
    let source = fixture.file("tensor.ax", EMPTY_TENSOR_SCOPE);
    let missing = fixture.0.join(archive_file("missing-runtime"));
    let missing_clang = fixture.0.join("missing-clang");
    for command in ["check", "interpret", "emit-ir", "emit-object"] {
        let artifact = fixture.0.join(format!("{command}.out"));
        let mut args = vec![OsString::from(command), source.as_os_str().to_owned()];
        if matches!(command, "emit-ir" | "emit-object") {
            args.extend([OsString::from("-o"), artifact.as_os_str().to_owned()]);
        }
        let clang = if command == "emit-object" {
            native_clang()
        } else {
            missing_clang.clone()
        };
        let output = fixture.run_with_runtime(args, Some(&clang), Some(&missing));
        expect_status(&output, 0);
        assert!(output.stderr.is_empty());
        assert_eq!(
            text(&output.stdout),
            if command == "interpret" {
                "result: 7\n"
            } else {
                ""
            }
        );
        if command == "emit-object" {
            let receipt = metadata(&artifact);
            assert!(receipt.contains("\"required_abi\":1"));
            assert!(receipt.contains("\"link_required\":true"));
            assert!(receipt.contains("\"archive_sha256\":null"));
        }
    }
}

#[test]
fn invalid_tensor_runtime_is_explicit_and_preserves_existing_artifacts() {
    let fixture = Fixture::new();
    let source = fixture.file("tensor.ax", EMPTY_TENSOR_SCOPE);
    let artifact = fixture.file("program", b"prior executable");
    let sidecar = fixture.file("program.json", b"prior metadata");
    let invalid = fixture.file(&archive_file("invalid"), b"not a static library");
    let missing = fixture.0.join(archive_file("missing"));
    for runtime in [
        invalid.as_path(),
        missing.as_path(),
        fixture.0.as_path(),
        Path::new(""),
    ] {
        let output = fixture.run_with_runtime(
            [
                OsStr::new("build"),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            None,
            Some(runtime),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("error[D001]"));
        assert!(
            text(&output.stderr)
                .to_lowercase()
                .contains("tensor_runtime")
                || text(&output.stderr).contains("tensor runtime")
        );
        assert_eq!(fs::read(&artifact).unwrap(), b"prior executable");
        assert_eq!(fs::read(&sidecar).unwrap(), b"prior metadata");
    }
    // The optional archive does not become a dependency for scalar programs.
    let scalar = fixture.file("scalar.ax", VALID);
    let output = fixture.run_with_runtime(
        [OsStr::new("run"), scalar.as_os_str()],
        None,
        Some(&missing),
    );
    expect_status(&output, 0);
    assert_eq!(text(&output.stdout), "42\ntrue\nresult: -7\n");
}

#[test]
fn tensor_linking_records_exact_runtime_archive_and_executes_standalone() {
    let fixture = Fixture::new();
    let source = fixture.file("tensor.ax", EMPTY_TENSOR_SCOPE);
    let compiler = Path::new(env!("CARGO_BIN_EXE_axeon"));
    let runtime = compiler.parent().unwrap().join(support::archive_name());
    assert!(
        runtime.is_file(),
        "build the workspace runtime before the CLI tensor tests"
    );
    let output_path = fixture.0.join("tensor-program");
    let output = fixture.run_with_runtime(
        [
            OsStr::new("build"),
            source.as_os_str(),
            OsStr::new("-o"),
            output_path.as_os_str(),
        ],
        None,
        Some(&runtime),
    );
    expect_status(&output, 0);
    let receipt = metadata(&output_path);
    assert!(receipt.contains(&format!("\"archive_sha256\":\"{}\"", sha256(&runtime))));
    assert!(receipt.contains("\"required_version\":\"0.1.0\""));
    assert!(receipt.contains("\"link_required\":false"));
    let execution = bounded(&output_path, "5s").output().unwrap();
    expect_status(&execution, 0);
    assert_eq!(text(&execution.stdout), "result: 7\n");
    assert!(execution.stderr.is_empty());
}

#[test]
fn incompatible_tensor_abi_or_version_is_rejected_before_user_main() {
    let fixture = Fixture::new();
    let source = fixture.file("tensor.ax", "fn main() -> i64 { print(12345); tensor_scope(0) {} catch (err: tensor_error) {} return 7; }");
    let clang = native_clang();
    for (abi, version) in [(2, 256), (1, 257)] {
        let c_source = fixture.file(
            "mock.c",
            format!(
                r#"
#include <stdint.h>
#include <stdlib.h>
uint32_t ax_tensor_runtime_abi_version(void) {{ return {abi}; }}
uint64_t ax_tensor_runtime_version(void) {{ return {version}; }}
int32_t ax_tensor_v1_scope(int64_t budget, int32_t (*callback)(void *, void *),
                          void *captures, int64_t start, int64_t end, int64_t *error) {{
    (void)budget; (void)callback; (void)captures; (void)start; (void)end; (void)error;
    abort();
}}
"#
            ),
        );
        let object = fixture.0.join("mock.o");
        let archive = fixture.0.join(archive_file("mock"));
        let compile = bounded(&clang, "10s")
            .args([
                OsStr::new("-c"),
                c_source.as_os_str(),
                OsStr::new("-o"),
                object.as_os_str(),
            ])
            .output()
            .unwrap();
        expect_status(&compile, 0);
        let pack = bounded(archive_tool(&clang), "10s")
            .args([OsStr::new("rcs"), archive.as_os_str(), object.as_os_str()])
            .output()
            .unwrap();
        expect_status(&pack, 0);
        let artifact = fixture.0.join("program");
        let build = fixture.run_with_runtime(
            [
                OsStr::new("build"),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&clang),
            Some(&archive),
        );
        expect_status(&build, 0);
        let execution = bounded(&artifact, "5s").output().unwrap();
        expect_status(&execution, 2);
        assert!(execution.stdout.is_empty());
        assert_eq!(
            text(&execution.stderr),
            "configuration[tensor_runtime]: incompatible internal ABI/version\n"
        );
    }
}

#[cfg(windows)]
#[test]
fn missing_windows_clang_preserves_existing_outputs_and_receipts() {
    let fixture = Fixture::new();
    let source = fixture.file("program.ax", VALID);
    let artifact = fixture.file("out.exe", b"previous executable");
    let sidecar = fixture.file("out.exe.json", b"previous receipt");
    let missing = fixture.0.join("missing-clang.exe");
    for command in ["emit-object", "build"] {
        let output = fixture.run_with_clang(
            [
                OsStr::new(command),
                source.as_os_str(),
                OsStr::new("-o"),
                artifact.as_os_str(),
            ],
            Some(&missing),
        );
        expect_status(&output, 2);
        assert!(text(&output.stderr).contains("error[D001]"));
        assert!(text(&output.stderr).contains("Clang"));
        assert_eq!(fs::read(&artifact).unwrap(), b"previous executable");
        assert_eq!(fs::read(&sidecar).unwrap(), b"previous receipt");
    }
}

#[cfg(windows)]
#[test]
fn unicode_windows_paths_and_crlf_preserve_source_spans_and_native_results() {
    let fixture = Fixture::new();
    let source = fixture.file(
        "雪 λ source.ax",
        "// 雪🙂\r\nfn main() -> i64 {\r\n print(42);\r\n return 1 / 0;\r\n}\r\n",
    );
    let interpreted = fixture.run([OsStr::new("interpret"), source.as_os_str()]);
    expect_status(&interpreted, 70);
    assert_eq!(interpreted.stdout, b"42\n");
    assert_eq!(interpreted.stderr, b"trap[division_by_zero] at 4:9\n");
    let clang = native_clang();
    for opt in ["0", "2", "3"] {
        let native = fixture.run_with_clang(
            [
                OsStr::new("run"),
                source.as_os_str(),
                OsStr::new("--opt"),
                OsStr::new(opt),
            ],
            Some(&clang),
        );
        expect_status(&native, 70);
        assert_eq!(native.stdout, interpreted.stdout);
        assert_eq!(native.stderr, interpreted.stderr);
    }
    let invalid = "// 雪🙂\r\nfn main() -> i64 {\r\n let x: i64 = 1;\r\n let x: i64 = 2;\r\n return x;\r\n}\r\n";
    fs::write(&source, invalid).unwrap();
    let diagnostic = fixture.run([
        OsStr::new("check"),
        source.as_os_str(),
        OsStr::new("--diagnostic-format"),
        OsStr::new("json"),
    ]);
    expect_status(&diagnostic, 1);
    let second = invalid.rfind("x:").unwrap();
    let json = text(&diagnostic.stderr);
    assert!(json.contains("\"code\":\"T004\""));
    assert!(json.contains(&format!(
        "\"start\":{second},\"end\":{},\"line\":4,\"column\":6",
        second + 1
    )));
    assert!(json.contains("雪 λ source.ax"));
}

#[cfg(windows)]
#[test]
fn wrong_machine_tensor_archive_is_rejected_without_publishing_outputs() {
    let fixture = Fixture::new();
    let source = fixture.file("tensor.ax", EMPTY_TENSOR_SCOPE);
    let c_source = fixture.file(
        "wrong-machine.c",
        r#"
unsigned int ax_tensor_runtime_abi_version(void) { return 1; }
unsigned long long ax_tensor_runtime_version(void) { return 256; }
int ax_tensor_v1_scope(long long budget, int (*callback)(void *, void *), void *captures,
                      long long start, long long end, long long *error) { return 0; }
"#,
    );
    let clang = native_clang();
    let object = fixture.0.join("wrong-machine.obj");
    let archive = fixture.0.join("wrong-machine.lib");
    let compile = bounded(&clang, "10s")
        .args([
            OsStr::new("--target=aarch64-pc-windows-msvc"),
            OsStr::new("-c"),
            c_source.as_os_str(),
            OsStr::new("-o"),
            object.as_os_str(),
        ])
        .output()
        .unwrap();
    expect_status(&compile, 0);
    let pack = bounded(archive_tool(&clang), "10s")
        .args([OsStr::new("rcs"), archive.as_os_str(), object.as_os_str()])
        .output()
        .unwrap();
    expect_status(&pack, 0);
    let artifact = fixture.file("out.exe", b"previous executable");
    let sidecar = fixture.file("out.exe.json", b"previous receipt");
    let output = fixture.run_with_runtime(
        [
            OsStr::new("build"),
            source.as_os_str(),
            OsStr::new("-o"),
            artifact.as_os_str(),
        ],
        Some(&clang),
        Some(&archive),
    );
    expect_status(&output, 2);
    assert!(text(&output.stderr).contains("native compilation failed"));
    assert_eq!(fs::read(&artifact).unwrap(), b"previous executable");
    assert_eq!(fs::read(&sidecar).unwrap(), b"previous receipt");
}
