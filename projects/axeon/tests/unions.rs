//! Local nominal unions: independent answers, rejected programs, and native
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
                std::env::temp_dir().join(format!("axeon-unions-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create union fixture: {error}"),
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
    let result = command.output().expect("union subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "union subprocess timed out"
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
    let project = directory.load().expect("load union fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent union effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent union result");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted
                .result
                .expect_err("union scalar operation traps");
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
        let executable = support::executable_path(directory.0.join(format!("program-O{opt}")));
        let compiled = output(
            bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .arg("build")
                .arg(directory.0.join("main.ax"))
                .args(["--opt", opt, "-o"])
                .arg(&executable),
        );
        assert!(
            compiled.status.success(),
            "union build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "union exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "union stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "union stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid union program must not expose IR");
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}\n{source}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
    }
}

const CHOICE: &str = "union Choice { Some { value:i64 }, None {} }";
fn rejects_body(body: &str, code: &str) {
    rejects(&format!("{CHOICE} fn main()->i64{{{body}}}"), code);
}

#[test]
fn constructors_private_returns_forwarding_and_exhaustive_matches() {
    verify_single(
        r#"
union Reading { Value { number:f64, count:i64, valid:bool }, Missing {}, Error { code:i64 } }
fn produce(which:i64)->Reading {
 if which==0 { return Reading::Missing{}; }
 if which<0 { let failed:Reading=Reading::Error{code:which}; return failed; }
 return Reading::Value{valid:true,count:which,number:1.5};
}
fn forward(which:i64)->Reading { return produce(which); }
fn consume(which:i64)->i64 {
 let value:Reading=forward(which);
 match value {
  Reading::Error{code:error}=>{return error;},
  Reading::Missing{}=>{return 0;},
  Reading::Value{count:count,valid:valid,number:number}=>{
    print(number); print(valid); return count;
  },
 }
}
fn main()->i64{print(consume(-7));print(consume(0));return consume(42);}
"#,
        "-7\n0\nf64:0x3ff8000000000000\ntrue\n",
        42,
    );
}

#[test]
fn field_and_argument_effect_order_is_written_order() {
    verify_single(
        r#"
union U { V { first:i64, second:f64, third:bool } }
fn mark(n:i64)->i64{print(n);return n;}
fn real()->f64{print(2);return 2.5;}
fn flag()->bool{print(3);return true;}
fn make(a:i64,b:i64)->U{return U::V{third:flag(),first:a+b,second:real()};}
fn main()->i64{
 let a:U=make(mark(10),mark(20)); let b:U=a;
 match b { U::V{second:r,third:ok,first:n}=>{print(r);print(ok);return n;}, }
}"#,
        "10\n20\n3\n2\nf64:0x4004000000000000\ntrue\n",
        30,
    );
}

#[test]
fn forward_types_contextual_names_and_pattern_shadowing_remain_lexical() {
    verify_single(
        r#"
fn union(match:i64)->i64{return match+1;}
fn match(union:i64)->Choice{return Choice::Some{value:union};}
fn main()->i64{
 let union:i64=union(7); let match:i64=union;
 let source:Choice=match(match);
 match source {
  Choice::Some{value:source}=>{return source;},
  Choice::None{}=>{return 0;},
 }
}
union Choice{Some{value:i64},None{}}
"#,
        "",
        8,
    );
    verify_single("fn union(match:bool)->bool{return match;} fn match(union:i64)->i64{return union;} fn main()->i64{let mut match:i64=match(3);match=4;let union:bool=union(true);if union{return match;}return 0;}", "", 4);
}

