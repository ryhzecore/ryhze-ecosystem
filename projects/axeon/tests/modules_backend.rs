//! File-aware backend parity, legacy runtime compatibility, and fatal cleanup.
mod support;
use axeon::diagnostic::{line_col, SourceTable};
use axeon::interpreter::{self, TrapKind};
use axeon::project::Project;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        for _ in 0..1000 {
            let id = NEXT.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("axeon-module-backend-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create module test directory: {error}"),
            }
        }
        panic!("temporary directory limit exhausted");
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn bounded(program: &Path, seconds: &str) -> Command {
    support::bounded_with_kill(program, seconds, "2s")
}
fn output(command: &mut Command) -> Output {
    let result = command.output().expect("bounded backend test subprocess");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "backend test subprocess timed out"
    );
    result
}
fn clang() -> PathBuf {
    std::env::var_os("AXEON_CLANG")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("clang"))
}
fn bridge() -> PathBuf {
    std::env::var_os("AXEON_TENSOR_RUNTIME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            Path::new(env!("CARGO_BIN_EXE_axeon"))
                .parent()
                .unwrap()
                .join(support::archive_name())
        })
}

enum Expected<'a> {
    Result(i64),
    Trap {
        kind: TrapKind,
        file: &'a str,
        marker: &'a str,
    },
}

