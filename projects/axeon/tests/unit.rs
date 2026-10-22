//! Unit type and values: independent answers, rejected programs, and native
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
            let path = std::env::temp_dir().join(format!("axeon-unit-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create unit fixture: {error}"),
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
    let result = command.output().expect("unit subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "unit subprocess timed out"
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
    let project = directory.load().expect("load unit fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent unit effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent unit result");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted.result.expect_err("unit scalar operation traps");
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
            "unit build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "unit exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "unit stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "unit stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid unit program must not expose IR");
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}\n{source}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
    }
}

fn rejects_operands(expression: &str, result_type: &str, expected: &[(&str, &str)]) {
    // A valid operator result must fit this binding. Otherwise an initializer
    // mismatch could hide a regression that accepted the invalid operands.
    let prefix = format!("fn main()->i64{{let result:{result_type}=");
    let source = format!("{prefix}{expression};return 0;}}");
    let errors = axeon::compile(&source).expect_err("invalid operands must not expose IR");
    assert_eq!(errors.len(), expected.len(), "{expression}: {errors:#?}");
    for (context, operand) in expected {
        let offset = if matches!(*context, "right operand" | "equality operand") {
            expression.rfind(*operand)
        } else {
            expression.find(*operand)
        }
        .expect("expected operand occurs in the fixture");
        let start = prefix.len() + offset;
        assert!(
            errors.iter().any(|error| {
                error.code == "T009"
                    && error.message.starts_with(&format!("{context} requires "))
                    && (error.span.start, error.span.end) == (start, start + operand.len())
            }),
            "missing {context} diagnostic at {operand:?} in {expression}: {errors:#?}"
        );
    }
}

fn rejects_project(files: &[(&str, &str)], code: &str) {
    let directory = Temp::new();
    directory.write(files);
    let errors = match directory.load() {
        Ok(project) => project.check().expect_err("invalid module interface"),
        Err(ProjectError::Diagnostics { diagnostics, .. }) => diagnostics,
        Err(error) => panic!("unexpected driver error: {error:?}"),
    };
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end);
    }
}

#[test]
fn unit_is_copyable_in_bindings_parameters_results_and_scalar_fields() {
    verify_single(
        r#"
record Token { done: () }
fn copy(value: ()) -> () { return value; }
fn choose(flag: bool, value: ()) -> () { if flag { return copy(value); } return (); }
fn ready(value: ()) -> bool { return value == (); }
fn main() -> i64 {
    let unit: () = (());
    let mut changed: () = copy(unit);
    changed = choose(false, unit);
    let mut token: Token = Token { done: changed };
    token.done = choose(true, token.done);
    let repeated: () = token.done;
    let mut count: i64 = 0;
    while ready(unit) && count < 3 { copy(unit); count = count + 1; }
    print(unit == changed);
    print(unit != repeated);
    print(token.done == ());
    return count;
}
"#,
        "true\nfalse\ntrue\n",
        3,
    );
}

#[test]
fn unit_crosses_modules_and_nested_owned_interfaces_without_numeric_changes() {
    verify(
        &[
            ("types.ax", "export record Done { marker: (), value: f64 } export union Outcome { Good { done: Done, marker: () }, Missing {} }"),
            ("producer.ax", "import types; export fn done(value: ()) -> () { return value; } export fn produce(value: ()) -> types::Outcome { return types::Outcome::Good { marker: done(value), done: types::Done { value: -0.0, marker: value } }; }"),
            ("consumer.ax", "import types; export fn consume(value: types::Outcome, marker: ()) -> () { match value { types::Outcome::Good { marker: repeated, done: done } => { print(marker == repeated); print(done.marker == ()); print(done.value); return done.marker; }, types::Outcome::Missing {} => { return marker; }, } }"),
            ("main.ax", "import producer; import consumer; import types; fn main() -> i64 { let value: types::Outcome = producer::produce(()); let result: () = consumer::consume(value, producer::done(())); print(result == ()); return 42; }"),
        ],
        "true\ntrue\nf64:0x8000000000000000\ntrue\n",
        Expected::Value(42),
    );
}

#[test]
fn unit_argument_constructor_and_equality_effects_remain_left_to_right() {
    verify_single(
        r#"
record Pair { first: (), second: () }
fn mark(n: i64) -> () { print(n); return (); }
fn join(a: (), b: (), c: ()) -> () { print(4); return a; }
fn main() -> i64 {
    join(mark(1), mark(2), mark(3));
    let mut pair: Pair = Pair { second: mark(5), first: mark(6) };
    pair.first = mark(7);
    print(mark(8) == mark(9));
    print(mark(10) != mark(11));
    print(false && mark(99) == ());
    print(true || mark(98) != ());
    return 0;
}
"#,
        "1\n2\n3\n4\n5\n6\n7\n8\n9\ntrue\n10\n11\nfalse\nfalse\ntrue\n",
        0,
    );
}

