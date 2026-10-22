//! Host adapters for the existing checked CPU backend. No shell evaluation.
use axeon::llvm::Target;
use std::{env, path::PathBuf, process::Command};

pub fn target() -> Result<Target, String> {
    if cfg!(all(
        target_os = "windows",
        target_arch = "x86_64",
        target_env = "msvc"
    )) {
        Ok(Target::WindowsX86_64)
    } else if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        Ok(Target::LinuxX86_64)
    } else {
        Err("native backend supports Linux x86-64 and Windows x86-64 MSVC hosts only".into())
    }
}

pub fn bounded(program: impl AsRef<std::ffi::OsStr>, seconds: u32) -> Result<Command, String> {
    #[cfg(windows)]
    {
        let supervisor = env::current_exe()
            .map_err(|e| e.to_string())?
            .parent()
            .ok_or("cannot locate compiler directory")?
            .join("axeon-process.exe");
        if !supervisor.is_file() {
            return Err("native Windows supervision is missing: axeon-process.exe must be beside axeon.exe; reinstall the complete package".into());
        }
        let mut command = Command::new(supervisor);
        command
            .args(["--timeout-seconds", &seconds.to_string(), "--"])
            .arg(program);
        Ok(command)
    }
    #[cfg(not(windows))]
    {
        let mut command = Command::new("timeout");
        command.arg(format!("{seconds}s")).arg(program);
        Ok(command)
    }
}

pub fn clang_path() -> Result<PathBuf, String> {
    let requested = if let Some(path) = env::var_os("AXEON_CLANG") {
        if path.is_empty() {
            return Err("AXEON_CLANG must name a Clang executable".into());
        }
        PathBuf::from(path)
    } else if cfg!(windows) {
        PathBuf::from("clang.exe")
    } else {
        let supplied = PathBuf::from("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang");
        if supplied.is_file() {
            supplied
        } else {
            PathBuf::from("clang-19")
        }
    };
    let selected = if requested.components().count() > 1 || requested.is_absolute() {
        requested
    } else {
        let name = if cfg!(windows) && requested.extension().is_none() {
            requested.with_extension("exe")
        } else {
            requested.clone()
        };
        env::split_paths(&env::var_os("PATH").unwrap_or_default())
            .map(|directory| directory.join(&name))
            .find(|candidate| candidate.is_file())
            .ok_or_else(|| {
                format!(
                    "cannot execute Clang: {} was not found on PATH; set AXEON_CLANG",
                    requested.display()
                )
            })?
    };
    selected.canonicalize().map_err(|error| {
        format!(
            "cannot execute Clang {} (set AXEON_CLANG): {error}",
            selected.display()
        )
    })
}

pub fn archive_name() -> &'static str {
    if cfg!(windows) {
        "axeon_tensor_runtime.lib"
    } else {
        "libaxeon_tensor_runtime.a"
    }
}

pub fn native_policy() -> &'static str {
    if cfg!(windows) {
        "clang19-x86_64-msvc-ffp-contract-off-no-fast-math-c11-coff-zero-time-lld-brepro-v2"
    } else {
        "clang19-x86_64-linux-ffp-contract-off-no-fast-math-c11-no-ident-no-build-id-v1"
    }
}
