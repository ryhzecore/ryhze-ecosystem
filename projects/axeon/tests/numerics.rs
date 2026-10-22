//! Strict binary64 expected-bit, visible-effect and native differential coverage.
mod support;
use axeon::interpreter;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::OnceLock;

const OPTIMIZATIONS: [u8; 3] = [0, 2, 3];
static TEMP_ID: AtomicUsize = AtomicUsize::new(0);

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Self {
        for _ in 0..1000 {
            let id = TEMP_ID.fetch_add(1, Ordering::Relaxed);
            let path = env::temp_dir().join(format!("axeon-numerics-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create numerical test directory: {error}"),
            }
        }
        panic!("unique temporary-directory budget exhausted");
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn bounded(executable: &Path) -> Command {
    support::bounded_with_kill(executable, "10s", "1s")
}

fn output(command: &mut Command, context: &str) -> Output {
    let result = command
        .output()
        .unwrap_or_else(|error| panic!("{context}: subprocess launch failed: {error}"));
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "{context}: exceeded the 10-second subprocess limit"
    );
    result
}

fn clang() -> &'static Path {
    static CLANG: OnceLock<PathBuf> = OnceLock::new();
    CLANG
        .get_or_init(|| {
            let path = env::var_os("AXEON_CLANG").map_or_else(
                || {
                    let supplied =
                        PathBuf::from("/workspace/tools/sysroot/usr/lib/llvm-19/bin/clang");
                    if supplied.is_file() {
                        supplied
                    } else {
                        PathBuf::from("clang-19")
                    }
                },
                PathBuf::from,
            );
            let result = output(
                bounded(&path).arg("--version"),
                "numerical Clang version check",
            );
            assert!(
                result.status.success(),
                "native numerical tests require Clang 19.1.7; set AXEON_CLANG: {}",
                String::from_utf8_lossy(&result.stderr)
            );
            assert!(
                String::from_utf8_lossy(&result.stdout)
                    .split_whitespace()
                    .any(|word| word == "19.1.7"),
                "native numerical tests require exactly Clang 19.1.7: {}",
                String::from_utf8_lossy(&result.stdout)
            );
            path
        })
        .as_path()
}

fn native_compile(directory: &Path, opt: u8, name: &str) -> PathBuf {
    let executable = support::executable_path(directory.join(format!("program-O{opt}")));
    let result = output(
        bounded(clang())
            .current_dir(directory)
            .env("SOURCE_DATE_EPOCH", "0")
            .args([
                "-target",
                support::target_triple(),
                "-Werror",
                "-Wno-override-module",
                "-ffp-contract=off",
            ])
            .arg(format!("-O{opt}"))
            .arg("program.ll")
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("runtime/runtime.c"))
            .args(["-std=c11", "-fno-ident"])
            .args(support::clang_link_flags())
            .arg("-o")
            .arg(&executable),
        &format!("{name}: Clang -O{opt}"),
    );
    assert!(
        result.status.success(),
        "{name}: Clang -O{opt} failed ({}):\n{}",
        result.status,
        String::from_utf8_lossy(&result.stderr)
    );
    executable
}

fn verify(name: &str, source: &str, expected_stdout: &str, expected_result: i64) {
    let program = axeon::compile(source).unwrap_or_else(|e| panic!("{name}: {e:#?}\n{source}"));
    let result = interpreter::run(&program, 1_000_000);
    assert_eq!(
        result.stdout, expected_stdout,
        "{name}: interpreter effects"
    );
    assert_eq!(
        result.result,
        Ok(expected_result),
        "{name}: interpreter result"
    );
    let temp = TempDir::new();
    fs::write(temp.0.join("program.ll"), support::emit(&program, source)).unwrap();
    for optimization in OPTIMIZATIONS {
        let executable = native_compile(&temp.0, optimization, name);
        let native = output(&mut bounded(&executable), name);
        assert_eq!(
            native.status.code(),
            Some(0),
            "{name}: O{optimization}: {:?}",
            native.stderr
        );
        assert_eq!(
            native.stdout,
            format!("{expected_stdout}result: {expected_result}\n").as_bytes(),
            "{name}: O{optimization}"
        );
        assert!(
            native.stderr.is_empty(),
            "{name}: O{optimization}: {:?}",
            native.stderr
        );
    }
}