#[test]
fn matches_join_only_continuing_arms_and_iteration_local_owners() {
    verify_single(
        r#"
union Choice{Some{value:i64},None{}}
record Cell{value:i64}
fn choose(flag:bool)->Choice{if flag{return Choice::Some{value:4};}return Choice::None{};}
fn continuing(flag:bool)->i64{
 let original:Cell=Cell{value:8}; let choice:Choice=choose(flag);
 match choice {
  Choice::Some{value:n}=>{let moved:Cell=original;return moved.value+n;},
  Choice::None{}=>{print(0);},
 }
 return original.value;
}
fn main()->i64{
 let mut i:i64=0; let mut total:i64=0;
 while i<5 {
  let item:Choice=choose(i==3); i=i+1;
  match item {
   Choice::Some{value:n}=>{total=total+n;break;},
   Choice::None{}=>{total=total+1;continue;},
  }
 }
 print(total); print(continuing(true)); return continuing(false);
}"#,
        "7\n12\n0\n",
        8,
    );
}

#[test]
fn recursive_producers_use_caller_results_and_independent_frames() {
    let program = verify(
        &[(
            "main.ax",
            r#"
union Sum{Done{value:i64},Missing{}}
fn sum(n:i64)->Sum{
 if n==0{return Sum::Done{value:0};}
 let prior:Sum=sum(n-1);
 match prior {
  Sum::Done{value:value}=>{return Sum::Done{value:value+n};},
  Sum::Missing{}=>{return Sum::Missing{};},
 }
}
fn tail(n:i64)->Sum{if n==0{return sum(8);}return tail(n-1);}
fn main()->i64{
 let total:Sum=tail(5);
 match total{Sum::Done{value:value}=>{return value;},Sum::Missing{}=>{return -1;},}
}"#,
        )],
        "",
        Expected::Value(36),
    );
    let text = support::emit(&program, "");
    assert!(text.contains("define internal void @ax_fn_0(ptr %union.result, i64 %arg0)"));
    assert!(text.contains("call void @ax_fn_1(ptr %union.result,"));
    // A forward return uses its received result pointer, with no local owner slot.
    let tail = text
        .split("define internal void @ax_fn_1")
        .nth(1)
        .unwrap()
        .split("define internal")
        .next()
        .unwrap();
    assert!(
        !tail.contains("alloca ["),
        "forward-only producer must not allocate union temporaries"
    );
}

#[test]
fn maximum_union_frames_reach_checked_depth_before_native_stack_exhaustion() {
    let params = (0..31)
        .map(|index| format!("p{index}:i64"))
        .collect::<Vec<_>>()
        .join(",");
    let forwarded = (0..31)
        .map(|index| format!("p{index}"))
        .collect::<Vec<_>>()
        .join(",");
    let arguments = vec!["0"; 31].join(",");
    let mut body = format!("let u0:U=U::V{{{}}};", values(31));
    for index in 1..8 {
        write!(body, "let u{index}:U=u{};", index - 1).unwrap();
    }
    for index in 0..216 {
        write!(body, "let mut x{index}:i64=0;").unwrap();
    }
    // 32 parameters + 8 owners + 216 scalar locals = 256 local bindings.
    // Each owner needs a tag and 31 fields, including all moved destinations:
    // 256 storage slots. The body has 523 nodes before these 508 three-node
    // conditionals, so the accepted recursive frame also reaches 2047 nodes.
    body.push_str(&"if false {}".repeat(508));
    let recursive = format!(
        "fn recurse(n:i64,{params})->U{{{body}if n==0{{return u7;}}return recurse(n-1,{forwarded});}}"
    );
    let make_source = |depth| {
        format!(
            "{recursive}union U{{V{{{}}}}}fn main()->i64{{let u:U=recurse({depth},{arguments});match u{{U::V{{{}}}=>{{return v30;}},}}}}",
            fields(31),
            patterns(31)
        )
    };
    // main and 63 producer frames fit. One more recursive call must report
    // the language trap, including at O0, rather than a host stack failure.
    let success_source = make_source(62);
    let program = verify(&[("main.ax", &success_source)], "", Expected::Value(30));
    assert_eq!(program.functions[0].params.len(), 32);
    assert_eq!(program.functions[0].locals.len(), 256);
    let slots: usize = program.functions[0]
        .locals
        .iter()
        .map(|local| match local.ty {
            axeon::ir::Type::Union(id) => {
                1 + program
                    .unions
                    .iter()
                    .find(|union| union.id == id)
                    .unwrap()
                    .payload_slots()
            }
            _ => 0,
        })
        .sum();
    assert_eq!(slots, 256);
    let failing_source = make_source(63);
    verify(
        &[("main.ax", &failing_source)],
        "",
        Expected::Trap {
            kind: TrapKind::CallDepth,
            file: "main.ax",
            marker: &recursive,
        },
    );
}

