//! Source tensor grammar, affine ownership, recovery edges and resource limits.
use axeon::diagnostic::{Diagnostic, Span};
use axeon::ir::{ExprKind, StmtKind, TensorArg, TensorIntrinsic, Type};
use std::fmt::Write;

fn scope(body: &str) -> String {
    format!("fn main() -> i64 {{ tensor_scope(4096) {{ {body} }} catch (error: tensor_error) {{ print(tensor_error_code(error)); }} return 0; }}")
}

fn rejects(source: &str, code: &str) -> Vec<Diagnostic> {
    let errors = match axeon::compile(source) {
        Err(errors) => errors,
        Ok(_) => panic!("invalid tensor source must not produce IR (expected {code})"),
    };
    assert!(
        errors.iter().any(|error| error.code == code),
        "{source}\n{errors:#?}"
    );
    assert_eq!(errors, axeon::compile(source).unwrap_err());
    for error in &errors {
        for span in std::iter::once(error.span).chain(error.related.iter().map(|r| r.span)) {
            assert!(span.start <= span.end && span.end <= source.len());
            assert!(source.is_char_boundary(span.start) && source.is_char_boundary(span.end));
        }
    }
    errors
}

#[test]
fn tensor_scope_placement_and_control_flow_are_explicitly_bounded() {
    let inner = "tensor_scope(0) {} catch (e: tensor_error) {}";
    for body in [
        format!("{{ {inner} }}"),
        format!("if true {{ {inner} }}"),
        format!("while false {{ {inner} }}"),
        format!("tensor_scope(0) {{ {inner} }} catch (e: tensor_error) {{}}"),
        format!("tensor_scope(0) {{}} catch (e: tensor_error) {{ {inner} }}"),
    ] {
        rejects(&format!("fn main() -> i64 {{ {body} return 0; }}"), "N001");
    }
    for body in ["return 0;", "if true { return 1; }", "break;", "continue;"] {
        rejects(&scope(body), "N006");
    }
    axeon::compile(&scope(
        "while false { if true { break; } else { continue; } }",
    ))
    .unwrap();
    axeon::compile("fn main() -> i64 { tensor_scope(0) {} catch (e: tensor_error) { return tensor_error_code(e); } return 0; }").unwrap();
}

#[test]
fn try_wraps_only_one_fallible_builtin_and_cannot_escape_its_scope() {
    for body in [
        "try helper();",
        "try (tensor_zeros([]));",
        "try try tensor_zeros([]);",
        "try 1;",
        "try tensor_rank(a);",
        "try tensor_error_code(error);",
    ] {
        rejects(&scope(body), "E_PARSE_TRY");
    }
    for body in [
        "let a: tensor = tensor_zeros([]);",
        "tensor_sum(a);",
        "tensor_set(a, 0, 1.0);",
    ] {
        rejects(&scope(body), "N003");
    }
    rejects(
        "fn main() -> i64 { let a: tensor = try tensor_zeros([]); return 0; }",
        "N002",
    );
    rejects(
        "fn main() -> i64 { print(try tensor_sum(a)); return 0; }",
        "N002",
    );
    rejects("fn main() -> i64 { tensor_scope(0) {} catch (e: tensor_error) { try tensor_sum(a); } return 0; }", "N002");
}

#[test]
fn aggregate_outputs_must_be_named_immediately_and_operands_are_named() {
    for body in [
        "try tensor_zeros([]);",
        "print(try tensor_zeros([]));",
        "let n: i64 = try tensor_zeros([]);",
        "let a: tensor = try tensor_add(try tensor_zeros([]), a);",
    ] {
        rejects(&scope(body), "N004");
    }
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); try tensor_view(&a);"),
        "N004",
    );
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); let b: tensor = a; print(tensor_len(a));"),
        "O006",
    );
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); let b: tensor = a; let c: tensor = a;"),
        "O006",
    );
    rejects(
        &scope("let mut a: tensor = try tensor_zeros([]); a = 1;"),
        "O008",
    );
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); print(a == a);"),
        "O004",
    );
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); print(a[0]);"),
        "O009",
    );
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); print(len(a));"),
        "O009",
    );
}

