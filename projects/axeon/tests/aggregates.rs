//! Independent aggregate answers, ownership rejections, and native differential
//! coverage. Every accepted fixture runs under Clang 19.1.7 at O0, O2, and O3.
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
            let path =
                env::temp_dir().join(format!("axeon-aggregates-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create aggregate test directory: {error}"),
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
                "aggregate Clang version check",
            );
            assert!(
                result.status.success(),
                "native aggregate tests require Clang 19.1.7; set AXEON_CLANG: {}",
                String::from_utf8_lossy(&result.stderr)
            );
            assert!(
                String::from_utf8_lossy(&result.stdout)
                    .split_whitespace()
                    .any(|word| word == "19.1.7"),
                "native aggregate tests require exactly Clang 19.1.7: {}",
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

#[derive(Clone, Debug, PartialEq, Eq)]
enum Expected {
    Value(i64),
    Trap {
        code: i32,
        name: &'static str,
        start: usize,
        end: usize,
        rendered: String,
    },
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

    fn trap(
        name: &str,
        source: &str,
        effects: &str,
        kind: &'static str,
        code: i32,
        marker: &str,
    ) -> Self {
        assert_eq!(
            source.matches(marker).count(),
            1,
            "{name}: ambiguous fixture marker"
        );
        let start = source.find(marker).unwrap();
        let end = start + marker.len();
        let prefix = &source[..start];
        let line = prefix.bytes().filter(|&byte| byte == b'\n').count() + 1;
        let column = prefix.rsplit('\n').next().unwrap().chars().count() + 1;
        Self {
            name: name.into(),
            source: source.into(),
            effects: effects.into(),
            expected: Expected::Trap {
                code,
                name: kind,
                start,
                end,
                rendered: format!("trap[{kind}] at {line}:{column}\n"),
            },
        }
    }

    fn bounds(name: &str, source: &str, effects: &str, marker: &str) -> Self {
        Self::trap(name, source, effects, "bounds", 7, marker)
    }

    fn verify(&self) {
        let program = axeon::compile(&self.source).unwrap_or_else(|errors| {
            panic!("{} must compile: {errors:#?}\n{}", self.name, self.source)
        });
        let interpreted = interpreter::run(&program, 1_000_000);
        assert_eq!(
            interpreted.stdout, self.effects,
            "{}: independent expected effects",
            self.name
        );
        let (stdout, stderr, code) = match (&self.expected, interpreted.result) {
            (Expected::Value(expected), Ok(actual)) => {
                assert_eq!(
                    actual, *expected,
                    "{}: independent expected result",
                    self.name
                );
                (
                    format!("{}result: {expected}\n", self.effects),
                    String::new(),
                    0,
                )
            }
            (
                Expected::Trap {
                    code,
                    name,
                    start,
                    end,
                    rendered,
                },
                Err(actual),
            ) => {
                assert_eq!(actual.kind.code(), *code, "{}: stable trap code", self.name);
                assert_eq!(actual.kind.name(), *name, "{}: stable trap name", self.name);
                assert_eq!(
                    (actual.span.start, actual.span.end),
                    (*start, *end),
                    "{}: complete independent trap byte span",
                    self.name
                );
                assert_eq!(
                    actual.render(&self.source),
                    *rendered,
                    "{}: independent trap rendering",
                    self.name
                );
                (self.effects.clone(), rendered.clone(), 70)
            }
            (expected, actual) => panic!("{}: expected {expected:?}, got {actual:?}", self.name),
        };
        let directory = TempDir::new();
        fs::write(
            directory.0.join("program.ll"),
            support::emit(&program, &self.source),
        )
        .unwrap();
        for opt in OPTIMIZATIONS {
            let executable = native_compile(&directory.0, opt, &self.name);
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
                "{} -O{opt}: complete stdout",
                self.name
            );
            assert_eq!(
                String::from_utf8(native.stderr).unwrap(),
                stderr,
                "{} -O{opt}: complete stderr",
                self.name
            );
        }
    }
}