fn verify(files: &[(&str, &str)], stdout: &str, expected: Expected<'_>) {
    let temp = Temp::new();
    for (name, text) in files {
        fs::write(temp.0.join(name), text).unwrap();
    }
    let entry = temp.0.join("main.ax");
    let project = Project::load(&entry, None).expect("valid module fixture");
    let program = project.check().expect("checked module fixture");
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, stdout, "interpreter stdout");
    let (status, native_stdout, stderr) = match expected {
        Expected::Result(value) => {
            assert_eq!(interpreted.result, Ok(value));
            (0, format!("{stdout}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let text = files.iter().find(|(name, _)| *name == file).unwrap().1;
            let start = text.find(marker).expect("trap marker in fixture");
            let (line, column) = line_col(text, start);
            let expected_stderr = format!("trap[{}] at {file}:{line}:{column}\n", kind.name());
            let trap = interpreted.result.expect_err("expected interpreter trap");
            assert_eq!(trap.kind, kind);
            assert_eq!(trap.span.start, start);
            assert_eq!(
                project.sources.get(trap.span.source).unwrap().logical_path,
                file
            );
            assert_eq!(trap.render_modules(&project.sources), expected_stderr);
            (70, stdout.to_owned(), expected_stderr)
        }
    };
    for optimization in ["0", "2", "3"] {
        let executable = support::executable_path(temp.0.join(format!("program-O{optimization}")));
        let compiled = output(
            bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .env("AXEON_TENSOR_RUNTIME", bridge())
                .arg("build")
                .arg(&entry)
                .args(["--opt", optimization, "-o"])
                .arg(&executable),
        );
        assert!(
            compiled.status.success(),
            "module O{optimization} build: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(native.status.code(), Some(status), "O{optimization}");
        assert_eq!(native.stdout, native_stdout.as_bytes(), "O{optimization}");
        assert_eq!(native.stderr, stderr.as_bytes(), "O{optimization}");
    }
}

#[test]
fn imported_scalar_traps_have_local_unicode_aware_locations() {
    for (expression, kind) in [
        ("9223372036854775807 + 1", TrapKind::Overflow),
        ("1 / 0", TrapKind::DivisionByZero),
        ("1 << 64", TrapKind::ShiftCount),
    ] {
        let helper = format!(
            "// λ original UTF-8 bytes\nexport fn fail()->i64{{\n\u{2003}return {expression};\n}}"
        );
        verify(
            &[
                (
                    "main.ax",
                    "import helper; fn main()->i64{print(7);return helper::fail();}",
                ),
                ("helper.ax", &helper),
            ],
            "7\n",
            Expected::Trap {
                kind,
                file: "helper.ax",
                marker: expression,
            },
        );
    }
}

#[test]
fn imported_bounds_trap_bypasses_outer_tensor_catch() {
    verify(
        &[
            ("main.ax", "import helper; fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(7);print(helper::fail());}catch(err:tensor_error){print(999);}return 0;}"),
            ("helper.ax", "export fn fail()->i64{let a:[i64;1]=[1];return a[1];}"),
        ],
        "7\n",
        Expected::Trap { kind: TrapKind::Bounds, file: "helper.ax", marker: "a[1]" },
    );
}

#[test]
fn imported_tensor_callback_trap_keeps_its_enclosing_file() {
    verify(
        &[
            ("main.ax", "import helper; fn main()->i64{return helper::fail();}"),
            ("helper.ax", "// callback belongs here\nexport fn fail()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(7);print(1 / 0);}catch(err:tensor_error){print(999);}return 0;}"),
        ],
        "7\n",
        Expected::Trap { kind: TrapKind::DivisionByZero, file: "helper.ax", marker: "1 / 0" },
    );
}

#[test]
fn imported_call_depth_and_output_limit_keep_file_and_outer_cleanup() {
    let main = "import helper; fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(7);print(helper::run());}catch(err:tensor_error){print(999);}return 0;}";
    verify(
        &[
            ("main.ax", main),
            ("helper.ax", "// recurse in imported module\nfn recurse()->i64{return recurse();}\nexport fn run()->i64{return recurse();}"),
        ],
        "7\n",
        Expected::Trap { kind: TrapKind::CallDepth, file: "helper.ax", marker: "fn recurse" },
    );
    verify(
        &[
            ("main.ax", main),
            (
                "helper.ax",
                "export fn run()->i64{while true{print(1.0);}return 0;}",
            ),
        ],
        &format!("7\n{}", "f64:0x3ff0000000000000\n".repeat(45_590)),
        Expected::Trap {
            kind: TrapKind::OutputLimit,
            file: "helper.ax",
            marker: "print(1.0);",
        },
    );
}

#[test]
fn imported_recursion_traps_on_default_and_two_mib_interpreter_stacks() {
    let temp = Temp::new();
    fs::write(
        temp.0.join("main.ax"),
        "import helper; fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(helper::recurse());}catch(err:tensor_error){print(999);}return 0;}",
    )
    .unwrap();
    fs::write(
        temp.0.join("helper.ax"),
        "export fn recurse()->i64{return recurse();}",
    )
    .unwrap();
    let project = Project::load(&temp.0.join("main.ax"), None).unwrap();
    let program = project.check().unwrap();
    assert_eq!(interpreter::MAX_CALL_DEPTH, 64);
    for stack_size in [None, Some(2 * 1024 * 1024)] {
        let mut thread = std::thread::Builder::new();
        if let Some(bytes) = stack_size {
            thread = thread.stack_size(bytes);
        }
        let program = program.clone();
        let execution = thread
            .spawn(move || interpreter::run(&program, interpreter::DEFAULT_FUEL))
            .unwrap()
            .join()
            .unwrap();
        assert!(execution.stdout.is_empty());
        let trap = execution.result.unwrap_err();
        assert_eq!(trap.kind, TrapKind::CallDepth);
        assert_eq!(
            trap.render_modules(&project.sources),
            "trap[call_depth] at helper.ax:1:8\n"
        );
    }
}

#[test]
fn interpreter_fuel_exhaustion_identifies_the_imported_loop() {
    let temp = Temp::new();
    fs::write(
        temp.0.join("main.ax"),
        "import helper; fn main()->i64{return helper::work();}",
    )
    .unwrap();
    fs::write(
        temp.0.join("helper.ax"),
        "export fn work()->i64{while true{}return 0;}",
    )
    .unwrap();
    let project = Project::load(&temp.0.join("main.ax"), None).unwrap();
    let program = project.check().unwrap();
    let execution = interpreter::run(&program, 200);
    assert!(execution.stdout.is_empty());
    let trap = execution.result.unwrap_err();
    assert_eq!(trap.kind, TrapKind::FuelExhausted);
    assert!(trap
        .render_modules(&project.sources)
        .starts_with("trap[fuel_exhausted] at helper.ax:1:"));
}

#[test]
fn imported_tensor_recovery_observes_source_local_bytes_and_preserves_context() {
    let failing = "tensor_zeros([-1])";
    let helper = format!("// λ UTF-8 offset\nexport fn recover()->i64{{tensor_scope(8){{let bad:tensor=try {failing};print(999);}}catch(err:tensor_error){{print(tensor_error_code(err));print(tensor_error_start(err));print(tensor_error_end(err));}}return 0;}}");
    let start = helper.find(failing).unwrap();
    verify(
        &[
            (
                "main.ax",
                "import helper; fn main()->i64{return helper::recover();}",
            ),
            ("helper.ax", &helper),
        ],
        &format!("2\n{start}\n{}\n", start + failing.len()),
        Expected::Result(0),
    );
    let nested = "export fn nested()->i64{tensor_scope(-1){print(999);}catch(err:tensor_error){print(tensor_error_code(err));print(tensor_error_start(err));print(tensor_error_end(err));}return 6;}";
    let start = nested.find("tensor_scope").unwrap();
    let end = nested.rfind("return 6;").unwrap();
    verify(
        &[
            ("main.ax", "import helper; fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(helper::nested());print(tensor_len(owner));}catch(err:tensor_error){print(999);}return 0;}"),
            ("helper.ax", nested),
        ],
        &format!("15\n{start}\n{end}\n6\n1\n"),
        Expected::Result(0),
    );
}

#[test]
fn imported_floating_and_tensor_results_keep_exact_precision_contract() {
    verify(
        &[
            ("main.ax", "import math; fn main()->i64{print(math::separate());print(math::zero());print(math::subnormal());print(math::nan());print(math::truth());return math::ordered_sum();}"),
            ("math.ax", "export fn separate()->f64{return (1.0000000000000002 * 0.9999999999999998) - 1.0;}export fn zero()->f64{return -0.0;}export fn subnormal()->f64{return f64_from_bits(1) * 1.5;}export fn nan()->f64{return 0.0 / 0.0;}export fn truth()->bool{return true;}export fn ordered_sum()->i64{tensor_scope(24){let data:[f64;3]=[1e16,-1e16,1.0];let a:tensor=try tensor_from_array([3],data);print(try tensor_sum(a));}catch(err:tensor_error){return tensor_error_code(err);}return 0;}"),
        ],
        "f64:0x0000000000000000\nf64:0x8000000000000000\nf64:0x0000000000000002\nf64:0x7ff8000000000000\ntrue\nf64:0x3ff0000000000000\n",
        Expected::Result(0),
    );
}

#[test]
fn file_globals_escape_utf8_bytes_and_legacy_emission_stays_path_free() {
    let source = "fn main()->i64{return 1 / 0;}";
    let program = axeon::compile(source).unwrap();
    let legacy = support::emit(&program, source);
    assert!(legacy.contains("call void @ax_rt_trap(i32 2, i64 1, i64 23)"));
    assert!(!legacy.contains("_v2"));
    assert!(!legacy.contains("@ax_source_"));
    let path = "a\"\\\nλ.ax";
    let mut sources = SourceTable::new();
    sources.push(path, source);
    let ir = support::emit_modules(&program, &sources);
    assert!(ir.contains("@ax_source_0 = private unnamed_addr constant [10 x i8] c\"\\61\\22\\5C\\0A\\CE\\BB\\2E\\61\\78\\00\", align 1"));
    assert!(!ir.contains(path));
    assert_eq!(ir, support::emit_modules(&program, &sources));
    let temp = Temp::new();
    let input = temp.0.join("escaped.ll");
    let executable = support::executable_path(temp.0.join("escaped"));
    fs::write(&input, ir).unwrap();
    compile_c_or_ir(&input, &executable);
    let native = output(&mut bounded(&executable, "10s"));
    assert_eq!(native.status.code(), Some(70));
    assert!(native.stdout.is_empty());
    assert_eq!(
        native.stderr,
        format!("trap[division_by_zero] at {path}:1:23\n").as_bytes()
    );
}

fn compile_c_or_ir(input: &Path, executable: &Path) {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let compiled = output(
        bounded(&clang(), "40s")
            .args(["-O2", "-ffp-contract=off", "-fno-fast-math"])
            .args(["-target", support::target_triple()])
            .args(support::clang_link_flags())
            .arg("-I")
            .arg(root.join("runtime"))
            .arg(input)
            .arg(root.join("runtime/runtime.c"))
            .arg("-o")
            .arg(executable),
    );
    assert!(
        compiled.status.success(),
        "backend probe compilation: {}",
        String::from_utf8_lossy(&compiled.stderr)
    );
}

#[test]
fn both_hosted_abis_clear_and_run_cleanup_once_for_every_fatal_route() {
    let temp = Temp::new();
    let input = temp.0.join("cleanup.c");
    let executable = support::executable_path(temp.0.join("cleanup"));
    fs::write(
        &input,
        r#"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
static void cleanup(void *context) {
    int *calls = context;
    *calls += 1;
    fprintf(stderr, "cleanup:%d:%d\n", *calls, ax_rt_cleanup_active());
}
int main(int argc, char **argv) {
    if (argc != 3) return 3;
    int version = atoi(argv[1]);
    int route = atoi(argv[2]);
    int calls = 0;
    if (ax_rt_register_cleanup(cleanup, &calls) != 0) return 4;
    ax_rt_print_i64(7, 1, 1);
    if (route == 0) {
        if (version == 2) ax_rt_trap_v2(AX_RT_BOUNDS, "dep.ax", 9, 4);
        ax_rt_trap(AX_RT_BOUNDS, 9, 4);
    }
    for (int i = 0; i < 600000; ++i) {
        if (version == 2) {
            if (route == 1) ax_rt_enter_v2("dep.ax", 9, 4);
            if (route == 2) ax_rt_print_i64_v2(1, "dep.ax", 9, 4);
            if (route == 3) ax_rt_print_f64_v2(1.0, "dep.ax", 9, 4);
            if (route == 4) ax_rt_print_bool_v2(1, "dep.ax", 9, 4);
        } else {
            if (route == 1) ax_rt_enter(9, 4);
            if (route == 2) ax_rt_print_i64(1, 9, 4);
            if (route == 3) ax_rt_print_f64(1.0, 9, 4);
            if (route == 4) ax_rt_print_bool(1, 9, 4);
        }
    }
    return 5;
}
"#,
    )
    .unwrap();
    compile_c_or_ir(&input, &executable);
    for version in ["1", "2"] {
        for route in 0..5 {
            let actual = output(bounded(&executable, "10s").args([version, &route.to_string()]));
            assert_eq!(actual.status.code(), Some(70));
            let (kind, unit) = match route {
                0 => ("bounds", ""),
                1 => ("call_depth", ""),
                2 => ("output_limit", "1\n"),
                3 => ("output_limit", "f64:0x3ff0000000000000\n"),
                _ => ("output_limit", "true\n"),
            };
            let expected = if unit.is_empty() {
                "7\n".to_owned()
            } else {
                format!(
                    "7\n{}",
                    unit.repeat((interpreter::MAX_OUTPUT_BYTES - 2) / unit.len())
                )
            };
            assert_eq!(actual.stdout, expected.as_bytes());
            let file = if version == "2" { "dep.ax:" } else { "" };
            assert_eq!(
                actual.stderr,
                format!("cleanup:1:0\ntrap[{kind}] at {file}9:4\n").as_bytes()
            );
        }
    }
}