#[test]
fn payload_bits_integer_boundaries_and_nan_observation_agree() {
    verify_single(r#"
union Numeric{Number{real:f64,exact:i64,flag:bool},Empty{}}
fn make(real:f64)->Numeric{return Numeric::Number{exact:9007199254740993,flag:true,real:real};}
fn inspect(real:f64)->i64{
 let first:Numeric=make(real); let second:Numeric=first;
 match second{
  Numeric::Number{flag:flag,real:value,exact:exact}=>{print(value);print(exact);print(flag);return f64_bits(value);},
  Numeric::Empty{}=>{return 0;},
 }
}
fn main()->i64{
 print(inspect(-0.0)); print(inspect(f64_from_bits(1)));
 print(inspect(1.0/0.0)); print(inspect(0.0/0.0));
 return inspect((1.0+1.1102230246251565e-16)-1.0);
}"#, "f64:0x8000000000000000\n9007199254740993\ntrue\n-9223372036854775808\nf64:0x0000000000000001\n9007199254740993\ntrue\n1\nf64:0x7ff0000000000000\n9007199254740993\ntrue\n9218868437227405312\nf64:0x7ff8000000000000\n9007199254740993\ntrue\n9221120237041090560\nf64:0x0000000000000000\n9007199254740993\ntrue\n", 0);
}

#[test]
fn module_private_unions_with_equal_names_have_distinct_identities() {
    let program = verify(&[
        ("main.ax", "import left; import right; union U{V{}} fn main()->i64{let u:U=U::V{};match u{U::V{}=>{return left::value()+right::value();},}}"),
        ("left.ax", "union U{V{value:i64},Empty{}} fn make()->U{return U::V{value:17};} export fn value()->i64{let u:U=make();match u{U::V{value:n}=>{return n;},U::Empty{}=>{return 0;},}}"),
        ("right.ax", "union U{V{flag:bool},Empty{}} fn make()->U{return U::V{flag:true};} export fn value()->i64{let u:U=make();match u{U::V{flag:flag}=>{if flag{return 25;}return 0;},U::Empty{}=>{return 0;},}}"),
    ], "", Expected::Value(42));
    let ids: std::collections::HashSet<_> = program.unions.iter().map(|union| union.id).collect();
    assert_eq!(ids.len(), 3);
}

#[test]
fn match_arms_release_tensor_owners_on_continue_break_and_failure() {
    verify_single(
        r#"
union Flag{More{},Stop{}}
fn choose(n:i64)->Flag{if n<2{return Flag::More{};}return Flag::Stop{};}
fn main()->i64{
 let mut status:i64=0;
 tensor_scope(8){
  let mut i:i64=0;
  while i<4{
   let action:Flag=choose(i); i=i+1;
   match action{
    Flag::More{}=>{let t:tensor=try tensor_zeros([1]);print(tensor_len(t));continue;},
    Flag::Stop{}=>{let t:tensor=try tensor_zeros([1]);print(tensor_len(t));break;},
   }
  }
  let reused:tensor=try tensor_zeros([1]); print(tensor_len(reused));
  let action:Flag=Flag::More{};
  match action{
   Flag::More{}=>{let fails:tensor=try tensor_zeros([1]);},
   Flag::Stop{}=>{},
  }
 }catch(error:tensor_error){status=7;}
 tensor_scope(8){let again:tensor=try tensor_zeros([1]);print(tensor_len(again));}catch(error:tensor_error){status=9;}
 return status;
}"#,
        "1\n1\n1\n1\n1\n",
        7,
    );
}

#[test]
fn catch_handlers_return_constructed_and_forwarded_unions_after_cleanup() {
    verify_single(
        r#"
union Recovered{Value{real:f64,code:i64},Unexpected{}}
fn forward(code:i64)->Recovered{return Recovered::Value{real:f64_from_bits(1),code:code};}
fn produce(construct:bool)->Recovered{
 tensor_scope(8){
  let first:tensor=try tensor_zeros([1]);print(tensor_len(first));
  let exceeds_budget:tensor=try tensor_zeros([1]);print(999);
 }catch(error:tensor_error){
  if construct{return Recovered::Value{code:tensor_error_code(error),real:-0.0};}
  return forward(tensor_error_code(error));
 }
 return Recovered::Unexpected{};
}
fn inspect(construct:bool)->i64{
 let result:Recovered=produce(construct);
 match result{
  Recovered::Value{real:real,code:code}=>{print(real);print(code);return code;},
  Recovered::Unexpected{}=>{return -100;},
 }
}
fn main()->i64{
 let first:i64=inspect(true);let second:i64=inspect(false);
 tensor_scope(8){let reused:tensor=try tensor_zeros([1]);print(tensor_len(reused));}
 catch(error:tensor_error){return -200;}
 return first+second;
}"#,
        "1\nf64:0x8000000000000000\n9\n1\nf64:0x0000000000000001\n9\n1\n",
        18,
    );
}

#[test]
fn constructor_failures_stop_later_effects_and_keep_callee_locations() {
    verify(&[
        ("main.ax", "import fail; fn main()->i64{return fail::run();}"),
        ("fail.ax", "union U{V{a:i64,b:i64,c:i64}} fn mark(n:i64)->i64{print(n);return n;} fn make()->U{return U::V{c:mark(3),a:9223372036854775807 + 1,b:mark(2)};} export fn run()->i64{let u:U=make();match u{U::V{a:a,b:b,c:c}=>{return a+b+c;},}}"),
    ], "3\n", Expected::Trap{kind:TrapKind::Overflow,file:"fail.ax",marker:"9223372036854775807 + 1"});
}

#[test]
fn declarations_constructors_nominality_and_visibility_are_checked() {
    for (source, code) in [
        ("union U{} fn main()->i64{return 0;}", "U001"),
        ("union U{A{},A{}} fn main()->i64{return 0;}", "U003"),
        ("union U{A{x:i64,x:f64}} fn main()->i64{return 0;}", "U004"),
        ("union U{A{x:&[i64]}} fn main()->i64{return 0;}", "U005"),
        ("union U{A{x:U}} fn main()->i64{return 0;}", "A001"),
        (
            "union U{A{x:tensor_error}} fn main()->i64{return 0;}",
            "U005",
        ),
        (
            "union U{A{}} record U{x:i64} fn main()->i64{return 0;}",
            "R003",
        ),
        (
            "union U{A{}} union U{B{}} fn main()->i64{return 0;}",
            "R003",
        ),
        (
            "union U{A{}} export fn f(x:U)->i64{return 0;} fn main()->i64{return 0;}",
            "I002",
        ),
        (
            "union U{A{}} export fn f()->U{return U::A{};} fn main()->i64{return 0;}",
            "I002",
        ),
        ("union U{A{}} fn main()->U{return U::A{};}", "T003"),
        ("union U{A{}} fn f()->U{} fn main()->i64{return 0;}", "T010"),
        (
            "record Private{x:i64} export union U{A{x:Private}} fn main()->i64{return 0;}",
            "I002",
        ),
        (
            "union U{A(i64)} fn main()->i64{return 0;}",
            "E_PARSE_EXPECTED",
        ),
    ] {
        rejects(source, code);
    }
    for (body, code) in [
        ("let u:Choice=Missing::Some{value:1};return 0;", "U006"),
        ("let u:Choice=Choice::Other{};return 0;", "U007"),
        ("let u:Choice=Choice::Some{other:1};return 0;", "U008"),
        (
            "let u:Choice=Choice::Some{value:1,value:2};return 0;",
            "U009",
        ),
        ("let u:Choice=Choice::Some{};return 0;", "U010"),
        ("let u:Choice=Choice::Some{value:1.0};return 0;", "T009"),
        ("let u:Choice=Choice::None{value:1};return 0;", "U008"),
        ("let u:Choice=1;return 0;", "O005"),
        ("let u:Choice=Choice::None{};print(u);return 0;", "O004"),
        (
            "let mut u:Choice=Choice::None{};u=Choice::None{};return 0;",
            "O008",
        ),
        ("let u:Choice=Choice::None{};return u.value;", "R010"),
        ("let x:i64=Choice::None{};return 0;", "O004"),
    ] {
        rejects_body(body, code);
    }
    rejects(
        "union A{V{}} union B{V{}} fn f()->A{return B::V{};} fn main()->i64{return 0;}",
        "T009",
    );
    rejects(
        "union A{V{}} union B{V{}} fn f()->A{let b:B=B::V{};return b;} fn main()->i64{return 0;}",
        "T009",
    );
    rejects(
        "union A{V{}} union B{V{}} fn f()->B{return B::V{};} fn main()->i64{let a:A=f();return 0;}",
        "T009",
    );
    rejects(
        "union A{V{}} fn f()->A{return A::V{};} fn main()->i64{f();return 0;}",
        "O004",
    );
    rejects("union A{V{}} fn f()->A{return A::V{};} fn g(a:i64)->i64{return a;} fn main()->i64{return g(f());}", "O004");
    let directory = Temp::new();
    directory.write(&[
        (
            "main.ax",
            "import helper; fn main()->i64{let u:Private=Private::V{};return 0;}",
        ),
        (
            "helper.ax",
            "union Private{V{}} export fn value()->i64{return 0;}",
        ),
    ]);
    let ProjectError::Diagnostics { diagnostics, .. } = directory.load().unwrap_err() else {
        panic!("private union type must fail")
    };
    assert!(diagnostics.iter().any(|error| error.code == "E_PARSE_TYPE"));
}

#[test]
fn patterns_exhaustiveness_immutability_and_affine_consumption_are_checked() {
    for (body, code) in [
        ("let u:Choice=Choice::None{};match u{}return 0;", "U014"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},}return 0;", "U014"),
        ("let u:Choice=Choice::None{};match u{Choice::Some{value:n}=>{},Choice::None{}=>{},Choice::None{}=>{},}return 0;", "U013"),
        ("let u:Choice=Choice::None{};match u{Choice::Other{}=>{},}return 0;", "U007"),
        ("let u:Choice=Choice::None{};match u{Other::None{}=>{},Choice::Some{value:n}=>{},}return 0;", "U012"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{}=>{},}return 0;", "U010"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{wrong:n}=>{},}return 0;", "U008"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{value:n,value:m}=>{},}return 0;", "U009"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{value:n}=>{n=1;},}return 0;", "T006"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{value:n}=>{let n:i64=1;},}return 0;", "T004"),
        ("let u:i64=0;match u{Choice::None{}=>{},}return 0;", "U011"),
        ("let u:Choice=Choice::None{};let v:Choice=u;match u{Choice::None{}=>{},Choice::Some{value:n}=>{},}return 0;", "O006"),
        ("let u:Choice=Choice::None{};match u{Choice::None{}=>{},Choice::Some{value:n}=>{},}let v:Choice=u;return 0;", "O006"),
        ("let u:Choice=Choice::None{};while false{match u{Choice::None{}=>{},Choice::Some{value:n}=>{},}}return 0;", "O011"),
        ("let u:Choice=Choice::None{};tensor_scope(0){match u{Choice::None{}=>{},Choice::Some{value:n}=>{},}}catch(e:tensor_error){}return 0;", "N007"),
        ("let u:Choice=Choice::None{};match u{_=>{},}return 0;", "E_PARSE_EXPECTED"),
        ("let u:Choice=Choice::None{};match u{Choice::None{} if true=>{},}return 0;", "E_PARSE_EXPECTED"),
        ("let u:Choice=Choice::None{};match u{Choice::Some{value:Choice::None{}}=>{},}return 0;", "E_PARSE_EXPECTED"),
    ] { rejects_body(body, code); }
    rejects("union U{V{x:i64,y:i64}} fn main()->i64{let u:U=U::V{x:1,y:2};match u{U::V{x:n,y:n}=>{return n;},}}", "T004");
    rejects(
        "union U{V{}} fn main()->i64{let u:U=U::V{};match u{U::V{}=>{return 0;},}return 1;}",
        "T011",
    );
    rejects("union U{V{}} fn f()->U{tensor_scope(0){return U::V{};}catch(e:tensor_error){}return U::V{};} fn main()->i64{return 0;}", "N006");
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
fn patterns(count: usize) -> String {
    (0..count)
        .map(|index| format!("f{index}:v{index}"))
        .collect::<Vec<_>>()
        .join(",")
}

