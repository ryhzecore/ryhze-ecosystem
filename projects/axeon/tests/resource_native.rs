//! Native resource regressions: depth traps must precede stack exhaustion.
mod support;
use axeon::interpreter;
use std::fmt::Write;
use std::fs;
use std::path::PathBuf;
use std::process::Output;
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT_TEMP: AtomicUsize = AtomicUsize::new(0);

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        for _ in 0..1000 {
            let id = NEXT_TEMP.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("axeon-resource-native-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create test directory: {error}"),
            }
        }
        panic!("temporary-directory budget exhausted");
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run_cli(args: &[&std::ffi::OsStr], clang_override: Option<&std::path::Path>) -> Output {
    let mut command = support::bounded_with_kill(env!("CARGO_BIN_EXE_axeon"), "40s", "1s");
    command.args(args);
    if let Some(path) = clang_override {
        command.env("AXEON_CLANG", path);
    }
    let output = command.output().expect("bounded CLI launch");
    assert!(
        !matches!(output.status.code(), Some(124 | 137)),
        "resource regression exceeded the subprocess deadline"
    );
    output
}

fn recursive_source(parameters: usize, extra_locals: usize, assignments: usize) -> String {
    let params = (0..parameters)
        .map(|i| format!("p{i}:i64"))
        .collect::<Vec<_>>()
        .join(",");
    let forwarding = (0..parameters)
        .map(|i| format!("p{i}"))
        .collect::<Vec<_>>()
        .join(",");
    let arguments = vec!["0"; parameters].join(",");
    let mut locals = String::new();
    for i in 0..extra_locals {
        write!(locals, "let mut x{i}:i64=0;").unwrap();
    }
    let body = "x0=x0+1;".repeat(assignments);
    format!(
        "fn f({params})->i64{{{locals}{body}return f({forwarding});}}\
         fn main()->i64{{return f({arguments});}}"
    )
}

#[test]
fn maximum_locals_and_parameters_reach_checked_depth_at_o0() {
    // 32 parameters + 224 local declarations = the 256-local limit.
    // 1 body + 448 declaration nodes + 1564 assignment nodes + 34 return
    // nodes = 2047 total nodes, one below the per-function limit.
    let source = recursive_source(32, 224, 391);
    let program = axeon::compile(&source).expect("boundary fixture must be accepted");
    assert_eq!(program.functions[0].params.len(), 32);
    assert_eq!(program.functions[0].locals.len(), 256);
    let interpreted = interpreter::run(&program, interpreter::DEFAULT_FUEL);
    assert!(interpreted.stdout.is_empty());
    assert_eq!(
        interpreted.result.unwrap_err().render(&source),
        "trap[call_depth] at 1:1\n"
    );

    let temp = TempDir::new();
    let path = temp.0.join("boundary.ax");
    fs::write(&path, &source).unwrap();
    let native = run_cli(
        &[
            "run".as_ref(),
            path.as_os_str(),
            "--opt".as_ref(),
            "0".as_ref(),
        ],
        None,
    );
    assert_eq!(
        native.status.code(),
        Some(70),
        "native boundary failed: {}",
        String::from_utf8_lossy(&native.stderr)
    );
    assert!(native.stdout.is_empty());
    assert_eq!(native.stderr, b"trap[call_depth] at 1:1\n");
}

fn verify_array_storage_boundary(floating: bool) {
    let params = (0..32)
        .map(|i| {
            format!(
                "p{i}:{}",
                if floating && i % 2 == 0 { "f64" } else { "i64" }
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    let arguments = (0..32)
        .map(|i| format!("p{i}"))
        .collect::<Vec<_>>()
        .join(",");
    let mut body = String::new();
    for i in 0..8 {
        let ty = if floating && i % 2 == 0 { "f64" } else { "i64" };
        let elements = (0..32)
            .map(|j| {
                if ty == "f64" {
                    format!("{j}.0")
                } else {
                    j.to_string()
                }
            })
            .collect::<Vec<_>>()
            .join(",");
        write!(body, "let a{i}:[{ty};32]=[{elements}];").unwrap();
    }
    for i in 0..216 {
        let ty = if floating { "f64" } else { "i64" };
        write!(body, "let s{i}:&[{ty}]=&a0[0..32];").unwrap();
    }
    // 32 scalar parameters + 8 owned arrays + 216 shared slices = 256 locals.
    // The arrays total 256 elements. Add 292 three-node conditionals to reach
    // 2047 syntax nodes, including the return and outer function body.
    body.push_str(&"if false {}".repeat(292));
    let source = format!(
        "fn f({params})->i64{{{body}return f({arguments});}}\
         fn main()->i64{{return f({});}}",
        (0..32)
            .map(|i| if floating && i % 2 == 0 { "0.0" } else { "0" })
            .collect::<Vec<_>>()
            .join(",")
    );
    let program = axeon::compile(&source).expect("aggregate boundary must be accepted");
    assert_eq!(program.functions[0].locals.len(), 256);
    let array_elements: usize = program.functions[0]
        .locals
        .iter()
        .map(|local| match local.ty {
            axeon::ir::Type::Array { length, .. } => length,
            _ => 0,
        })
        .sum();
    assert_eq!(array_elements, 256);
    let interpreted = interpreter::run(&program, interpreter::DEFAULT_FUEL);
    assert!(interpreted.stdout.is_empty());
    assert_eq!(
        interpreted.result.unwrap_err().render(&source),
        "trap[call_depth] at 1:1\n"
    );

    let temp = TempDir::new();
    let path = temp.0.join("aggregate-boundary.ax");
    fs::write(&path, &source).unwrap();
    for optimization in ["0", "2", "3"] {
        let native = run_cli(
            &[
                "run".as_ref(),
                path.as_os_str(),
                "--opt".as_ref(),
                optimization.as_ref(),
            ],
            None,
        );
        assert_eq!(
            native.status.code(),
            Some(70),
            "aggregate native boundary failed at O{optimization}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert!(native.stdout.is_empty());
        assert_eq!(native.stderr, b"trap[call_depth] at 1:1\n");
    }
}

#[test]
fn maximum_array_storage_and_slice_slots_reach_checked_depth_at_all_optimizations() {
    verify_array_storage_boundary(false);
}

#[test]
fn mixed_floating_frames_reach_checked_depth_at_all_optimizations() {
    verify_array_storage_boundary(true);
}

#[test]
fn oversized_recursive_frame_is_rejected_before_native_compilation() {
    // Before the checked resource limits, this source was within the overall
    // source/token bounds but its O0 recursion exhausted an 8 MiB Linux stack.
    let source = recursive_source(10_000, 0, 0);
    let errors = axeon::compile(&source).unwrap_err();
    assert!(errors.iter().any(|error| error.code == "T014"));

    let temp = TempDir::new();
    let path = temp.0.join("oversized.ax");
    let artifact = temp.0.join("must-not-exist");
    fs::write(&path, &source).unwrap();
    let native = run_cli(
        &[
            "build".as_ref(),
            path.as_os_str(),
            "-o".as_ref(),
            artifact.as_os_str(),
        ],
        Some(&temp.0.join("clang-must-not-be-started")),
    );
    assert_eq!(native.status.code(), Some(1));
    assert!(native.stdout.is_empty());
    assert!(String::from_utf8_lossy(&native.stderr).contains("error[T014]"));
    assert!(!artifact.exists());
}
