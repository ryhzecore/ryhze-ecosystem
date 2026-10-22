//! Independent IR invariant failures. Runtime fault injection remains a
//! separate contract: rejecting malformed IR must not remove backend defenses.
use axeon::diagnostic::{SourceId, Span};
use axeon::ir::{self, ExprKind, StmtKind, Type};
use axeon::verify::{verify, MAX_ERRORS, MAX_IR_DEPTH, MAX_IR_NODES};
use std::fmt::Write;
use std::path::Path;

fn compiled(source: &str) -> ir::Program {
    axeon::compile(source).unwrap_or_else(|errors| panic!("{source}\n{errors:#?}"))
}
fn main(program: &mut ir::Program) -> &mut ir::Function {
    &mut program.functions[program.main]
}
fn rejected(program: &ir::Program, code: &str) {
    let errors = verify(program).expect_err("malformed IR must be rejected");
    assert!(errors.iter().any(|error| error.code == code), "{errors:#?}");
    assert!(errors.len() <= MAX_ERRORS);
    assert_eq!(
        verify(program),
        Err(errors),
        "diagnostics must be deterministic"
    );
}
fn integer(value: i64) -> ir::Expr {
    ir::Expr {
        kind: ExprKind::Int(value),
        ty: Type::I64,
        span: Span::default(),
    }
}
fn returned(program: &mut ir::Program) -> &mut ir::Expr {
    let StmtKind::Return(value) = &mut main(program).body.stmts.last_mut().unwrap().kind else {
        panic!("return fixture")
    };
    value
}

#[test]
fn generated_record_identity_is_disjoint_and_runs_through_existing_backends() {
    let source = "record R{x:i64}record Z{x:i64}fn main()->i64{let r:R=R{x:42};return r.x;}";
    let mut program = compiled(source);
    let generated = ir::NominalId::Instance(0);
    program.records[0].id = generated;
    program.records[1].id = ir::RecordId {
        source: SourceId(0),
        offset: 0,
    }
    .into();
    main(&mut program).locals[0].ty = Type::Record(generated);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("record initializer")
    };
    value.ty = Type::Record(generated);
    assert_eq!(verify(&program), Ok(()));
    let execution = axeon::interpreter::run(&program, axeon::interpreter::DEFAULT_FUEL);
    assert_eq!(execution.result, Ok(42));
    assert_eq!(execution.stdout, "");
    let llvm = axeon::llvm::emit(&program, source);
    assert!(llvm.contains("%ax_record_instance_0 = type [1 x i64]"));
    assert!(llvm.contains("%ax_record_0_0 = type [1 x i64]"));
}

#[test]
fn generated_union_identity_preserves_matching_and_payload_layout() {
    let source = "record R{x:i64}union U{Value{r:R},Empty{}}fn main()->i64{let r:R=R{x:42};let u:U=U::Value{r:r};match u{U::Value{r:r}=>{return r.x;},U::Empty{}=>{return 0;},}}";
    let mut program = compiled(source);
    let payload = ir::NominalId::Instance(0);
    let generated = ir::NominalId::Instance(1);
    program.records[0].id = payload;
    program.unions[0].id = generated;
    program.unions[0].variants[0].fields[0].ty = Type::Record(payload);
    main(&mut program).locals[0].ty = Type::Record(payload);
    main(&mut program).locals[1].ty = Type::Union(generated);
    main(&mut program).locals[2].ty = Type::Record(payload);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("record initializer")
    };
    value.ty = Type::Record(payload);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[1].kind else {
        panic!("union initializer")
    };
    value.ty = Type::Union(generated);
    let ExprKind::Union { fields, .. } = &mut value.kind else {
        panic!("union payload")
    };
    fields[0].1.ty = Type::Record(payload);
    assert_eq!(verify(&program), Ok(()));
    let execution = axeon::interpreter::run(&program, axeon::interpreter::DEFAULT_FUEL);
    assert_eq!(execution.result, Ok(42));
    assert_eq!(execution.stdout, "");
    let llvm = axeon::llvm::emit(&program, source);
    assert!(llvm.contains("%ax_record_instance_0 = type [1 x i64]"));
    assert!(llvm.contains("%local1 = alloca [2 x i64], align 8"));
}

