//! Local nominal records: independent answers, rejected programs, and native
//! O0/O2/O3 parity. Native acceptance is deliberately limited to the hosted target.
mod support;
use axeon::diagnostic::line_col;
use axeon::interpreter::{self, TrapKind};
use axeon::project::{Project, ProjectError};
use std::fmt::Write;
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
            let path =
                std::env::temp_dir().join(format!("axeon-records-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create record fixture: {error}"),
            }
        }
        panic!("temporary directory budget exhausted");
    }
    fn write(&self, files: &[(&str, &str)]) {
        for (name, text) in files {
            fs::write(self.0.join(name), text).unwrap();
        }
    }
    fn load(&self) -> Result<Project, ProjectError> {
        Project::load(&self.0.join("main.ax"), None)
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn bounded(executable: &Path, seconds: &str) -> Command {
    support::bounded_with_kill(executable, seconds, "2s")
}
fn output(command: &mut Command) -> Output {
    let result = command.output().expect("record subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "record subprocess timed out"
    );
    result
}
enum Expected<'a> {
    Value(i64),
    Trap {
        kind: TrapKind,
        file: &'a str,
        marker: &'a str,
    },
}
fn verify(files: &[(&str, &str)], effects: &str, expected: Expected<'_>) -> axeon::ir::Program {
    let directory = Temp::new();
    directory.write(files);
    let project = directory.load().expect("load record fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent record effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent record result");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted
                .result
                .expect_err("record scalar operation traps");
            assert_eq!(trap.kind, kind);
            assert_eq!(
                (trap.span.start, trap.span.end),
                (start, start + marker.len())
            );
            assert_eq!(
                project.sources.get(trap.span.source).unwrap().logical_path,
                file
            );
            let (line, column) = line_col(source, start);
            let location = if project.is_module_project() {
                format!("{file}:{line}:{column}")
            } else {
                format!("{line}:{column}")
            };
            (
                70,
                effects.to_owned(),
                format!("trap[{}] at {location}\n", kind.name()),
            )
        }
    };
    for opt in ["0", "2", "3"] {
        let executable = directory.0.join(format!("program-O{opt}"));
        let compiled = output(
            bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .arg("build")
                .arg(directory.0.join("main.ax"))
                .args(["--opt", opt, "-o"])
                .arg(&executable),
        );
        assert!(
            compiled.status.success(),
            "record build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "record exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "record stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "record stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid record program must not expose IR");
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}\n{source}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
    }
}
const CELL: &str = "record Cell { value: i64 }";
fn rejects_body(body: &str, code: &str) {
    rejects(&format!("{CELL} fn main()->i64{{{body}}}"), code);
}

#[test]
fn fields_moves_mutability_and_written_initializer_order() {
    verify_single(
        r#"
record Metrics { count: i64, mean: f64, ready: bool, }
fn mark(n:i64)->i64 { print(n); return n; }
fn number()->f64 { print(2); return 1.5; }
fn flag()->bool { print(3); return true; }
fn main()->i64 {
 let a:Metrics=Metrics{ready:flag(),count:mark(1),mean:number(),};
 print(a.ready); print(a.mean);
 let mut b:Metrics=a; b.count=mark(40)+2; b.mean=b.mean+0.5; b.ready=false;
 print(b.mean); print(b.ready);
 let c:Metrics=b; return c.count;
}"#,
        "3\n1\n2\ntrue\nf64:0x3ff8000000000000\n40\nf64:0x4000000000000000\nfalse\n",
        42,
    );
}

#[test]
fn forward_declarations_contextual_names_and_shadowing() {
    verify_single(
        r#"
fn record(record:i64)->i64 { return record+1; }
fn main()->i64 {
 let Cell:bool=true;
 if Cell { print(record(6)); }
 let value:Cell=Cell{value:8};
 { let mut value:Cell=value; value.value=value.value+1; print(value.value); }
 let record:Cell=Cell{value:10};
 { let record:Cell=Cell{value:record.value+1}; print(record.value); }
 return record.value;
}
record Cell { value:i64 }
"#,
        "7\n9\n11\n",
        10,
    );
    // A newly contextual spelling remains legal throughout legacy programs.
    verify_single("fn record(record:bool)->bool{return record;} fn main()->i64{let record:bool=record(true);if record{return 7;}return 8;}", "", 7);
}