#[test]
fn array_and_slice_values_moves_and_lexical_loans() {
    let fixtures = [
        Fixture::value("single element and signed minimum", "fn main() -> i64 { let a: [i64;1] = [-9223372036854775808]; print(len(a)); return a[0]; }", "1\n", i64::MIN),
        Fixture::value("full shared loans coexist with owner reads", "fn main() -> i64 { let a: [i64;3] = [4,5,6]; let s: &[i64] = &a; let t: &[i64] = &a[1..3]; print(len(a)); print(a[0]); print(len(s)); print(len(t)); return s[1] + t[1]; }", "3\n4\n3\n2\n", 11),
        Fixture::value("mutable slice capability and later owner access", "fn main() -> i64 { let mut a: [i64;3] = [10,20,30]; { let s: &mut [i64] = &mut a[1..3]; s[0] = s[1] + 2; s[1] = 7; print(len(s)); print(s[0]); } print(a[1]); return a[0] + a[1] + a[2]; }", "2\n32\n32\n", 49),
        Fixture::value("mutable full borrow endpoint observes owner before loan", "fn main() -> i64 { let mut a: [i64;2] = [5,6]; { let s: &mut[i64] = &mut a[0..len(a)]; s[len(s)-1] = 19; } { let t: &mut[i64] = &mut a; t[0] = 8; } return a[0] + a[1]; }", "", 27),
        Fixture::value("both mutable range endpoints read owner before loan", "fn main() -> i64 { let mut a: [i64;3] = [1,3,5]; { let s: &mut[i64] = &mut a[a[0]..a[1]]; print(len(s)); s[0] = 99; } return a[1]; }", "2\n", 99),
        Fixture::value("recursive frames retain independent live views", "fn sum(n: i64) -> i64 { let mut a: [i64;1] = [n]; let s: &mut[i64] = &mut a; if n == 0 { return s[0]; } s[0] = s[0] + sum(n-1); return s[0]; } fn main() -> i64 { return sum(12); }", "", 78),
        Fixture::value("zero length slices include end boundary", "fn main() -> i64 { let mut a: [i64;2] = [3,4]; { let s: &[i64] = &a[0..0]; let t: &[i64] = &a[2..2]; print(len(s)); print(len(t)); } { let s: &mut[i64] = &mut a[2..2]; print(len(s)); } a[0] = 9; return a[0]; }", "0\n0\n0\n", 9),
        Fixture::value("scope exit releases all shared loans before move", "fn main() -> i64 { let mut a: [i64;2] = [2,3]; { let s: &[i64] = &a; { let t: &[i64] = &a[1..2]; print(t[0]); } print(s[0]); } a[1] = 8; let b: [i64;2] = a; let mut c: [i64;2] = b; c[0] = 7; return c[0] + c[1]; }", "3\n2\n", 15),
        Fixture::value("loan identity survives owner name shadowing", "fn main() -> i64 { let mut a: [i64;1] = [5]; { let s: &[i64] = &a; { let mut a: [i64;1] = [20]; a[0] = 21; print(a[0]); print(s[0]); } print(s[0]); } a[0] = 8; return a[0]; }", "21\n5\n5\n", 8),
        Fixture::value("slice names shadow by local identity", "fn main() -> i64 { let a: [i64;2] = [7,8]; let s: &[i64] = &a; { let b: [i64;1] = [19]; let s: &[i64] = &b; print(s[0]); } print(s[1]); return len(s); }", "19\n8\n", 2),
        Fixture::value("move to shadowing owner resolves outer initializer", "fn main() -> i64 { let a: [i64;1] = [13]; { let mut a: [i64;1] = a; a[0] = a[0] + 1; return a[0]; } }", "", 14),
    ];
    for fixture in fixtures {
        fixture.verify();
    }

    let values: Vec<i64> = (0..32).map(|n| n - 16).collect();
    let literal = values
        .iter()
        .map(i64::to_string)
        .collect::<Vec<_>>()
        .join(",");
    Fixture::value("32 element boundary", &format!("fn main() -> i64 {{ let a: [i64;32] = [{literal}]; let mut i: i64 = 0; let mut sum: i64 = 0; while i < len(a) {{ sum = sum + a[i]; i = i + 1; }} print(a[0]); print(a[31]); return sum; }}"), "-16\n15\n", -16).verify();
}