#[test]
fn generated_layouts_share_one_namespace_and_unknown_instances_are_rejected() {
    let mut program = compiled("record R{x:i64}union U{Value{x:i64}}fn main()->i64{return 0;}");
    program.records[0].id = ir::NominalId::Instance(0);
    program.unions[0].id = ir::NominalId::Instance(0);
    rejected(&program, "IR002");
    program.unions[0].id = ir::NominalId::Instance(1);
    assert_eq!(verify(&program), Ok(()));
    program.records[0].fields[0].ty = Type::Record(ir::NominalId::Instance(2));
    rejected(&program, "IR003");

    let mut program = compiled("fn main()->i64{let x:i64=1;return x;}");
    main(&mut program).locals[0].ty = Type::Union(ir::NominalId::Instance(u32::MAX));
    rejected(&program, "IR003");
}

#[test]
fn current_examples_and_cross_module_programs_are_valid_without_mutation() {
    for file in [
        "arrays.ax",
        "bounds.ax",
        "control.ax",
        "effects.ax",
        "fibonacci.ax",
        "numerics.ax",
        "overflow.ax",
        "records.ax",
        "unions.ax",
        "tensors.ax",
        "tensors-trap.ax",
        "tensors-recovery.ax",
        "modules/main.ax",
        "aggregate-modules/main.ax",
        "nested-modules/main.ax",
        "unit-modules/main.ax",
    ] {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("examples")
            .join(file);
        let project = axeon::project::Project::load(&path, None).unwrap();
        let program = project
            .check()
            .unwrap_or_else(|errors| panic!("{file}: {errors:#?}"));
        let before = format!("{program:?}");
        assert_eq!(verify(&program), Ok(()), "{file}");
        assert_eq!(
            format!("{program:?}"),
            before,
            "verification mutated {file}"
        );
    }
}

#[test]
fn unit_recursion_and_selected_test_entry_are_valid() {
    let source = "fn action(x: ())->(){return x;}fn recurse(n:i64)->i64{if n==0{return 0;}return recurse(n-1);}fn test_one()->i64{let x:()=action(());if x==(){return recurse(2);}return 1;}fn main()->i64{return 0;}";
    let mut program = compiled(source);
    program.main = program
        .functions
        .iter()
        .position(|function| function.name == "test_one")
        .unwrap();
    assert_eq!(verify(&program), Ok(()));
}

#[test]
fn malformed_entry_and_function_references_are_rejected() {
    let mut program =
        compiled("fn identity(x:i64)->i64{return x;}fn main()->i64{return identity(1);}");
    program.main = usize::MAX;
    rejected(&program, "IR002");
    program.main = 1;
    let ExprKind::Call { function, .. } = &mut returned(&mut program).kind else {
        panic!("call")
    };
    *function = usize::MAX;
    rejected(&program, "IR002");
    program.main = 0;
    rejected(&program, "IR003");
}

#[test]
fn parameter_local_and_call_arity_are_checked_without_indexing_panics() {
    let source = "fn f(x:i64)->i64{return x;}fn main()->i64{return f(1);}";
    let mut program = compiled(source);
    program.functions[0].params[0] = usize::MAX;
    rejected(&program, "IR002");
    let mut program = compiled(source);
    let ExprKind::Call { args, .. } = &mut returned(&mut program).kind else {
        panic!("call")
    };
    args.clear();
    rejected(&program, "IR003");
    let mut program = compiled(source);
    program.functions[0].params.push(0);
    rejected(&program, "IR004");
    let mut program = compiled(source);
    program.functions[0].locals[0].mutable = true;
    rejected(&program, "IR004");
}

#[test]
fn type_annotations_and_operator_operands_are_independently_checked() {
    let mut program = compiled("fn main()->i64{return 1+2;}");
    let ExprKind::Binary { left, .. } = &mut returned(&mut program).kind else {
        panic!("binary")
    };
    left.ty = Type::Bool;
    rejected(&program, "IR003");
    let mut program = compiled("fn main()->i64{return 1;}");
    returned(&mut program).kind = ExprKind::Unit;
    rejected(&program, "IR003");
    let mut program = compiled("fn main()->i64{return 1;}");
    returned(&mut program).kind = ExprKind::Var(usize::MAX);
    rejected(&program, "IR004");
}