#[test]
fn unit_results_do_not_eliminate_traps_or_evaluate_later_arguments() {
    for (expression, effects) in [
        ("join(fail(), mark(99));", "7\n"),
        ("join(mark(1), fail());", "1\n7\n"),
        ("print(fail() == mark(99));", "7\n"),
        ("print(mark(1) != fail());", "1\n7\n"),
        ("let p: Pair = Pair { b: mark(1), a: fail() };", "1\n7\n"),
    ] {
        let source = format!(
            "record Pair{{a:(),b:()}}fn mark(n:i64)->(){{print(n);return ();}}fn fail()->(){{print(7);let n:i64=1 / 0;return ();}}fn join(a:(),b:())->(){{print(88);return ();}}fn main()->i64{{{expression}return 0;}}"
        );
        verify(
            &[("main.ax", &source)],
            effects,
            Expected::Trap {
                kind: TrapKind::DivisionByZero,
                file: "main.ax",
                marker: "1 / 0",
            },
        );
    }
}

#[test]
fn unit_return_recovery_and_fatal_tensor_cleanup_preserve_effects() {
    verify_single(
        r#"
fn forward(value: ()) -> () { return value; }
fn recover() -> () {
    let mut captured: () = ();
    tensor_scope(8) {
        let tensor_value: tensor = try tensor_zeros([1]);
        captured = forward(captured);
        let fails: tensor = try tensor_zeros([1]);
    } catch (error: tensor_error) {
        print(tensor_error_code(error));
        return forward(captured);
    }
    return ();
}
fn main() -> i64 {
    let value: () = recover();
    print(value == recover());
    tensor_scope(8) {
        let tensor_value: tensor = try tensor_zeros([1]);
        print(tensor_len(tensor_value));
    } catch (error: tensor_error) { print(999); }
    return 0;
}
"#,
        "9\n9\ntrue\n1\n",
        0,
    );
    let source = "fn fail(value:())->(){print(7);let result:i64=1 / 0;return value;}fn main()->i64{tensor_scope(8){let t:tensor=try tensor_zeros([1]);fail(());}catch(e:tensor_error){print(999);}return 0;}";
    verify(
        &[("main.ax", source)],
        "7\n",
        Expected::Trap {
            kind: TrapKind::DivisionByZero,
            file: "main.ax",
            marker: "1 / 0",
        },
    );
}

#[test]
fn unit_fields_copy_but_enclosing_owners_keep_partial_move_guards() {
    verify_single(
        r#"
record Data { values: [i64; 1], done: () }
fn main() -> i64 {
    let data: Data = Data { done: (), values: [42] };
    let before: () = data.done;
    let values: [i64; 1] = data.values;
    let after: () = data.done;
    print(before == after);
    return values[0];
}
"#,
        "true\n",
        42,
    );
    rejects("record R{done:()}fn take(value:R)->(){return value.done;}fn main()->i64{let mut r:R=R{done:()};r.done=take(r);return 0;}", "O006");
    rejects("record R{done:()}fn main()->i64{let r:R=R{done:()};let moved:R=r;let copied:()=r.done;return 0;}", "O006");
    rejects(
        "record R{done:()}fn main()->i64{let r:R=R{done:()};r.done=();return 0;}",
        "O010",
    );
    rejects("fn main()->i64{let value:()=();value=();return 0;}", "T006");
    rejects("record R{done:()}fn take(value:R)->(){return value.done;}fn main()->i64{let value:R=R{done:()};while take(value)==(){}return 0;}", "O011");
    rejects("record R{done:()}fn take(value:R)->(){return value.done;}fn main()->i64{let value:R=R{done:()};tensor_scope(0){take(value);}catch(error:tensor_error){}return 0;}", "N007");
}

