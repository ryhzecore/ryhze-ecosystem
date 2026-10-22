//! Explicit target selection must preserve the shared checked representation.
//! These emission tests do not claim native target execution or ABI qualification.
use axeon::diagnostic::SourceTable;
use axeon::llvm::{self, Target};

fn differs_only_in_target(linux: &str, windows: &str) {
    assert_eq!(
        linux
            .matches("target triple = \"x86_64-pc-linux-gnu\"")
            .count(),
        1
    );
    assert_eq!(
        windows
            .matches("target triple = \"x86_64-pc-windows-msvc\"")
            .count(),
        1
    );
    assert_eq!(
        linux.replacen(
            "target triple = \"x86_64-pc-linux-gnu\"",
            "target triple = \"x86_64-pc-windows-msvc\"",
            1
        ),
        windows
    );
}

#[test]
fn explicit_targets_keep_checked_scalars_and_finite_nominal_layouts() {
    let source = r#"
record Cell<T> { value: T }
fn wrap<T>(value: T) -> Cell<T> { return Cell::<T>{value: value}; }
fn main() -> i64 {
    let integer: Cell<i64> = wrap::<i64>(9007199254740993);
    let floating: Cell<f64> = wrap::<f64>(-0.0);
    print(floating.value);
    return integer.value + 1;
}
"#;
    let program = axeon::compile(source).unwrap();
    let linux = llvm::emit_for_target(&program, source, Target::LinuxX86_64);
    let windows = llvm::emit_for_target(&program, source, Target::WindowsX86_64);
    assert_eq!(linux, llvm::emit(&program, source));
    differs_only_in_target(&linux, &windows);
    assert!(windows.contains("@llvm.sadd.with.overflow.i64"));
    assert!(windows.contains("%ax_record_instance_"));
    assert!(windows.contains("call void @ax_rt_print_f64(double"));
}

#[test]
fn explicit_targets_keep_module_traps_utf8_paths_and_tensor_callbacks() {
    let source = "fn main()->i64 { tensor_scope(0) {} catch (err:tensor_error) { print(tensor_error_code(err)); } return 1 / 0; }";
    let program = axeon::compile(source).unwrap();
    let mut sources = SourceTable::new();
    sources.push("folder/\u{96ea}.ax", source);
    let linux = llvm::emit_modules_for_target(&program, &sources, Target::LinuxX86_64);
    let windows = llvm::emit_modules_for_target(&program, &sources, Target::WindowsX86_64);
    assert_eq!(linux, llvm::emit_modules(&program, &sources));
    differs_only_in_target(&linux, &windows);
    assert!(windows.contains("\\E9\\9B\\AA"));
    assert!(windows.contains("call void @ax_rt_trap_v2(i32 2, ptr @ax_source_0"));
    assert!(windows.contains("define internal i32 @ax_tensor_body_"));
    assert!(windows.contains("call i32 @ax_tensor_v1_scope"));
}