#[test]
fn declaration_order_lexical_scope_and_binding_uniqueness_are_checked() {
    let source = "fn main()->i64{let x:i64=1;{let y:i64=2;}return x;}";
    let mut program = compiled(source);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    value.kind = ExprKind::Var(0);
    rejected(&program, "IR004");
    let mut program = compiled(source);
    returned(&mut program).kind = ExprKind::Var(1);
    rejected(&program, "IR004");
    let mut program = compiled(source);
    let duplicate = main(&mut program).body.stmts[0].clone();
    main(&mut program).body.stmts.insert(1, duplicate);
    rejected(&program, "IR004");
    let mut program = compiled(source);
    main(&mut program).body.stmts.remove(1);
    rejected(&program, "IR004");
}

#[test]
fn control_flow_rejects_fallthrough_unreachable_and_escaping_loop_exits() {
    let mut program = compiled("fn main()->i64{return 0;}");
    main(&mut program).body.stmts.clear();
    rejected(&program, "IR005");
    let mut program = compiled("fn main()->i64{return 0;}");
    let tail = main(&mut program).body.stmts[0].clone();
    main(&mut program).body.stmts.push(tail);
    rejected(&program, "IR005");
    for kind in [StmtKind::Break, StmtKind::Continue] {
        let mut program = compiled("fn main()->i64{return 0;}");
        main(&mut program).body.stmts.insert(
            0,
            ir::Stmt {
                kind,
                span: Span::default(),
            },
        );
        rejected(&program, "IR005");
    }
    let program = compiled("fn main()->i64{let mut n:i64=0;while n<4{n=n+1;if n==2{continue;}if n==3{break;}}return n;}");
    assert_eq!(verify(&program), Ok(()));
}

#[test]
fn nominal_identity_references_cycles_and_size_limits_are_safe() {
    let source = "record R{x:i64}record S{r:R}fn main()->i64{let r:R=R{x:1};return r.x;}";
    let mut program = compiled(source);
    program.records[1].id = program.records[0].id;
    rejected(&program, "IR002");
    let mut program = compiled(source);
    program.records[0].fields[0].ty = Type::Record(
        ir::RecordId {
            source: SourceId(999),
            offset: usize::MAX,
        }
        .into(),
    );
    rejected(&program, "IR003");
    let mut program = compiled(source);
    program.records[0].fields[0].ty = Type::Record(program.records[1].id);
    rejected(&program, "IR008");
    let mut program = compiled(source);
    program.records[0].fields[0].ty = Type::Array {
        element: ir::ElementType::I64,
        length: usize::MAX,
    };
    rejected(&program, "IR003");
    let mut program = compiled(source);
    let field = program.records[0].fields[0].clone();
    program.records[0].fields = (0..9)
        .map(|index| {
            let mut field = field.clone();
            field.name = format!("f{index}");
            field.ty = Type::Array {
                element: ir::ElementType::I64,
                length: 32,
            };
            field
        })
        .collect();
    rejected(&program, "IR008");
}

#[test]
fn constructors_and_nested_field_paths_require_exact_declared_fields() {
    let source =
        "record R{x:i64,y:i64}record S{r:R}fn main()->i64{let s:S=S{r:R{x:1,y:2}};return s.r.x;}";
    let mut program = compiled(source);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    let ExprKind::Record(fields) = &mut value.kind else {
        panic!("record")
    };
    fields[0].0 = usize::MAX;
    rejected(&program, "IR002");
    let mut program = compiled(source);
    let ExprKind::Field { field, .. } = &mut returned(&mut program).kind else {
        panic!("field")
    };
    *field = vec![0, 0, 0];
    rejected(&program, "IR003");
    let mut program = compiled(source);
    let ExprKind::Field { field, .. } = &mut returned(&mut program).kind else {
        panic!("field")
    };
    field.clear();
    rejected(&program, "IR001");
    let mut program =
        compiled("record R{x:i64,y:i64}fn main()->i64{let r:R=R{x:1,y:2};return r.x;}");
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    let ExprKind::Record(fields) = &mut value.kind else {
        panic!("record")
    };
    fields[1].0 = 0;
    rejected(&program, "IR009");
}

