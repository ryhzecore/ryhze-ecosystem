//! Actual source-to-CLI native parity, including typed recovery and cleanup.
mod support;
use axeon::diagnostic::Span;
use axeon::interpreter::{self, Trap, TrapKind};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicUsize, Ordering};

static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let id = NEXT.fetch_add(1, Ordering::Relaxed);
        let path =
            std::env::temp_dir().join(format!("axeon-source-tensors-{}-{id}", std::process::id()));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn command(program: &Path, seconds: &str) -> Command {
    support::bounded_with_kill(program, seconds, "2s")
}
fn output(command: &mut Command) -> Output {
    let result = command.output().expect("bounded source tensor subprocess");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "source tensor subprocess timed out"
    );
    result
}
fn runtime() -> PathBuf {
    std::env::var_os("AXEON_TENSOR_RUNTIME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            Path::new(env!("CARGO_BIN_EXE_axeon"))
                .parent()
                .unwrap()
                .join(support::archive_name())
        })
}

fn verify(name: &str, source: &str, stdout: &str, result: Result<i64, Trap>) {
    let program =
        axeon::compile(source).unwrap_or_else(|errors| panic!("{name}: {errors:#?}\n{source}"));
    let execution = interpreter::run(&program, 5_000_000);
    assert_eq!(execution.stdout, stdout, "{name}: interpreter stdout");
    assert_eq!(execution.result, result, "{name}: interpreter result");
    let (status, native_stdout, stderr) = match result {
        Ok(value) => (0, format!("{stdout}result: {value}\n"), String::new()),
        Err(trap) => (70, stdout.to_owned(), trap.render(source)),
    };
    let runtime = runtime();
    assert!(
        runtime.is_file(),
        "build axeon-tensor-runtime in the active test profile first: {}",
        runtime.display()
    );
    let temporary = Temp::new();
    let input = temporary.0.join("program.ax");
    fs::write(&input, source).unwrap();
    for optimization in [0, 2, 3] {
        let executable = temporary.0.join(format!("program-O{optimization}"));
        let compilation = output(
            command(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .env("AXEON_TENSOR_RUNTIME", &runtime)
                .arg("build")
                .arg(&input)
                .arg("--opt")
                .arg(optimization.to_string())
                .arg("-o")
                .arg(&executable),
        );
        assert!(
            compilation.status.success(),
            "{name} O{optimization} CLI build: {}",
            String::from_utf8_lossy(&compilation.stderr)
        );
        let native = output(&mut command(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "{name} O{optimization}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(
            native.stdout,
            native_stdout.as_bytes(),
            "{name} O{optimization} stdout"
        );
        assert_eq!(
            native.stderr,
            stderr.as_bytes(),
            "{name} O{optimization} stderr"
        );
    }
}

fn floating(values: &[f64]) -> String {
    values
        .iter()
        .map(|value| format!("f64:0x{:016x}\n", value.to_bits()))
        .collect::<Vec<_>>()
        .join("")
}
fn scope(body: &str) -> String {
    format!("fn main() -> i64 {{ tensor_scope(1048576) {{ {body} }} catch (err: tensor_error) {{ return tensor_error_code(err); }} return 0; }}")
}

#[test]
fn shipped_source_examples_have_independent_expected_values() {
    verify(
        "matrix-view-stable-example",
        include_str!("../examples/tensors.ax"),
        &floating(&[5.0, 11.0, 11.0, 25.0, 52.0, 0.5, 0.5, 1.0]),
        Ok(0),
    );
    verify(
        "recover-and-reenter-example",
        include_str!("../examples/tensors-recovery.ax"),
        "1\n9\n7\n1\n2\n",
        Ok(7),
    );
    let source = include_str!("../examples/tensors-trap.ax");
    let start = source.find("1 / 0").unwrap();
    verify(
        "fatal-scalar-example",
        source,
        "1\n",
        Err(Trap {
            kind: TrapKind::DivisionByZero,
            span: Span {
                source: axeon::diagnostic::SourceId(0),
                start,
                end: start + 5,
            },
        }),
    );
}

#[test]
fn generated_rectangular_matrices_match_integer_computed_full_results() {
    let mut seed = 0xa0e0_0501u64;
    for case in 0..6 {
        let (m, k, n) = (1 + case % 3, 1 + (case * 3 + 1) % 4, 1 + (case * 5 + 2) % 4);
        let mut integer = || {
            seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
            ((seed >> 32) % 17) as i64 - 8
        };
        let left: Vec<i64> = (0..m * k).map(|_| integer()).collect();
        let right: Vec<i64> = (0..k * n).map(|_| integer()).collect();
        let literals = |values: &[i64]| {
            values
                .iter()
                .map(|value| format!("{value}.0"))
                .collect::<Vec<_>>()
                .join(",")
        };
        let mut expected = Vec::new();
        for row in 0..m {
            for column in 0..n {
                let mut value = 0i64;
                for inner in 0..k {
                    value += left[row * k + inner] * right[inner * n + column];
                }
                expected.push(value as f64);
            }
        }
        let source = scope(&format!("let aa:[f64;{}]=[{}]; let bb:[f64;{}]=[{}]; let a:tensor=try tensor_from_array([{m},{k}],aa); let b:tensor=try tensor_from_array([{k},{n}],bb); let c:tensor=try tensor_matmul(a,b); let mut i:i64=0; while i<tensor_len(c) {{ print(try tensor_get(c,i)); i=i+1; }}", left.len(), literals(&left), right.len(), literals(&right)));
        verify(
            &format!("generated-matrix-{case}"),
            &source,
            &floating(&expected),
            Ok(0),
        );
    }
}

#[test]
fn views_moves_mutation_and_metadata_preserve_logical_storage() {
    let source = scope("let data:[f64;6]=[1.0,2.0,3.0,4.0,5.0,6.0]; let mut a:tensor=try tensor_from_array([2,3],data); { let v:&mut tensor=try tensor_transpose(&mut a,[1,0]); try tensor_set(v,1,10.0); print(try tensor_get(v,1)); print(tensor_rank(v)); print(try tensor_dim(v,0)); } { let v:&tensor=try tensor_slice(&a,1,1,3,1); print(try tensor_sum(v)); } let moved:tensor=a; print(try tensor_sum(moved)); { let moved:&tensor=try tensor_view(&moved); print(tensor_len(moved)); } print(tensor_len(moved));");
    verify(
        "views-moves-shadow",
        &source,
        &format!(
            "{}2\n3\n{}6\n6\n",
            floating(&[10.0]),
            floating(&[16.0, 27.0])
        ),
        Ok(0),
    );
}

#[test]
fn scalar_empty_ieee_and_separate_rounding_kernels() {
    let source = scope("let values:[f64;4]=[-0.0,f64_from_bits(1),1.0/0.0,0.0/0.0]; let a:tensor=try tensor_from_array([4],values); let one_values:[f64;4]=[1.0,1.5,0.0,1.0]; let b:tensor=try tensor_from_array([4],one_values); let c:tensor=try tensor_mul(a,b); let mut i:i64=0; while i<4 { print(try tensor_get(c,i)); i=i+1; } let empty:tensor=try tensor_zeros([3,0]); print(tensor_len(empty)); print(try tensor_sum(empty)); let scalar:tensor=try tensor_zeros([]); print(tensor_rank(scalar)); print(try tensor_get(scalar,0));");
    verify("ieee-and-empty", &source, "f64:0x8000000000000000\nf64:0x0000000000000002\nf64:0x7ff8000000000000\nf64:0x7ff8000000000000\n0\nf64:0x0000000000000000\n0\nf64:0x0000000000000000\n", Ok(0));
    let source = scope("let aa:[f64;2]=[1.0000000074505806,-1.0]; let bb:[f64;2]=[0.9999999925494194,1.0]; let a:tensor=try tensor_from_array([1,2],aa); let b:tensor=try tensor_from_array([2,1],bb); let c:tensor=try tensor_matmul(a,b); print(try tensor_get(c,0)); let ordered:[f64;3]=[1e16,1.0,-1e16]; let t:tensor=try tensor_from_array([3],ordered); print(try tensor_sum(t)); let z:[f64;1]=[-0.0]; let singleton:tensor=try tensor_from_array([1],z); print(try tensor_logsumexp(singleton));");
    verify(
        "ordered-no-fma-and-negative-zero",
        &source,
        &floating(&[0.0, 0.0, -0.0]),
        Ok(0),
    );
}

fn recover(name: &str, setup: &str, failing: &str, code: i64, budget: i64) {
    let source = format!("// λ source byte offsets\nfn main()->i64{{ tensor_scope({budget}){{ {setup} let bad:tensor=try {failing}; print(999); }} catch(err:tensor_error){{print(tensor_error_code(err));print(tensor_error_start(err));print(tensor_error_end(err));}} return 0; }}");
    let start = source.rfind(failing).unwrap();
    verify(
        name,
        &source,
        &format!("{code}\n{start}\n{}\n", start + failing.len()),
        Ok(0),
    );
}

#[test]
fn typed_error_categories_preserve_original_utf8_spans_and_preflight_order() {
    recover("negative-extent", "", "tensor_zeros([-1])", 2, 0);
    recover("element-limit", "", "tensor_zeros([65536,2])", 3, 0);
    recover(
        "copy-shape",
        "let data:[f64;1]=[1.0];",
        "tensor_from_array([2],data)",
        4,
        0,
    );
    recover(
        "matrix-rank",
        "let a:tensor=try tensor_zeros([1]);",
        "tensor_matmul(a,a)",
        5,
        8,
    );
    recover(
        "budget",
        "let a:tensor=try tensor_zeros([1]);",
        "tensor_zeros([1])",
        9,
        8,
    );
    recover(
        "work-before-output-budget",
        "let a:tensor=try tensor_zeros([256,16]);let b:tensor=try tensor_zeros([16,256]);",
        "tensor_matmul(a,b)",
        11,
        65536,
    );
    recover(
        "nonfinite-before-output-budget",
        "let data:[f64;1]=[0.0/0.0];let a:tensor=try tensor_from_array([1],data);",
        "tensor_sigmoid(a)",
        12,
        8,
    );
    recover(
        "empty-softmax",
        "let a:tensor=try tensor_zeros([0]);",
        "tensor_softmax(a)",
        13,
        0,
    );
}

#[test]
fn invalid_views_and_scalar_accesses_report_the_exact_call() {
    for (name, ty, failing, code) in [
        (
            "permutation-length",
            "&tensor",
            "tensor_transpose(&a,[0,0])",
            7,
        ),
        (
            "permutation-negative",
            "&tensor",
            "tensor_transpose(&a,[-1])",
            7,
        ),
        ("invalid-step", "&tensor", "tensor_slice(&a,0,0,2,0)", 8),
        ("get-bounds", "f64", "tensor_get(a,2)", 6),
        ("dim-bounds", "i64", "tensor_dim(a,-1)", 6),
    ] {
        let source = format!("// λ\nfn main()->i64{{tensor_scope(16){{let a:tensor=try tensor_zeros([2]);let bad:{ty}=try {failing};print(999);}}catch(err:tensor_error){{print(tensor_error_code(err));print(tensor_error_start(err));print(tensor_error_end(err));}}return 0;}}");
        let start = source.find(failing).unwrap();
        verify(
            name,
            &source,
            &format!("{code}\n{start}\n{}\n", start + failing.len()),
            Ok(0),
        );
    }
}

#[test]
fn array_slice_copy_and_tensor_slice_edges_preserve_logical_values() {
    let source = scope("let data:[f64;6]=[1.0,2.0,3.0,4.0,5.0,6.0];let part:&[f64]=&data[1..5];let a:tensor=try tensor_from_array([2,2],part);{let v:&tensor=try tensor_transpose(&a,[1,0]);let mut i:i64=0;while i<tensor_len(v){print(try tensor_get(v,i));i=i+1;}}{let v:&tensor=try tensor_slice(&a,0,1,2,9223372036854775807);print(try tensor_get(v,0));print(try tensor_get(v,1));}{let v:&tensor=try tensor_slice(&a,0,2,2,1);print(tensor_len(v));print(try tensor_sum(v));}let empty:tensor=try tensor_zeros([65536,65536,0,65536,65536,65536,65536,65536]);print(tensor_rank(empty));print(tensor_len(empty));");
    verify(
        "copy-slice-and-descriptor-edges",
        &source,
        &format!(
            "{}0\n{}8\n0\n",
            floating(&[2.0, 4.0, 3.0, 5.0, 4.0, 5.0]),
            floating(&[0.0])
        ),
        Ok(0),
    );
}

#[test]
fn scope_entry_evaluates_budget_before_admission_and_reports_the_whole_statement() {
    let prefix = "// λ\nfn budget()->i64{print(5);return -1;}fn main()->i64{";
    let statement = "tensor_scope(budget()){print(999);}catch(err:tensor_error){print(tensor_error_code(err));print(tensor_error_start(err));print(tensor_error_end(err));}";
    let source = format!("{prefix}{statement}return 0;}}");
    verify(
        "scope-admission-span",
        &source,
        &format!(
            "5\n9\n{}\n{}\n",
            prefix.len(),
            prefix.len() + statement.len()
        ),
        Ok(0),
    );
}

#[test]
fn all_arguments_precede_validation_and_bad_set_target_suppresses_rhs() {
    let source = "fn mark(x:i64)->i64{print(x);return x;} fn rhs()->f64{print(999);return 4.0;} fn main()->i64{ let mut changed:i64=0; tensor_scope(8){let a:tensor=try tensor_zeros([mark(-1),mark(2)]);}catch(err:tensor_error){print(tensor_error_code(err));} tensor_scope(8){let mut a:tensor=try tensor_zeros([1]);changed=7;try tensor_set(a,mark(1),rhs());print(888);}catch(err:tensor_error){print(tensor_error_code(err));print(changed);} return 0;}";
    verify(
        "argument-and-set-order",
        source,
        "-1\n2\n2\n1\n6\n7\n",
        Ok(0),
    );
    let source = "fn mark(x:i64)->i64{print(x);return x;} fn main()->i64{tensor_scope(8){let mut a:tensor=try tensor_zeros([1]);let v:&mut tensor=try tensor_slice(&mut a,mark(0),mark(-1),mark(1),mark(0));}catch(err:tensor_error){print(tensor_error_code(err));}return 0;}";
    verify(
        "all-view-arguments-before-bounds",
        source,
        "0\n-1\n1\n0\n6\n",
        Ok(0),
    );
}

#[test]
fn repeated_loop_cleanup_moves_and_nested_entry_preserve_the_active_context() {
    let source = "fn nested()->i64{tensor_scope(-1){print(999);}catch(err:tensor_error){print(tensor_error_code(err));}return 6;} fn main()->i64{tensor_scope(8){let mut i:i64=0;while i<600{let original:tensor=try tensor_zeros([1]);let moved:tensor=original;{let view:&tensor=try tensor_view(&moved);i=i+tensor_len(view);}if i<600{continue;}break;}let last:tensor=try tensor_zeros([1]);print(nested());print(tensor_len(last));}catch(err:tensor_error){return tensor_error_code(err);}tensor_scope(8){let again:tensor=try tensor_zeros([1]);print(tensor_len(again));}catch(err:tensor_error){return tensor_error_code(err);}return 0;}";
    verify(
        "loop-slot-payload-reuse-and-nested-guard",
        source,
        "15\n6\n1\n1\n",
        Ok(0),
    );
}

#[test]
fn handlers_can_return_and_only_continuing_ownership_paths_join() {
    let success = "fn main()->i64{let outside:[i64;1]=[11];tensor_scope(0){}catch(err:tensor_error){let moved:[i64;1]=outside;return moved[0];}print(outside[0]);return 0;}";
    verify(
        "returning-handler-does-not-poison-success",
        success,
        "11\n",
        Ok(0),
    );
    let failure = "fn main()->i64{let outside:[i64;1]=[11];tensor_scope(-1){}catch(err:tensor_error){let moved:[i64;1]=outside;return moved[0];}print(outside[0]);return 0;}";
    verify("handler-return-move", failure, "", Ok(11));
}

#[test]
fn fatal_scalar_callee_traps_preserve_output_and_bypass_typed_catch() {
    for (name, expression, kind) in [
        ("overflow", "9223372036854775807 + 1", TrapKind::Overflow),
        ("division", "1 / 0", TrapKind::DivisionByZero),
        ("shift", "1 << 64", TrapKind::ShiftCount),
    ] {
        let source = format!("fn fail()->i64{{return {expression};}} fn main()->i64{{tensor_scope(8){{let owner:tensor=try tensor_zeros([1]);print(7);print(fail());}}catch(err:tensor_error){{print(999);}}return 0;}}");
        let start = source.find(expression).unwrap();
        verify(
            name,
            &source,
            "7\n",
            Err(Trap {
                kind,
                span: Span {
                    source: axeon::diagnostic::SourceId(0),
                    start,
                    end: start + expression.len(),
                },
            }),
        );
    }
    let source = "fn fail()->i64{let a:[i64;1]=[1];return a[1];} fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(7);print(fail());}catch(err:tensor_error){print(999);}return 0;}";
    let start = source.find("a[1]").unwrap();
    verify(
        "scalar-bounds",
        source,
        "7\n",
        Err(Trap {
            kind: TrapKind::Bounds,
            span: Span {
                source: axeon::diagnostic::SourceId(0),
                start,
                end: start + 4,
            },
        }),
    );
    let function = "fn recurse()->i64{return recurse();}";
    let source = format!("{function} fn main()->i64{{tensor_scope(8){{let owner:tensor=try tensor_zeros([1]);print(7);print(recurse());}}catch(err:tensor_error){{print(999);}}return 0;}}");
    verify(
        "call-depth",
        &source,
        "7\n",
        Err(Trap {
            kind: TrapKind::CallDepth,
            span: Span {
                source: axeon::diagnostic::SourceId(0),
                start: 0,
                end: function.len(),
            },
        }),
    );
}

#[test]
fn native_output_limit_from_a_callee_cleans_up_before_exit() {
    let source = "fn emit()->i64{while true{print(1.0);}return 0;} fn main()->i64{tensor_scope(8){let owner:tensor=try tensor_zeros([1]);print(7);print(emit());}catch(err:tensor_error){print(999);}return 0;}";
    let start = source.find("print(1.0);").unwrap();
    let expected = format!("7\n{}", "f64:0x3ff0000000000000\n".repeat(45_590));
    verify(
        "transitive-output-limit",
        source,
        &expected,
        Err(Trap {
            kind: TrapKind::OutputLimit,
            span: Span {
                source: axeon::diagnostic::SourceId(0),
                start,
                end: start + "print(1.0);".len(),
            },
        }),
    );
}

#[test]
fn bulk_fuel_is_checked_after_admission_and_is_not_caught() {
    let source = "fn main()->i64{tensor_scope(8000){let first:tensor=try tensor_zeros([1]);print(7);let large:tensor=try tensor_zeros([900]);}catch(err:tensor_error){print(999);}return 0;}";
    let program = axeon::compile(source).unwrap();
    let execution = interpreter::run(&program, 100);
    assert_eq!(execution.stdout, "7\n");
    let start = source.find("tensor_zeros([900])").unwrap();
    assert_eq!(
        execution.result,
        Err(Trap {
            kind: TrapKind::FuelExhausted,
            span: Span {
                source: axeon::diagnostic::SourceId(0),
                start,
                end: start + "tensor_zeros([900])".len()
            }
        })
    );
    let source = "fn main()->i64{tensor_scope(0){let bad:tensor=try tensor_zeros([900]);}catch(err:tensor_error){print(tensor_error_code(err));}return 0;}";
    let execution = interpreter::run(&axeon::compile(source).unwrap(), 100);
    assert_eq!(execution.stdout, "9\n");
    assert_eq!(execution.result, Ok(0));
}
