//! Finite nominal applications and the common opaque/concrete body checker.
//! Native evidence is specific to the exercised hosted target at O0/O2/O3.
mod support;
use axeon::diagnostic::line_col;
use axeon::interpreter::{self, TrapKind};
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
            let path = std::env::temp_dir().join(format!(
                "axeon-generic-nominals-{}-{id}",
                std::process::id()
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("cannot create nominal fixture: {error}"),
            }
        }
        panic!("temporary directory budget exhausted");
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
    let result = command.output().expect("nominal subprocess starts");
    assert!(
        !matches!(result.status.code(), Some(124 | 137)),
        "nominal subprocess timed out"
    );
    result
}
enum Expected<'a> {
    Value(i64),
    Trap(TrapKind, &'a str),
}
fn verify(source: &str, effects: &str, expected: Expected<'_>) -> axeon::ir::Program {
    let directory = Temp::new();
    let input = directory.0.join("main.ax");
    fs::write(&input, source).unwrap();
    let program = axeon::compile(source).unwrap_or_else(|errors| panic!("{errors:#?}\n{source}"));
    let interpreted = interpreter::run(&program, 5_000_000);
    assert_eq!(interpreted.stdout, effects, "independent nominal effects");
    let (status, stdout, stderr) = match expected {
        Expected::Value(value) => {
            assert_eq!(interpreted.result, Ok(value), "independent nominal answer");
            (0, format!("{effects}result: {value}\n"), String::new())
        }
        Expected::Trap(kind, marker) => {
            assert_eq!(source.matches(marker).count(), 1, "unique trap marker");
            let start = source.find(marker).unwrap();
            let trap = interpreted.result.expect_err("nominal operation traps");
            assert_eq!(trap.kind, kind);
            assert_eq!(
                (trap.span.start, trap.span.end),
                (start, start + marker.len())
            );
            let (line, column) = line_col(source, start);
            (
                70,
                effects.to_owned(),
                format!("trap[{}] at {line}:{column}\n", kind.name()),
            )
        }
    };
    for opt in ["0", "2", "3"] {
        let executable = directory.0.join(format!("program-O{opt}"));
        let compiled = output(
            bounded(Path::new(env!("CARGO_BIN_EXE_axeon")), "40s")
                .arg("build")
                .arg(&input)
                .args(["--opt", opt, "-o"])
                .arg(&executable),
        );
        assert!(
            compiled.status.success(),
            "nominal build O{opt}: {}",
            String::from_utf8_lossy(&compiled.stderr)
        );
        let native = output(&mut bounded(&executable, "10s"));
        assert_eq!(
            native.status.code(),
            Some(status),
            "nominal exit O{opt}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert_eq!(native.stdout, stdout.as_bytes(), "nominal stdout O{opt}");
        assert_eq!(native.stderr, stderr.as_bytes(), "nominal stderr O{opt}");
    }
    program
}
fn rejects(source: &str, code: &str) {
    let errors = axeon::compile(source).expect_err("invalid nominal program must not expose IR");
    assert!(
        errors.iter().any(|error| error.code == code),
        "wanted {code}, got {errors:#?}\n{source}"
    );
    for error in errors {
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
    }
}

const PIPELINE: &str = r#"
record Cell<T> { value:T }
union Choice<T> { Present { value:Cell<T> }, Empty {} }
fn wrap<T>(value:T)->Cell<T> { return Cell::<T>{value:value}; }
fn unwrap<T>(cell:Cell<T>)->T { return cell.value; }
fn choose<T>(choice:Choice<T>,fallback:T)->T {
 match choice {
  Choice::<T>::Present{value:cell}=>{return unwrap::<T>(cell);},
  Choice::<T>::Empty{}=>{return fallback;}
 }
}
fn roundtrip<T>(value:T,fallback:T)->T {
 let cell:Cell<T>=wrap::<T>(value);
 let choice:Choice<T>=Choice::<T>::Present{value:cell};
 return choose::<T>(choice,fallback);
}
"#;

#[test]
fn symbolic_record_union_pipeline_preserves_scalar_unit_and_strict_float_values() {
    let source = format!(
        r#"{PIPELINE}
fn main()->i64{{
 let exact:i64=roundtrip::<i64>(9007199254740993,0);
 let zero:f64=roundtrip::<f64>(-0.0,1.0);
 let tiny:f64=roundtrip::<f64>(5e-324,1.0);
 let nan:f64=roundtrip::<f64>(f64_from_bits(9221120237041090561),1.0);
 let yes:bool=roundtrip::<bool>(true,false);
 let done:()=roundtrip::<()>((),());
 if exact!=9007199254740993 || f64_bits(zero)!=-9223372036854775808 {{return 1;}}
 if f64_bits(tiny)!=1 || f64_bits(nan)!=9221120237041090560 || !yes || done!=() {{return 2;}}
 let empty:Choice<i64>=Choice::<i64>::Empty{{}};
 return choose::<i64>(empty,42);
}}
"#
    );
    verify(&source, "", Expected::Value(42));
}

#[test]
fn symbolic_pipeline_moves_arrays_and_nested_nominals_without_erasing_layouts() {
    let source = format!(
        r#"{PIPELINE}
record Packet {{ exact:i64, payload:[i64;2] }}
fn main()->i64{{
 let a:[i64;2]=[20,22];let fallback:[i64;2]=[0,0];
 let numbers:[i64;2]=roundtrip::<[i64;2]>(a,fallback);
 let z:[f64;3]=[-0.0,5e-324,f64_from_bits(9221120237041090561)];
 let default_z:[f64;3]=[1.0,1.0,1.0];
 let floats:[f64;3]=roundtrip::<[f64;3]>(z,default_z);
 if f64_bits(floats[0])!=-9223372036854775808 || f64_bits(floats[1])!=1 || f64_bits(floats[2])!=9221120237041090560 {{return 1;}}
 let p:Packet=Packet{{exact:9007199254740993,payload:numbers}};
 let default_p:Packet=Packet{{exact:0,payload:[0,0]}};
 let packet:Packet=roundtrip::<Packet>(p,default_p);
 let cell:Cell<Packet>=wrap::<Packet>(packet);
 let default_cell:Cell<Packet>=Cell::<Packet>{{value:Packet{{exact:0,payload:[0,0]}}}};
 let restored:Cell<Packet>=roundtrip::<Cell<Packet>>(cell,default_cell);
 if restored.value.exact!=9007199254740993 {{return 2;}}
 let values:[i64;2]=restored.value.payload;
 return values[0]+values[1];
}}
"#
    );
    verify(&source, "", Expected::Value(42));
}

#[test]
fn known_scalar_fields_and_opaque_siblings_keep_distinct_capabilities() {
    let source = r#"
record Pair<T,U>{first:T,second:U,count:i64}
fn first<T,U>(pair:Pair<T,U>)->T{let second:U=pair.second;return pair.first;}
fn adjust<T>(value:T)->Pair<T,i64>{let mut pair:Pair<T,i64>=Pair::<T,i64>{second:20,count:0,first:value};pair.count=2;pair.second=pair.second+pair.count;return pair;}
fn main()->i64{
 let a:[i64;2]=[20,22];let pair:Pair<[i64;2],i64>=adjust::<[i64;2]>(a);
 if pair.second!=22 || pair.count!=2{return 1;}
 let values:[i64;2]=first::<[i64;2],i64>(pair);return values[0]+values[1];
}
"#;
    verify(source, "", Expected::Value(42));
}

#[test]
fn nominal_bounds_use_exact_contracts_and_concrete_application_implementations() {
    let source = r#"
trait Read{fn read(value:Self)->i64;}
record Checked<T:Read>{value:T}
record Cell<T>{value:T}
fn read_int(value:i64)->i64{return value;}
impl Read for i64{read=read_int;}
fn read_cell(value:Cell<i64>)->i64{return value.value;}
impl Read for Cell<i64>{read=read_cell;}
fn finish<T:Read>(value:T)->i64{let checked:Checked<T>=Checked::<T>{value:value};let payload:T=checked.value;return Read::read(payload);}
fn main()->i64{let cell:Cell<i64>=Cell::<i64>{value:21};return finish::<i64>(21)+finish::<Cell<i64>>(cell);}
"#;
    verify(source, "", Expected::Value(42));
}

#[test]
fn constructor_field_order_and_first_trap_are_preserved_at_all_optimizations() {
    let source = r#"
record Pair<T>{first:T,second:i64}
union Event<T>{Present{pair:Pair<T>,last:i64},Empty{}}
fn effect(n:i64)->i64{print(n);return n;}
fn main()->i64{
 let event:Event<i64>=Event::<i64>::Present{last:effect(3),pair:Pair::<i64>{second:effect(2),first:effect(1)}};
 match event{Event::<i64>::Present{pair:p,last:n}=>{return p.first+p.second+n;},Event::<i64>::Empty{}=>{return 0;}}
}
"#;
    verify(source, "3\n2\n1\n", Expected::Value(6));
    for (original, effects) in [
        ("effect(3)", ""),
        ("effect(2)", "3\n"),
        ("effect(1)", "3\n2\n"),
    ] {
        verify(
            &source.replace(original, "7 / 0"),
            effects,
            Expected::Trap(TrapKind::DivisionByZero, "7 / 0"),
        );
    }
}

#[test]
fn continuing_match_arms_and_recovery_handlers_preserve_opaque_owner_paths() {
    let source = r#"
record Cell<T>{value:T}
union Flag<T>{Stop{value:T},Go{}}
fn choose<T>(flag:Flag<T>,cell:Cell<T>)->T{
 match flag{Flag::<T>::Stop{value:v}=>{let ignored:T=cell.value;return v;},Flag::<T>::Go{}=>{}}
 return cell.value;
}
fn recover<T>(cell:Cell<T>)->T{
 tensor_scope(8){let first:tensor=try tensor_zeros([1]);let fails:tensor=try tensor_zeros([1]);}
 catch(e:tensor_error){print(tensor_error_code(e));return cell.value;}
 return cell.value;
}
fn main()->i64{
 let c:Cell<[i64;2]>=Cell::<[i64;2]>{value:[20,22]};
 let f:Flag<[i64;2]>=Flag::<[i64;2]>::Go{};
 let values:[i64;2]=choose::<[i64;2]>(f,c);
 let wrapped:Cell<[i64;2]>=Cell::<[i64;2]>{value:values};
 let recovered:[i64;2]=recover::<[i64;2]>(wrapped);return recovered[0]+recovered[1];
}
"#;
    verify(source, "9\n", Expected::Value(42));
}

#[test]
fn unused_nominal_function_bodies_reject_opaque_scalar_and_structural_operations() {
    for (body, code) in [
        ("print(cell.value);return 0;", "O004"),
        ("return cell.value+1;", "O004"),
        ("let same:bool=cell.value==cell.value;return 0;", "O004"),
        ("return cell.value.next;", "R010"),
        ("let payload:T=cell.value;return payload[0];", "O009"),
        (
            "let payload:T=cell.value;let slice:&[i64]=&payload;return 0;",
            "O005",
        ),
        ("let mut local:Cell<T>=cell;local.value=1;return 0;", "O008"),
        ("let wrong:bool=1;return 0;", "T009"),
        ("return absent();", "T007"),
    ] {
        rejects(&format!("record Cell<T>{{value:T}}fn unused<T>(cell:Cell<T>)->i64{{{body}}}fn main()->i64{{return 0;}}"),code);
    }
}

#[test]
fn opaque_partial_moves_obey_same_owner_paths_joins_loops_and_recovery_rules() {
    let declarations =
        "record Cell<T>{value:T,other:T}fn consume<T>(cell:Cell<T>)->bool{return true;}";
    for (body, code) in [
        (
            "let first:T=cell.value;let again:T=cell.value;return 0;",
            "O006",
        ),
        (
            "let first:T=cell.value;consume::<T>(cell);return 0;",
            "O006",
        ),
        (
            "if true{let first:T=cell.value;}let again:T=cell.value;return 0;",
            "O006",
        ),
        ("while true{let first:T=cell.value;break;}return 0;", "O011"),
        (
            "tensor_scope(0){let first:T=cell.value;}catch(e:tensor_error){}return 0;",
            "N007",
        ),
        (
            "let short:bool=false&&consume::<T>(cell);let first:T=cell.value;return 0;",
            "O006",
        ),
    ] {
        rejects(
            &format!(
                "{declarations}fn bad<T>(cell:Cell<T>)->i64{{{body}}}fn main()->i64{{return 0;}}"
            ),
            code,
        );
    }
    rejects("record Cell<T>{value:T}record Outer<T>{nested:Cell<T>}fn bad<T>(outer:Outer<T>)->i64{let payload:T=outer.nested.value;let child:Cell<T>=outer.nested;return 0;}fn main()->i64{return 0;}","O006");
    // A later scalar instantiation cannot justify duplicating the opaque value.
    rejects("record Cell<T>{value:T}fn bad<T>(cell:Cell<T>)->i64{let a:T=cell.value;let b:T=cell.value;return 0;}fn main()->i64{let c:Cell<i64>=Cell::<i64>{value:1};return bad::<i64>(c);}","O006");
}

#[test]
fn mismatched_nominal_applications_and_patterns_never_gain_structural_equality() {
    rejects("record A<T>{value:T}record B<T>{value:T}fn bad<T>(a:A<T>)->B<T>{return a;}fn main()->i64{return 0;}","T009");
    rejects("record Cell<T>{value:T}fn bad<T,U>(c:Cell<T>)->Cell<U>{return c;}fn main()->i64{return 0;}","T009");
    rejects("record Cell<T>{value:T}fn bad<T,U>(x:T)->Cell<U>{return Cell::<T>{value:x};}fn main()->i64{return 0;}","T009");
    rejects("union Choice<T>{Present{value:T},Empty{}}fn bad<T,U>(c:Choice<T>)->i64{match c{Choice::<U>::Present{value:v}=>{return 1;},Choice::<U>::Empty{}=>{return 0;}}}fn main()->i64{return 0;}","U012");
    rejects("record Phantom<T>{tag:i64}fn main()->i64{let a:Phantom<i64>=Phantom::<i64>{tag:1};let b:Phantom<bool>=a;return 0;}","T009");
}

#[test]
fn explicit_nominal_arguments_require_correct_arity_and_cannot_be_inferred() {
    for source in [
        "record Cell<T>{value:T}fn main()->i64{let c:Cell=Cell::<i64>{value:1};return 0;}",
        "record Cell<T>{value:T}fn main()->i64{let c:Cell<i64,bool>=Cell::<i64>{value:1};return 0;}",
        "record Cell<T,U>{first:T,second:U}fn main()->i64{let c:Cell<i64>=Cell::<i64>{first:1,second:2};return 0;}",
        "record Plain{value:i64}fn main()->i64{let p:Plain<i64>=Plain{value:1};return 0;}",
        "record Cell<T>{value:T}fn main()->i64{let c:Cell<i64>=Cell::<i64,bool>{value:1};return 0;}",
        "union Choice<T>{Empty{}}fn main()->i64{let c:Choice<i64>=Choice::<i64>::Empty{};match c{Choice::<i64,bool>::Empty{}=>{return 0;}}}",
    ] { rejects(source,"G003"); }
    rejects(
        "record TooMany<A,B,C,D,E,F,G,H,I>{value:A}fn main()->i64{return 0;}",
        "G002",
    );
}

#[test]
fn public_source_ast_cannot_pair_a_constructor_or_pattern_with_another_application() {
    use axeon::ast::{ExprKind, StmtKind};
    let source = "record Cell<T>{value:T}record Other<T>{value:T}fn main()->i64{let c:Cell<i64>=Cell::<i64>{value:1};return c.value;}";
    for wrong_name in ["Other", "missing"] {
        let mut syntax = axeon::parser::parse_source(source).unwrap();
        let StmtKind::Let { value, .. } = &mut syntax.functions[0].body.stmts[0].kind else {
            panic!("fixture let");
        };
        let ExprKind::Record { name, .. } = &mut value.kind else {
            panic!("fixture constructor");
        };
        *name = wrong_name.to_owned();
        let errors = axeon::check::check_source(&syntax).unwrap_err();
        assert!(
            errors.iter().any(|error| error.code == "G007"),
            "{errors:#?}"
        );
    }
    let source = "union Choice<T>{Empty{}}union Other<T>{Empty{}}fn main()->i64{let c:Choice<i64>=Choice::<i64>::Empty{};match c{Choice::<i64>::Empty{}=>{return 0;}}}";
    let mut syntax = axeon::parser::parse_source(source).unwrap();
    let StmtKind::Match { arms, .. } = &mut syntax.functions[0].body.stmts[1].kind else {
        panic!("fixture match");
    };
    arms[0].union_name = "Other".to_owned();
    let errors = axeon::check::check_source(&syntax).unwrap_err();
    assert!(
        errors.iter().any(|error| error.code == "G007"),
        "{errors:#?}"
    );
}

#[test]
fn generic_union_patterns_remain_consuming_exhaustive_and_exact() {
    let declarations = "union Choice<T>{Present{value:T,other:i64},Empty{}}";
    for (body, code) in [
        ("match c{Choice::<T>::Present{value:v,other:n}=>{return n;}}", "U014"),
        ("match c{Choice::<T>::Present{value:v,other:n}=>{},Choice::<T>::Empty{}=>{}}match c{Choice::<T>::Present{value:v,other:n}=>{return n;},Choice::<T>::Empty{}=>{return 0;}}", "O006"),
        ("match c{Choice::<T>::Present{value:v}=>{return 1;},Choice::<T>::Empty{}=>{return 0;}}", "U010"),
        ("match c{Choice::<T>::Present{value:v,other:n}=>{let a:T=v;let b:T=v;return n;},Choice::<T>::Empty{}=>{return 0;}}", "O006"),
        ("match c{Choice::<T>::Present{value:v,other:n}=>{return n;},Choice::<T>::Empty{}=>{return 0;},Choice::<T>::Empty{}=>{return 1;}}", "U013"),
    ] {
        rejects(&format!("{declarations}fn bad<T>(c:Choice<T>)->i64{{{body}}}fn main()->i64{{return 0;}}"),code);
    }
}

#[test]
fn concrete_field_stores_recheck_owner_after_rhs_effects_and_keep_loans() {
    rejects("record Cell<T>{value:T,count:i64}fn consume<T>(cell:Cell<T>)->i64{return 1;}fn main()->i64{let mut c:Cell<i64>=Cell::<i64>{value:1,count:2};c.count=consume::<i64>(c);return 0;}","O006");
    rejects("record Cell<T>{value:T}fn main()->i64{let a:[i64;2]=[1,2];let borrowed:&[i64]=&a;let c:Cell<[i64;2]>=Cell::<[i64;2]>{value:a};return 0;}","O007");
    rejects("record Cell<T>{value:T}fn main()->i64{let mut c:Cell<[i64;2]>=Cell::<[i64;2]>{value:[1,2]};let moved:[i64;2]=c.value;c.value=[3,4];return 0;}","O006");
}

#[test]
fn unused_nominal_cycles_include_payloads_nested_arguments_and_phantoms() {
    for declarations in [
        "record Bad<T>{next:Bad<T>}",
        "record A<T>{next:B<T>}union B<U>{End{},Next{value:A<U>}}",
        "record Cell<T>{value:T}record Bad<T>{next:Bad<Cell<T>>}",
        "record Phantom<T>{tag:i64}record Bad<T>{next:Phantom<Bad<T>>}",
        "record Bad<T>{next:Ordinary}record Ordinary{next:Bad<i64>}",
    ] {
        rejects(
            &format!("{declarations}fn main()->i64{{return 0;}}"),
            "A001",
        );
    }
}

#[test]
fn phantom_arguments_and_unused_bodies_do_not_bypass_bounds_or_type_categories() {
    let contract = "trait Read{fn read(value:Self)->i64;}trait Other{fn read(value:Self)->i64;}fn read_int(value:i64)->i64{return value;}impl Read for i64{read=read_int;}record Checked<T:Read>{tag:i64}";
    for fragment in [
        "fn unused<T>(x:T)->i64{let c:Checked<T>=Checked::<T>{tag:0};return 0;}",
        "fn unused<T:Other>(x:T)->i64{let c:Checked<T>=Checked::<T>{tag:0};return 0;}",
        "record Invalid<T>{value:Checked<T>}",
        "record Invalid{value:Checked<bool>}",
    ] {
        rejects(
            &format!("{contract}{fragment}fn main()->i64{{return 0;}}"),
            "G004",
        );
    }
    rejects("trait Read{fn read(value:Self)->i64;}record Cell<T>{value:T}fn bad<T:Read>(cell:Cell<T>)->i64{return Read::read(cell);}fn main()->i64{return 0;}","TR007");
    for ty in ["tensor", "tensor_error", "&[i64]", "&mut [i64]", "&tensor"] {
        rejects(&format!("record Phantom<T>{{tag:i64}}fn main()->i64{{let p:Phantom<{ty}>=Phantom::<{ty}>{{tag:0}};return 0;}}"),"G003");
    }
}

#[test]
fn nested_type_delimiters_do_not_change_shift_and_comparison_expressions() {
    let source = r#"
record Cell<T>{value:T}
fn main()->i64{
 let nested:Cell<Cell<i64>>=Cell::<Cell<i64>>{value:Cell::<i64>{value:42}};
 let shifted:i64=84>>1;let left:i64=21<<1;
 if shifted>=nested.value.value && left<=42{return nested.value.value;}return 0;
}
"#;
    let program = axeon::compile(source).unwrap();
    assert_eq!(interpreter::run(&program, 1_000_000).result, Ok(42));
}

fn nested_application(depth: usize) -> String {
    format!("{}i64{}", "Cell<".repeat(depth), ">".repeat(depth))
}

#[test]
fn finite_repeated_wrapper_application_depth_is_distinct_from_declaration_cycles() {
    let source = |depth| {
        format!("record Cell<T>{{value:T}}fn inspect(x:{})->i64{{return 0;}}fn main()->i64{{return 0;}}",nested_application(depth))
    };
    let program = axeon::compile(&source(32)).expect("32 finite nested applications fit");
    assert_eq!(program.records.len(), 32);
    rejects(&source(33), "G002");
    // All these layouts are one slot: phantom arguments still count in identity depth.
    let phantom = source(33).replace("value:T", "tag:i64");
    rejects(&phantom, "G002");
}

fn nominal_declarations(count: usize) -> String {
    let mut source = String::new();
    for index in 0..count {
        write!(source, "record Cell{index}<T>{{value:T}}").unwrap();
    }
    source.push_str("fn main()->i64{return 0;}");
    source
}

#[test]
fn unused_generic_declaration_limit_is_exact_and_does_not_admit_concrete_layouts() {
    let program = axeon::compile(&nominal_declarations(128)).unwrap();
    assert!(program.records.is_empty());
    rejects(&nominal_declarations(129), "G002");
}

fn concrete_applications(count: usize) -> String {
    let mut source = String::from("record Phantom<A,B,C,D,E,F,G,H>{tag:i64}");
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
        // Each independent function has one slot, so only the nominal layout cap
        // is under test; the frame and generic function instance caps are not.
        write!(
            source,
            "fn inspect{index}(x:Phantom<{arguments}>)->i64{{return x.tag;}}"
        )
        .unwrap();
    }
    source.push_str("fn main()->i64{return 0;}");
    source
}