#[test]
fn unit_is_not_numeric_boolean_printable_or_implicitly_convertible() {
    for (expression, result_type) in [
        ("() + ()", "i64"),
        ("() - ()", "i64"),
        ("() * ()", "i64"),
        ("() / ()", "i64"),
        ("() % ()", "i64"),
        ("() < ()", "bool"),
        ("() <= ()", "bool"),
        ("() > ()", "bool"),
        ("() >= ()", "bool"),
        ("() & ()", "i64"),
        ("() | ()", "i64"),
        ("() ^ ()", "i64"),
        ("() << ()", "i64"),
        ("() >> ()", "i64"),
        ("() && ()", "bool"),
        ("() || ()", "bool"),
    ] {
        rejects_operands(
            expression,
            result_type,
            &[("left operand", "()"), ("right operand", "()")],
        );
    }
    for (expression, result_type, context, operand) in [
        ("!()", "bool", "unary operand", "()"),
        ("~()", "i64", "unary operand", "()"),
        ("-()", "i64", "unary operand", "()"),
        ("() == 0", "bool", "equality operand", "0"),
        ("0 == ()", "bool", "equality operand", "()"),
        ("() != false", "bool", "equality operand", "false"),
        ("false != ()", "bool", "equality operand", "()"),
        ("() == 0.0", "bool", "equality operand", "0.0"),
        ("0.0 == ()", "bool", "equality operand", "()"),
        ("f64_from_i64(())", "f64", "builtin argument", "()"),
        ("f64_bits(())", "i64", "builtin argument", "()"),
        ("f64_from_bits(())", "f64", "builtin argument", "()"),
    ] {
        rejects_operands(expression, result_type, &[(context, operand)]);
    }
    for body in [
        "let value:i64=();",
        "let value:f64=();",
        "let value:bool=();",
        "let value:()=0;",
        "let value:()=0.0;",
        "let value:()=false;",
        "if () {}",
        "while () {}",
        "let values:[i64;1]=[()];",
        "let values:[f64;1]=[()];",
    ] {
        rejects(&format!("fn main()->i64{{{body}return 0;}}"), "T009");
    }
    let source = "fn produce()->(){return ();}fn main()->i64{print(produce());return 0;}";
    let errors = axeon::compile(source).unwrap_err();
    let print = errors.iter().find(|error| error.code == "T020").unwrap();
    assert_eq!(&source[print.span.start..print.span.end], "produce()");
    assert_eq!(
        print.message,
        "print requires i64, f64 or bool; unit () has no print representation"
    );
    rejects("fn main()->i64{print(());return 0;}", "T020");
    rejects(
        "fn take(value:())->(){return value;}fn main()->i64{take(0);return 0;}",
        "T009",
    );
    rejects(
        "fn take(value:i64)->i64{return value;}fn main()->i64{return take(());}",
        "T009",
    );
}

#[test]
fn unit_does_not_add_tuples_implicit_returns_or_new_entry_signatures() {
    for source in [
        "fn value()->(i64){return 0;}fn main()->i64{return 0;}",
        "fn value()->(()){return ();}fn main()->i64{return 0;}",
        "fn main()->i64{let value:()=((),);return 0;}",
        "fn main()->i64{let value:()=((),());return 0;}",
        "fn main()->i64{let values:[();1]=[()];return 0;}",
        "fn main()->i64{let values:&[()]=&data;return 0;}",
    ] {
        assert!(
            axeon::compile(source).is_err(),
            "unsupported grammar: {source}"
        );
    }
    rejects(
        "fn value()->(){return;}fn main()->i64{return 0;}",
        "E_PARSE_EXPRESSION",
    );
    rejects("fn value()->(){}fn main()->i64{return 0;}", "T010");
    rejects(
        "fn value(flag:bool)->(){if flag{return ();}}fn main()->i64{return 0;}",
        "T010",
    );
    rejects("fn main()->i64{();return 0;}", "E_PARSE_STATEMENT");
    rejects("fn main()->(){return ();}", "T003");
    rejects("fn main(value:())->i64{return 0;}", "T003");
    rejects("fn main()->i64{return ();}", "T009");
    rejects_project(
        &[
            (
                "main.ax",
                "import hidden; fn main()->i64{hidden::done();return 0;}",
            ),
            ("hidden.ax", "fn done()->(){return ();}"),
        ],
        "T007",
    );
}

#[test]
fn unit_spelling_preserves_existing_identifiers_and_nominal_names() {
    verify(
        &[
            ("unit.ax", "export record unit { unit: i64, done: () } export fn unit(unit: i64) -> i64 { return unit; } export fn empty() -> () { return (); }"),
            ("main.ax", "import unit; record Local { unit: () } fn main()->i64{let unit:unit::unit=unit::unit{unit:unit::unit(42),done:unit::empty()};let local:Local=Local{unit:unit.done};print(local.unit==());return unit.unit;}"),
        ],
        "true\n",
        Expected::Value(42),
    );
}