#[test]
fn tensor_and_error_types_do_not_extend_function_interfaces() {
    for ty in ["tensor", "&tensor", "&mut tensor", "tensor_error"] {
        rejects(
            &format!("fn bad(a: {ty}) -> i64 {{ return 0; }} fn main() -> i64 {{ return 0; }}"),
            "O003",
        );
        rejects(
            &format!("fn bad() -> {ty} {{ return 0; }} fn main() -> i64 {{ return 0; }}"),
            "O003",
        );
    }
    rejects(
        "fn main() -> i64 { let e: tensor_error = 1; return 0; }",
        "N008",
    );
    rejects("fn main() -> i64 { tensor_scope(0) {} catch (e: tensor_error) { let copied: tensor_error = e; } return 0; }", "N008");
    rejects("fn main() -> i64 { tensor_scope(0) {} catch (e: tensor_error) { print(e == e); } return 0; }", "O004");
    rejects(
        "fn main() -> i64 { print(tensor_error_code(1)); return 0; }",
        "N008",
    );
    rejects(
        "fn main() -> i64 { let e: i64 = 0; print(tensor_error_code(e)); return 0; }",
        "T009",
    );
}

#[test]
fn special_shape_lists_do_not_weaken_array_or_type_rules() {
    for (body, code) in [
        (
            "let a: tensor = try tensor_zeros([1,2,3,4,5,6,7,8,9]);",
            "N005",
        ),
        (
            "let n: [i64; 1] = [1]; let a: tensor = try tensor_zeros(n);",
            "N005",
        ),
        ("let a: tensor = try tensor_zeros([1.0]);", "T009"),
        ("let a: tensor = try tensor_zeros([true]);", "T009"),
        ("let a: tensor = try tensor_zeros([[1]]);", "O004"),
        ("let a: [i64; 0] = [];", "O001"),
        (
            "let a: tensor = try tensor_zeros([1; 2]);",
            "E_PARSE_ARRAY_REPEAT",
        ),
        ("let a: tensor = try tensor_from_array([1], [1.0]);", "N004"),
        (
            "let data: [i64; 1] = [1]; let a: tensor = try tensor_from_array([1], data);",
            "T009",
        ),
        ("let a: tensor = try tensor_zeros([], []);", "T008"),
    ] {
        rejects(&scope(body), code);
    }
    // These are runtime-domain failures, not constant-only compile rejections.
    for dimensions in [
        "[]",
        "[0]",
        "[-1]",
        "[65537]",
        "[0, 65537]",
        "[1,1,1,1,1,1,1,1,]",
    ] {
        axeon::compile(&scope(&format!(
            "let a: tensor = try tensor_zeros({dimensions});"
        )))
        .unwrap();
    }
}

#[test]
fn mutable_capabilities_and_whole_owner_loans_are_enforced() {
    let owner = "let mut a: tensor = try tensor_zeros([2]);";
    for suffix in [
        "try tensor_set(a, 0, 1.0);",
        "let moved: tensor = a;",
        "let other: &mut tensor = try tensor_view(&mut a);",
    ] {
        rejects(
            &scope(&format!(
                "{owner} let view: &tensor = try tensor_view(&a); {suffix}"
            )),
            "O007",
        );
    }
    for suffix in [
        "print(try tensor_get(a, 0));",
        "print(tensor_len(a));",
        "print(tensor_rank(a));",
        "let other: &tensor = try tensor_view(&a);",
    ] {
        rejects(
            &scope(&format!(
                "{owner} let view: &mut tensor = try tensor_view(&mut a); {suffix}"
            )),
            "O007",
        );
    }
    rejects(
        &scope("let a: tensor = try tensor_zeros([1]); try tensor_set(a, 0, 1.0);"),
        "O010",
    );
    rejects(
        &scope(
            "let a: tensor = try tensor_zeros([1]); let v: &mut tensor = try tensor_view(&mut a);",
        ),
        "O010",
    );
    rejects(
        &scope(&format!(
            "{owner} let v: &tensor = try tensor_view(&a); try tensor_set(v, 0, 1.0);"
        )),
        "O010",
    );
    axeon::compile(&scope(&format!("{owner} {{ let v: &mut tensor = try tensor_view(&mut a); try tensor_set(v, 0, 1.0); }} let b: tensor = a;"))).unwrap();
    // Empty/disjoint transformations still borrow the entire owner.
    rejects(
        &scope(&format!(
            "{owner} let empty: &tensor = try tensor_slice(&a,0,0,0,1); try tensor_set(a,1,2.0);"
        )),
        "O007",
    );
}