#[test]
fn strict_tag_validation_preserves_raw_runtime_corruption_defense() {
    let source = "record R{x:i64}union U{Empty{},Value{r:R}}fn main()->i64{let u:U=U::Value{r:R{x:7}};match u{U::Empty{}=>{return 0;},U::Value{r:r}=>{return r.x;},}}";
    let mut program = compiled(source);
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    let ExprKind::Union { variant, .. } = &mut value.kind else {
        panic!("union")
    };
    *variant = 77;
    rejected(&program, "IR009");
    let result = axeon::interpreter::run(&program, 10000).result.unwrap_err();
    assert_eq!(result.kind, axeon::interpreter::TrapKind::InvalidUnionTag);
    assert!(
        !axeon::llvm::emit(&program, source).is_empty(),
        "raw emission remains available for defense tests"
    );
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    let ExprKind::Union { layout_variant, .. } = &mut value.kind else {
        panic!("union")
    };
    *layout_variant = usize::MAX;
    rejected(&program, "IR002");
}

#[test]
fn match_requires_exhaustive_unique_arms_and_typed_unique_bindings() {
    let source = "union U{A{x:i64},B{}}fn main()->i64{let u:U=U::A{x:1};match u{U::A{x:v}=>{return v;},U::B{}=>{return 0;},}}";
    let mut program = compiled(source);
    let StmtKind::Match { arms, .. } = &mut main(&mut program).body.stmts[1].kind else {
        panic!("match")
    };
    arms.pop();
    rejected(&program, "IR005");
    let mut program = compiled(source);
    let StmtKind::Match { arms, .. } = &mut main(&mut program).body.stmts[1].kind else {
        panic!("match")
    };
    arms[1].variant = 0;
    rejected(&program, "IR005");
    let mut program = compiled(source);
    let StmtKind::Match { arms, .. } = &mut main(&mut program).body.stmts[1].kind else {
        panic!("match")
    };
    arms[0].bindings[0].0 = usize::MAX;
    rejected(&program, "IR002");
    let mut program = compiled(source);
    main(&mut program).locals[1].ty = Type::Bool;
    rejected(&program, "IR003");
}

#[test]
fn owned_calls_require_bare_moves_and_scalar_stores_require_mutability() {
    let source =
        "fn take(a:[i64;1])->i64{return a[0];}fn main()->i64{let a:[i64;1]=[1];return take(a);}";
    let mut program = compiled(source);
    let ExprKind::Call { args, .. } = &mut returned(&mut program).kind else {
        panic!("call")
    };
    args[0].kind = ExprKind::Array(vec![integer(1)]);
    rejected(&program, "IR005");
    let mut program = compiled("fn main()->i64{let mut x:i64=0;x=1;return x;}");
    main(&mut program).locals[0].mutable = false;
    rejected(&program, "IR003");
    let mut program = compiled("fn main()->i64{let mut a:[i64;1]=[0];a[0]=1;return a[0];}");
    main(&mut program).locals[0].mutable = false;
    rejected(&program, "IR004");
}

const TENSOR_SOURCE: &str = "fn main()->i64{tensor_scope(1024){let t:tensor=try tensor_zeros([2]);let v:&tensor=try tensor_view(&t);print(try tensor_sum(v));}catch(e:tensor_error){print(tensor_error_code(e));}return 0;}";

#[test]
fn tensor_cleanup_requires_exact_order_and_lexical_owners() {
    for mutation in 0..3 {
        let mut program = compiled(TENSOR_SOURCE);
        let StmtKind::TensorScope { body, .. } = &mut main(&mut program).body.stmts[0].kind else {
            panic!("scope")
        };
        match mutation {
            0 => {
                body.cleanup.pop();
            }
            1 => body.cleanup.reverse(),
            _ => body.cleanup.push(usize::MAX),
        }
        rejected(&program, "IR006");
    }
}