#[test]
fn ownership_control_flow_uses_only_paths_that_continue() {
    let fixtures = [
        Fixture::value("returning moved branch is excluded from join", "fn choose(flag: bool) -> i64 { let a: [i64;2] = [10,20]; if flag { let b: [i64;2] = a; return b[0]; } else { print(3); } return a[1]; } fn main() -> i64 { print(choose(true)); return choose(false); }", "10\n3\n", 20),
        Fixture::value("both branches move independently", "fn choose(flag: bool) -> i64 { let a: [i64;2] = [4,9]; let mut n: i64 = 0; if flag { let b: [i64;2] = a; n = b[0]; } else { let c: [i64;2] = a; n = c[1]; } return n; } fn main() -> i64 { return choose(true) + choose(false); }", "", 13),
        Fixture::value("branch loans end before join", "fn choose(flag: bool) -> i64 { let mut a: [i64;1] = [4]; if flag { let s: &mut[i64] = &mut a; s[0] = 8; } else { let s: &[i64] = &a; print(s[0]); } a[0] = a[0] + 1; return a[0]; } fn main() -> i64 { print(choose(false)); return choose(true); }", "4\n5\n", 9),
        Fixture::value("fresh iteration owners move across continue joins", "fn main() -> i64 { let mut i: i64 = 0; let mut total: i64 = 0; while i < 3 { i = i + 1; let a: [i64;2] = [i,10]; if i == 2 { let b: [i64;2] = a; total = total + b[0]; continue; } total = total + a[1]; } return total; }", "", 22),
        Fixture::value("fresh iteration owner move on break path", "fn main() -> i64 { let mut i: i64 = 0; let mut total: i64 = 0; while i < 4 { i = i + 1; let a: [i64;1] = [i]; if i == 3 { let b: [i64;1] = a; total = total + b[0]; break; } total = total + a[0]; } return total; }", "", 6),
        Fixture::value("innermost fresh owners can move in nested loops", "fn main() -> i64 { let mut i: i64 = 0; let mut total: i64 = 0; while i < 2 { i = i + 1; let mut j: i64 = 0; while j < 2 { j = j + 1; let a: [i64;1] = [i*10+j]; let b: [i64;1] = a; total = total + b[0]; } } return total; }", "", 66),
        Fixture::value("loans end on continue and break", "fn main() -> i64 { let mut a: [i64;1] = [0]; let mut i: i64 = 0; while i < 3 { i = i + 1; let s: &mut[i64] = &mut a; s[0] = s[0] + i; if i == 2 { continue; } if i == 3 { break; } } a[0] = a[0] + 10; return a[0]; }", "", 16),
        Fixture::value("iteration storage and views reinitialize before moves", "fn main() -> i64 { let mut i: i64 = 0; let mut total: i64 = 0; while i < 5 { let mut a: [i64;2] = [i,i+1]; { let s: &mut[i64] = &mut a[i%2..2]; s[0] = s[0] + 10; } let b: [i64;2] = a; total = total + b[0] + b[1]; i = i + 1; } return total; }", "", 75),
    ];
    for fixture in fixtures {
        fixture.verify();
    }
}

const MARK: &str = "fn mark(n: i64) -> i64 { print(n); return n; }\n";
const FAIL: &str = "fn fail() -> i64 { print(99); return 1 / 0; }\n";