fn bits_fixture(name: &str, expression: &str, bits: u64) {
    let source = format!("fn value() -> f64 {{ return {expression}; }} fn main() -> i64 {{ let x:f64=value(); print(x); return f64_bits(x); }}");
    verify(name, &source, &format!("f64:0x{bits:016x}\n"), bits as i64);
}

#[test]
fn literals_round_to_binary64_with_signed_zero_and_gradual_underflow() {
    let cases = [
        ("zero", "0.0", 0x0000000000000000),
        ("negative-zero", "-0.0", 0x8000000000000000),
        ("exponent", "1e0", 0x3ff0000000000000),
        ("one-tenth", "0.1", 0x3fb999999999999a),
        (
            "tie-even-down",
            "1.00000000000000011102230246251565404236316680908203125",
            0x3ff0000000000000,
        ),
        (
            "tie-even-up",
            "1.00000000000000033306690738754696212708950042724609375",
            0x3ff0000000000002,
        ),
        (
            "maximum-finite",
            "1.7976931348623157e308",
            0x7fefffffffffffff,
        ),
        ("minimum-subnormal", "4.9406564584124654e-324", 1),
        ("under-half-minimum", "2.4703282292062327e-324", 0),
        ("over-half-minimum", "2.4703282292062328e-324", 1),
        ("underflow-zero", "1e-999", 0),
        ("negative-underflow-zero", "-1e-999", 0x8000000000000000),
    ];
    for (name, expression, bits) in cases {
        bits_fixture(name, expression, bits);
    }
}

#[test]
fn ieee_operations_preserve_nonfinite_values_subnormals_and_separate_rounding() {
    let cases = [
        ("positive-infinity", "1.0 / 0.0", 0x7ff0000000000000),
        ("negative-infinity", "-1.0 / 0.0", 0xfff0000000000000),
        ("invalid-zero-div-zero", "0.0 / 0.0", 0x7ff8000000000000),
        (
            "invalid-infinity-minus-infinity",
            "(1.0 / 0.0) - (1.0 / 0.0)",
            0x7ff8000000000000,
        ),
        ("negative-zero-product", "-0.0 * 3.0", 0x8000000000000000),
        ("opposite-zero-sum", "-0.0 + 0.0", 0),
        ("negative-zero-sum", "-0.0 + -0.0", 0x8000000000000000),
        ("exact-cancellation", "1.0 + -1.0", 0),
        (
            "gradual-underflow",
            "f64_from_bits(4503599627370496) * 0.5",
            0x0008000000000000,
        ),
        ("subnormal-tie-zero", "f64_from_bits(1) * 0.5", 0),
        ("subnormal-tie-even", "f64_from_bits(1) * 1.5", 2),
        (
            "subnormal-to-normal",
            "f64_from_bits(4503599627370495) + f64_from_bits(1)",
            0x0010000000000000,
        ),
        ("finite-overflow", "1e308 * 2.0", 0x7ff0000000000000),
        ("reciprocal-infinity", "1.0 / (1.0 / 0.0)", 0),
        (
            "negative-reciprocal-infinity",
            "-1.0 / (1.0 / 0.0)",
            0x8000000000000000,
        ),
        (
            "no-fma-contract",
            "(1.0000000000000002 * 0.9999999999999998) - 1.0",
            0,
        ),
        (
            "ordered-sum-left",
            "(1e16 + -1e16) + 1.0",
            0x3ff0000000000000,
        ),
        ("ordered-sum-right", "1e16 + (-1e16 + 1.0)", 0),
        (
            "addition-tie-even",
            "1.0 + 1.1102230246251565e-16",
            0x3ff0000000000000,
        ),
    ];
    for (name, expression, bits) in cases {
        bits_fixture(name, expression, bits);
    }
}