#[test]
fn shared_type_storage_pattern_and_parser_resource_limits_are_enforced() {
    let mut variants = String::new();
    for index in 0..32 {
        write!(variants, "V{index}{{}},").unwrap();
    }
    let mut arms = String::new();
    for index in 0..32 {
        write!(arms, "U::V{index}{{}}=>{{return {index};}},").unwrap();
    }
    verify_single(
        &format!("union U{{{variants}}}fn main()->i64{{let u:U=U::V31{{}};match u{{{arms}}}}}"),
        "",
        31,
    );
    rejects(
        &format!("union U{{{variants}Extra{{}}}}fn main()->i64{{return 0;}}"),
        "U001",
    );
    rejects(
        &format!("union U{{V{{{}}}}}fn main()->i64{{return 0;}}", fields(33)),
        "U002",
    );
    let mut types = String::new();
    for index in 0..128 {
        write!(types, "record R{index}{{x:i64}} union U{index}{{V{{}}}} ").unwrap();
    }
    let source = format!(
        "{types}fn main()->i64{{let u:U127=U127::V{{}};match u{{U127::V{{}}=>{{return 7;}},}}}}"
    );
    verify_single(&source, "", 7);
    rejects(&format!("union Extra{{V{{}}}}{source}"), "R001");

    // 8 * (tag + 31 fields) consumes all256 storage slots, including moves.
    let mut body = format!("let u0:U=U::V{{{}}};", values(31));
    for index in 1..8 {
        write!(body, "let u{index}:U=u{};", index - 1).unwrap();
    }
    let prefix = format!(
        "union U{{V{{{}}},Empty{{}}}}fn main()->i64{{{body}",
        fields(31)
    );
    verify_single(
        &format!(
            "{prefix}match u7{{U::V{{{}}}=>{{return v30;}},U::Empty{{}}=>{{return -1;}},}}}}",
            patterns(31)
        ),
        "",
        30,
    );
    rejects(
        &format!("{prefix}let extra:[i64;1]=[0];return 0;}}"),
        "O002",
    );
    verify_single(&format!("union U{{V{{{}}},Empty{{}}}}fn main()->i64{{let u:U=U::V{{{}}};match u{{U::V{{{}}}=>{{return v31;}},U::Empty{{}}=>{{return -1;}},}}}}",fields(32),values(32),patterns(32)),"",31);

    let mut declarations = String::new();
    let mut patterns_all = String::new();
    for index in 0..9 {
        write!(declarations, "V{index}{{{}}},", fields(32)).unwrap();
        write!(patterns_all, "U::V{index}{{{}}}=>{{}},", patterns(32)).unwrap();
    }
    rejects(&format!("union U{{{declarations}}}fn main()->i64{{let u:U=U::V0{{{}}};match u{{{patterns_all}}}return 0;}}",values(32)),"T015");
    let expression = std::iter::repeat("1")
        .take(40)
        .collect::<Vec<_>>()
        .join("+");
    let initializers = (0..32)
        .map(|index| format!("f{index}:{expression}"))
        .collect::<Vec<_>>()
        .join(",");
    rejects(
        &format!(
            "union U{{V{{{}}}}}fn main()->i64{{let u:U=U::V{{{initializers}}};return 0;}}",
            fields(32)
        ),
        "T016",
    );
    let nested = format!("{}1{}", "(".repeat(80), ")".repeat(80));
    rejects_body(
        &format!("let u:Choice=Choice::Some{{value:{nested}}};return 0;"),
        "E_NESTING_LIMIT",
    );
}