#[test]
fn concrete_nominal_instance_cap_is_shared_across_functions_and_deterministic() {
    let exact = axeon::compile(&concrete_applications(256)).unwrap();
    assert_eq!(exact.records.len(), 256);
    let excess = concrete_applications(257);
    let first = axeon::compile(&excess).unwrap_err();
    let second = axeon::compile(&excess).unwrap_err();
    assert_eq!(first, second);
    assert!(first.iter().any(|error| error.code == "G002"), "{first:#?}");
}

#[test]
fn actual_substituted_layout_and_whole_function_frames_keep_original_storage_limits() {
    let fields = (0..8)
        .map(|n| format!("f{n}:[i64;32]"))
        .collect::<Vec<_>>()
        .join(",");
    let prefix = format!("record Big{{{fields}}}record Cell<T>{{value:T}}");
    // An ordinary wrapper has exactly its actual payload size, not a proof leaf.
    let exact =
        format!("{prefix}fn inspect(x:Cell<Big>)->i64{{return 0;}}fn main()->i64{{return 0;}}");
    assert!(axeon::compile(&exact).is_ok());
    let larger = exact.replace("value:T", "value:T,tag:i64");
    rejects(&larger, "A003");
    let doubled = format!("{prefix}fn inspect(x:Cell<Big>)->i64{{let moved:Cell<Big>=x;return 0;}}fn main()->i64{{return 0;}}");
    rejects(&doubled, "O002");
    // Even an unused generic has to admit its known fixed contribution.
    rejects(
        &format!("record TooBig<T>{{payload:T,{fields}}}fn main()->i64{{return 0;}}"),
        "A003",
    );
}
