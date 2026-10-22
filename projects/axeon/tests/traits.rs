//! Concrete static traits: independent answers, rejected programs, and native
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
                std::env::temp_dir().join(format!("axeon-traits-{}-{id}", std::process::id()));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create trait fixture: {error}"),
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
    let result = command.output().expect("trait subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "trait subprocess timed out"
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
    let project = directory.load().expect("load trait fixture");
    let program = project
        .check()
        .unwrap_or_else(|errors| panic!("{errors:#?}\n{files:#?}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent trait effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent trait result");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap { kind, file, marker } => {
            let source = files.iter().find(|(name, _)| *name == file).unwrap().1;
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted
                .result
                .expect_err("trait scalar operation traps");
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
            "trait build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "trait exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "trait stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "trait stderr O{opt}");
    }
    program
}
fn verify_single(source: &str, effects: &str, value: i64) {
    verify(&[("main.ax", source)], effects, Expected::Value(value));
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid trait program must not expose IR");
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
fn static_dispatch_uses_exact_concrete_contracts_and_existing_owned_abi() {
    verify_single(
        r#"
trait Consume { fn consume(value: Self, extra: i64) -> i64; }
trait Forward { fn forward(value: Self) -> Self; }
record Packet { numbers: [i64; 2], flag: bool }
union Choice { Value { packet: Packet }, Empty {} }
fn packet(value: Packet, extra: i64) -> i64 { let numbers: [i64; 2] = value.numbers; return numbers[0] + numbers[1] + extra; }
fn number(value: i64, extra: i64) -> i64 { return value + extra; }
fn array(value: [i64; 2], extra: i64) -> i64 { return value[0] + value[1] + extra; }
fn unit(value: (), extra: i64) -> i64 { if value == () { return extra; } return 0; }
fn choice(value: Choice) -> Choice { return value; }
impl Consume for Packet { consume = packet; }
impl Consume for i64 { consume = number; }
impl Consume for [i64; 2] { consume = array; }
impl Consume for () { consume = unit; }
impl Forward for Choice { forward = choice; }
fn main() -> i64 {
 let p: Packet = Packet { numbers: [10, 20], flag: true };
 let c: Choice = Choice::Value { packet: p };
 let d: Choice = Forward::forward(c);
 let n: i64 = 3; let a: [i64; 2] = [1, 2]; let u: () = ();
 match d { Choice::Value { packet: q } => { return Consume::consume(q, Consume::consume(n, Consume::consume(a, Consume::consume(u, 6)))); }, Choice::Empty {} => { return 0; } }
}
"#,
        "",
        42,
    );
}

#[test]
fn imported_traits_are_nominal_and_bind_private_local_functions() {
    verify(&[
        ("contract.ax", "export trait Read { fn read(value: Self) -> i64; }"),
        ("other.ax", "export trait Read { fn read(value: Self) -> i64; } export fn read(value:i64)->i64{return value;}"),
        ("data.ax", r#"
import contract; import other;
export record Packet { value: i64 }
fn read(value: Packet) -> i64 { return value.value; }
fn alternate(value: Packet) -> i64 { return value.value + 1; }
impl contract::Read for Packet { read = read; }
impl other::Read for Packet { read = alternate; }
export fn make(value: i64) -> Packet { return Packet { value: value }; }
"#),
        ("left.ax", "import data; export fn make()->data::Packet{return data::make(20);}"),
        ("right.ax", "import data; export fn make()->data::Packet{return data::make(21);}"),
        ("main.ax", r#"
import contract; import other; import data; import left; import right;
fn main()->i64 { let a:data::Packet=left::make(); let b:data::Packet=right::make(); return contract::Read::read(a)+other::Read::read(b); }
"#),
    ], "", Expected::Value(42));
}

#[test]
fn dispatch_preserves_written_argument_effects_and_trap_locations() {
    let source = r#"
trait Apply { fn apply(receiver: Self, value: i64, tail: i64) -> i64; }
record R { value: i64 }
fn apply(receiver:R,value:i64,tail:i64)->i64 { print(receiver.value); return value + tail; }
impl Apply for R { apply = apply; }
fn effect(value:i64)->i64 { print(value); return value; }
fn main()->i64 { let r:R=R{value:3}; return Apply::apply(r,effect(1),effect(2)); }
"#;
    verify_single(source, "1\n2\n3\n", 3);
    let trapped = source.replace("effect(2)", "7 / 0");
    verify(
        &[("main.ax", &trapped)],
        "1\n",
        Expected::Trap {
            kind: TrapKind::DivisionByZero,
            file: "main.ax",
            marker: "7 / 0",
        },
    );
}

#[test]
fn methods_preserve_numeric_payloads_and_recursive_forwarding() {
    verify_single(
        r#"
trait Forward { fn forward(value: Self, depth:i64) -> Self; }
record Payload { wide:i64, zero:f64, tiny:f64, nan:f64, flag:bool, done:() }
fn forward(value:Payload,depth:i64)->Payload { if depth == 0 { return value; } return Forward::forward(value,depth-1); }
impl Forward for Payload { forward = forward; }
fn main()->i64 {
 let p:Payload=Payload{wide:9007199254740993,zero:-0.0,tiny:5e-324,nan:f64_from_bits(9221120237041090561),flag:true,done:()};
 let q:Payload=Forward::forward(p,8);
 if q.wide != 9007199254740993 { return 1; }
 if f64_bits(q.zero) != -9223372036854775808 { return 2; }
 if f64_bits(q.tiny) != 1 { return 3; }
 if f64_bits(q.nan) != 9221120237041090560 { return 4; }
 if !q.flag || q.done != () { return 5; }
 return 0;
}
"#,
        "",
        0,
    );
}

#[test]
fn exact_signatures_missing_methods_and_coherence_are_checked_even_when_unused() {
    let pre = "trait Take {fn take(v:Self)->i64;} fn take(v:i64)->i64{return v;} fn main()->i64{return 0;} ";
    for tail in [
        "impl Take for i64 {}",
        "impl Take for i64 {take=missing;}",
        "impl Take for i64 {bogus=take;}",
        "impl Take for i64 {take=take;take=take;}",
        "impl Take for f64 {take=take;}",
    ] {
        rejects(&format!("{pre}{tail}"), "TR006");
    }
    rejects(
        &format!("{pre}impl Take for i64 {{take=take;}} impl Take for i64 {{take=take;}}"),
        "TR005",
    );
    rejects("trait T {fn t(v:Self)->Self;} fn t(v:i64)->bool{return true;} impl T for i64{t=t;} fn main()->i64{return 0;}","TR006");
    rejects("trait T {fn t(v:Self)->i64;} fn t(v:[i64;2])->i64{return 0;} impl T for [i64;3]{t=t;} fn main()->i64{return 0;}","TR006");
    rejects("record A{x:i64} record B{x:i64} trait T{fn t(v:Self)->i64;} fn t(v:B)->i64{return 0;} impl T for A{t=t;} fn main()->i64{return 0;}","TR006");
    rejects("trait T{fn t(v:Self)->i64;} fn t(v:i64)->i64{return missing;} impl T for i64{t=t;} fn main()->i64{return 0;}","T005");
}

#[test]
fn declarations_receivers_and_unsupported_forms_have_stable_diagnostics() {
    for (source, code) in [
        ("trait T{}", "TR001"),
        ("trait T{fn f(v:i64)->i64;}", "TR003"),
        ("trait T{fn f()->i64;}", "TR003"),
        ("trait T{fn f(v:Self)->tensor;}", "TR003"),
        ("trait T{fn f(v:Self,x:&[i64])->i64;}", "TR003"),
        ("trait T{fn f(v:Self,v:i64)->i64;}", "T004"),
        ("trait T{fn f(v:Self)->i64;fn f(v:Self)->i64;}", "TR002"),
        ("record T{x:i64} trait T{fn f(v:Self)->i64;}", "TR002"),
        (
            "trait T{fn f(v:Self)->i64;} trait T{fn g(v:Self)->i64;}",
            "TR002",
        ),
        ("impl Unknown for i64{}", "TR004"),
    ] {
        rejects(&format!("{source} fn main()->i64{{return 0;}}"), code);
    }
    rejects(
        "trait T{fn f(v:Self)->i64;} fn main()->i64{let v:i64=1;return T::f(v);}",
        "TR007",
    );
    rejects(
        "trait T{fn f(v:Self)->i64;} fn main()->i64{return T::f(1);}",
        "TR008",
    );
    rejects(
        "trait T{fn f(v:Self)->i64;} fn main()->i64{return T::f();}",
        "TR008",
    );
    rejects(
        "trait T{fn f(v:Self)->i64;} fn main()->i64{return T::f(missing);}",
        "T005",
    );
    for source in [
        "trait T{fn f(v:Self)->i64{ return 0; }}",
        "trait T<X>{fn f(v:Self)->i64;}",
        "trait T{fn f(v:Self)->i64;} export impl T for i64{}",
        "trait T{fn f(v:Self)->i64;} impl T for i64{f=module::f;}",
    ] {
        assert!(axeon::compile(&format!("{source} fn main()->i64{{return 0;}}")).is_err());
    }
}

#[test]
fn direct_import_privacy_orphans_and_public_signature_edges_are_enforced() {
    rejects_project(
        &[
            ("hidden.ax", "trait T{fn f(v:Self)->i64;}"),
            (
                "main.ax",
                "import hidden; impl hidden::T for i64{} fn main()->i64{return 0;}",
            ),
        ],
        "TR004",
    );
    rejects_project(
        &[
            ("traits.ax", "export trait T{fn f(v:Self)->i64;}"),
            (
                "relay.ax",
                "import traits; export fn relay()->i64{return 0;}",
            ),
            (
                "main.ax",
                "import relay; impl traits::T for i64{} fn main()->i64{return 0;}",
            ),
        ],
        "TR004",
    );
    rejects_project(&[("traits.ax","export trait T{fn f(v:Self)->i64;}"),("main.ax","import traits; fn f(v:i64)->i64{return v;} impl traits::T for i64{f=f;} fn main()->i64{return 0;}")],"TR005");
    rejects_project(&[("types.ax","export record R{x:i64}"),("traits.ax","export trait T{fn f(v:Self)->i64;}"),("main.ax","import types; import traits; fn f(v:types::R)->i64{return v.x;} impl traits::T for types::R{f=f;} fn main()->i64{return 0;}")],"TR005");
    rejects_project(&[("main.ax","record Hidden{x:i64} export trait T{fn f(v:Self,x:Hidden)->i64;} fn main()->i64{return 0;}")],"I002");
    rejects_project(
        &[
            ("mod.ax", "export fn f()->i64{return 0;}"),
            (
                "main.ax",
                "import mod; trait mod{fn f(v:Self)->i64;} fn main()->i64{return 0;}",
            ),
        ],
        "TR002",
    );
    rejects_project(&[("traits.ax","export trait T{fn f(v:Self)->i64;} fn f(v:i64)->i64{return v;} impl T for i64{f=f;}"),("main.ax","import traits; fn main()->i64{return traits::f(1);}")],"T007");
}

#[test]
fn moves_loans_partial_owners_loops_and_protected_captures_are_not_bypassed() {
    let pre = "record R{a:[i64;1],b:i64} trait T{fn f(v:Self,n:i64)->i64;} fn f(v:R,n:i64)->i64{return n;} impl T for R{f=f;}";
    for body in [
        "let r:R=R{a:[1],b:2};let n:i64=T::f(r,r.b);return n;",
        "let r:R=R{a:[1],b:2};let n:i64=T::f(r,0);return r.b;",
        "let r:R=R{a:[1],b:2};let a:[i64;1]=r.a;return T::f(r,0);",
    ] {
        rejects(&format!("{pre} fn main()->i64{{{body}}}"), "O006");
    }
    rejects(
        &format!("{pre} fn main()->i64{{let r:R=R{{a:[1],b:2}};while T::f(r,0)==0{{}}return 0;}}"),
        "O011",
    );
    let arrays =
        "trait T{fn f(v:Self)->i64;} fn f(v:[i64;1])->i64{return v[0];} impl T for [i64;1]{f=f;}";
    rejects(
        &format!("{arrays} fn main()->i64{{let a:[i64;1]=[1];let s:&[i64]=&a;return T::f(a);}}"),
        "O007",
    );
    let protected=format!("{pre} fn main()->i64{{let r:R=R{{a:[1],b:2}};tensor_scope(0) {{let n:i64=T::f(r,0);}} catch(e:tensor_error){{return 1;}}return 0;}}");
    rejects(&protected, "N007");
    verify_single(&format!("{pre} fn main()->i64{{let mut result:i64=0;let mut i:i64=0;while i<2{{let r:R=R{{a:[1],b:2}};result=result+T::f(r,21);i=i+1;}}return result;}}"),"",42);
}

#[test]
fn contextual_identifiers_and_existing_qualified_paths_remain_compatible() {
    verify(
        &[
            (
                "trait.ax",
                "export fn impl(for:i64)->i64{return for;} export union Self{impl{for:i64}}",
            ),
            (
                "main.ax",
                r#"
import trait;
record Self{trait:i64,impl:i64,for:i64}
fn impl(trait:i64,for:i64)->i64{return trait+for;}
fn main()->i64{
 let for:Self=Self{trait:10,impl:11,for:12};
 let v:trait::Self=trait::Self::impl{for:9};
 match v{trait::Self::impl{for:trait}=>{return impl(for.trait,for.impl)+trait::impl(for.for)+trait;}}
}
"#,
            ),
        ],
        "",
        Expected::Value(42),
    );
}

#[test]
fn resource_limits_include_unused_contracts_and_keep_old_maximum_frames() {
    let mut fields = String::new();
    let mut values = String::new();
    for index in 0..32 {
        write!(fields, "f{index}:i64,").unwrap();
        write!(values, "f{index}:1,").unwrap();
    }
    let mut source=format!("record R{{{fields}}} trait T{{fn f(v:Self)->i64;}} fn f(v:R)->i64{{return v.f0;}} impl T for R{{f=f;}} fn main()->i64{{");
    for index in 0..8 {
        write!(source, "let r{index}:R=R{{{values}}};").unwrap();
    }
    source.push_str("return T::f(r7);}");
    verify_single(&source, "", 1);
    rejects(
        &source.replace(
            "return T::f(r7);",
            &format!("let extra:R=R{{{values}}};return T::f(r7);"),
        ),
        "O002",
    );
    let contracts = (0..64).fold(String::new(), |mut text, index| {
        write!(text, "trait T{index}{{fn f(v:Self)->i64;}}").unwrap();
        text
    });
    assert!(axeon::compile(&format!("{contracts}fn main()->i64{{return 0;}}")).is_ok());
    rejects(
        &format!("{contracts}trait Extra{{fn f(v:Self)->i64;}}fn main()->i64{{return 0;}}"),
        "TR001",
    );
    let methods = (0..32).fold(String::new(), |mut text, index| {
        write!(text, "fn f{index}(v:Self)->i64;").unwrap();
        text
    });
    assert!(axeon::compile(&format!("trait T{{{methods}}}fn main()->i64{{return 0;}}")).is_ok());
    rejects(
        &format!("trait T{{{methods}fn extra(v:Self)->i64;}}fn main()->i64{{return 0;}}"),
        "TR001",
    );
    let params = (0..32).fold(String::new(), |mut text, index| {
        write!(text, ",p{index}:i64").unwrap();
        text
    });
    rejects(
        &format!("trait T{{fn f(v:Self{params})->i64;}}fn main()->i64{{return 0;}}"),
        "T014",
    );
}

#[test]
fn trait_methods_return_after_recovery_and_preserve_fatal_cleanup() {
    verify_single(
        r#"
trait Recover { fn recover(value:Self)->Self; }
record R { value:i64 }
fn recover(value:R)->R {
 tensor_scope(8) { let t:tensor=try tensor_zeros([1]); let fail:tensor=try tensor_zeros([1]); }
 catch(e:tensor_error) { print(tensor_error_code(e)); return value; }
 return value;
}
impl Recover for R { recover=recover; }
fn main()->i64 { let r:R=R{value:42}; let result:R=Recover::recover(r); return result.value; }
"#,
        "9\n",
        42,
    );
    let source="trait Fail{fn fail(value:Self)->();} fn fail(value:())->(){print(7);let n:i64=1 / 0;return value;}impl Fail for (){fail=fail;}fn main()->i64{let value:()=();tensor_scope(8){let t:tensor=try tensor_zeros([1]);Fail::fail(value);}catch(e:tensor_error){print(999);}return 0;}";
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
fn canonical_trait_metadata_is_relocatable_and_rehashed_independently() {
    let files=[
        ("contracts.ax","export trait Get { fn get(value:Self)->i64; }"),
        ("data.ax","import contracts;export record R{x:i64}fn read(value:R)->i64{return value.x;}impl contracts::Get for R{get=read;}"),
        ("main.ax","import data;import contracts;fn main()->i64{let r:data::R=data::R{x:42};return contracts::Get::get(r);}"),
    ];
    let first = Temp::new();
    first.write(&files);
    let second = Temp::new();
    second.write(&files);
    for directory in [&first, &second] {
        let built = output(
            bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .current_dir(&directory.0)
                .args(["build", "main.ax", "-o", "program"]),
        );
        assert!(
            built.status.success(),
            "{}",
            String::from_utf8_lossy(&built.stderr)
        );
    }
    assert_eq!(
        fs::read(first.0.join("program.json")).unwrap(),
        fs::read(second.0.join("program.json")).unwrap(),
        "physical relocation must not alter metadata"
    );
    let checked=output(bounded(&support::python(),"10s").current_dir(&first.0).args(["-c",r#"
import hashlib,json
from pathlib import Path
d=json.loads(Path('program.json').read_text())
m={x['path']:x for x in d['modules']}
compact=lambda x:json.dumps(x,separators=(',',':'),ensure_ascii=False)
digest=lambda domain,x:hashlib.sha256((domain+'\n'+compact(x)).encode()).hexdigest()
c=m['contracts.ax']; a=m['data.ax']
assert c['interface_schema']==a['interface_schema']==3
assert m['main.ax']['interface_schema']==1
assert 'trait_exports' not in m['main.ax']
t=c['trait_exports'][0]
assert t['definition']=={'kind':'trait','module':'contracts.ax','name':'Get','methods':[{'name':'get','params':[{'kind':'self'}],'result':'i64'}]}
assert t['definition_sha256']==digest('axeon-export-trait-v1',t['definition'])
i=a['implementations'][0]
assert i['trait']=={'module':'contracts.ax','name':'Get','definition_sha256':t['definition_sha256']}
assert i['target']['definition_sha256']==a['type_exports'][0]['definition_sha256']
assert i['methods']==[{'method':'get','function':'read'}]
for module in [c,a]:
    shape={'module':module['path'],'types':[v['definition'] for v in module['type_exports']],'functions':[v['signature'] for v in module['exports']],'traits':[v['definition'] for v in module['trait_exports']],'implementations':module['implementations']}
    assert module['interface_sha256']==digest('axeon-module-interface-v3',shape)
"#]));
    assert!(
        checked.status.success(),
        "{}",
        String::from_utf8_lossy(&checked.stderr)
    );
    let old = fs::read(first.0.join("program.json")).unwrap();
    fs::write(
        first.0.join("data.ax"),
        files[1].1.replace("return value.x;", "return value.x+1;"),
    )
    .unwrap();
    let built = output(
        bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
            .current_dir(&first.0)
            .args(["build", "main.ax", "-o", "program"]),
    );
    assert!(built.status.success());
    fs::write(first.0.join("old.json"), old).unwrap();
    let checked=output(bounded(&support::python(),"10s").current_dir(&first.0).args(["-c","import json;from pathlib import Path;a=json.loads(Path('old.json').read_text());b=json.loads(Path('program.json').read_text());ma={m['path']:m for m in a['modules']};mb={m['path']:m for m in b['modules']};assert ma['data.ax']['interface_sha256']==mb['data.ax']['interface_sha256'];assert ma['data.ax']['dependency_sha256']!=mb['data.ax']['dependency_sha256'];assert ma['main.ax']['dependency_sha256']!=mb['main.ax']['dependency_sha256']"]));
    assert!(
        checked.status.success(),
        "{}",
        String::from_utf8_lossy(&checked.stderr)
    );
}

#[test]
fn project_trait_and_concrete_implementation_boundaries_are_distinct() {
    let mut source=String::from("fn si(v:i64)->i64{return 0;}fn sf(v:f64)->i64{return 0;}fn sb(v:bool)->i64{return 0;}fn su(v:())->i64{return 0;}");
    for index in 0..64 {
        write!(source, "trait T{index}{{fn f(v:Self)->i64;}}").unwrap();
        for (ty, function) in [("i64", "si"), ("f64", "sf"), ("bool", "sb"), ("()", "su")] {
            write!(source, "impl T{index} for {ty}{{f={function};}}").unwrap();
        }
    }
    source.push_str("fn main()->i64{let x:()=();return T63::f(x);}");
    assert!(
        axeon::compile(&source).is_ok(),
        "256 exact concrete implementation pairs are accepted"
    );
    rejects(
        &format!("{source}fn sa(v:[i64;1])->i64{{return 0;}}impl T0 for [i64;1]{{f=sa;}}"),
        "TR001",
    );
    let declarations = |prefix: &str, count| {
        (0..count).fold(String::new(), |mut text, index| {
            write!(text, "export trait {prefix}{index}{{fn f(v:Self)->i64;}}").unwrap();
            text
        })
    };
    rejects_project(
        &[
            ("left.ax", &declarations("L", 33)),
            ("right.ax", &declarations("R", 32)),
            (
                "main.ax",
                "import left;import right;fn main()->i64{return 0;}",
            ),
        ],
        "TR001",
    );
    let params = (0..31).fold(String::new(), |mut text, index| {
        write!(text, ",p{index}:i64").unwrap();
        text
    });
    assert!(axeon::compile(&format!(
        "trait T{{fn f(v:Self{params})->i64;}}fn main()->i64{{return 0;}}"
    ))
    .is_ok());
}