#[test]
fn aggregate_effects_have_defined_left_to_right_order() {
    let fixtures = [
        Fixture::value("array literal evaluates left to right", &format!("{MARK}fn main() -> i64 {{ let a: [i64;3] = [mark(3),mark(1),mark(2)]; return a[0]*100+a[1]*10+a[2]; }}"), "3\n1\n2\n", 312),
        Fixture::value("index precedes assignment RHS", &format!("{MARK}fn main() -> i64 {{ let mut a: [i64;2] = [3,4]; a[mark(1)] = mark(7); return a[mark(1)]; }}"), "1\n7\n1\n", 7),
        Fixture::value("range endpoints evaluated once and left to right", &format!("{MARK}fn main() -> i64 {{ let a: [i64;3] = [6,7,8]; let s: &[i64] = &a[mark(1)..mark(3)]; print(len(s)); return s[mark(1)]; }}"), "1\n3\n2\n1\n", 8),
        Fixture::trap("literal trap suppresses later elements", &format!("{MARK}{FAIL}fn main() -> i64 {{ let a: [i64;3] = [mark(1),fail(),mark(3)]; return a[0]; }}"), "1\n99\n", "division_by_zero", 2, "1 / 0"),
        Fixture::bounds("assignment bounds precede all RHS effects", &format!("{MARK}{FAIL}fn main() -> i64 {{ let mut a: [i64;2] = [3,4]; a[mark(2)] = fail(); return 0; }}"), "2\n", "a[mark(2)] = fail();"),
        Fixture::bounds("slice assignment checks before RHS", &format!("{MARK}fn main() -> i64 {{ let mut a: [i64;2] = [3,4]; let s: &mut[i64] = &mut a[1..2]; s[mark(-1)] = mark(77); return 0; }}"), "-1\n", "s[mark(-1)] = mark(77);"),
        Fixture::bounds("invalid low still evaluates high before bounds", &format!("{MARK}fn main() -> i64 {{ let a: [i64;2] = [1,2]; let s: &[i64] = &a[mark(-1)..mark(2)]; return len(s); }}"), "-1\n2\n", "&a[mark(-1)..mark(2)]"),
        Fixture::trap("high trap takes priority over invalid low bounds", &format!("{MARK}{FAIL}fn main() -> i64 {{ let a: [i64;2] = [1,2]; let s: &[i64] = &a[mark(-1)..fail()]; return len(s); }}"), "-1\n99\n", "division_by_zero", 2, "1 / 0"),
        Fixture::trap("low trap suppresses high evaluation", &format!("{MARK}{FAIL}fn main() -> i64 {{ let a: [i64;2] = [1,2]; let s: &[i64] = &a[fail()..mark(2)]; return len(s); }}"), "99\n", "division_by_zero", 2, "1 / 0"),
        Fixture::trap("index evaluation trap suppresses RHS", &format!("{MARK}{FAIL}fn main() -> i64 {{ let mut a: [i64;2] = [1,2]; a[fail()] = mark(7); return 0; }}"), "99\n", "division_by_zero", 2, "1 / 0"),
    ];
    for fixture in fixtures {
        fixture.verify();
    }
}

#[test]
fn bounds_traps_use_complete_operation_spans() {
    for index in ["-1", "2", "9223372036854775807", "-9223372036854775808"] {
        let marker = format!("a[{index}]");
        let source = format!("// café 雪\nfn main() -> i64 {{\n let a: [i64;2] = [10,20];\n\u{2003}return {marker};\n}}\n");
        Fixture::bounds(&format!("owner index {index}"), &source, "", &marker).verify();
    }
    for (name, range) in [
        ("negative low", "-1..1"),
        ("negative high", "0..-1"),
        ("reversed", "2..1"),
        ("high beyond owner", "0..3"),
        ("low beyond owner", "3..3"),
        ("minimum low", "-9223372036854775808..0"),
        ("maximum high", "0..9223372036854775807"),
        (
            "range subtraction must follow bounds",
            "-9223372036854775808..9223372036854775807",
        ),
    ] {
        let marker = format!("&a[{range}]");
        let source = format!("fn main() -> i64 {{ let a: [i64;2] = [1,2]; let s: &[i64] = {marker}; return len(s); }}");
        Fixture::bounds(&format!("slice range {name}"), &source, "", &marker).verify();
    }
    let fixtures = [
        Fixture::bounds("slice upper bound uses slice length", "fn main() -> i64 { let a: [i64;3] = [1,2,3]; let s: &[i64] = &a[1..2]; return s[1]; }", "", "s[1]"),
        Fixture::bounds("empty slice has no readable index", "fn main() -> i64 { let a: [i64;1] = [8]; let s: &[i64] = &a[1..1]; return s[0]; }", "", "s[0]"),
        Fixture::bounds("empty mutable slice has no writable index", "fn main() -> i64 { let mut a: [i64;1] = [8]; let s: &mut[i64] = &mut a[1..1]; s[0] = 9; return 0; }", "", "s[0] = 9;"),
        Fixture::bounds("mutable range covers ampersand through bracket", "fn main() -> i64 { let mut a: [i64;1] = [8]; let s: &mut[i64] = &mut a[0..2]; return len(s); }", "", "&mut a[0..2]"),
    ];
    for fixture in fixtures {
        fixture.verify();
    }
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
    fn below(&mut self, bound: usize) -> usize {
        ((self.next() >> 32) % bound as u64) as usize
    }
    fn small(&mut self) -> i64 {
        self.below(101) as i64 - 50
    }
}