#[test]
fn tensor_operand_representations_and_types_are_checked() {
    let mut program = compiled(TENSOR_SOURCE);
    let StmtKind::TensorScope { body, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("scope")
    };
    let StmtKind::Let { value, .. } = &mut body.stmts[0].kind else {
        panic!("let")
    };
    let ExprKind::TensorCall { args, .. } = &mut value.kind else {
        panic!("tensor call")
    };
    args[0] = ir::TensorArg::Scalar(integer(2));
    rejected(&program, "IR003");
    let mut program = compiled(TENSOR_SOURCE);
    let StmtKind::TensorScope { body, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("scope")
    };
    let StmtKind::Let { value, .. } = &mut body.stmts[1].kind else {
        panic!("let")
    };
    let ExprKind::TensorCall { args, .. } = &mut value.kind else {
        panic!("tensor call")
    };
    args[0] = ir::TensorArg::Local {
        local: usize::MAX,
        span: Span::default(),
    };
    rejected(&program, "IR004");
}

#[test]
fn tensor_scope_placement_protected_exits_and_catch_identity_are_checked() {
    let mut program = compiled(TENSOR_SOURCE);
    let scope = main(&mut program).body.stmts.remove(0);
    main(&mut program).body.stmts.insert(
        0,
        ir::Stmt {
            span: scope.span,
            kind: StmtKind::Block(ir::Block {
                stmts: vec![scope],
                span: Span::default(),
                cleanup: Vec::new(),
            }),
        },
    );
    rejected(&program, "IR005");
    let mut program = compiled(TENSOR_SOURCE);
    let StmtKind::TensorScope { body, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("scope")
    };
    body.stmts.push(ir::Stmt {
        kind: StmtKind::Return(integer(0)),
        span: Span::default(),
    });
    rejected(&program, "IR005");
    let mut program = compiled(TENSOR_SOURCE);
    let StmtKind::TensorScope { error_local, .. } = &mut main(&mut program).body.stmts[0].kind
    else {
        panic!("scope")
    };
    *error_local = usize::MAX;
    rejected(&program, "IR002");
}

#[test]
fn implicit_slice_bounds_and_dynamic_failure_values_remain_valid() {
    let source = "fn main()->i64{let mut a:[i64;2]=[1,2];{let s:&mut [i64]=&mut a;s[0]=3;}let r:&[i64]=&a[0..3];return r[0];}";
    assert_eq!(
        verify(&compiled(source)),
        Ok(()),
        "bounds failure remains a runtime concern"
    );
    assert_eq!(verify(&compiled("fn main()->i64{tensor_scope(-1){let t:tensor=try tensor_zeros([-1]);}catch(e:tensor_error){return tensor_error_code(e);}return 0;}")), Ok(()));
    let mut program = compiled("fn main()->i64{let x:f64=1.0;return f64_bits(x);}");
    let StmtKind::Let { value, .. } = &mut main(&mut program).body.stmts[0].kind else {
        panic!("let")
    };
    value.kind = ExprKind::Float(f64::NAN.to_bits());
    assert_eq!(
        verify(&program),
        Ok(()),
        "all IEEE payload bits are valid typed IR"
    );
}

#[test]
fn exact_source_depth_limits_and_nested_match_lowering_remain_accepted() {
    let limit = axeon::parser::MAX_NESTING;
    let blocks = format!(
        "fn main()->i64{{{}{}return 0;}}",
        "{".repeat(limit - 1),
        "}".repeat(limit - 1)
    );
    assert_eq!(verify(&compiled(&blocks)), Ok(()));
    let expression = "1+".repeat(limit - 2) + "1";
    assert_eq!(
        verify(&compiled(&format!(
            "fn main()->i64{{return {expression};}}"
        ))),
        Ok(())
    );
    // A match adds a statement, arm and block to the IR traversal. Discover
    // the parser boundary independently instead of assuming a 1:1 depth ratio.
    let mut body = "return 0;".to_owned();
    let mut last = String::new();
    let mut rejected_depth = false;
    for level in 0..limit {
        body = format!("let u{level}:U=U::V{{}};match u{level}{{U::V{{}}=>{{{body}}},}}");
        let source = format!("union U{{V{{}}}}fn main()->i64{{{body}}}");
        match axeon::parser::parse(&source) {
            Ok(_) => last = source,
            Err(errors) => {
                assert!(
                    errors.iter().any(|error| error.code == "E_NESTING_LIMIT"),
                    "{errors:#?}"
                );
                rejected_depth = true;
                break;
            }
        }
    }
    assert!(rejected_depth && !last.is_empty());
    assert_eq!(verify(&compiled(&last)), Ok(()));
}