#[test]
fn view_copy_reborrow_composition_and_wrong_capabilities_are_rejected() {
    let prefix = "let mut a: tensor = try tensor_zeros([2]); let v: &tensor = try tensor_view(&a);";
    for (suffix, code) in [
        ("let copied: &tensor = v;", "O005"),
        ("let copied: &tensor = &a;", "N003"),
        ("let copied: &tensor = try tensor_view(&v);", "T009"),
        (
            "let copied: &tensor = try tensor_transpose(v, [0]);",
            "N004",
        ),
        ("let copied: &tensor = try tensor_view(&a[0..1]);", "N004"),
        ("let copied: &mut tensor = try tensor_view(&a);", "T009"),
        (
            "let copied: &tensor = try tensor_transpose(&a, [0.0]);",
            "T009",
        ),
        (
            "let copied: &tensor = try tensor_slice(&a, 0, 0, 1, 1.0);",
            "T009",
        ),
    ] {
        rejects(&scope(&format!("{prefix} {suffix}")), code);
    }
}

#[test]
fn protected_capture_moves_and_handler_joins_preserve_sound_availability() {
    let moved = "let b: [i64; 1] = a;";
    let program = format!("fn main() -> i64 {{ let a: [i64; 1] = [1]; tensor_scope(0) {{ {moved} }} catch (e: tensor_error) {{ print(a[0]); }} return a[0]; }}");
    let errors = rejects(&program, "N007");
    let error = errors.iter().find(|e| e.code == "N007").unwrap();
    let move_start = program.find(moved).unwrap() + moved.rfind('a').unwrap();
    assert_eq!(error.span, Span::new(move_start, move_start + 1));
    assert_eq!(error.related.len(), 1);
    let returning = "fn main() -> i64 { let a: [i64; 1] = [1]; tensor_scope(0) {} catch (e: tensor_error) { let b: [i64; 1] = a; return b[0]; } return a[0]; }";
    axeon::compile(returning).unwrap();
    rejects(&returning.replace("return b[0];", "print(b[0]);"), "O006");
    axeon::compile("fn main() -> i64 { let mut a: [f64; 1] = [1.0]; tensor_scope(8) { a[0] = 2.0; let t: tensor = try tensor_from_array([1], a); } catch (e: tensor_error) { a[0] = 3.0; } print(a[0]); return 0; }").unwrap();
    rejects(
        &scope("let a: tensor = try tensor_zeros([]); while true { let b: tensor = a; break; }"),
        "O011",
    );
    axeon::compile(&scope(
        "while false { let a: tensor = try tensor_zeros([]); let b: tensor = a; continue; }",
    ))
    .unwrap();
}

#[test]
fn scalar_callees_may_attempt_nested_entry_and_scope_bodies_do_not_leak() {
    let program = "fn inner() -> i64 { tensor_scope(-1) {} catch (e: tensor_error) { return tensor_error_code(e); } return 0; } fn main() -> i64 { tensor_scope(8) { let a: tensor = try tensor_zeros([]); print(inner()); } catch (e: tensor_error) {} return 0; }";
    assert!(axeon::compile(program).unwrap().uses_tensors());
    rejects("fn main() -> i64 { tensor_scope(8) { let a: tensor = try tensor_zeros([]); } catch (e: tensor_error) { print(tensor_len(a)); } return 0; }", "N002");
    rejects("fn main() -> i64 { tensor_scope(8) {} catch (e: tensor_error) {} print(tensor_error_code(e)); return 0; }", "T005");
    rejects(&scope("print(tensor_error_code(error));"), "T005");
}

