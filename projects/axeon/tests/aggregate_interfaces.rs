//! Owned aggregate interfaces: independent answers, rejected programs, and native
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
                std::env::temp_dir().join(format!("axeon-interfaces-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create aggregate fixture: {error}"),
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
    let result = command.output().expect("aggregate subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "aggregate subprocess timed out"
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
    let project = directory.load().expect("load aggregate fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent aggregate effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(
                interpreted.result,
                Ok(value),
                "independent aggregate result"
            );
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted
                .result
                .expect_err("aggregate scalar operation traps");
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
            "aggregate build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "aggregate exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "aggregate stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "aggregate stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid aggregate program must not expose IR");
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}\n{source}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
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
fn arrays_records_and_unions_cross_owned_parameter_and_result_boundaries() {
    verify_single(
        r#"
record Pair{number:f64,count:i64,valid:bool}
union Answer{Value{real:f64,count:i64},Missing{}}
fn numbers()->[i64;2]{return [40,2];}
fn array_forward(a:[i64;2])->[i64;2]{return a;}
fn sum(a:[i64;2])->i64{return a[0]+a[1];}
fn make(n:i64)->Pair{return Pair{valid:true,count:n,number:-0.0};}
fn update(p:Pair)->Pair{let mut q:Pair=p;q.count=q.count+1;return q;}
fn record_forward(p:Pair)->Pair{return update(p);}
fn convert(p:Pair)->Answer{print(p.valid);return Answer::Value{real:p.number,count:p.count};}
fn union_forward(a:Answer)->Answer{return a;}
fn consume(a:Answer)->i64{match a{Answer::Value{real:r,count:n}=>{print(r);return n;},Answer::Missing{}=>{return -1;},}}
fn main()->i64{
 let a:[i64;2]=numbers();let b:[i64;2]=array_forward(a);let n:i64=sum(b);
 let p:Pair=make(n);let q:Pair=record_forward(p);let u:Answer=convert(q);
 let v:Answer=union_forward(u);return consume(v);
}"#,
        "true\nf64:0x8000000000000000\n",
        43,
    );
}

#[test]
fn floating_arrays_and_nominal_payloads_preserve_exact_observations() {
    verify_single(r#"
record Bits{real:f64,exact:i64,flag:bool}
union U{V{real:f64,exact:i64,flag:bool}}
fn array(a:[f64;3])->[f64;3]{return a;}
fn record(p:Bits)->Bits{return p;}
fn union(u:U)->U{return u;}
fn observe(p:Bits)->i64{
 let q:Bits=record(p);let u:U=U::V{real:q.real,exact:q.exact,flag:q.flag};let v:U=union(u);
 match v{U::V{real:r,exact:n,flag:b}=>{print(r);print(n);print(b);return f64_bits(r);},}
}
fn main()->i64{
 let a:[f64;3]=[-0.0,f64_from_bits(1),0.0/0.0];let b:[f64;3]=array(a);
 let p:Bits=Bits{real:b[0],exact:9007199254740993,flag:true};print(observe(p));
 let q:Bits=Bits{real:b[1],exact:-9223372036854775808,flag:false};print(observe(q));
 let r:Bits=Bits{real:b[2],exact:9223372036854775807,flag:true};return observe(r);
}"#, "f64:0x8000000000000000\n9007199254740993\ntrue\n-9223372036854775808\nf64:0x0000000000000001\n-9223372036854775808\nfalse\n1\nf64:0x7ff8000000000000\n9223372036854775807\ntrue\n", 9221120237041090560);
}

#[test]
fn mixed_arguments_evaluate_left_to_right_and_allow_reads_before_transfer() {
    verify_single(
        r#"
record R{x:i64}
union U{V{x:i64}}
fn mark(n:i64)->i64{print(n);return n;}
fn take(before:i64,r:R,middle:i64,a:[i64;1],u:U,last:i64)->i64{
 print(r.x);print(a[0]);match u{U::V{x:x}=>{print(x);return before+middle+last;},}
}
fn read_first(n:i64,r:R)->i64{return n+r.x;}
fn main()->i64{
 let r:R=R{x:10};let a:[i64;1]=[20];let u:U=U::V{x:30};
 print(take(mark(1),r,mark(2),a,u,mark(3)));
 let next:R=R{x:21};return read_first(next.x,next);
}"#,
        "1\n2\n3\n10\n20\n30\n6\n",
        42,
    );
    for (body, effects) in [
        ("return take(fail(),r,mark(2));", ""),
        ("return take(mark(1),r,fail());", "1\n"),
    ] {
        let source = format!("record R{{x:i64}} fn mark(n:i64)->i64{{print(n);return n;}} fn fail()->i64{{return 9223372036854775807 + 1;}} fn take(a:i64,r:R,b:i64)->i64{{print(999);return a+r.x+b;}} fn main()->i64{{let r:R=R{{x:10}};{body}}}");
        verify(
            &[("main.ax", &source)],
            effects,
            Expected::Trap {
                kind: TrapKind::Overflow,
                file: "main.ax",
                marker: "9223372036854775807 + 1",
            },
        );
    }
}

#[test]
fn public_types_and_forwarding_functions_share_identity_through_diamonds() {
    let program = verify(&[
        ("main.ax", r#"import types;import left;import right;
fn main()->i64{
 let r:types::R=types::R{x:40,real:-0.0};let s:types::R=left::forward(r);
 print(s.real);let u:types::U=types::U::V{x:s.x};let v:types::U=right::forward(u);
 match v{types::U::V{x:n}=>{let a:[i64;2]=left::numbers();return n+a[0]+a[1];},types::U::Empty{}=>{return -1;},}
}"#),
        ("types.ax", "export record R{x:i64,real:f64} export union U{V{x:i64},Empty{}} export fn identity(r:R)->R{return r;}"),
        ("left.ax", "import types;export fn forward(r:types::R)->types::R{return types::identity(r);}export fn numbers()->[i64;2]{return [1,1];}"),
        ("right.ax", "import types;export fn forward(u:types::U)->types::U{return u;}"),
    ], "f64:0x8000000000000000\n", Expected::Value(42));
    assert_eq!(program.records.len(), 1);
    assert_eq!(program.unions.len(), 1);
}

#[test]
fn contextual_names_forward_types_and_two_segment_constructors_remain_unambiguous() {
    verify(&[
        ("main.ax", "import U;union U{V{x:i64}}fn use_local(a:U)->i64{match a{U::V{x:n}=>{return n;},}}fn main()->i64{let a:U=U::V{x:20};let b:U::V=U::V{x:22};return use_local(a)+b.x;}"),
        ("U.ax", "export record V{x:i64}"),
    ], "", Expected::Value(42));
    verify_single("fn export(record:Later)->Later{return record;}fn main()->i64{let union:Later=Later{match:42};let import:Later=export(union);return import.match;}record Later{match:i64}", "", 42);
}

#[test]
fn transfers_preserve_ownership_loans_loop_rules_and_parameter_immutability() {
    const PRELUDE: &str = "record R{x:i64} union U{V{x:i64}} fn take_r(r:R)->i64{return r.x;} fn take_a(a:[i64;1])->i64{return a[0];} fn take_u(u:U)->i64{match u{U::V{x:n}=>{return n;},}} ";
    for (body, code) in [
        ("let r:R=R{x:1};print(take_r(r));return r.x;", "O006"),
        ("let a:[i64;1]=[1];print(take_a(a));return len(a);", "O006"),
        (
            "let u:U=U::V{x:1};print(take_u(u));return take_u(u);",
            "O006",
        ),
        (
            "let a:[i64;1]=[1];let view:&[i64]=&a[0..1];return take_a(a);",
            "O007",
        ),
        (
            "let r:R=R{x:1};while false{print(take_r(r));}return 0;",
            "O011",
        ),
        (
            "let r:R=R{x:1};tensor_scope(0){print(take_r(r));}catch(e:tensor_error){}return 0;",
            "N007",
        ),
        (
            "let r:R=R{x:1};if true{print(take_r(r));}return r.x;",
            "O006",
        ),
        ("return take_r(R{x:1});", "I001"),
        ("return take_a([1]);", "I001"),
        ("return take_u(U::V{x:1});", "I001"),
    ] {
        rejects(&format!("{PRELUDE}fn main()->i64{{{body}}}"), code);
    }
    rejects("record R{x:i64}fn f(a:R,b:R)->i64{return a.x+b.x;}fn main()->i64{let r:R=R{x:1};return f(r,r);}","O006");
    rejects("record R{x:i64}fn f(a:R,b:i64)->i64{return a.x+b;}fn main()->i64{let r:R=R{x:1};return f(r,r.x);}","O006");
    rejects("fn make()->[i64;1]{return [1];}fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{return take(make());}","I001");
    rejects(
        "record R{x:i64}fn change(r:R)->i64{r.x=2;return r.x;}fn main()->i64{return 0;}",
        "O010",
    );
    rejects(
        "fn change(a:[i64;1])->i64{a[0]=2;return a[0];}fn main()->i64{return 0;}",
        "O010",
    );
    verify_single(&format!("{PRELUDE}fn branch(flag:bool)->i64{{let r:R=R{{x:20}};if flag{{return take_r(r);}}return r.x;}}fn main()->i64{{let mut total:i64=branch(false);let mut n:i64=0;while n<2{{let a:[i64;1]=[11];total=total+take_a(a);n=n+1;}}return total;}}"),"",42);
}

#[test]
fn exact_shapes_visibility_and_unsupported_escapes_are_diagnosed() {
    for (source,code) in [
        ("fn take(a:[i64;2])->i64{return a[0];}fn main()->i64{let a:[i64;1]=[1];return take(a);}","T009"),
        ("fn take(a:[f64;1])->i64{return 0;}fn main()->i64{let a:[i64;1]=[1];return take(a);}","T009"),
        ("record A{x:i64}record B{x:i64}fn take(a:A)->i64{return a.x;}fn main()->i64{let b:B=B{x:1};return take(b);}","T009"),
        ("union A{V{x:i64}}union B{V{x:i64}}fn take(a:A)->i64{return 0;}fn main()->i64{let b:B=B::V{x:1};return take(b);}","T009"),
        ("record R{x:i64}export fn private(r:R)->i64{return r.x;}fn main()->i64{return 0;}","I002"),
        ("union U{V{}}export fn private()->U{return U::V{};}fn main()->i64{return 0;}","I002"),
        ("fn f(a:&[i64])->i64{return 0;}fn main()->i64{return 0;}","O003"),
        ("fn f()->tensor{return 0;}fn main()->i64{return 0;}","O003"),
        ("fn f(a:tensor_error)->i64{return 0;}fn main()->i64{return 0;}","O003"),
        ("fn f()->[i64;1]{return [1];}fn main()->i64{f();return 0;}","O004"),
        ("record R{x:i64}fn f()->R{return R{x:1};}fn main()->i64{print(f());return 0;}","O004"),
        ("export record R{a:&[i64]}fn main()->i64{return 0;}","R005"),
        ("export union U{V{a:tensor}}fn main()->i64{return 0;}","U005"),
    ] { rejects(source,code); }
    rejects_project(
        &[
            (
                "main.ax",
                "import hidden;fn main()->i64{let x:hidden::R=hidden::R{x:1};return x.x;}",
            ),
            ("hidden.ax", "record R{x:i64}"),
        ],
        "E_PARSE_TYPE",
    );
    rejects_project(
        &[
            (
                "main.ax",
                "import middle;fn main()->i64{let x:types::R=types::R{x:1};return x.x;}",
            ),
            (
                "middle.ax",
                "import types;export fn value()->i64{return 0;}",
            ),
            ("types.ax", "export record R{x:i64}"),
        ],
        "E_PARSE_TYPE",
    );
    rejects_project(&[("main.ax","import left;import right;fn main()->i64{let x:right::R=right::R{x:1};return left::take(x);}"),("left.ax","export record R{x:i64}export fn take(r:R)->i64{return r.x;}"),("right.ax","export record R{x:i64}")],"T009");
}

#[test]
fn tensor_recovery_and_fatal_callee_cleanup_keep_owned_call_semantics() {
    verify_single(
        r#"
record R{real:f64,code:i64}
fn take(r:R)->i64{print(r.real);return r.code;}
fn forward(r:R)->R{return r;}
fn recover()->R{
 tensor_scope(8){
  let a:[i64;1]=[3];let t:tensor=try tensor_zeros([1]);
  let r:R=R{real:-0.0,code:a[0]};print(take(r));
  let fails:tensor=try tensor_zeros([1]);
 }catch(e:tensor_error){let recovered:R=R{real:f64_from_bits(1),code:tensor_error_code(e)};return forward(recovered);}
 return R{real:99.0,code:-1};
}

fn main()->i64{let r:R=recover();let result:i64=take(r);tensor_scope(8){let t:tensor=try tensor_zeros([1]);print(tensor_len(t));}catch(e:tensor_error){return -1;}return result;}
"#,
        "f64:0x8000000000000000\n3\nf64:0x0000000000000001\n1\n",
        9,
    );
    let source="record R{x:i64}fn fail(r:R)->i64{return r.x / 0;}fn main()->i64{tensor_scope(8){let t:tensor=try tensor_zeros([1]);let r:R=R{x:1};print(tensor_len(t));print(fail(r));}catch(e:tensor_error){print(999);}return 0;}";
    verify(
        &[("main.ax", source)],
        "1\n",
        Expected::Trap {
            kind: TrapKind::DivisionByZero,
            file: "main.ax",
            marker: "r.x / 0",
        },
    );
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
fn maximum_mixed_parameter_frames_and_caller_result_storage_are_bounded() {
    let mut params = vec!["n:i64".to_owned()];
    let mut forwarding = vec!["n-1".to_owned()];
    let mut initial = vec!["DEPTH".to_owned()];
    for index in 0..23 {
        params.push(format!("p{index}:i64"));
        forwarding.push(format!("p{index}"));
        initial.push("0".to_owned());
    }
    let mut setup = String::new();
    for (prefix, ty, count) in [("a", "[i64;32]", 3), ("r", "R", 3), ("u", "U", 2)] {
        for index in 0..count {
            params.push(format!("{prefix}{index}:{ty}"));
            forwarding.push(format!("{prefix}{index}"));
            initial.push(format!("{prefix}{index}"));
            let value = match prefix {
                "a" => format!(
                    "[{}]",
                    (0..32).map(|n| n.to_string()).collect::<Vec<_>>().join(",")
                ),
                "r" => format!("R{{{}}}", values(32)),
                _ => format!("U::V{{{}}}", values(31)),
            };
            write!(setup, "let {prefix}{index}:{ty}={value};").unwrap();
        }
    }
    let mut body = String::new();
    for index in 0..224 {
        write!(body, "let mut x{index}:i64=0;").unwrap();
    }
    // 32 parameters, 256 total locals, 256 inline slots, 2046 syntax nodes.
    body.push_str(&"if false {}".repeat(518));
    let recursive = format!(
        "fn recurse({})->U{{{body}if n==0{{return u1;}}return recurse({});}}",
        params.join(","),
        forwarding.join(",")
    );
    let pattern = (0..31)
        .map(|index| format!("f{index}:v{index}"))
        .collect::<Vec<_>>()
        .join(",");
    let make_source = |depth| {
        format!("{recursive}record R{{{}}}union U{{V{{{}}}}}fn prepare()->U{{{setup}return recurse({});}}fn main()->i64{{let result:U=prepare();match result{{U::V{{{pattern}}}=>{{return v30;}},}}}}",fields(32),fields(31),initial.join(",").replace("DEPTH",&format!("{depth}")))
    };
    let successful = make_source(61);
    let program = verify(&[("main.ax", &successful)], "", Expected::Value(30));
    assert_eq!(program.functions[0].params.len(), 32);
    assert_eq!(program.functions[0].locals.len(), 256);
    let slots: usize = program.functions[0]
        .locals
        .iter()
        .map(|local| match local.ty {
            axeon::ir::Type::Array { length, .. } => length,
            axeon::ir::Type::Record(id) => program
                .records
                .iter()
                .find(|record| record.id == id)
                .unwrap()
                .fields
                .len(),
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
    let failing = make_source(62);
    verify(
        &[("main.ax", &failing)],
        "",
        Expected::Trap {
            kind: TrapKind::CallDepth,
            file: "main.ax",
            marker: &recursive,
        },
    );
    // A caller result is a real destination and cannot exceed its frame budget.
    let over_result=format!("record R{{{}}}union U{{V{{{}}}}}fn result()->U{{return U::V{{{}}};}}fn main()->i64{{{setup}let extra:U=result();return 0;}}",fields(32),fields(31),values(31));
    rejects(&over_result, "O002");
    let large_params = (0..9)
        .map(|index| format!("p{index}:types::R"))
        .collect::<Vec<_>>()
        .join(",");
    rejects_project(&[("main.ax",&format!("import types;fn f({large_params})->i64{{return 0;}}fn main()->i64{{return 0;}}")),("types.ax",&format!("export record R{{{}}}",fields(32)))],"O002");
}

#[test]
fn header_discovery_uses_full_grammar_and_formatter_does_not_resolve_external_types() {
    let source = "import data;fn main()->i64{let value:data::R=data::R{x:42};return value.x;}";
    axeon::parser::validate_syntax(source).unwrap();
    let formatted = axeon::tooling::format_source(source).unwrap();
    assert_eq!(
        axeon::tooling::format_source(&formatted).unwrap(),
        formatted
    );
    verify(
        &[
            ("main.ax", &formatted),
            ("data.ax", "export record R{x:i64}"),
        ],
        "",
        Expected::Value(42),
    );
    // A malformed import inside a body must be a syntax error, not a request
    // to open missing.ax. Comments and contextual function/variable names
    // likewise do not introduce graph edges.
    rejects_project(
        &[("main.ax", "fn main()->i64{import missing;return 0;}")],
        "E_PARSE_STATEMENT",
    );
    verify_single("// import missing;\nfn import(export:i64)->i64{return export;}fn main()->i64{let record:i64=import(42);return record;}","",42);
    rejects_project(
        &[
            (
                "main.ax",
                "import data;fn main()->i64{let r:data::R=data::R{x:1};return r.x;}",
            ),
            ("data.ax", "export record R{x:i64"),
        ],
        "E_PARSE_EXPECTED",
    );
    rejects_project(
        &[
            (
                "main.ax",
                "import data;import data;fn main()->i64{return 0;}",
            ),
            ("data.ax", "export record R{x:i64}"),
        ],
        "E_MODULE_DUPLICATE_IMPORT",
    );
}

#[test]
fn nested_call_moves_cannot_invalidate_targets_or_repeat_across_loop_conditions() {
    for source in [
        "record R{x:i64}fn take(r:R)->i64{return r.x;}fn main()->i64{let mut r:R=R{x:1};r.x=take(r);return 0;}",
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let mut a:[i64;1]=[0];a[0]=take(a);return 0;}",
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let mut a:[i64;1]=[0];a[take(a)]=1;return 0;}",
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let a:[i64;1]=[0];return a[take(a)];}",
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let a:[i64;1]=[0];let b:&[i64]=&a[take(a)..1];return 0;}",
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let mut a:[i64;1]=[0];let b:&mut [i64]=&mut a[0..take(a)];return 0;}",
        "fn take(a:[f64;1])->i64{return 1;}fn main()->i64{tensor_scope(8){let a:[f64;1]=[1.0];let t:tensor=try tensor_from_array([take(a)],a);}catch(e:tensor_error){}return 0;}",
    ] { rejects(source,"O006"); }
    for source in [
        "fn again(a:[i64;1])->bool{return true;}fn main()->i64{let a:[i64;1]=[1];while again(a){}return 0;}",
        "record R{x:i64}fn again(r:R)->bool{return false;}fn main()->i64{let r:R=R{x:1};while again(r){}return 0;}",
        "union U{V{}}fn again(u:U)->bool{return true;}fn main()->i64{let u:U=U::V{};while false && again(u){}return 0;}",
        "fn again(a:[i64;1])->bool{return true;}fn main()->i64{while false{let a:[i64;1]=[1];while again(a){break;}}return 0;}",
    ] { rejects(source,"O011"); }
    // Read-only target expressions remain valid, and consuming another owner
    // in an assignment RHS does not reserve or forbid unrelated storage.
    verify_single("record R{x:i64}fn take(r:R)->i64{return r.x;}fn main()->i64{let mut a:[i64;1]=[0];let r:R=R{x:40};a[a[0]]=take(r);let mut s:R=R{x:0};let t:R=R{x:2};s.x=take(t);let view:&[i64]=&a[0..len(a)];return view[0]+s.x;}","",42);
}