#[test]
fn records_preserve_binary64_and_integer_boundaries() {
    verify_single(r#"
record Numeric { enabled:bool, real:f64, exact:i64, tail:bool }
fn main()->i64 {
 let mut a:Numeric=Numeric{tail:false,real:-0.0,enabled:true,exact:9007199254740993};
 print(a.real); print(a.exact);
 a.real=f64_from_bits(1); print(a.real);
 a.real=1.0/0.0; print(a.real);
 a.real=0.0/0.0; print(a.real); print(a.real==a.real);
 a.real=(1.0+1.1102230246251565e-16)-1.0; print(a.real);
 a.exact=-9223372036854775808;
 let b:Numeric=a; print(b.exact); print(b.enabled); print(b.tail);
 return f64_bits(b.real);
}"#, "f64:0x8000000000000000\n9007199254740993\nf64:0x0000000000000001\nf64:0x7ff0000000000000\nf64:0x7ff8000000000000\nfalse\nf64:0x0000000000000000\n-9223372036854775808\ntrue\nfalse\n", 0);
}

#[test]
fn control_flow_recursion_and_iteration_local_moves() {
    verify_single(
        r#"
record Cell { value:i64 }
fn sum(n:i64)->i64 {
 let local:Cell=Cell{value:n};
 if n==0 { let moved:Cell=local; return moved.value; }
 return local.value+sum(n-1);
}
fn choose(flag:bool)->i64 {
 let local:Cell=Cell{value:8};
 if flag { let moved:Cell=local; return moved.value+1; }
 return local.value;
}
fn main()->i64 {
 let mut total:Cell=Cell{value:0}; let mut i:i64=0;
 while i<4 {
   let iteration:Cell=Cell{value:i}; let mut moved:Cell=iteration;
   moved.value=moved.value+1; total.value=total.value+moved.value;
   i=i+1;
   if i==2 { continue; }
   print(total.value);
 }
 print(choose(true)); print(choose(false));
 return total.value+sum(6);
}"#,
        "1\n6\n10\n9\n8\n",
        31,
    );
}

#[test]
fn private_records_with_same_name_in_different_modules_have_distinct_identity() {
    let program = verify(&[
        ("main.ax", "import left; import right; record Item { n:i64 } fn main()->i64{let x:Item=Item{n:3};return x.n+left::value()+right::value();}"),
        ("left.ax", "record Item { flag:bool, n:i64 } export fn value()->i64{let x:Item=Item{n:17,flag:true};if x.flag{return x.n;}return 0;}"),
        ("right.ax", "record Item { n:f64 } export fn value()->i64{let x:Item=Item{n:22.0};if x.n==22.0{return 22;}return 0;}"),
    ], "", Expected::Value(42));
    assert_eq!(program.records.len(), 3);
    let identities: std::collections::HashSet<_> =
        program.records.iter().map(|record| record.id).collect();
    assert_eq!(identities.len(), 3);
}

#[test]
fn record_captures_survive_tensor_failure_and_local_moves() {
    verify_single(
        r#"
record Cell { value:i64 }
fn main()->i64 {
 let mut captured:Cell=Cell{value:1};
 tensor_scope(0) {
   captured.value=2;
   let local:Cell=Cell{value:7}; let moved:Cell=local; print(moved.value);
   let failed:tensor=try tensor_zeros([1]);
 } catch(error:tensor_error) { print(captured.value); }
 return captured.value;
}"#,
        "7\n2\n",
        2,
    );
}

#[test]
fn initializer_and_assignment_traps_preserve_effect_order_and_locations() {
    let source = r#"record R { a:i64, b:i64, c:i64 }
fn mark(n:i64)->i64{print(n);return n;}
fn main()->i64{let r:R=R{c:mark(3),a:9223372036854775807 + 1,b:mark(2)};return r.a;}"#;
    verify(
        &[("main.ax", source)],
        "3\n",
        Expected::Trap {
            kind: TrapKind::Overflow,
            file: "main.ax",
            marker: "9223372036854775807 + 1",
        },
    );
    verify(&[
        ("main.ax", "import fail; fn main()->i64{return fail::run();}"),
        ("fail.ax", "record R { x:i64 } fn mark()->i64{print(4);return 1;} export fn run()->i64{let mut r:R=R{x:5};r.x=mark() / 0;return r.x;}"),
    ], "4\n", Expected::Trap { kind:TrapKind::DivisionByZero, file:"fail.ax", marker:"mark() / 0" });
}

#[test]
fn record_declarations_construction_and_types_reject_invalid_programs() {
    for (source, code) in [
        ("record Empty {} fn main()->i64{return 0;}", "R002"),
        (
            "record R{x:i64} record R{x:i64} fn main()->i64{return 0;}",
            "R003",
        ),
        ("record R{x:i64,x:f64} fn main()->i64{return 0;}", "R004"),
        ("record R{x:tensor_error} fn main()->i64{return 0;}", "R005"),
        ("record R{x:&[f64]} fn main()->i64{return 0;}", "R005"),
        ("record R{x:tensor} fn main()->i64{return 0;}", "R005"),
        ("record R{x:R} fn main()->i64{return 0;}", "A001"),
        (
            "record R{x:S} record S{x:R} fn main()->i64{return 0;}",
            "A001",
        ),
        (
            "record R{x:i64} export fn f(r:R)->i64{return r.x;} fn main()->i64{return 0;}",
            "I002",
        ),
        (
            "record R{x:i64} export fn f()->R{let r:R=R{x:1};return r;} fn main()->i64{return 0;}",
            "I002",
        ),
        (
            "record Private{x:i64} export record R{x:Private} fn main()->i64{return 0;}",
            "I002",
        ),
        (
            "record R{x i64} fn main()->i64{return 0;}",
            "E_PARSE_EXPECTED",
        ),
        (
            "record R{x:i64 fn main()->i64{return 0;}",
            "E_PARSE_EXPECTED",
        ),
        (
            "record R{x:i64} fn main()->i64{let a:R=R{x};return 0;}",
            "E_PARSE_EXPECTED",
        ),
        (
            "record R{x:i64} fn main()->i64{let a:R=R{x:1};return a.x.y;}",
            "R010",
        ),
    ] {
        rejects(source, code);
    }
    for (body, code) in [
        ("let a:Cell=Other{value:1};return 0;", "R006"),
        ("let a:Cell=Cell{other:1};return 0;", "R007"),
        ("let a:Cell=Cell{value:1,value:2};return 0;", "R008"),
        ("let a:Cell=Cell{};return 0;", "R009"),
        ("let a:Cell=Cell{value:1};return a.other;", "R007"),
        ("let a:i64=1;return a.value;", "R010"),
        ("let a:Cell=Cell{value:1.0};return 0;", "T009"),
        (
            "let mut a:Cell=Cell{value:1};a.value=true;return 0;",
            "T009",
        ),
        ("let a:Cell=Cell{value:1};a.value=2;return 0;", "O010"),
        ("let a:Cell=Cell{value:1};print(a);return 0;", "O004"),
        ("let a:i64=Cell{value:1};return 0;", "O004"),
        ("let a:Cell=1;return 0;", "O005"),
        (
            "let mut a:Cell=Cell{value:1};a=Cell{value:2};return 0;",
            "O008",
        ),
        ("let a:Cell=Cell{value:1};let b:&[i64]=&a;return 0;", "O005"),
        ("let a:Cell=Cell{value:1};return len(a);", "O009"),
        ("let a:Cell=Cell{value:1};return a[0];", "O009"),
        (
            "let a:Cell=Cell{value:1};a.value;return 0;",
            "E_PARSE_EXPECTED",
        ),
        (
            "let a:Cell=Cell{value:1};(a.value);return 0;",
            "E_PARSE_STATEMENT",
        ),
    ] {
        rejects_body(body, code);
    }
    for initializer in ["Other{value:1}", "source"] {
        rejects(&format!("record Cell{{value:i64}} record Other{{value:i64}} fn main()->i64{{let source:Other=Other{{value:1}};let target:Cell={initializer};return 0;}}"), "T009");
    }
}

#[test]
fn affine_record_owners_reject_invalid_moves_on_all_paths() {
    for (body, code) in [
        ("let a:Cell=Cell{value:1};let b:Cell=a;return a.value;", "O006"),
        ("let mut a:Cell=Cell{value:1};let b:Cell=a;a.value=2;return 0;", "O006"),
        ("let a:Cell=Cell{value:1};let b:Cell=a;let c:Cell=a;return 0;", "O006"),
        ("let a:Cell=Cell{value:1};if true{let b:Cell=a;}return a.value;", "O006"),
        ("let a:Cell=Cell{value:1};while true{let b:Cell=a;break;}return 0;", "O011"),
        ("let a:Cell=Cell{value:1};while false{let b:Cell=a;}return 0;", "O011"),
        ("let a:Cell=Cell{value:1};tensor_scope(0){let b:Cell=a;}catch(e:tensor_error){}return a.value;", "N007"),
        ("let a:Cell=Cell{value:1};{let b:Cell=a;}return a.value;", "O006"),
    ] { rejects_body(body, code); }
}

fn fields(count: usize) -> String {
    (0..count)
        .map(|index| format!("f{index}:i64"))
        .collect::<Vec<_>>()
        .join(",")
}
fn values(count: usize) -> String {
    (0..count)
        .map(|index| format!("f{index}:{index}"))
        .collect::<Vec<_>>()
        .join(",")
}

#[test]
fn declaration_field_storage_and_expression_limits_are_enforced() {
    rejects(
        &format!("record Wide{{{}}} fn main()->i64{{return 0;}}", fields(33)),
        "R002",
    );
    let mut records = String::new();
    for index in 0..256 {
        write!(records, "record R{index}{{x:i64}} ").unwrap();
    }
    verify_single(
        &format!("{records} fn main()->i64{{let r:R255=R255{{x:7}};return r.x;}}"),
        "",
        7,
    );
    records.push_str("record Excess{x:i64} fn main()->i64{return 0;}");
    rejects(&records, "R001");

    let mut source = format!(
        "record Wide{{{}}} fn main()->i64{{let a0:Wide=Wide{{{}}};",
        fields(28),
        values(28)
    );
    for index in 1..8 {
        source.push_str(&format!("let a{index}:Wide=a{};", index - 1));
    }
    let array = (0..32)
        .map(|index| index.to_string())
        .collect::<Vec<_>>()
        .join(",");
    source.push_str(&format!("let array:[i64;32]=[{array}];"));
    verify_single(&format!("{source}return a7.f27+array[31];}}"), "", 58);
    rejects(
        &format!("{source}let extra:[i64;1]=[0];return 0;}}"),
        "O002",
    );
    let full = format!(
        "record Wide{{{}}} fn main()->i64{{let a:Wide=Wide{{{}}};return a.f31;}}",
        fields(32),
        values(32)
    );
    verify_single(&full, "", 31);

    // The outer constructor itself is shallow; each field's binary tree still
    // participates in the same function-wide node and nesting limits.
    let expr = std::iter::repeat("1")
        .take(40)
        .collect::<Vec<_>>()
        .join("+");
    let initializers = (0..32)
        .map(|index| format!("f{index}:{expr}"))
        .collect::<Vec<_>>()
        .join(",");
    rejects(
        &format!(
            "record Wide{{{}}} fn main()->i64{{let a:Wide=Wide{{{initializers}}};return 0;}}",
            fields(32)
        ),
        "T016",
    );
    let nested = format!("{}1{}", "(".repeat(80), ")".repeat(80));
    rejects_body(
        &format!("let a:Cell=Cell{{value:{nested}}};return 0;"),
        "E_NESTING_LIMIT",
    );
}

#[test]
fn module_privacy_and_project_wide_declaration_limit() {
    let directory = Temp::new();
    directory.write(&[
        (
            "main.ax",
            "import helper; fn main()->i64{let a:Secret=Secret{x:1};return 0;}",
        ),
        (
            "helper.ax",
            "record Secret{x:i64} export fn value()->i64{return 0;}",
        ),
    ]);
    let ProjectError::Diagnostics { diagnostics, .. } = directory.load().unwrap_err() else {
        panic!("expected private type diagnostic")
    };
    assert!(diagnostics.iter().any(|error| error.code == "E_PARSE_TYPE"));
    let mut declarations = String::new();
    for index in 0..129 {
        write!(declarations, "record R{index}{{x:i64}} ").unwrap();
    }
    directory.write(&[
        (
            "main.ax",
            &format!("import helper; {declarations} fn main()->i64{{return 0;}}"),
        ),
        (
            "helper.ax",
            &format!("{declarations} export fn value()->i64{{return 0;}}"),
        ),
    ]);
    let errors = directory.load().unwrap().check().unwrap_err();
    assert!(errors.iter().any(|error| error.code == "R001"));
}
