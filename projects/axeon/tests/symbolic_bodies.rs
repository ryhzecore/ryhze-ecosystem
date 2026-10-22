//! The same semantic traversal checks opaque bodies before concrete substitution.
//! Fixtures construct opaque annotations explicitly; no concrete stand-in is
//! ever checked as a substitute for a type parameter.
use axeon::ast::{self, TypeExpr as T};
use axeon::diagnostic::{Diagnostic, Span};
use std::fmt::Write;

fn site() -> Span {
    Span::new(10_000, 10_001)
}
fn expression(source: &str) -> ast::Expr {
    let mut p = axeon::parser::parse(&format!("fn main()->i64{{return {source};}}")).unwrap();
    match p.functions.remove(0).body.stmts.remove(0).kind {
        ast::StmtKind::Return(value) => value,
        _ => unreachable!(),
    }
}
fn stmt(kind: ast::StmtKind<T>) -> ast::Stmt<T> {
    ast::Stmt { kind, span: site() }
}
fn lift(block: ast::Block) -> ast::Block<T> {
    use ast::StmtKind as S;
    ast::Block {
        span: block.span,
        stmts: block
            .stmts
            .into_iter()
            .map(|statement| ast::Stmt {
                span: statement.span,
                kind: match statement.kind {
                    S::Let {
                        name,
                        name_span,
                        mutable,
                        ty,
                        value,
                    } => S::Let {
                        name,
                        name_span,
                        mutable,
                        ty: ty.into(),
                        value,
                    },
                    S::Assign {
                        name,
                        name_span,
                        value,
                    } => S::Assign {
                        name,
                        name_span,
                        value,
                    },
                    S::IndexAssign {
                        name,
                        name_span,
                        index,
                        value,
                    } => S::IndexAssign {
                        name,
                        name_span,
                        index,
                        value,
                    },
                    S::FieldAssign {
                        name,
                        name_span,
                        field,
                        field_span,
                        value,
                    } => S::FieldAssign {
                        name,
                        name_span,
                        field,
                        field_span,
                        value,
                    },
                    S::If {
                        condition,
                        then_block,
                        else_block,
                    } => S::If {
                        condition,
                        then_block: lift(then_block),
                        else_block: else_block.map(lift),
                    },
                    S::While { condition, body } => S::While {
                        condition,
                        body: lift(body),
                    },
                    S::Block(body) => S::Block(lift(body)),
                    S::Match {
                        name,
                        name_span,
                        arms,
                    } => S::Match {
                        name,
                        name_span,
                        arms: arms
                            .into_iter()
                            .map(|arm| ast::MatchArm {
                                union_name: arm.union_name,
                                union_span: arm.union_span,
                                application: arm.application,
                                variant: arm.variant,
                                variant_span: arm.variant_span,
                                fields: arm.fields,
                                body: lift(arm.body),
                            })
                            .collect(),
                    },
                    S::TensorScope {
                        budget,
                        body,
                        error_name,
                        error_span,
                        handler,
                    } => S::TensorScope {
                        budget,
                        body: lift(body),
                        error_name,
                        error_span,
                        handler: lift(handler),
                    },
                    S::Return(e) => S::Return(e),
                    S::Print(e) => S::Print(e),
                    S::Expr(e) => S::Expr(e),
                    S::Break => S::Break,
                    S::Continue => S::Continue,
                },
            })
            .collect(),
    }
}
fn body(source: &str) -> ast::Block<T> {
    let mut program = axeon::parser::parse(&format!("fn main()->i64{{{source}}}")).unwrap();
    lift(program.functions.remove(0).body)
}
fn parameter() -> T {
    T::Parameter(ast::TypeParameterId {
        binder: ast::RecordId {
            source: site().source,
            offset: site().start,
        },
        index: 0,
    })
}
fn template(source: &str, result: T, bounds: &[&str]) -> ast::GenericFunction {
    ast::GenericFunction {
        parameters: vec![ast::TypeParameter {
            name: "T".into(),
            name_span: site(),
            bounds: bounds
                .iter()
                .map(|name| ast::TraitBound {
                    name: (*name).into(),
                    span: site(),
                })
                .collect(),
        }],
        function: ast::Function {
            exported: false,
            name: "generic".into(),
            name_span: site(),
            params: vec![ast::Param {
                name: "value".into(),
                name_span: site(),
                ty: parameter(),
            }],
            return_type: result,
            body: body(source),
            span: site(),
        },
    }
}
fn let_parameter(name: &str, value: &str) -> ast::Stmt<T> {
    stmt(ast::StmtKind::Let {
        name: name.into(),
        name_span: site(),
        mutable: false,
        ty: parameter(),
        value: expression(value),
    })
}
fn check(context: &str, templates: &[ast::GenericFunction]) -> Result<(), Vec<Diagnostic>> {
    let program = axeon::parser::parse(&format!("{context}\nfn main()->i64{{return 0;}}")).unwrap();
    axeon::check::check_templates(&program, templates)
}
fn rejects(context: &str, template: ast::GenericFunction, code: &str) {
    let errors = check(context, &[template]).expect_err("symbolic proof must reject");
    assert!(
        errors.iter().any(|e| e.code == code),
        "missing {code}: {errors:?}"
    );
}
const CONTRACTS: &str = "trait Forward { fn forward(value:Self)->Self; } trait Measure { fn measure(value:Self)->i64; } trait Test { fn test(value:Self)->bool; }";