#[test]
fn unit_fields_keep_positive_layout_sizes_and_previous_maximum_frames() {
    let fields = (0..32)
        .map(|n| format!("f{n}:()"))
        .collect::<Vec<_>>()
        .join(",");
    let values = (0..32)
        .map(|n| format!("f{n}:()"))
        .collect::<Vec<_>>()
        .join(",");
    let mut source = format!("record R{{{fields}}}fn main()->i64{{");
    for n in 0..8 {
        write!(source, "let r{n}:R=R{{{values}}};").unwrap();
    }
    for n in 0..248 {
        write!(source, "let u{n}:()=();").unwrap();
    }
    source.push_str("if r0.f0==r7.f31 && u0==u247{return 42;}return 0;}");
    let program = verify(&[("main.ax", &source)], "", Expected::Value(42));
    let layouts = axeon::ast::InlineLayouts::build(&program.records, &program.unions).unwrap();
    assert_eq!(layouts.slots(axeon::ast::Type::Unit), 1);
    assert_eq!(
        layouts.slots(axeon::ast::Type::Record(program.records[0].id)),
        32
    );
    rejects(
        &source.replace("if r0.f0", "let extra:()=();if r0.f0"),
        "T015",
    );
    let mut too_wide = format!("record R{{{fields}}}fn main()->i64{{");
    for n in 0..9 {
        write!(too_wide, "let r{n}:R=R{{{values}}};").unwrap();
    }
    too_wide.push_str("return 0;}");
    rejects(&too_wide, "O002");
    rejects(&format!("record R{{{fields}}}record S{{a:R,b:R,c:R,d:R,e:R,f:R,g:R,h:R,extra:()}}fn main()->i64{{return 0;}}"), "A003");
    let mut old = format!(
        "record R{{{}}}fn main()->i64{{",
        fields.replace("()", "i64")
    );
    for n in 0..8 {
        write!(old, "let r{n}:R=R{{{}}};", values.replace("()", "42")).unwrap();
    }
    old.push_str("return r7.f31;}");
    verify_single(&old, "", 42);
}

#[test]
fn unit_parameters_recursion_and_parser_limits_remain_bounded() {
    let params = (0..32)
        .map(|n| format!("p{n}:()"))
        .collect::<Vec<_>>()
        .join(",");
    let args = ["()"; 32].join(",");
    let source = format!(
        "fn many({params})->(){{return p31;}}fn main()->i64{{print(many({args})==());return 0;}}"
    );
    verify_single(&source, "true\n", 0);
    rejects(&source.replace("p31:()", "p31:(),p32:()"), "T014");
    let function =
        "fn recurse(n:i64,value:())->(){if n==0{return value;}return recurse(n-1,value);}";
    verify_single(
        &format!("{function}fn main()->i64{{print(recurse(62,())==());return 0;}}"),
        "true\n",
        0,
    );
    let failure = format!("{function}fn main()->i64{{recurse(63,());return 0;}}");
    verify(
        &[("main.ax", &failure)],
        "",
        Expected::Trap {
            kind: TrapKind::CallDepth,
            file: "main.ax",
            marker: function,
        },
    );
    let too_deep = format!(
        "fn main()->i64{{let value:()={}(){};return 0;}}",
        "(".repeat(70),
        ")".repeat(70)
    );
    rejects(&too_deep, "E_NESTING_LIMIT");
    let source = format!(
        "fn main()->i64{{{}let value:()=();{}return 0;}}",
        "{".repeat(63),
        "}".repeat(63)
    );
    rejects(&source, "E_NESTING_LIMIT");
}

#[test]
fn unit_slots_preserve_adjacent_float_integer_and_bool_payload_bits() {
    verify_single(
        r#"
record Numeric { marker: (), wide: i64, tiny: f64, minus_zero: f64, flag: bool }
union Value { Some { marker: (), numeric: Numeric, nan: f64 }, None {} }
fn forward(value: Value) -> Value { return value; }
fn main() -> i64 {
    let original: Value = Value::Some {
        nan: f64_from_bits(9221120237041090561),
        numeric: Numeric { flag: true, marker: (), wide: 9007199254740993, tiny: 5e-324, minus_zero: -0.0 },
        marker: (),
    };
    let moved: Value = forward(original);
    match moved {
        Value::Some { numeric: numeric, nan: nan, marker: marker } => {
            print(numeric.tiny);
            print(numeric.minus_zero);
            print(nan);
            print(numeric.flag);
            print(numeric.marker == marker);
            return numeric.wide;
        },
        Value::None {} => { return 0; },
    }
}
"#,
        "f64:0x0000000000000001\nf64:0x8000000000000000\nf64:0x7ff8000000000000\ntrue\ntrue\n",
        9007199254740993,
    );
}