#[test]
fn explicit_conversions_and_nan_observers_have_defined_bits() {
    let cases = [
        (
            "integer-max-rounds-up",
            "f64_from_i64(9223372036854775807)",
            0x43e0000000000000,
        ),
        (
            "integer-min-exact",
            "f64_from_i64(-9223372036854775808)",
            0xc3e0000000000000,
        ),
        (
            "integer-tie-down",
            "f64_from_i64(9007199254740993)",
            0x4340000000000000,
        ),
        (
            "integer-tie-up",
            "f64_from_i64(9007199254740995)",
            0x4340000000000002,
        ),
        (
            "bits-negative-zero",
            "f64_from_bits(-9223372036854775808)",
            0x8000000000000000,
        ),
        (
            "bits-negative-infinity",
            "f64_from_bits(-4503599627370496)",
            0xfff0000000000000,
        ),
        (
            "bits-signaling-nan",
            "f64_from_bits(9218868437227405313)",
            0x7ff8000000000000,
        ),
        (
            "bits-negative-nan",
            "f64_from_bits(-2251799813685248)",
            0x7ff8000000000000,
        ),
        ("negated-nan-observed", "-(0.0 / 0.0)", 0x7ff8000000000000),
    ];
    for (name, expression, bits) in cases {
        bits_fixture(name, expression, bits);
    }
}

#[test]
fn floating_comparisons_follow_ieee_rules_and_boolean_short_circuiting() {
    let source = r#"fn marker() -> bool { print(999); return true; }
fn main() -> i64 {
 let nan:f64=0.0/0.0; let inf:f64=1.0/0.0;
 print(nan==nan); print(nan!=nan); print(nan<0.0); print(nan<=0.0);
 print(nan>0.0); print(nan>=0.0); print(0.0== -0.0); print(0.0!= -0.0);
 print(inf==inf); print(-inf<inf); print(nan==nan && marker());
 print(nan!=nan || marker()); return 0;
}"#;
    verify(
        "IEEE comparisons",
        source,
        "false\ntrue\nfalse\nfalse\nfalse\nfalse\ntrue\nfalse\ntrue\ntrue\nfalse\ntrue\n",
        0,
    );
}

#[test]
fn floating_arrays_moves_loans_calls_and_effect_order_agree() {
    let source = r#"fn mark(x:f64)->f64 { print(x); return x; }
fn combine(a:f64,b:f64)->f64 { return a*2.0+b; }
fn main()->i64 {
 let mut a:[f64;4]=[mark(1.0),mark(2.0),3.0,4.0];
 { let s:&mut[f64]=&mut a[1..3]; s[0]=s[0]+0.5; print(s[0]); }
 let moved:[f64;4]=a; let s:&[f64]=&moved; let t:&[f64]=&moved[1..4];
 let x:f64=combine(mark(s[0]),mark(t[0])); print(x); return f64_bits(x);
}"#;
    verify("float ownership/effects",source,"f64:0x3ff0000000000000\nf64:0x4000000000000000\nf64:0x4004000000000000\nf64:0x3ff0000000000000\nf64:0x4004000000000000\nf64:0x4012000000000000\n",0x4012000000000000);
}

#[test]
fn seeded_exact_dyadic_programs_match_mathematical_results() {
    const SEED: u64 = 0x4158454f4e5f4633;
    let mut state = SEED;
    for index in 0..24 {
        state = state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        let a = ((state >> 32) % 2049) as i64 - 1024;
        state = state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        let b = ((state >> 32) % 2049) as i64 - 1024;
        // Every operation is exact: small integers and powers-of-two denominators.
        // The expected numerator is calculated with integer arithmetic separately.
        let source=format!("fn main()->i64{{let a:f64=f64_from_i64({a})/256.0;let b:f64=f64_from_i64({b})/1024.0;let mut v:[f64;2]=[a,b];{{let s:&mut[f64]=&mut v;s[0]=(s[0]+s[1])*0.25-a/16.0;}} print(v[0]);return f64_bits(v[0]);}}");
        // (a/256+b/1024)/4 - (a/256)/16 = (3*a+b)/4096.
        let expected = (3 * a + b) as f64 / 4096.0;
        verify(
            &format!("dyadic-{index}"),
            &source,
            &format!("f64:0x{:016x}\n", expected.to_bits()),
            expected.to_bits() as i64,
        );
    }
}