#[test]
fn opaque_identity_and_move_destinations_require_no_physical_type() {
    check("", &[template("return value;", parameter(), &[])]).unwrap();
    let mut t = template("return next;", parameter(), &[]);
    t.function
        .body
        .stmts
        .insert(0, let_parameter("next", "value"));
    check("", &[t]).unwrap();
}

#[test]
fn every_unused_body_is_checked_and_opaque_values_cannot_be_duplicated() {
    let mut t = template("return value;", parameter(), &[]);
    t.function
        .body
        .stmts
        .insert(0, let_parameter("moved", "value"));
    rejects("", t, "O006");
    rejects(
        "",
        template("let wrong:bool=1;return 0;", T::I64, &[]),
        "T009",
    );
}

#[test]
fn opaque_values_have_no_undeclared_scalar_projection_or_borrow_capabilities() {
    for source in [
        "return value+1;",
        "return value==value;",
        "print(value);return 0;",
        "return value.field;",
        "return value[0];",
        "let view:&[i64]=&value;return 0;",
    ] {
        assert!(
            check("", &[template(source, T::I64, &[])]).is_err(),
            "accepted {source}"
        );
    }
}

#[test]
fn bound_calls_use_exact_contracts_and_opaque_results_require_capture() {
    let mut t = template(
        "return Measure::measure(next);",
        T::I64,
        &["Forward", "Measure"],
    );
    t.function
        .body
        .stmts
        .insert(0, let_parameter("next", "Forward::forward(value)"));
    check(CONTRACTS, &[t]).unwrap();
    check(
        CONTRACTS,
        &[template(
            "return Forward::forward(value);",
            parameter(),
            &["Forward"],
        )],
    )
    .unwrap();
    rejects(
        CONTRACTS,
        template("return Measure::measure(value);", T::I64, &["Forward"]),
        "G004",
    );
    rejects(
        CONTRACTS,
        template("Forward::forward(value);return 0;", T::I64, &["Forward"]),
        "O004",
    );
}

#[test]
fn argument_transfers_and_short_circuit_paths_are_conservative_and_ordered() {
    let contracts =
        "trait Pair { fn pair(a:Self,b:Self)->i64; } trait Test { fn test(value:Self)->bool; }";
    rejects(
        contracts,
        template("return Pair::pair(value,value);", T::I64, &["Pair"]),
        "O006",
    );
    let mut t = template(
        "if false&&Test::test(value){}return value;",
        parameter(),
        &["Test"],
    );
    rejects(contracts, t.clone(), "O006");
    t.function.body = body("if true||Test::test(value){}return value;");
    rejects(contracts, t, "O006");
}

#[test]
fn continuing_branches_and_loop_backedges_share_the_ordinary_move_rules() {
    check(
        CONTRACTS,
        &[template(
            "if true{return Measure::measure(value);}return Measure::measure(value);",
            T::I64,
            &["Measure"],
        )],
    )
    .unwrap();
    rejects(
        CONTRACTS,
        template(
            "if true{Measure::measure(value);}return Measure::measure(value);",
            T::I64,
            &["Measure"],
        ),
        "O006",
    );
    rejects(
        CONTRACTS,
        template("while Test::test(value){}return 0;", T::I64, &["Test"]),
        "O011",
    );
}

#[test]
fn captured_opaque_owners_remain_available_to_tensor_recovery() {
    rejects(
        CONTRACTS,
        template(
            "tensor_scope(64){Measure::measure(value);}catch(error:tensor_error){}return 0;",
            T::I64,
            &["Measure"],
        ),
        "N007",
    );
    check(CONTRACTS, &[template("tensor_scope(64){let a:tensor=try tensor_zeros([1]);print(try tensor_sum(a));}catch(error:tensor_error){return Measure::measure(value);}return Measure::measure(value);", T::I64, &["Measure"])]).unwrap();
}

#[test]
fn concrete_array_loans_are_checked_inside_an_opaque_body() {
    check(
        "",
        &[template(
            "let a:[i64;1]=[4];{let view:&[i64]=&a;print(view[0]);}let b:[i64;1]=a;return b[0];",
            T::I64,
            &[],
        )],
    )
    .unwrap();
    rejects(
        "",
        template(
            "let mut a:[i64;1]=[4];let view:&mut [i64]=&mut a;return a[0];",
            T::I64,
            &[],
        ),
        "O007",
    );
}