fn generated(seed: u64) -> Vec<Fixture> {
    let mut rng = Lcg(seed);
    (0..24).map(|case| {
        let n = rng.below(7) + 2;
        let mut values: Vec<i64> = (0..n).map(|_| rng.small()).collect();
        let literal = values.iter().map(i64::to_string).collect::<Vec<_>>().join(",");
        let name = format!("aggregate generated seed={seed:#x} case={case}");
        match case % 6 {
            0 => {
                let i = rng.below(n);
                let j = rng.below(n);
                let expected = values[i] * 3 + values[j] + n as i64;
                Fixture::value(&name, &format!("fn main() -> i64 {{ let a: [i64;{n}] = [{literal}]; let i: i64 = {i}; print(a[i]); return a[i]*3 + a[{j}] + len(a); }}"), &format!("{}\n", values[i]), expected)
            }
            1 => {
                let i = rng.below(n);
                let replacement = rng.small();
                let old = values[i];
                values[i] = replacement;
                let sum: i64 = values.iter().sum();
                Fixture::value(&name, &format!("fn main() -> i64 {{ let mut a: [i64;{n}] = [{literal}]; print(a[{i}]); a[{i}] = {replacement}; print(a[{i}]); let mut j: i64 = 0; let mut total: i64 = 0; while j < len(a) {{ total = total + a[j]; j = j + 1; }} return total; }}"), &format!("{old}\n{replacement}\n"), sum)
            }
            2 => {
                let lo = rng.below(n);
                let hi = lo + 1 + rng.below(n - lo);
                let length = hi - lo;
                Fixture::value(&name, &format!("fn main() -> i64 {{ let a: [i64;{n}] = [{literal}]; let s: &[i64] = &a[{lo}..{hi}]; print(len(s)); return s[0] + s[len(s)-1]; }}"), &format!("{length}\n"), values[lo] + values[hi - 1])
            }
            3 => {
                let lo = rng.below(n);
                let hi = lo + 1 + rng.below(n - lo);
                let index = rng.below(hi - lo);
                let delta = rng.small();
                values[lo + index] += delta;
                let sum: i64 = values.iter().sum();
                Fixture::value(&name, &format!("fn main() -> i64 {{ let mut a: [i64;{n}] = [{literal}]; {{ let s: &mut[i64] = &mut a[{lo}..{hi}]; s[{index}] = s[{index}] + {delta}; print(s[{index}]); }} let mut j: i64 = 0; let mut total: i64 = 0; while j < len(a) {{ total = total + a[j]; j = j + 1; }} return total; }}"), &format!("{}\n", values[lo + index]), sum)
            }
            4 => {
                let cut = rng.below(n + 1);
                let expected = values[0] - values[n - 1] + n as i64;
                Fixture::value(&name, &format!("fn main() -> i64 {{ let a: [i64;{n}] = [{literal}]; {{ let s: &[i64] = &a[{cut}..{cut}]; print(len(s)); }} let b: [i64;{n}] = a; return b[0] - b[{n}-1] + len(b); }}"), "0\n", expected)
            }
            _ => {
                let iterations = rng.below(4) + 1;
                let base = values[0];
                let expected: i64 = (0..iterations).map(|i| base + i as i64).sum();
                Fixture::value(&name, &format!("fn main() -> i64 {{ let mut i: i64 = 0; let mut total: i64 = 0; while i < {iterations} {{ let a: [i64;1] = [{base}+i]; let b: [i64;1] = a; total = total + b[0]; i = i + 1; }} print(total); return total; }}"), &format!("{expected}\n"), expected)
            }
        }
    }).collect()
}

#[test]
fn seeded_aggregate_programs_match_independent_answers() {
    const SEED: u64 = 0x4158_454f_4e5f_4132;
    let fixtures = generated(SEED);
    assert_eq!(fixtures.len(), 24);
    assert_eq!(fixtures, generated(SEED));
    assert_ne!(fixtures[0].source, generated(SEED ^ 1)[0].source);
    for fixture in fixtures {
        fixture.verify();
    }
    eprintln!("aggregate generated coverage: 24 programs; LCG seed={SEED:#018x}; O0/O2/O3; 72 native executions");
}

