//! Finite nested aggregates: independent answers, rejected programs, and native
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
                std::env::temp_dir().join(format!("axeon-nested-{}-{id}", std::process::id()));
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
fn nested_modules_construct_forward_match_extract_and_borrow() {
    let program = verify(&[
        ("main.ax", r#"import types;import make;import forward;
fn main()->i64{
 let u:types::Outcome=make::run();let v:types::Outcome=forward::pass(u);
 match v{types::Outcome::Data{batch:b}=>{
  print(b.stats.valid);print(b.stats.zero);
  let a:[i64;3]=b.values;let s:types::Stats=b.stats;let view:&[i64]=&a[0..3];
  return view[0]+view[1]+view[2]+s.count;
 },types::Outcome::Empty{}=>{return -1;},}
}"#),
        ("types.ax", "export record Stats{count:i64,zero:f64,valid:bool}export record Batch{values:[i64;3],stats:Stats}export union Outcome{Data{batch:Batch},Empty{}}"),
        ("make.ax", "import types;export fn run()->types::Outcome{return types::Outcome::Data{batch:types::Batch{stats:types::Stats{valid:true,count:3,zero:-0.0},values:[10,20,9]}};}"),
        ("forward.ax", "import types;export fn pass(u:types::Outcome)->types::Outcome{return u;}"),
    ], "true\nf64:0x8000000000000000\n", Expected::Value(42));
    assert_eq!(program.records.len(), 2);
    assert_eq!(program.unions.len(), 1);
}

#[test]
fn nested_float_bits_bool_and_wide_integer_payloads_are_exact() {
    verify_single(r#"
record Bits{flag:bool,real:f64,exact:i64}
record Packet{before:bool,values:[f64;3],bits:Bits,after:bool}
union Result{Value{before:bool,packet:Packet,after:f64},Empty{}}
fn pass(r:Result)->Result{return r;}
fn main()->i64{
 let p:Packet=Packet{before:false,values:[-0.0,f64_from_bits(1),0.0/0.0],bits:Bits{flag:true,real:f64_from_bits(1),exact:9007199254740993},after:true};
 let u:Result=Result::Value{before:true,packet:p,after:-0.0};let v:Result=pass(u);
 match v{Result::Value{before:first,packet:q,after:last}=>{
  print(first);print(q.before);print(q.bits.flag);print(q.after);print(q.bits.exact);print(q.bits.real);print(last);
  let a:[f64;3]=q.values;print(a[0]);print(a[1]);print(a[2]);return f64_bits(a[2]);
 },Result::Empty{}=>{return -1;},}
}"#, "true\nfalse\ntrue\ntrue\n9007199254740993\nf64:0x0000000000000001\nf64:0x8000000000000000\nf64:0x8000000000000000\nf64:0x0000000000000001\nf64:0x7ff8000000000000\n", 9221120237041090560);
}

#[test]
fn deep_scalar_paths_and_disjoint_partial_moves_preserve_written_effect_order() {
    verify_single(
        r#"
record Leaf{x:i64,flag:bool}
record Pair{left:Leaf,right:Leaf}
record Root{pair:Pair,values:[i64;2],tail:i64}
fn mark(n:i64)->i64{print(n);return n;}
fn take(x:Leaf)->i64{return x.x;}
fn main()->i64{
 let mut r:Root=Root{tail:mark(1),pair:Pair{right:Leaf{flag:true,x:mark(2)},left:Leaf{x:mark(3),flag:false}},values:[mark(4),mark(5)]};
 r.pair.left.x=20;r.pair.right.x=22;r.pair.right.flag=false;
 let a:Leaf=r.pair.left;r.pair.right.x=r.pair.right.x+1;
 let b:Leaf=r.pair.right;print(b.flag);print(r.tail);
 let values:[i64;2]=r.values;let view:&[i64]=&values[0..2];print(view[1]);
 return take(a)+take(b)-1;
}"#,
        "1\n2\n3\n4\n5\nfalse\n1\n5\n",
        42,
    );
}

#[test]
fn nested_constructor_calls_and_traps_keep_source_order() {
    verify_single("record R{x:i64}record O{a:R,b:R}fn make(n:i64)->R{print(n);return R{x:n};}fn main()->i64{let o:O=O{b:make(22),a:make(20)};return o.a.x+o.b.x;}", "22\n20\n", 42);
    let source = "record R{x:i64}record O{a:R,b:R,c:i64}fn mark(n:i64)->i64{print(n);return n;}fn fail()->R{return R{x:9223372036854775807 + 1};}fn main()->i64{let o:O=O{c:mark(1),a:fail(),b:R{x:mark(2)}};return 0;}";
    verify(
        &[("main.ax", source)],
        "1\n",
        Expected::Trap {
            kind: TrapKind::Overflow,
            file: "main.ax",
            marker: "9223372036854775807 + 1",
        },
    );
}

#[test]
fn partial_moves_join_continuing_branches_matches_and_iteration_local_owners() {
    verify_single(
        r#"
record R{x:i64}record P{a:R,b:R}union Choice{Yes{},No{}}
fn choose(flag:bool)->i64{
 let p:P=P{a:R{x:20},b:R{x:22}};
 if flag{let a:R=p.a;return a.x;}else{let b:R=p.b;print(b.x);}
 let a:R=p.a;return a.x;
}
fn by_match(c:Choice)->i64{
 let p:P=P{a:R{x:20},b:R{x:22}};
 match c{Choice::Yes{}=>{let a:R=p.a;return a.x;},Choice::No{}=>{let b:R=p.b;print(b.x);},}
 return p.a.x;
}
fn main()->i64{
 print(choose(true));print(choose(false));let c:Choice=Choice::No{};print(by_match(c));
 let mut total:i64=0;let mut n:i64=0;
 while n<2{let p:P=P{a:R{x:20},b:R{x:1}};let a:R=p.a;total=total+a.x+p.b.x;n=n+1;}
 return total;
}"#,
        "20\n22\n20\n22\n20\n",
        42,
    );
}

const OWNERS: &str = "record R{x:i64}record P{a:R,b:R,arr:[i64;1]}fn make()->P{return P{a:R{x:20},b:R{x:22},arr:[1]};}fn take(p:P)->i64{return p.a.x;}fn take_r(r:R)->i64{return r.x;}";
#[test]
fn partial_move_overlap_loops_capture_and_assignment_failures_are_rejected() {
    for (body, code) in [
        ("let p:P=make();let a:R=p.a;return p.a.x;", "O006"),
        ("let p:P=make();let a:R=p.a;return take(p);", "O006"),
        ("let p:P=make();let a:R=p.a;let b:R=p.a;return 0;", "O006"),
        ("let p:P=make();if true{let a:R=p.a;}return p.a.x;", "O006"),
        ("let p:P=make();while false{let a:R=p.a;}return 0;", "O011"),
        ("let p:P=make();tensor_scope(0){let a:R=p.a;}catch(e:tensor_error){}return 0;", "N007"),
        ("let mut p:P=make();p.a.x=take(p);return 0;", "O006"),
        ("let p:P=make();p.a.x=1;return 0;", "O010"),
        ("let mut p:P=make();p.a=R{x:1};return 0;", "O008"),
        ("let mut p:P=make();let a:R=p.a;p.a=R{x:1};return 0;", "O006"),
        ("let p:P=make();return take_r(p.a);", "I001"),
        ("let p:P=make();print(p.a);return 0;", "O004"),
        ("let p:P=make();let a:[i64;2]=p.arr;return 0;", "T009"),
        ("let p:P=make();let a:[i64;1]=p.arr;let v:&[i64]=&a;let q:P=P{a:p.a,b:p.b,arr:a};return 0;", "O007"),
    ] { rejects(&format!("{OWNERS}fn main()->i64{{{body}}}"), code); }
    rejects("record R{x:i64}record P{a:R}record O{p:P}fn main()->i64{let o:O=O{p:P{a:R{x:1}}};let a:R=o.p.a;let p:P=o.p;return 0;}","O006");
    rejects(
        "record R{x:i64}record S{x:i64}record P{a:R}fn main()->i64{let p:P=P{a:S{x:1}};return 0;}",
        "T009",
    );
}

#[test]
fn nested_public_visibility_is_checked_on_every_definition_edge() {
    for source in [
        "record R{x:i64}export record P{r:R}fn main()->i64{return 0;}",
        "union U{Empty{}}export record P{u:U}fn main()->i64{return 0;}",
        "record R{x:i64}export union U{V{r:R}}fn main()->i64{return 0;}",
        "record R{x:i64}export record P{r:R}export union U{V{p:P}}fn main()->i64{return 0;}",
    ] {
        rejects(source, "I002");
    }
    rejects_project(
        &[
            ("main.ax", "import outer;fn main()->i64{return 0;}"),
            (
                "outer.ax",
                "record Hidden{x:i64}export record Outer{value:Hidden}",
            ),
        ],
        "I002",
    );
    rejects_project(
        &[
            (
                "main.ax",
                "import outer;record Local{x:outer::Hidden}fn main()->i64{return 0;}",
            ),
            (
                "outer.ax",
                "record Hidden{x:i64}export record Outer{value:i64}",
            ),
        ],
        "E_PARSE_TYPE",
    );
}

fn chain(depth: usize) -> String {
    let mut source = String::from("record R0{value:i64}");
    for index in 1..depth {
        write!(source, "record R{index}{{next:R{}}}", index - 1).unwrap();
    }
    source
}
#[test]
fn finite_layout_cycles_depth_and_expansion_have_bounded_diagnostics() {
    for source in [
        "record R{x:R}fn main()->i64{return 0;}",
        "record R{u:U}union U{Empty{},Value{r:R}}fn main()->i64{return 0;}",
        "union U{None{},Some{value:U}}fn main()->i64{return 0;}",
        "record A{b:B}record B{c:C}record C{a:A}fn main()->i64{return 0;}",
    ] {
        rejects(source, "A001");
    }
    rejects(&format!("{}fn main()->i64{{return 0;}}", chain(33)), "A002");
    let mut value = String::from("R0{value:42}");
    for index in 1..32 {
        value = format!("R{index}{{next:{value}}}");
    }
    verify_single(
        &format!(
            "{}fn main()->i64{{let x:R31={value};let leaf:R0=x.{};return leaf.value;}}",
            chain(32),
            ["next"; 31].join(".")
        ),
        "",
        42,
    );
    let oversized="record Leaf{a:[i64;32],b:[i64;32],c:[i64;32],d:[i64;32],e:[i64;32],f:[i64;32],g:[i64;32],h:[i64;32]}record TooBig{value:Leaf,extra:bool}fn main()->i64{return 0;}";
    rejects(oversized, "A003");
    let mut exponential = String::from("record R0{x:i64}");
    for index in 1..16 {
        write!(
            exponential,
            "record R{index}{{a:R{},b:R{}}}",
            index - 1,
            index - 1
        )
        .unwrap();
    }
    exponential.push_str("fn main()->i64{return 0;}");
    rejects(&exponential, "A003");
    rejects("record R{x:[i64;0]}fn main()->i64{return 0;}", "O001");
    rejects("record R{x:&[i64]}fn main()->i64{return 0;}", "R005");
    rejects("union U{V{x:tensor}}fn main()->i64{return 0;}", "U005");
}

#[test]
fn pattern_owner_storage_is_charged_for_every_arm() {
    for (variants, accepted) in [(6, true), (7, false)] {
        let mut declaration = String::from("union U{");
        let mut arms = String::new();
        for index in 0..variants {
            write!(declaration, "V{index}{{a:[i64;32]}},").unwrap();
            write!(arms, "U::V{index}{{a:a}}=>{{return a[31];}},").unwrap();
        }
        declaration.push('}');
        let values = (0..32).map(|x| x.to_string()).collect::<Vec<_>>().join(",");
        let source = format!(
            "{declaration}fn main()->i64{{let u:U=U::V0{{a:[{values}]}};match u{{{arms}}}}}"
        );
        if accepted {
            verify_single(&source, "", 31);
        } else {
            rejects(&source, "O002");
        }
    }
}

#[test]
fn old_256_slot_scalar_records_and_new_nested_frames_remain_accepted() {
    let fields = (0..32)
        .map(|n| format!("f{n}:i64"))
        .collect::<Vec<_>>()
        .join(",");
    let values = (0..32)
        .map(|n| format!("f{n}:{n}"))
        .collect::<Vec<_>>()
        .join(",");
    let mut old = format!("record R{{{fields}}}fn main()->i64{{");
    for n in 0..8 {
        write!(old, "let r{n}:R=R{{{values}}};").unwrap();
    }
    old.push_str("return r0.f31+r7.f11;}");
    verify_single(&old, "", 42);
    let fields = (0..8)
        .map(|n| format!("a{n}:[i64;32]"))
        .collect::<Vec<_>>()
        .join(",");
    let array = (0..32).map(|n| n.to_string()).collect::<Vec<_>>().join(",");
    let values = (0..8)
        .map(|n| format!("a{n}:[{array}]"))
        .collect::<Vec<_>>()
        .join(",");
    let recurse = "fn recurse(n:i64,x:Big)->Big{if n==0{return x;}return recurse(n-1,x);}";
    let source=format!("record Big{{{fields}}}{recurse}fn make()->Big{{let x:Big=Big{{{values}}};return recurse(DEPTH,x);}}fn consume(x:Big)->i64{{return 42;}}fn main()->i64{{let x:Big=make();return consume(x);}}");
    verify_single(&source.replace("DEPTH", "61"), "", 42);
    verify(
        &[("main.ax", &source.replace("DEPTH", "62"))],
        "",
        Expected::Trap {
            kind: TrapKind::CallDepth,
            file: "main.ax",
            marker: recurse,
        },
    );
    rejects(&format!("record Big{{{fields}}}fn main()->i64{{let x:Big=Big{{{values}}};let a:[i64;32]=x.a0;return 0;}}"),"O002");
}

#[test]
fn nested_recovery_returns_and_fatal_cleanup_preserve_payloads() {
    verify_single(
        r#"
record R{real:f64,code:i64}record P{r:R,values:[i64;1]}union U{V{p:P}}
fn forward(u:U)->U{return u;}
fn recover()->U{
 tensor_scope(8){let t:tensor=try tensor_zeros([1]);let fails:tensor=try tensor_zeros([1]);}
 catch(e:tensor_error){let p:P=P{r:R{real:-0.0,code:tensor_error_code(e)},values:[33]};let u:U=U::V{p:p};return forward(u);}
 return U::V{p:P{r:R{real:1.0,code:-1},values:[0]}};
}

fn main()->i64{let u:U=recover();match u{U::V{p:p}=>{print(p.r.real);let a:[i64;1]=p.values;return p.r.code+a[0];},}}
"#,
        "f64:0x8000000000000000\n",
        42,
    );
    let source="record R{x:i64}record P{r:R}fn fail(p:P)->i64{return p.r.x / 0;}fn main()->i64{tensor_scope(8){let t:tensor=try tensor_zeros([1]);let p:P=P{r:R{x:1}};print(tensor_len(t));print(fail(p));}catch(e:tensor_error){print(999);}return 0;}";
    verify(
        &[("main.ax", source)],
        "1\n",
        Expected::Trap {
            kind: TrapKind::DivisionByZero,
            file: "main.ax",
            marker: "p.r.x / 0",
        },
    );
}

#[test]
fn corrupted_tag_with_nested_payload_retains_match_trap_and_constructor_effects() {
    let source="record R{x:f64,a:[i64;2]}union U{Empty{},V{flag:bool,r:R}}fn mark()->f64{print(7);return -0.0;}fn main()->i64{let u:U=U::V{r:R{x:mark(),a:[20,22]},flag:true};match u{U::Empty{}=>{return -1;},U::V{flag:b,r:r}=>{return 999;},}}";
    let mut program = axeon::compile(source).unwrap();
    let function = &mut program.functions[1];
    let axeon::ir::StmtKind::Let { value, .. } = &mut function.body.stmts[0].kind else {
        panic!("union let")
    };
    let axeon::ir::ExprKind::Union { variant, .. } = &mut value.kind else {
        panic!("union constructor")
    };
    *variant = 77;
    let expected_span = function.body.stmts[1].span;
    let interpreted = interpreter::run(&program, 1_000_000);
    assert_eq!(interpreted.stdout, "7\n");
    let trap = interpreted.result.unwrap_err();
    assert_eq!(trap.kind, TrapKind::InvalidUnionTag);
    assert_eq!(trap.span, expected_span);
    let directory = Temp::new();
    fs::write(
        directory.0.join("invalid.ll"),
        support::emit(&program, source),
    )
    .unwrap();
    let clang = std::env::var_os("AXEON_CLANG")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("clang-19"));
    for opt in ["0", "2", "3"] {
        let executable = support::executable_path(directory.0.join(format!("invalid-O{opt}")));
        let compiled = output(
            bounded(&clang, "40s")
                .args([
                    "-target",
                    support::target_triple(),
                    "-Werror",
                    "-Wno-override-module",
                ])
                .arg(format!("-O{opt}"))
                .arg(directory.0.join("invalid.ll"))
                .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("runtime/runtime.c"))
                .arg("-std=c11")
                .args(support::math_link_flags())
                .args(support::clang_link_flags())
                .arg("-o")
                .arg(&executable),
        );
        assert!(
            compiled.status.success(),
            "{}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(native.status.code(), Some(70));
        assert_eq!(native.stdout, b"7\n");
        assert_eq!(native.stderr, trap.render(source).as_bytes());
    }
}