#[test]
fn concrete_partial_moves_remain_exact_inside_an_opaque_body() {
    let context = "record Pair {left:[i64;1],right:[i64;1]}";
    let fixture = |source| {
        let mut program =
            axeon::parser::parse(&format!("{context}\nfn main()->i64{{{source}}}")).unwrap();
        let mut t = template("return 0;", T::I64, &[]);
        t.function.body = lift(program.functions.remove(0).body);
        t
    };
    check(context, &[fixture("let pair:Pair=Pair{left:[1],right:[2]};let left:[i64;1]=pair.left;let right:[i64;1]=pair.right;return left[0]+right[0];")]).unwrap();
    rejects(context, fixture("let pair:Pair=Pair{left:[1],right:[2]};let left:[i64;1]=pair.left;let again:Pair=pair;return 0;"), "O006");
}

#[test]
fn known_storage_limits_are_preserved_without_inventing_parameter_size() {
    let values = ["0"; 32].join(",");
    let mut source = String::new();
    for index in 0..8 {
        write!(&mut source, "let a{index}:[i64;32]=[{values}];").unwrap();
    }
    check(
        "",
        &[template(&(source.clone() + "return 0;"), T::I64, &[])],
    )
    .unwrap();
    write!(&mut source, "let extra:[i64;32]=[{values}];return 0;").unwrap();
    rejects("", template(&source, T::I64, &[]), "O002");
}

#[test]
fn parameter_identity_is_scoped_to_its_declaration() {
    let mut t = template("return value;", parameter(), &[]);
    t.function.params[0].ty = T::Parameter(ast::TypeParameterId {
        binder: ast::RecordId {
            source: site().source,
            offset: site().start + 1,
        },
        index: 0,
    });
    rejects("", t, "G001");
}

#[test]
fn parameter_bounds_and_generic_interfaces_are_validated_without_instantiation() {
    rejects(
        CONTRACTS,
        template("return 0;", T::I64, &["Missing"]),
        "TR004",
    );
    rejects(
        CONTRACTS,
        template("return 0;", T::I64, &["Forward", "Forward"]),
        "G004",
    );
    let mut t = template("return 0;", T::I64, &["Forward"]);
    t.function.exported = true;
    rejects(CONTRACTS, t, "G004");
    let mut t = template("return 0;", T::I64, &[]);
    t.function.params.push(ast::Param {
        name: "tensor".into(),
        name_span: site(),
        ty: ast::Type::Tensor.into(),
    });
    rejects("", t, "O003");
}

#[test]
fn proof_recursion_uses_predeclared_signatures_without_expanding_bodies() {
    let explicit_call = |template: &mut ast::GenericFunction| {
        let expression = match &mut template.function.body.stmts[0].kind {
            ast::StmtKind::Return(expression) | ast::StmtKind::Expr(expression) => expression,
            _ => unreachable!(),
        };
        let ast::ExprKind::Call { name, args } = &expression.kind else {
            unreachable!()
        };
        expression.kind = ast::ExprKind::GenericCall {
            name: name.clone(),
            type_args: vec![parameter()],
            args: args.clone(),
        };
    };
    let mut recursive = template("return generic(value);", parameter(), &[]);
    explicit_call(&mut recursive);
    check("", &[recursive]).unwrap();
    let mut t = template("generic(value);return 0;", T::I64, &[]);
    explicit_call(&mut t);
    t.function
        .body
        .stmts
        .insert(1, let_parameter("moved", "value"));
    rejects("", t, "O006");
}

#[test]
fn concrete_and_proof_modes_share_resource_and_diagnostic_rules() {
    let mut t = template("return 0;", T::I64, &[]);
    for index in 0..256 {
        t.function.body.stmts.insert(
            0,
            stmt(ast::StmtKind::Let {
                name: format!("x{index}"),
                name_span: site(),
                mutable: false,
                ty: T::I64,
                value: expression("0"),
            }),
        );
    }
    rejects("", t, "T015");
    let t = template(
        "let wrong:bool=1;print(true+false);return missing();",
        T::I64,
        &[],
    );
    let proof = check("", &[t]).unwrap_err();
    let concrete =
        axeon::compile("fn main()->i64{let wrong:bool=1;print(true+false);return missing();}")
            .unwrap_err();
    assert_eq!(
        proof
            .iter()
            .map(|e| (&e.code, &e.message))
            .collect::<Vec<_>>(),
        concrete
            .iter()
            .map(|e| (&e.code, &e.message))
            .collect::<Vec<_>>()
    );
}