#[test]
fn invalid_tag_aborts_with_tensor_cleanup_in_interpreter_and_native_ir() {
    let source = r#"union U{V{}}
fn fail()->i64{let u:U=U::V{};match u{U::V{}=>{return 999;},}}
fn main()->i64{tensor_scope(8){let t:tensor=try tensor_zeros([1]);print(tensor_len(t));print(fail());}catch(e:tensor_error){print(999);}return 0;}"#;
    let mut program = axeon::compile(source).unwrap();
    let fail = &mut program.functions[0];
    let axeon::ir::StmtKind::Let { value, .. } = &mut fail.body.stmts[0].kind else {
        panic!("union local")
    };
    let axeon::ir::ExprKind::Union { variant, .. } = &mut value.kind else {
        panic!("union initializer")
    };
    *variant = 77; // Corrupt internal IR deliberately; valid source cannot forge a tag.
    let span = fail.body.stmts[1].span;
    let interpreted = interpreter::run(&program, 1_000_000);
    assert_eq!(interpreted.stdout, "1\n");
    let trap = interpreted.result.unwrap_err();
    assert_eq!(trap.kind, TrapKind::InvalidUnionTag);
    assert_eq!(trap.span, span);
    let stderr = trap.render(source);
    let directory = Temp::new();
    let llvm = support::emit(&program, source);
    fs::write(directory.0.join("bad.ll"), &llvm).unwrap();
    assert!(llvm.contains("store [1 x i64] zeroinitializer"));
    assert!(llvm.contains("call void @ax_rt_trap(i32 8,"));
    let clang = std::env::var_os("AXEON_CLANG")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("clang-19"));
    let bridge = std::env::var_os("AXEON_TENSOR_RUNTIME")
        .map(PathBuf::from)
        .unwrap_or_else(|| {
            Path::new(env!("CARGO_BIN_EXE_axeon"))
                .parent()
                .unwrap()
                .join(support::archive_name())
        });
    for opt in ["0", "2", "3"] {
        let executable = support::executable_path(directory.0.join(format!("invalid-O{opt}")));
        let result = output(
            bounded(&clang, "40s")
                .args([
                    "-target",
                    support::target_triple(),
                    "-Werror",
                    "-Wno-override-module",
                ])
                .arg(format!("-O{opt}"))
                .arg(directory.0.join("bad.ll"))
                .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("runtime/runtime.c"))
                .arg(&bridge)
                .arg("-std=c11")
                .args(support::tensor_link_flags())
                .args(support::clang_link_flags())
                .arg("-o")
                .arg(&executable),
        );
        assert!(
            result.status.success(),
            "invalid-tag O{opt}: {}",
            String::from_utf8_lossy(&result.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(native.status.code(), Some(70));
        assert_eq!(native.stdout, b"1\n");
        assert_eq!(native.stderr, stderr.as_bytes());
    }
}
