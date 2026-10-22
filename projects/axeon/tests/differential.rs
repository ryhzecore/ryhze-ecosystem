//! Independent expected answers plus interpreter/native agreement at every
//! supported optimization level. Native tests deliberately fail without Clang.
mod support;
use axeon::interpreter;
use std::env;
use std::fmt::Write;
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
            let path =
                env::temp_dir().join(format!("axeon-differential-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create test directory: {error}"),
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
            let result = output(bounded(&path).arg("--version"), "Clang version check");
            assert!(
                result.status.success(),
                "native tests require Clang 19.1.7; set AXEON_CLANG: {}",
                String::from_utf8_lossy(&result.stderr)
            );
            assert!(
                String::from_utf8_lossy(&result.stdout)
                    .split_whitespace()
                    .any(|part| part == "19.1.7"),
                "native tests require exactly Clang 19.1.7: {}",
                String::from_utf8_lossy(&result.stdout)
            );
            path
        })
        .as_path()
}

fn native_compile(directory: &Path, input: &str, opt: u8, object: bool) -> PathBuf {
    let artifact = if object {
        directory.join("program.o")
    } else {
        support::executable_path(directory.join("program"))
    };
    let mut command = bounded(clang());
    command
        .current_dir(directory)
        .env("SOURCE_DATE_EPOCH", "0")
        .args([
            "-target",
            support::target_triple(),
            "-Werror",
            "-Wno-override-module",
        ])
        .arg(format!("-O{opt}"))
        .arg(input);
    if cfg!(windows) {
        command.arg("-mno-incremental-linker-compatible");
    }
    if object {
        command.arg("-c");
    } else {
        command
            .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("runtime/runtime.c"))
            .args(["-std=c11", "-fno-ident"])
            .args(support::clang_link_flags());
    }
    command.arg("-o").arg(&artifact);
    let result = output(&mut command, &format!("Clang -O{opt}"));
    assert!(
        result.status.success(),
        "Clang -O{opt} failed ({}):\n{}",
        result.status,
        String::from_utf8_lossy(&result.stderr)
    );
    artifact
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Expected {
    Value(i64),
    Trap(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct Fixture {
    name: String,
    source: String,
    effects: String,
    expected: Expected,
}

impl Fixture {
    fn value(name: &str, source: &str, effects: &str, value: i64) -> Self {
        Self {
            name: name.into(),
            source: source.into(),
            effects: effects.into(),
            expected: Expected::Value(value),
        }
    }

    fn expression(name: &str, expression: &str, value: i64) -> Self {
        Self::value(
            name,
            &format!("fn main() -> i64 {{\n  return {expression};\n}}\n"),
            "",
            value,
        )
    }

    fn trap(name: &str, source: &str, effects: &str, kind: &str, marker: &str) -> Self {
        // Locate an explicitly chosen fixture lexeme, independently of all
        // compiler spans and the interpreter's trap-rendering helper.
        assert_eq!(
            source.matches(marker).count(),
            1,
            "ambiguous fixture marker"
        );
        let prefix = &source[..source.find(marker).unwrap()];
        let line = prefix.bytes().filter(|&byte| byte == b'\n').count() + 1;
        let column = prefix.rsplit('\n').next().unwrap().chars().count() + 1;
        Self {
            name: name.into(),
            source: source.into(),
            effects: effects.into(),
            expected: Expected::Trap(format!("trap[{kind}] at {line}:{column}\n")),
        }
    }

    fn expression_trap(name: &str, expression: &str, kind: &str) -> Self {
        Self::trap(
            name,
            &format!("fn main() -> i64 {{\n  return {expression};\n}}\n"),
            "",
            kind,
            expression,
        )
    }

    fn verify(&self) {
        let program = axeon::compile(&self.source).unwrap_or_else(|errors| {
            panic!("{} must compile: {errors:#?}\n{}", self.name, self.source)
        });
        let interpreted = interpreter::run(&program, 1_000_000);
        assert_eq!(
            interpreted.stdout, self.effects,
            "{}: independent expected interpreter effects",
            self.name
        );
        let (stdout, stderr, code) = match (&self.expected, interpreted.result) {
            (Expected::Value(expected), Ok(actual)) => {
                assert_eq!(
                    actual, *expected,
                    "{}: independent expected value",
                    self.name
                );
                (
                    format!("{}result: {expected}\n", self.effects),
                    String::new(),
                    0,
                )
            }
            (Expected::Trap(expected), Err(actual)) => {
                assert_eq!(
                    actual.render(&self.source),
                    *expected,
                    "{}: independent expected trap",
                    self.name
                );
                (self.effects.clone(), expected.clone(), 70)
            }
            (expected, actual) => panic!("{}: expected {expected:?}, got {actual:?}", self.name),
        };
        let ir = support::emit(&program, &self.source);
        let directory = TempDir::new();
        fs::write(directory.0.join("program.ll"), &ir).unwrap();
        for opt in OPTIMIZATIONS {
            let executable = native_compile(&directory.0, "program.ll", opt, false);
            let native = output(&mut bounded(&executable), &format!("{} -O{opt}", self.name));
            assert_eq!(
                native.status.code(),
                Some(code),
                "{} -O{opt}: exit status; stderr={}\nsource:\n{}",
                self.name,
                String::from_utf8_lossy(&native.stderr),
                self.source
            );
            assert_eq!(
                String::from_utf8(native.stdout).unwrap(),
                stdout,
                "{} -O{opt}: full stdout",
                self.name
            );
            assert_eq!(
                String::from_utf8(native.stderr).unwrap(),
                stderr,
                "{} -O{opt}: full stderr",
                self.name
            );
        }
    }
}

#[test]
fn checked_arithmetic_and_shift_boundaries() {
    let values = [
        ("maximum", "9223372036854775807", i64::MAX),
        ("minimum", "-9223372036854775808", i64::MIN),
        ("add fits maximum", "9223372036854775806 + 1", i64::MAX),
        ("add fits minimum", "-9223372036854775807 + -1", i64::MIN),
        ("subtract fits", "-9223372036854775807 - 1", i64::MIN),
        ("multiply minimum", "-4611686018427387904 * 2", i64::MIN),
        ("multiply maximum", "9223372036854775807 * 1", i64::MAX),
        ("negation fits", "-(-9223372036854775807)", i64::MAX),
        ("negative quotient", "-7 / 3", -2),
        ("negative divisor", "7 / -3", -2),
        ("negative remainder", "-7 % 3", -1),
        ("negative divisor remainder", "7 % -3", 1),
        ("minimum division", "-9223372036854775808 / 1", i64::MIN),
        ("minimum remainder", "-9223372036854775808 % 3", -2),
        ("left shift into minimum", "-1 << 63", i64::MIN),
        ("left shift maximum", "9223372036854775807 << 0", i64::MAX),
        ("left shift zero", "0 << 63", 0),
        ("arithmetic right shift", "-7 >> 1", -4),
        ("right shift sign", "-9223372036854775808 >> 63", -1),
        ("right shift positive", "9223372036854775807 >> 63", 0),
        ("bitwise", "(~0 & 15) ^ (2 | 4)", 9),
        ("precedence and associativity", "20 - 6 - 2 * 3", 8),
    ];
    for (name, expression, expected) in values {
        Fixture::expression(name, expression, expected).verify();
    }
    let traps = [
        (
            "add positive overflow",
            "9223372036854775807 + 1",
            "overflow",
        ),
        (
            "add negative overflow",
            "-9223372036854775808 + -1",
            "overflow",
        ),
        (
            "subtract negative overflow",
            "-9223372036854775808 - 1",
            "overflow",
        ),
        (
            "subtract positive overflow",
            "9223372036854775807 - -1",
            "overflow",
        ),
        (
            "multiply positive overflow",
            "3037000500 * 3037000500",
            "overflow",
        ),
        (
            "multiply negative overflow",
            "-4611686018427387905 * 2",
            "overflow",
        ),
        ("negate minimum", "-(-9223372036854775808)", "overflow"),
        (
            "minimum divided by minus one",
            "-9223372036854775808 / -1",
            "overflow",
        ),
        (
            "minimum remainder minus one",
            "-9223372036854775808 % -1",
            "overflow",
        ),
        ("division by zero", "17 / 0", "division_by_zero"),
        ("remainder by zero", "17 % 0", "division_by_zero"),
        ("negative left count", "1 << -1", "shift_count"),
        ("large left count", "1 << 64", "shift_count"),
        ("negative right count", "1 >> -1", "shift_count"),
        ("large right count", "1 >> 64", "shift_count"),
        ("left shift sign overflow", "1 << 63", "overflow"),
        ("left shift negative overflow", "-2 << 63", "overflow"),
        (
            "left shift one bit overflow",
            "9223372036854775807 << 1",
            "overflow",
        ),
    ];
    for (name, expression, kind) in traps {
        Fixture::expression_trap(name, expression, kind).verify();
    }
}

#[test]
fn effects_evaluation_order_and_short_circuit() {
    let helpers = "fn mark(n: i64) -> i64 { print(n); return n; }\n\
        fn combine(a: i64, b: i64, c: i64) -> i64 { print(4); return a * 100 + b * 10 + c; }\n\
        fn fail() -> i64 { print(8); return 1 / 0; }\n";
    Fixture::value(
        "left-to-right operands",
        &format!("{helpers}fn main() -> i64 {{ return mark(1) + mark(2) * mark(3); }}"),
        "1\n2\n3\n",
        7,
    )
    .verify();
    Fixture::value(
        "left-to-right arguments",
        &format!("{helpers}fn main() -> i64 {{ return combine(mark(1), mark(2), mark(3)); }}"),
        "1\n2\n3\n4\n",
        123,
    )
    .verify();
    Fixture::trap(
        "argument trap stops later arguments",
        &format!("{helpers}fn main() -> i64 {{ return combine(mark(1), fail(), mark(3)); }}"),
        "1\n8\n",
        "division_by_zero",
        "1 / 0",
    )
    .verify();
    Fixture::trap(
        "left operand trap stops right operand",
        &format!("{helpers}fn main() -> i64 {{ return fail() + mark(2); }}"),
        "8\n",
        "division_by_zero",
        "1 / 0",
    )
    .verify();
    Fixture::trap(
        "right operand effects precede overflow",
        &format!(
            "{helpers}fn main() -> i64 {{ print(true); return 9223372036854775807 + mark(1); }}"
        ),
        "true\n1\n",
        "overflow",
        "9223372036854775807 + mark(1)",
    )
    .verify();
    Fixture::trap(
        "division evaluates both operands",
        &format!("{helpers}fn main() -> i64 {{ return mark(9) / mark(0); }}"),
        "9\n0\n",
        "division_by_zero",
        "mark(9) / mark(0)",
    )
    .verify();
    Fixture::value(
        "boolean branches and short circuit",
        r#"
fn mark(n: i64, b: bool) -> bool { print(n); return b; }
fn bomb() -> bool { print(99); return 1 / 0 == 0; }
fn main() -> i64 {
    print(false && bomb());
    print(true || bomb());
    print(mark(1, true) && mark(2, false));
    print(mark(3, false) || mark(4, true));
    print((false && bomb()) || (true && !false));
    return 0;
}
"#,
        "false\ntrue\n1\n2\nfalse\n3\n4\ntrue\ntrue\n",
        0,
    )
    .verify();
}

#[test]
fn control_flow_scopes_and_boolean_functions() {
    Fixture::value(
        "nested loops break and continue",
        r#"
fn main() -> i64 {
    let mut i: i64 = 0;
    let mut total: i64 = 0;
    while i < 3 {
        i = i + 1;
        let mut j: i64 = 0;
        while j < 5 {
            j = j + 1;
            if j == 2 { continue; }
            if j == 4 { break; }
            total = total + i * 10 + j;
        }
    }
    print(total);
    return total;
}
"#,
        "132\n",
        132,
    )
    .verify();
    Fixture::value(
        "return from a loop",
        r#"
fn search(n: i64) -> i64 {
    let mut i: i64 = 0;
    while i < n {
        i = i + 1;
        if i == 4 { return 40; }
        continue;
    }
    return 99;
}
fn main() -> i64 { print(search(2)); return search(6); }
"#,
        "99\n",
        40,
    )
    .verify();
    Fixture::value(
        "shadow initializer and lexical scope",
        r#"
fn main() -> i64 {
    let x: i64 = 5;
    print(x);
    {
        let mut x: i64 = x + 1;
        print(x);
        x = 7;
        print(x);
    }
    print(x);
    return x;
}
"#,
        "5\n6\n7\n5\n",
        5,
    )
    .verify();
    Fixture::value(
        "bool parameters returns equality and comparisons",
        r#"
fn choose(b: bool, n: i64) -> bool {
    if b { return n <= -1; } else { return n >= 0; }
}
fn main() -> i64 {
    let a: bool = choose(true, -9223372036854775808);
    let b: bool = choose(false, 9223372036854775807);
    print(a == b);
    print(a != false);
    print(!choose(true, 0));
    print(-1 < 0 && 2 > 1);
    if a && b { return 42; } else { return 99; }
}
"#,
        "true\ntrue\ntrue\ntrue\n",
        42,
    )
    .verify();
    Fixture::value(
        "zero iteration loop",
        r#"
fn main() -> i64 {
    let mut n: i64 = 7;
    while false { print(99); n = 1 / 0; }
    return n;
}
"#,
        "",
        7,
    )
    .verify();
}

#[test]
fn recursion_depth_includes_main_and_restores_on_return() {
    let down =
        "fn down(n: i64) -> i64 { if n == 0 { return 0; } else { return down(n - 1) + 1; } }\n";
    Fixture::value(
        "depth exactly 64",
        &format!("{down}fn main() -> i64 {{ return down(62); }}"),
        "",
        62,
    )
    .verify();
    Fixture::value(
        "depth restored after return",
        &format!("{down}fn main() -> i64 {{ return down(62) + down(62); }}"),
        "",
        124,
    )
    .verify();
    let source = "fn down(n: i64) -> i64 { print(n); if n == 0 { return 0; } else { return down(n - 1); } }\nfn main() -> i64 { return down(63); }\n";
    let mut effects = String::new();
    for value in (1..=63).rev() {
        writeln!(effects, "{value}").unwrap();
    }
    Fixture::trap(
        "65th active call",
        source,
        &effects,
        "call_depth",
        "fn down",
    )
    .verify();
}

#[test]
fn output_limit_is_exact_atomic_and_excludes_result() {
    let body = "fn main() -> i64 {\n\
        let mut i: i64 = 0;\n\
        while i < 49932 { print(-9223372036854775808); i = i + 1; }\n\
        print(0);\n\
        print(0);\n";
    let effects = format!("{}0\n0\n", "-9223372036854775808\n".repeat(49932));
    assert_eq!(effects.len(), 1 << 20);
    Fixture::value(
        "exact output limit",
        &format!("{body}return 9223372036854775807;\n}}\n"),
        &effects,
        i64::MAX,
    )
    .verify();
    Fixture::trap(
        "atomic output limit trap",
        &format!("{body}print(true);\nreturn 0;\n}}\n"),
        &effects,
        "output_limit",
        "print(true)",
    )
    .verify();
    let partial_body = body.strip_suffix("print(0);\nprint(0);\n").unwrap();
    Fixture::trap(
        "over-limit print writes no partial prefix",
        &format!("{partial_body}print(true);\nreturn 0;\n}}\n"),
        &effects[..effects.len() - 4],
        "output_limit",
        "print(true)",
    )
    .verify();
}

struct Lcg(u64);

impl Lcg {
    fn next(&mut self) -> u64 {
        self.0 = self
            .0
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        self.0
    }

    fn small(&mut self) -> i64 {
        ((self.next() >> 32) % 401) as i64 - 200
    }
}

fn generated(seed: u64) -> Vec<Fixture> {
    let mut rng = Lcg(seed);
    (0..64).map(|index| {
        let name = format!("generated seed={seed:#x} index={index}");
        let (a, b, c) = (rng.small(), rng.small(), rng.small());
        match index % 8 {
            0 => Fixture::expression(&name, &format!("({a} + {b}) * {c} - {a}"),
                (((a as i128) + (b as i128)) * (c as i128) - (a as i128)) as i64),
            1 => {
                let shift = (rng.next() % 6) as u32;
                Fixture::expression(&name, &format!("(({a} & 255) << {shift}) ^ ({b} | {c})"),
                    ((a & 255) * (1_i64 << shift)) ^ (b | c))
            }
            2 => {
                let divisor = if b == 0 { 1 } else { b };
                Fixture::expression(&name, &format!("({a} / {divisor}) * 100 + ({a} % {divisor})"),
                    (((a as i128) / (divisor as i128)) * 100 + ((a as i128) % (divisor as i128))) as i64)
            }
            3 => {
                let bound = rng.next() % 15 + 1;
                let expected: i64 = (1..=bound as i64).filter(|value| value % 3 != 0).map(|value| value + a).sum();
                let source = format!("fn main() -> i64 {{ let mut i: i64 = 0; let mut total: i64 = 0; while i < {bound} {{ i = i + 1; if i % 3 == 0 {{ continue; }} total = total + i + {a}; }} print(total); return total; }}");
                Fixture::value(&name, &source, &format!("{expected}\n"), expected)
            }
            4 => {
                let predicate = a < b || a == c;
                let expected = if predicate { a - b } else { b - a };
                let source = format!("fn pred(a: i64, b: i64, c: i64) -> bool {{ return a < b || a == c; }} fn main() -> i64 {{ let b: bool = pred({a}, {b}, {c}); print(b); if b {{ return {a} - {b}; }} else {{ return {b} - {a}; }} }}");
                Fixture::value(&name, &source, &format!("{predicate}\n"), expected)
            }
            5 => {
                let source = format!("fn mark(n: i64) -> i64 {{ print(n); return n; }} fn combine(a: i64, b: i64) -> i64 {{ return a - b; }} fn main() -> i64 {{ let n: i64 = {a}; {{ let n: i64 = n + {c}; print(n); }} return combine(mark(n), mark({b})); }}");
                Fixture::value(&name, &source, &format!("{}\n{a}\n{b}\n", a + c), a - b)
            }
            6 => {
                let positive = rng.next() % 100 + 1;
                let expression = format!("9223372036854775807 + {positive}");
                let source = format!("fn main() -> i64 {{ print({a}); return {expression}; }}");
                Fixture::trap(&name, &source, &format!("{a}\n"), "overflow", &expression)
            }
            _ => {
                let count = if rng.next() & 1 == 0 { -1 } else { 64 };
                let expression = format!("{a} >> {count}");
                let source = format!("fn main() -> i64 {{ print({b}); return {expression}; }}");
                Fixture::trap(&name, &source, &format!("{b}\n"), "shift_count", &expression)
            }
        }
    }).collect()
}

#[test]
fn deterministic_generated_programs_match_independent_answers() {
    const SEED: u64 = 0x5259_485a_455f_3031;
    let fixtures = generated(SEED);
    assert_eq!(fixtures.len(), 64);
    assert_eq!(fixtures, generated(SEED));
    assert_ne!(fixtures[0].source, generated(SEED ^ 1)[0].source);
    for fixture in fixtures {
        fixture.verify();
    }
    eprintln!(
        "generated coverage: 64 programs; LCG seed={SEED:#018x}; O0/O2/O3; 192 native executions"
    );
}

#[test]
fn emitted_ir_and_object_bytes_are_reproducible_across_directories() {
    let source = "fn square(n: i64) -> i64 { return n * n; }\nfn main() -> i64 { let mut n: i64 = 0; while n < 5 { print(square(n)); n = n + 1; } return n; }\n";
    let left = TempDir::new();
    let right = TempDir::new();
    let first = support::emit(&axeon::compile(source).unwrap(), source);
    let second = support::emit(&axeon::compile(source).unwrap(), source);
    assert_eq!(
        first, second,
        "fresh compilation must emit identical IR bytes"
    );
    assert!(!first.contains(&left.0.to_string_lossy().to_string()));
    assert!(!second.contains(&right.0.to_string_lossy().to_string()));
    fs::write(left.0.join("left.ll"), first).unwrap();
    fs::write(right.0.join("right.ll"), second).unwrap();
    for opt in OPTIMIZATIONS {
        let left_object = native_compile(&left.0, "left.ll", opt, true);
        let right_object = native_compile(&right.0, "right.ll", opt, true);
        let left_bytes = fs::read(left_object).unwrap();
        let right_bytes = fs::read(right_object).unwrap();
        #[cfg(windows)]
        assert_eq!(&left_bytes[4..8], &[0; 4], "deterministic COFF timestamp");
        assert!(!left_bytes.is_empty());
        assert_eq!(
            left_bytes, right_bytes,
            "-O{opt}: object bytes differ between paths"
        );
    }
}