#[test]
fn exact_source_node_limit_with_implicit_bounds_remains_accepted() {
    let slices = (0..255).fold(String::new(), |mut text, index| {
        write!(text, "let s{index}:&[i64]=&a;").unwrap();
        text
    });
    // 1 block + 3 array declaration nodes + 510 slice declaration nodes
    // + 1532 print nodes + 2 return nodes = 2048 source nodes. Lowering adds
    // 510 implicit bound expressions, yielding 2558 IR nodes and 256 locals.
    let body = format!(
        "let a:[i64;1]=[0];{slices}{}return 0;",
        "print(0);".repeat(766)
    );
    let source = format!("fn main()->i64{{{body}}}");
    let program = compiled(&source);
    assert_eq!(program.functions[program.main].locals.len(), 256);
    assert_eq!(verify(&program), Ok(()));
    let over = format!("fn main()->i64{{print(0);{body}}}");
    let errors = axeon::compile(&over).unwrap_err();
    assert!(
        errors.iter().any(|error| error.code == "T016"),
        "{errors:#?}"
    );
}

#[test]
fn reversed_spans_are_rejected_but_source_map_provenance_is_not_invented() {
    let mut program = compiled("fn main()->i64{return 0;}");
    returned(&mut program).span = Span::in_source(SourceId(usize::MAX), 9, 3);
    rejected(&program, "IR007");
    returned(&mut program).span = Span::in_source(SourceId(usize::MAX), 3, 9);
    assert_eq!(
        verify(&program),
        Ok(()),
        "source-aware bounds validation is a separate contract"
    );
}

#[test]
fn errors_and_traversal_are_bounded_and_deterministic() {
    let mut program = compiled("fn main()->i64{return 0;}");
    main(&mut program).body.stmts = (0..MAX_ERRORS * 2)
        .map(|_| ir::Stmt {
            kind: StmtKind::Print(ir::Expr {
                kind: ExprKind::Var(usize::MAX),
                ty: Type::I64,
                span: Span::default(),
            }),
            span: Span::default(),
        })
        .collect();
    let errors = verify(&program).unwrap_err();
    assert_eq!(errors.len(), MAX_ERRORS);
    assert_eq!(verify(&program), Err(errors));
    let mut program = compiled("fn main()->i64{return 0;}");
    for _ in 0..MAX_IR_DEPTH + 2 {
        let old = returned(&mut program).clone();
        *returned(&mut program) = ir::Expr {
            kind: ExprKind::Unary {
                op: ir::UnaryOp::Neg,
                value: Box::new(old),
            },
            ty: Type::I64,
            span: Span::default(),
        };
    }
    rejected(&program, "IR001");
    let mut program = compiled("fn main()->i64{return 0;}");
    main(&mut program).body.stmts = vec![
        ir::Stmt {
            kind: StmtKind::Print(integer(1)),
            span: Span::default()
        };
        MAX_IR_NODES + 1
    ];
    rejected(&program, "IR001");
}

#[test]
fn structural_verification_does_not_claim_an_independent_ownership_proof() {
    let mut program = compiled("fn main()->i64{let a:[i64;1]=[1];let b:[i64;1]=a;return b[0];}");
    let ExprKind::Index { base, .. } = &mut returned(&mut program).kind else {
        panic!("index")
    };
    *base = 0;
    assert_eq!(
        verify(&program),
        Ok(()),
        "well-typed use after move needs the ownership checker; do not run malformed IR"
    );
}