#[test]
fn lowering_preserves_cleanup_order_typed_arguments_and_operation_spans() {
    let program = scope("let mut a: tensor = try tensor_zeros([2]); { let v: &mut tensor = try tensor_view(&mut a); try tensor_set(v, 0, 2.0); } let b: tensor = a; print(try tensor_sum(b) + 1.0);");
    let ir = axeon::compile(&program).unwrap();
    let StmtKind::TensorScope {
        body,
        error_local,
        handler,
        ..
    } = &ir.functions[0].body.stmts[0].kind
    else {
        panic!("scope");
    };
    assert_eq!(body.cleanup, [0, 2]);
    assert_eq!(ir.functions[0].locals[*error_local].ty, Type::TensorError);
    assert!(handler.cleanup.is_empty());
    let StmtKind::Block(inner) = &body.stmts[1].kind else {
        panic!("inner block");
    };
    assert_eq!(inner.cleanup, [1]);
    let StmtKind::Let { value, .. } = &inner.stmts[0].kind else {
        panic!("view let");
    };
    assert_eq!(
        &program[value.span.start..value.span.end],
        "tensor_view(&mut a)"
    );
    assert!(
        matches!(&value.kind, ExprKind::TensorCall { op: TensorIntrinsic::View, args } if matches!(args[0], TensorArg::Local { local: 0, .. }))
    );
    let setter = &inner.stmts[1];
    assert!(matches!(setter.kind, StmtKind::TensorSet { base: 1, .. }));
    assert_eq!(
        &program[setter.span.start..setter.span.end],
        "try tensor_set(v, 0, 2.0);"
    );
    assert!(!axeon::compile("fn main() -> i64 { return 0; }")
        .unwrap()
        .uses_tensors());
}

#[test]
fn malformed_scope_recovery_and_reserved_names_are_bounded() {
    for body in [
        "tensor_scope(8 {} catch (e: tensor_error) {}",
        "tensor_scope(8) {} (e: tensor_error) {}",
        "tensor_scope(8) {} catch e: tensor_error) {}",
        "tensor_scope(8) {} catch (e tensor_error) {}",
        "tensor_scope(8) {} catch (e: i64) {}",
        "tensor_scope(8) {} catch (e: tensor_error {}",
        "catch (e: tensor_error) {}",
        "try",
        "tensor_scope",
    ] {
        let source = format!("fn main() -> i64 {{ {body} return 0; }} fn later() -> i64 {{ let x: i64 = ; return 0; }}");
        let errors = axeon::parser::parse(&source).unwrap_err();
        assert!(errors.len() <= 64);
        assert!(
            errors
                .iter()
                .any(|e| e.span.start >= source.find("fn later").unwrap()),
            "{source}\n{errors:#?}"
        );
    }
    for name in [
        "try",
        "catch",
        "tensor",
        "tensor_error",
        "tensor_scope",
        "tensor_zeros",
        "tensor_error_end",
    ] {
        assert!(axeon::parser::parse(&format!("fn {name}() -> i64 {{ return 0; }}")).is_err());
    }
    axeon::compile(
        "fn tensor_custom() -> i64 { return 1; } fn main() -> i64 { return tensor_custom(); }",
    )
    .unwrap();
}

#[test]
fn tensor_syntax_keeps_all_frontend_resource_limits() {
    let mut locals = String::new();
    for i in 0..256 {
        write!(locals, "let v{i}: i64 = 0;").unwrap();
    }
    rejects(&scope(&locals), "T015"); // The catch binding is the 257th local.
    let nodes = "print(tensor_rank(a));".repeat(800);
    rejects(
        &scope(&format!("let a: tensor = try tensor_zeros([]); {nodes}")),
        "T016",
    );
    let deep = format!("{}0{}", "(".repeat(70), ")".repeat(70));
    rejects(
        &scope(&format!("let a: tensor = try tensor_zeros([{deep}]);")),
        "E_NESTING_LIMIT",
    );
    let wide = vec!["0"; 100_001].join(",");
    rejects(
        &scope(&format!("let a: tensor = try tensor_zeros([{wide}]);")),
        "E_TOKEN_LIMIT",
    );
    rejects(&format!("//{}", "x".repeat(1_048_576)), "E_SOURCE_LIMIT");
}
