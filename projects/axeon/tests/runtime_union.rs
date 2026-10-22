//! Internal union-invariant traps must retain location and active cleanup.
mod support;
use std::fs;
use std::path::PathBuf;

struct Fixture(PathBuf);
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn invalid_union_tag_cleans_once_before_aborting_in_both_hosted_abis() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let temp =
        Fixture(std::env::temp_dir().join(format!("axeon-union-runtime-{}", std::process::id())));
    fs::create_dir(&temp.0).unwrap();
    let source = temp.0.join("trap.c");
    fs::write(
        &source,
        r#"#include "runtime.h"
#include <stdio.h>
static void cleanup(void *context) {
    int *count = context;
    ++*count;
    printf("cleanup:%d active:%d\n", *count, ax_rt_cleanup_active());
}
int main(int argc, char **argv) {
    (void)argv;
    int count = 0;
    if (ax_rt_register_cleanup(cleanup, &count) != 0) return 9;
    if (argc == 1) ax_rt_trap(AX_RT_INVALID_UNION_TAG, 12, 7);
    ax_rt_trap_v2(AX_RT_INVALID_UNION_TAG, "values.ax", 12, 7);
}
"#,
    )
    .unwrap();
    let clang = std::env::var_os("AXEON_CLANG").unwrap_or_else(|| "clang-19".into());
    for opt in [0, 2, 3] {
        let executable = support::executable_path(temp.0.join(format!("trap-O{opt}")));
        let build = support::bounded_with_kill(&clang, "30s", "1s")
            .args(["-std=c11", "-Wall", "-Wextra", "-Werror"])
            .args(["-target", support::target_triple()])
            .args(support::clang_link_flags())
            .arg(format!("-O{opt}"))
            .arg("-I")
            .arg(root.join("runtime"))
            .arg(&source)
            .arg(root.join("runtime/runtime.c"))
            .arg("-o")
            .arg(&executable)
            .output()
            .unwrap();
        assert!(
            build.status.success(),
            "{}",
            String::from_utf8_lossy(&build.stderr)
        );
        for module in [false, true] {
            let mut command = support::bounded_with_kill(&executable, "5s", "1s");
            if module {
                command.arg("module");
            }
            let output = command.output().unwrap();
            assert_eq!(output.status.code(), Some(70));
            assert_eq!(output.stdout, b"cleanup:1 active:0\n");
            let expected = if module {
                "trap[invalid_union_tag] at values.ax:12:7\n"
            } else {
                "trap[invalid_union_tag] at 12:7\n"
            };
            assert_eq!(output.stderr, expected.as_bytes());
        }
    }
}
