//! Native test adapters. Every Windows child runs through the explicit private
//! job supervisor; semantic assertions and subprocess deadlines remain shared.
#![allow(dead_code)]

use std::ffi::OsStr;
use std::path::PathBuf;
use std::process::Command;

pub fn bounded(program: impl AsRef<OsStr>, seconds: &str) -> Command {
    bounded_with_kill(program, seconds, "2s")
}

pub fn bounded_with_kill(program: impl AsRef<OsStr>, seconds: &str, kill_after: &str) -> Command {
    #[cfg(windows)]
    {
        let _ = kill_after;
        let seconds = seconds
            .strip_suffix('s')
            .expect("native test deadline uses whole seconds")
            .parse::<u32>()
            .expect("native test deadline is an integer");
        assert!(
            (1..=3600).contains(&seconds),
            "native test deadline is bounded"
        );
        let supervisor = PathBuf::from(
            std::env::var_os("AXEON_PROCESS_TEST")
                .expect("Windows tests require AXEON_PROCESS_TEST for bounded native execution"),
        );
        assert!(
            supervisor.is_absolute() && supervisor.is_file(),
            "AXEON_PROCESS_TEST must name an existing absolute native supervisor"
        );
        let mut command = Command::new(supervisor);
        command
            .args(["--timeout-seconds", &seconds.to_string(), "--"])
            .arg(program);
        command
    }
    #[cfg(not(windows))]
    {
        let mut command = Command::new("timeout");
        command
            .arg(format!("--kill-after={kill_after}"))
            .arg(seconds)
            .arg(program);
        command
    }
}

pub fn target() -> axeon::llvm::Target {
    if cfg!(windows) {
        axeon::llvm::Target::WindowsX86_64
    } else {
        axeon::llvm::Target::LinuxX86_64
    }
}

pub fn target_triple() -> &'static str {
    target().triple()
}

pub fn emit(program: &axeon::ir::Program, source: &str) -> String {
    axeon::llvm::emit_for_target(program, source, target())
}

pub fn emit_modules(
    program: &axeon::ir::Program,
    sources: &axeon::diagnostic::SourceTable,
) -> String {
    axeon::llvm::emit_modules_for_target(program, sources, target())
}

pub fn archive_name() -> &'static str {
    if cfg!(windows) {
        "axeon_tensor_runtime.lib"
    } else {
        "libaxeon_tensor_runtime.a"
    }
}

pub fn python() -> PathBuf {
    std::env::var_os("AXEON_PYTHON").map_or_else(
        || {
            PathBuf::from(if cfg!(windows) {
                "python.exe"
            } else {
                "python3"
            })
        },
        PathBuf::from,
    )
}

pub fn executable_path(path: PathBuf) -> PathBuf {
    if cfg!(windows) {
        let mut name = path.into_os_string();
        name.push(".exe");
        PathBuf::from(name)
    } else {
        path
    }
}

pub fn clang_compile_flags() -> &'static [&'static str] {
    &["-std=c11", "-fno-ident", "-ffp-contract=off"]
}

pub fn clang_link_flags() -> &'static [&'static str] {
    if cfg!(windows) {
        &["-fuse-ld=lld", "-Xlinker", "/Brepro"]
    } else {
        &["-Wl,--build-id=none"]
    }
}

pub fn tensor_link_flags() -> &'static [&'static str] {
    if cfg!(windows) {
        &["-lws2_32", "-luserenv", "-lbcrypt", "-lntdll", "-ladvapi32"]
    } else {
        &[
            "-lgcc_s",
            "-lutil",
            "-lrt",
            "-lpthread",
            "-lm",
            "-ldl",
            "-lc",
        ]
    }
}

pub fn math_link_flags() -> &'static [&'static str] {
    if cfg!(windows) {
        &[]
    } else {
        &["-lm"]
    }
}