fn rejects(name: &str, source: &str, code: &str) {
    let first = axeon::compile(source).expect_err(name);
    assert!(
        first.iter().any(|error| error.code == code),
        "{name}: expected {code}, got {first:#?}\n{source}"
    );
    assert!(
        !first.iter().any(|error| error.code.starts_with("E_")),
        "{name}: safety fixture must reach checking, got {first:#?}"
    );
    for error in &first {
        for span in
            std::iter::once(error.span).chain(error.related.iter().map(|related| related.span))
        {
            assert!(
                span.start <= span.end && span.end <= source.len(),
                "{name}: invalid span {span:?}"
            );
            assert!(
                source.is_char_boundary(span.start) && source.is_char_boundary(span.end),
                "{name}: span splits a UTF-8 character"
            );
        }
    }
    assert_eq!(
        axeon::compile(source).unwrap_err(),
        first,
        "{name}: diagnostics must be deterministic"
    );
}

#[test]
fn unsafe_moves_and_conflicting_loans_are_rejected() {
    let cases = [
        ("read moved owner", "let a: [i64;1] = [1]; let b: [i64;1] = a; print(a[0]);", "O006"),
        ("len moved owner", "let a: [i64;1] = [1]; let b: [i64;1] = a; print(len(a));", "O006"),
        ("borrow moved owner", "let a: [i64;1] = [1]; let b: [i64;1] = a; let s: &[i64] = &a;", "O006"),
        ("move owner twice", "let a: [i64;1] = [1]; let b: [i64;1] = a; let c: [i64;1] = a;", "O006"),
        ("move shared borrowed owner", "let a: [i64;1] = [1]; let s: &[i64] = &a; let b: [i64;1] = a;", "O007"),
        ("move exclusively borrowed owner", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; let b: [i64;1] = a;", "O007"),
        ("owner read under exclusive loan", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; print(a[0]);", "O007"),
        ("owner len under exclusive loan", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; print(len(a));", "O007"),
        ("owner write under shared loan", "let mut a: [i64;1] = [1]; let s: &[i64] = &a; a[0] = 2;", "O007"),
        ("owner write under exclusive loan", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; a[0] = 2;", "O007"),
        ("exclusive loan after shared", "let mut a: [i64;1] = [1]; let s: &[i64] = &a; let t: &mut[i64] = &mut a;", "O007"),
        ("shared loan after exclusive", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; let t: &[i64] = &a;", "O007"),
        ("two exclusive loans", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; let t: &mut[i64] = &mut a;", "O007"),
        ("disjoint exclusive ranges still conflict", "let mut a: [i64;2] = [1,2]; let s: &mut[i64] = &mut a[0..1]; let t: &mut[i64] = &mut a[1..2];", "O007"),
        ("last use does not release lexical loan", "let mut a: [i64;1] = [1]; let s: &[i64] = &a; print(s[0]); a[0] = 2;", "O007"),
        ("owner shadow does not release original loan", "let mut a: [i64;1] = [1]; let s: &[i64] = &a; { let mut a: [i64;1] = [4]; a[0] = 5; } a[0] = 2;", "O007"),
        ("empty shared loan still prevents mutation", "let mut a: [i64;1] = [1]; let s: &[i64] = &a[0..0]; a[0] = 2;", "O007"),
        ("empty exclusive loan still prevents shared loan", "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a[0..0]; let t: &[i64] = &a;", "O007"),
        ("outer loan remains after inner loan ends", "let mut a: [i64;1] = [1]; let s: &[i64] = &a; { let t: &[i64] = &a; print(t[0]); } a[0] = 2;", "O007"),
        ("immutable owner element write", "let a: [i64;1] = [1]; a[0] = 2;", "O010"),
        ("immutable owner exclusive loan", "let a: [i64;1] = [1]; let s: &mut[i64] = &mut a;", "O010"),
        ("mutable binding cannot upgrade shared slice", "let mut a: [i64;1] = [1]; let mut s: &[i64] = &a; s[0] = 2;", "O010"),
        ("external owner moved within loop", "let a: [i64;1] = [1]; while true { let b: [i64;1] = a; break; }", "O011"),
        ("outer iteration owner moved in inner loop", "while false { let a: [i64;1] = [1]; while true { let b: [i64;1] = a; break; } }", "O011"),
        ("preloop owner moved in nested loop", "let a: [i64;1] = [1]; while false { while true { let b: [i64;1] = a; break; } }", "O011"),
        ("one continuing branch moves owner", "let a: [i64;1] = [1]; if true { let b: [i64;1] = a; } print(a[0]);", "O006"),
        ("returning branch does not hide moved continuing branch", "let a: [i64;1] = [1]; if false { return 4; } else { let b: [i64;1] = a; } print(a[0]);", "O006"),
    ];
    assert!(cases.len() >= 20);
    for (name, body, code) in cases {
        rejects(
            name,
            &format!("fn main() -> i64 {{ {body} return 0; }}"),
            code,
        );
    }
}

#[test]
fn aggregate_boundaries_and_unsupported_escapes_are_rejected() {
    let cases = [
        ("zero array type", "let a: [i64;0] = [];", "O001"),
        ("oversized array type", "let a: [i64;33] = [1];", "O001"),
        ("literal element count", "let a: [i64;2] = [1];", "T009"),
        ("literal element type", "let a: [i64;1] = [true];", "T009"),
        (
            "empty literal reaches checker",
            "let a: [i64;1] = [];",
            "T009",
        ),
        (
            "move length mismatch",
            "let a: [i64;1] = [1]; let b: [i64;2] = a;",
            "T009",
        ),
        (
            "slice cannot copy",
            "let a: [i64;1] = [1]; let s: &[i64] = &a; let t: &[i64] = s;",
            "O005",
        ),
        (
            "mutable slice cannot move",
            "let mut a: [i64;1] = [1]; let s: &mut[i64] = &mut a; let t: &mut[i64] = s;",
            "O005",
        ),
        (
            "slice cannot reborrow",
            "let a: [i64;1] = [1]; let s: &[i64] = &a; let t: &[i64] = &s;",
            "O005",
        ),
        (
            "slice cannot range reborrow",
            "let a: [i64;1] = [1]; let s: &[i64] = &a; let t: &[i64] = &s[0..1];",
            "O005",
        ),
        (
            "owner whole reassignment",
            "let mut a: [i64;1] = [1]; a = [2];",
            "O008",
        ),
        (
            "slice whole reassignment",
            "let a: [i64;1] = [1]; let mut s: &[i64] = &a; s = &a;",
            "O008",
        ),
        ("aggregate print", "let a: [i64;1] = [1]; print(a);", "O004"),
        (
            "aggregate equality",
            "let a: [i64;1] = [1]; print(a == a);",
            "O004",
        ),
        (
            "aggregate arithmetic",
            "let a: [i64;1] = [1]; print(a + 1);",
            "O004",
        ),
        (
            "aggregate scalar initializer",
            "let a: [i64;1] = [1]; let n: i64 = a;",
            "O004",
        ),
        ("scalar indexing", "let a: i64 = 1; print(a[0]);", "O009"),
        ("scalar length", "let a: i64 = 1; print(len(a));", "O009"),
        (
            "boolean index",
            "let a: [i64;1] = [1]; print(a[false]);",
            "T009",
        ),
        (
            "boolean range endpoint",
            "let a: [i64;1] = [1]; let s: &[i64] = &a[0..true];",
            "T009",
        ),
        (
            "boolean assigned element",
            "let mut a: [i64;1] = [1]; a[0] = false;",
            "T009",
        ),
        (
            "slice annotation cannot upgrade loan",
            "let mut a: [i64;1] = [1]; let s: &mut[i64] = &a;",
            "T009",
        ),
    ];
    for (name, body, code) in cases {
        rejects(
            name,
            &format!("fn main() -> i64 {{ {body} return 0; }}"),
            code,
        );
    }
    rejects(
        "aggregate scalar return",
        "fn main() -> i64 { let a: [i64;1] = [1]; return a; }",
        "O004",
    );
    rejects(
        "aggregate argument",
        "fn f(n: i64) -> i64 { return n; } fn main() -> i64 { let a: [i64;1] = [1]; return f(a); }",
        "O004",
    );
    rejects(
        "invalid array parameter length",
        "fn f(a: [i64;0]) -> i64 { return 0; } fn main() -> i64 { return 0; }",
        "O001",
    );
    rejects(
        "slice parameter",
        "fn f(s: &[i64]) -> i64 { return 0; } fn main() -> i64 { return 0; }",
        "O003",
    );
    rejects(
        "array function result length mismatch",
        "fn f() -> [i64;1] { return [1,2]; } fn main() -> i64 { return 0; }",
        "T009",
    );
    rejects(
        "slice function result",
        "fn f() -> &[i64] { let a: [i64;1] = [1]; return &a; } fn main() -> i64 { return 0; }",
        "O003",
    );
}