fn verify_trap(name: &str, source: &str, effects: &str, kind: &str, marker: &str) {
    let start = source.find(marker).expect("unique trap marker");
    assert_eq!(source.matches(marker).count(), 1);
    let line = source[..start].bytes().filter(|&b| b == b'\n').count() + 1;
    let column = source[..start].rsplit('\n').next().unwrap().chars().count() + 1;
    let rendered = format!("trap[{kind}] at {line}:{column}\n");
    let program = axeon::compile(source).unwrap();
    let interpreted = interpreter::run(&program, 1_000_000);
    assert_eq!(
        interpreted.stdout.len(),
        effects.len(),
        "{name}: interpreter output length"
    );
    assert_eq!(interpreted.stdout, effects);
    assert_eq!(interpreted.result.unwrap_err().render(source), rendered);
    let temp = TempDir::new();
    fs::write(temp.0.join("program.ll"), support::emit(&program, source)).unwrap();
    for optimization in OPTIMIZATIONS {
        let executable = native_compile(&temp.0, optimization, name);
        let native = output(&mut bounded(&executable), name);
        assert_eq!(native.status.code(), Some(70), "{name}: O{optimization}");
        assert_eq!(
            native.stdout.len(),
            effects.len(),
            "{name}: O{optimization} output length"
        );
        assert_eq!(
            native.stdout,
            effects.as_bytes(),
            "{name}: O{optimization} output bytes"
        );
        assert_eq!(
            native.stderr,
            rendered.as_bytes(),
            "{name}: O{optimization}"
        );
    }
}

#[test]
fn floating_bounds_failure_prevents_rhs_effects() {
    let source = "fn effect()->f64{print(999);return 1.0;} fn main()->i64{let mut a:[f64;1]=[2.0];print(a[0]);a[1]=effect();return 0;}";
    verify_trap(
        "floating bounds before RHS",
        source,
        "f64:0x4000000000000000\n",
        "bounds",
        "a[1]=effect();",
    );
}

#[test]
fn floating_output_limit_rejects_the_complete_next_line() {
    let source = "fn main()->i64{let mut i:i64=0;while i<45591{print(-0.0);i=i+1;}return 0;}";
    let effects = "f64:0x8000000000000000\n".repeat(45590);
    assert_eq!(effects.len(), 1_048_570);
    verify_trap(
        "floating output limit",
        source,
        &effects,
        "output_limit",
        "print(-0.0);",
    );
}

#[test]
fn floating_and_mixed_parameters_cross_register_and_stack_boundaries() {
    for mixed in [false, true] {
        let parameters = (0..32)
            .map(|i| format!("p{i}:{}", if mixed && i % 2 == 1 { "i64" } else { "f64" }))
            .collect::<Vec<_>>()
            .join(",");
        let arguments = (0..32)
            .map(|i| {
                if mixed && i % 2 == 1 {
                    i.to_string()
                } else {
                    format!("{i}.0")
                }
            })
            .collect::<Vec<_>>()
            .join(",");
        let sum = (0..32)
            .map(|i| {
                if mixed && i % 2 == 1 {
                    format!("f64_from_i64(p{i})")
                } else {
                    format!("p{i}")
                }
            })
            .collect::<Vec<_>>()
            .join("+");
        let source = format!("fn sum({parameters})->f64{{return {sum};}}fn main()->i64{{let x:f64=sum({arguments});print(x);return f64_bits(x);}}");
        verify(
            if mixed {
                "mixed ABI"
            } else {
                "32 floating parameters"
            },
            &source,
            "f64:0x407f000000000000\n",
            0x407f000000000000,
        );
    }
}
