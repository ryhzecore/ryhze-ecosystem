//! Explicit bounded generic functions: independent answers, rejected programs, and native
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
                std::env::temp_dir().join(format!("axeon-generics-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create generic fixture: {error}"),
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
    let result = command.output().expect("generic subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "generic subprocess timed out"
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
    let project = directory.load().expect("load generic fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent generic effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent generic result");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted
                .result
                .expect_err("generic scalar operation traps");
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
            "generic build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "generic exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "generic stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "generic stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid generic program must not expose IR");
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
fn explicit_instances_preserve_scalar_unit_array_and_nested_owner_values() {
    let source = r#"
record Packet { exact:i64, zero:f64, tiny:f64, nan:f64, flag:bool, done:(), values:[i64;2] }
union Choice { Some { packet:Packet }, None {} }
fn identity<T>(value:T)->T { return value; }
fn main()->i64 {
 let integer:i64=identity::<i64>(9007199254740993);
 let done:()=identity::<()>(());
 let flag:bool=identity::<bool>(true);
 let zero:f64=identity::<f64>(-0.0);
 let array:[i64;2]=[20,22]; let moved:[i64;2]=identity::<[i64;2]>(array);
 let p:Packet=Packet{exact:integer,zero:zero,tiny:5e-324,nan:f64_from_bits(9221120237041090561),flag:flag,done:done,values:moved};
 let c:Choice=Choice::Some{packet:p}; let result:Choice=identity::<Choice>(c);
 match result {
  Choice::None{}=>{return 1;},
  Choice::Some{packet:q}=>{
   if q.exact!=9007199254740993 || f64_bits(q.zero)!=-9223372036854775808 || f64_bits(q.tiny)!=1 {return 2;}
   if f64_bits(q.nan)!=9221120237041090560 || !q.flag || q.done!=() {return 3;}
   let values:[i64;2]=q.values; return values[0]+values[1];
  },
 }
}
"#;
    let program = verify(&[("main.ax", source)], "", Expected::Value(42));
    assert_eq!(program.functions.len(), 7, "main plus six exact instances");
}

#[test]
fn same_key_self_and_mutual_recursion_are_interned_once() {
    let source = r#"
record R { x:i64 }
fn first<T>(x:T,n:i64)->T { if n==0 {return x;} return second::<T>(x,n-1); }
fn second<U>(x:U,n:i64)->U { if n==0 {return x;} return first::<U>(x,n-1); }
fn repeat<T>(x:T,n:i64)->T {if n==0{return x;}return repeat::<T>(x,n-1);}
fn main()->i64{let r:R=R{x:42};let a:R=first::<R>(r,8);let b:R=repeat::<R>(a,8);return b.x;}
"#;
    let program = verify(&[("main.ax", source)], "", Expected::Value(42));
    assert_eq!(program.functions.len(), 4);
}

#[test]
fn exact_bounds_and_definition_private_helpers_work_across_modules() {
    let files = [
        ("contracts.ax", "export trait Read{fn read(value:Self)->i64;} export trait Forward{fn forward(value:Self)->Self;}"),
        ("types.ax", r#"import contracts; export record Packet{x:i64}
fn read(value:Packet)->i64{return value.x;}fn forward(value:Packet)->Packet{return value;}
impl contracts::Read for Packet{read=read;}impl contracts::Forward for Packet{forward=forward;}
export fn make()->Packet{return Packet{x:41};}"#),
        ("algorithm.ax", r#"import contracts;
fn private_add(n:i64)->i64{return n+1;}
export fn finish<T:contracts::Read>(value:T)->i64{return private_add(contracts::Read::read(value));}
export fn pipeline<T:contracts::Read+contracts::Forward>(value:T)->i64{
let moved:T=contracts::Forward::forward(value);return finish::<T>(moved);}"#),
        ("left.ax", "import algorithm;import types;export fn run()->i64{let p:types::Packet=types::make();return algorithm::pipeline::<types::Packet>(p);}"),
        ("right.ax", "import algorithm;import types;export fn run()->i64{let p:types::Packet=types::make();return algorithm::pipeline::<types::Packet>(p);}"),
        ("main.ax", "import left;import right;fn private_add(n:i64)->i64{return 999;}fn main()->i64{return left::run()+right::run();}"),
    ];
    let program = verify(&files, "", Expected::Value(84));
    assert_eq!(
        program
            .functions
            .iter()
            .filter(|f| f.name == "pipeline" || f.name == "finish")
            .count(),
        2,
        "diamond imports reuse template and concrete nominal identities"
    );
}

#[test]
fn generic_argument_effect_order_and_traps_are_unchanged() {
    let source = r#"
fn effect(n:i64)->i64{print(n);return n;}
fn combine<T>(a:i64,value:T,b:i64)->i64{print(9);return a+b;}
fn main()->i64{let a:[i64;1]=[5];return combine::<[i64;1]>(effect(1),a,effect(2));}
"#;
    verify_single(source, "1\n2\n9\n", 3);
    for (old, effects) in [("effect(1)", ""), ("effect(2)", "1\n")] {
        let trapped = source.replace(old, "7 / 0");
        verify(
            &[("main.ax", &trapped)],
            effects,
            Expected::Trap {
                kind: TrapKind::DivisionByZero,
                file: "main.ax",
                marker: "7 / 0",
            },
        );
    }
}

#[test]
fn unused_templates_are_proved_without_scalar_stand_ins() {
    for (body, code) in [
        ("let a:T=x;let b:T=x;return 0;", "O006"),
        ("print(x);return 0;", "O004"),
        ("return x+1;", "O004"),
        ("let a:bool=x==x;return 0;", "O004"),
        ("return x.field;", "R010"),
        ("return x[0];", "O009"),
        ("let a:&[i64]=&x;return 0;", "O005"),
        ("let concrete:bool=1;return 0;", "T009"),
        ("return missing();", "T007"),
    ] {
        rejects(
            &format!("fn unused<T>(x:T)->i64{{{body}}}fn main()->i64{{return 0;}}"),
            code,
        );
    }
    rejects("fn twice<T>(x:T)->i64{let a:T=x;let b:T=x;return 0;}fn main()->i64{return twice::<i64>(42);}","O006");
}

#[test]
fn symbolic_calls_require_explicit_exact_entailed_bounds_and_capture() {
    let prefix="trait Read{fn read(value:Self)->i64;}trait Other{fn read(value:Self)->i64;}fn read(n:i64)->i64{return n;}impl Read for i64{read=read;}";
    for (body,code) in [
        ("fn bad<T>(x:T)->i64{return Read::read(x);}","G004"),
        ("fn callee<T:Read>(x:T)->i64{return Read::read(x);}fn bad<T:Other>(x:T)->i64{return callee::<T>(x);}","G004"),
        ("fn callee<T>(x:T)->T{return x;}fn bad<U>(x:U)->i64{callee::<U>(x);return 0;}","O004"),
        ("fn callee<T>(x:T)->T{return x;}fn bad<U>(x:U)->U{return callee(x);}","G006"),
        ("fn callee<T>(x:T)->T{return x;}fn bad<A,B>(x:A)->B{return callee::<A>(x);}","T009"),
    ] { rejects(&format!("{prefix}{body}fn main()->i64{{return 0;}}"),code); }
    verify_single(&format!("{prefix}fn generic<T:Read>(x:T)->i64{{return Read::read(x);}}fn main()->i64{{return generic::<i64>(42);}}"),"",42);
}

#[test]
fn concrete_type_arguments_arity_category_and_nominality_are_exact() {
    for (call, code) in [
        ("id(1)", "G006"),
        ("ordinary::<i64>(1)", "G006"),
        ("missing::<i64>(1)", "G006"),
        ("id::<i64,bool>(1)", "G003"),
        ("id::<bool>(1)", "T009"),
    ] {
        rejects(&format!("fn id<T>(x:T)->T{{return x;}}fn ordinary(x:i64)->i64{{return x;}}fn main()->i64{{return {call};}}"),code);
    }
    for ty in ["tensor", "tensor_error", "&[i64]", "&tensor"] {
        rejects(
            &format!(
                "fn phantom<T>()->i64{{return 0;}}fn main()->i64{{return phantom::<{ty}>();}}"
            ),
            "G003",
        );
    }
    rejects("record A{x:i64}record B{x:i64}fn id<T>(x:T)->T{return x;}fn main()->i64{let a:A=A{x:1};let b:B=id::<B>(a);return 0;}","T009");
    rejects("fn id<T>(x:T)->T{return x;}fn main()->i64{let a:[i64;1]=[1];let b:[i64;2]=id::<[i64;2]>(a);return 0;}","T009");
    rejects(
        "fn id<T>(x:T)->T{return x;}fn main()->i64{let b:[i64;1]=id::<[i64;1]>([1]);return 0;}",
        "I001",
    );
}

#[test]
fn generic_moves_preserve_branches_loops_partial_moves_and_loans() {
    let declarations = "record R{a:[i64;1],b:i64}fn consume<T>(x:T)->bool{return true;}";
    for (body,code) in [
        ("let a:[i64;1]=[1];let x:bool=consume::<[i64;1]>(a);return a[0];","O006"),
        ("let a:[i64;1]=[1];let b:&[i64]=&a;let x:bool=consume::<[i64;1]>(a);return 0;","O007"),
        ("let a:[i64;1]=[1];while consume::<[i64;1]>(a){}return 0;","O011"),
        ("let a:[i64;1]=[1];if true{consume::<[i64;1]>(a);}return a[0];","O006"),
        ("let a:[i64;1]=[1];let b:bool=false&&consume::<[i64;1]>(a);return a[0];","O006"),
        ("let r:R=R{a:[1],b:2};let a:[i64;1]=r.a;consume::<R>(r);return 0;","O006"),
        ("let a:[i64;1]=[1];tensor_scope(0){consume::<[i64;1]>(a);}catch(e:tensor_error){}return 0;","N007"),
    ] { rejects(&format!("{declarations}fn main()->i64{{{body}}}"),code); }
    rejects("fn consume<U>(x:U)->bool{return true;}fn bad<T>(x:T)->i64{let b:bool=false&&consume::<T>(x);let y:T=x;return 0;}fn main()->i64{return 0;}","O006");
    rejects("fn consume<U>(x:U)->bool{return true;}fn bad<T>(x:T)->i64{tensor_scope(0){consume::<T>(x);}catch(e:tensor_error){}return 0;}fn main()->i64{return 0;}","N007");
    verify_single("record R{a:[i64;1],b:[i64;1]}fn id<T>(x:T)->T{return x;}fn main()->i64{let r:R=R{a:[20],b:[22]};let a:[i64;1]=r.a;let b:[i64;1]=r.b;let c:[i64;1]=id::<[i64;1]>(a);return c[0]+b[0];}","",42);
}

#[test]
fn recovery_and_fatal_cleanup_preserve_generic_owned_results() {
    verify_single(
        r#"
record R{x:i64}
fn id<T>(x:T)->T{return x;}
fn recover<T>(x:T)->T{
 tensor_scope(8){let t:tensor=try tensor_zeros([1]);let fails:tensor=try tensor_zeros([1]);}
 catch(e:tensor_error){print(tensor_error_code(e));return id::<T>(x);}
 return x;
}
fn main()->i64{let r:R=R{x:42};let a:R=recover::<R>(r);return a.x;}
"#,
        "9\n",
        42,
    );
    let source="fn fail<T>(x:T)->i64{print(7);return 1 / 0;}fn main()->i64{tensor_scope(8){let t:tensor=try tensor_zeros([1]);let a:[i64;1]=[2];print(fail::<[i64;1]>(a));}catch(e:tensor_error){print(999);}return 0;}";
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
fn generic_exports_enforce_private_and_direct_import_boundaries() {
    rejects_project(
        &[
            (
                "lib.ax",
                "fn hidden<T>(x:T)->T{return x;}export fn ordinary()->i64{return 0;}",
            ),
            (
                "main.ax",
                "import lib;fn main()->i64{return lib::hidden::<i64>(1);}",
            ),
        ],
        "G006",
    );
    rejects_project(
        &[
            ("base.ax", "export fn id<T>(x:T)->T{return x;}"),
            (
                "middle.ax",
                "import base;export fn wrapper()->i64{return base::id::<i64>(1);}",
            ),
            (
                "main.ax",
                "import middle;fn main()->i64{return base::id::<i64>(1);}",
            ),
        ],
        "G006",
    );
    rejects("trait Private{fn f(x:Self)->i64;}export fn bad<T:Private>(x:T)->i64{return Private::f(x);}fn main()->i64{return 0;}","G004");
    rejects("record Private{x:i64}export fn bad<T>(x:Private)->i64{return x.x;}fn main()->i64{return 0;}","I002");
    rejects_project(&[("a.ax","export trait Read{fn read(x:Self)->i64;}"),("b.ax","export trait Read{fn read(x:Self)->i64;}fn read(x:i64)->i64{return x;}impl Read for i64{read=read;}"),("lib.ax","import a;export fn read<T:a::Read>(x:T)->i64{return a::Read::read(x);}"),("main.ax","import b;import lib;fn main()->i64{return lib::read::<i64>(1);}")],"G004");
}

fn phantom_calls(count: usize, body: &str) -> String {
    let mut source = format!(
        "fn nop()->i64{{return 0;}}fn phantom<A,B,C,D,E,F,G,H>()->i64{{{body}}}fn main()->i64{{"
    );
    for index in 0..count {
        let arguments = (0..8)
            .map(|bit| {
                if index == 256 && bit == 0 {
                    "f64"
                } else if index & (1 << bit) == 0 {
                    "i64"
                } else {
                    "bool"
                }
            })
            .collect::<Vec<_>>()
            .join(",");
        write!(source, "phantom::<{arguments}>();").unwrap();
    }
    source.push_str("return 0;}");
    source
}

#[test]
fn distinct_instance_and_expanded_node_limits_are_exact_and_repeatable() {
    let exact = phantom_calls(256, "return 0;");
    let checked = axeon::compile(&exact).unwrap();
    assert_eq!(checked.functions.len(), 258);
    let excess = phantom_calls(257, "return 0;");
    let a = axeon::compile(&excess).unwrap_err();
    let b = axeon::compile(&excess).unwrap_err();
    assert_eq!(a, b);
    assert!(a
        .iter()
        .any(|d| d.code == "G002" && d.message.contains("256 distinct")));
    // Root block(1) + 510 call statements(1020) + return/unary/literal(3) = 1024.
    let body = format!("{}return -1;", "nop();".repeat(510));
    assert!(axeon::compile(&phantom_calls(128, &body)).is_ok());
    let errors = axeon::compile(&phantom_calls(129, &body)).unwrap_err();
    assert!(
        errors
            .iter()
            .any(|d| d.code == "G002" && d.message.contains("131072")),
        "{errors:#?}"
    );
}

fn expansion_chain(count: usize) -> String {
    let mut source = String::new();
    for index in 0..count {
        if index + 1 == count {
            write!(source, "fn f{index}<T>(x:T)->T{{return x;}}").unwrap();
        } else {
            write!(
                source,
                "fn f{index}<T>(x:T)->T{{return f{}::<T>(x);}}",
                index + 1
            )
            .unwrap();
        }
    }
    source.push_str("fn main()->i64{return f0::<i64>(42);}");
    source
}
#[test]
fn expansion_depth_and_combined_function_limits_are_exact() {
    let program = axeon::compile(&expansion_chain(32)).unwrap();
    assert_eq!(program.functions.len(), 33);
    rejects(&expansion_chain(33), "G002");
    let mut source = String::from("fn id<T>(x:T)->T{return x;}");
    for index in 0..1022 {
        write!(source, "fn ordinary_{index}()->i64{{return 0;}}").unwrap();
    }
    source.push_str("fn main()->i64{return id::<i64>(1);}");
    assert_eq!(axeon::compile(&source).unwrap().functions.len(), 1024);
    let excessive = source.replace(
        "return id::<i64>(1);",
        "let x:bool=id::<bool>(true);return id::<i64>(1);",
    );
    rejects(&excessive, "G002");
}

#[test]
fn exact_concrete_storage_and_maximum_frame_recursion_remain_guarded() {
    let fields = (0..8)
        .map(|n| format!("a{n}:[i64;32]"))
        .collect::<Vec<_>>()
        .join(",");
    let array = (0..32).map(|n| n.to_string()).collect::<Vec<_>>().join(",");
    let values = (0..8)
        .map(|n| format!("a{n}:[{array}]"))
        .collect::<Vec<_>>()
        .join(",");
    let recursive = "fn recurse<T>(n:i64,x:T)->T{if n==0{return x;}return recurse::<T>(n-1,x);}";
    let source=format!("record Big{{{fields}}}{recursive}fn make()->Big{{let x:Big=Big{{{values}}};return recurse::<Big>(DEPTH,x);}}fn main()->i64{{let x:Big=make();return x.a7[0];}}");
    // Nested array indexing currently requires projecting the owner first, which
    // would exceed this deliberately maximal frame; inspect a scalar field in
    // a separate consumer frame instead.
    let source = source.replace("return x.a7[0];", "return consume(x);")
        + "fn consume(x:Big)->i64{return 42;}";
    verify_single(&source.replace("DEPTH", "61"), "", 42);
    verify(
        &[("main.ax", &source.replace("DEPTH", "62"))],
        "",
        Expected::Trap {
            kind: TrapKind::CallDepth,
            file: "main.ax",
            marker: recursive,
        },
    );
    let invalid=format!("record Big{{{fields}}}fn bad<T>(x:T)->i64{{let moved:T=x;return 0;}}fn main()->i64{{let x:Big=Big{{{values}}};return bad::<Big>(x);}}");
    rejects(&invalid, "O002");
    // The template itself is valid when unused: no unknown size was invented.
    assert!(axeon::compile(&format!("record Big{{{fields}}}fn bad<T>(x:T)->i64{{let moved:T=x;return 0;}}fn main()->i64{{return 0;}}")).is_ok());
}
