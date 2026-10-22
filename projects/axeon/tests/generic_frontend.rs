//! Generic grammar and frozen module interfaces. Semantic proof/instantiation
//! acceptance lives in the common checker tests, not a second frontend checker.
use axeon::ast::{self, ExprKind, StmtKind, Type, TypeExpr};
use axeon::diagnostic::SourceId;
use axeon::parser;
use std::fmt::Write;

fn rejects(source: &str, code: &str) {
    let errors = parser::parse(source).unwrap_err();
    assert!(errors.iter().any(|error| error.code == code), "{errors:?}");
    assert!(!errors.is_empty() && errors.len() <= 64);
    for error in errors {
        assert_eq!(error.span.source, SourceId(0));
        assert!(error.span.start <= error.span.end && error.span.end <= source.len());
        assert!(source.is_char_boundary(error.span.start));
        assert!(source.is_char_boundary(error.span.end));
    }
}

#[test]
fn declarations_preserve_bounds_spans_and_declaration_scoped_parameter_identity() {
    let source = "// λ before declarations\nfn first<T: cap::Forward + Summarize, U>(value:T, other:U)->T{return value;} fn second<T>(value:T)->T{return value;} fn main()->i64{return 0;}";
    let program = parser::parse(source).unwrap();
    assert_eq!(program.functions.len(), 1);
    assert_eq!(program.generic_functions.len(), 2);
    let first = &program.generic_functions[0];
    let second = &program.generic_functions[1];
    assert_eq!(first.parameters.len(), 2);
    assert_eq!(first.parameters[0].name, "T");
    assert_eq!(first.parameters[1].name, "U");
    assert_eq!(
        first.parameters[0]
            .bounds
            .iter()
            .map(|bound| bound.name.as_str())
            .collect::<Vec<_>>(),
        ["cap::Forward", "Summarize"]
    );
    for bound in &first.parameters[0].bounds {
        assert_eq!(&source[bound.span.start..bound.span.end], bound.name);
    }
    let TypeExpr::Parameter(id) = first.function.return_type else {
        panic!("symbolic return annotation")
    };
    assert_eq!(id.binder.source, SourceId(0));
    assert_eq!(id.binder.offset, first.function.name_span.start);
    assert_eq!(id.index, 0);
    assert_eq!(first.function.params[0].ty, TypeExpr::Parameter(id));
    let TypeExpr::Parameter(other) = first.function.params[1].ty else {
        panic!("second parameter annotation")
    };
    assert_eq!(other.binder, id.binder);
    assert_eq!(other.index, 1);
    assert_ne!(first.function.return_type, second.function.return_type);
    assert_eq!(
        &source[first.function.body.span.start..first.function.body.span.end],
        "{return value;}"
    );
}

#[test]
fn generic_calls_keep_explicit_concrete_and_symbolic_arguments_in_one_expression_ast() {
    let source = "record R{x:i64} union U{Empty{}} fn forward<T>(x:T)->T{return identity::<T>(x);} fn main()->i64{return choose::<i64,f64,bool,(),[i64;2],[f64;2],R,U>(0);}";
    let program = parser::parse(source).unwrap();
    let template = &program.generic_functions[0];
    let StmtKind::Return(call) = &template.function.body.stmts[0].kind else {
        panic!("return")
    };
    let ExprKind::GenericCall {
        name,
        type_args,
        args,
    } = &call.kind
    else {
        panic!("generic call")
    };
    assert_eq!(name, "identity");
    assert_eq!(type_args, &[template.function.return_type]);
    assert!(matches!(&args[0].kind, ExprKind::Var(name) if name == "x"));
    let StmtKind::Return(call) = &program.functions[0].body.stmts[0].kind else {
        panic!("return")
    };
    let ExprKind::GenericCall {
        name, type_args, ..
    } = &call.kind
    else {
        panic!("concrete generic call")
    };
    assert_eq!(name, "choose");
    assert_eq!(type_args.len(), 8);
    assert_eq!(
        &type_args[..4],
        &[TypeExpr::I64, TypeExpr::F64, TypeExpr::Bool, TypeExpr::Unit]
    );
    assert_eq!(
        type_args[6],
        TypeExpr::Concrete(Type::Record(program.records[0].id))
    );
    assert_eq!(
        type_args[7],
        TypeExpr::Concrete(Type::Union(program.unions[0].id))
    );
    assert_eq!(
        &source[call.span.start..call.span.end],
        "choose::<i64,f64,bool,(),[i64;2],[f64;2],R,U>(0)"
    );
}

#[test]
fn annotations_propagate_through_shared_branch_loop_match_and_recovery_grammar() {
    let source = "union Choice{Yes{},No{}} fn branch<T>(value:T, flag:bool)->T{let mut n:i64=0;while n<1{n=n+1;continue;}if flag{let yes:T=value;return yes;}else{let no:T=value;return no;}} fn select<T>(value:T,choice:Choice)->T{match choice{Choice::Yes{}=>{let yes:T=value;return yes;},Choice::No{}=>{let no:T=value;return no;},}} fn recover<T>(value:T)->T{tensor_scope(0){let t:tensor=try tensor_zeros([1]);}catch(error:tensor_error){let code:i64=tensor_error_code(error);print(code);} {let next:T=value;return next;}}";
    let program = parser::parse(source).unwrap();
    fn symbolic_lets(block: &ast::Block<TypeExpr>) -> usize {
        block
            .stmts
            .iter()
            .map(|stmt| match &stmt.kind {
                StmtKind::Let {
                    ty: TypeExpr::Parameter(_),
                    ..
                } => 1,
                StmtKind::Block(body) | StmtKind::While { body, .. } => symbolic_lets(body),
                StmtKind::If {
                    then_block,
                    else_block,
                    ..
                } => symbolic_lets(then_block) + else_block.as_ref().map_or(0, symbolic_lets),
                StmtKind::Match { arms, .. } => {
                    arms.iter().map(|arm| symbolic_lets(&arm.body)).sum()
                }
                StmtKind::TensorScope { body, handler, .. } => {
                    symbolic_lets(body) + symbolic_lets(handler)
                }
                _ => 0,
            })
            .sum()
    }
    assert_eq!(
        program
            .generic_functions
            .iter()
            .map(|template| symbolic_lets(&template.function.body))
            .collect::<Vec<_>>(),
        [2, 2, 1]
    );
}

#[test]
fn ordinary_comparisons_calls_and_constructor_paths_keep_their_ast_shapes() {
    let source = "union U{Value{x:i64}} fn compare()->bool{return (1<2)&&(3>2);} fn ordinary()->i64{let u:U=U::Value{x:1};local();module::function();Trait::method(1);module::Trait::method(1);return 0;}";
    let program = parser::parse(source).unwrap();
    assert!(program.generic_functions.is_empty());
    let StmtKind::Return(value) = &program.functions[0].body.stmts[0].kind else {
        panic!("comparison")
    };
    assert!(matches!(
        value.kind,
        ExprKind::Binary {
            op: ast::BinaryOp::And,
            ..
        }
    ));
    let body = &program.functions[1].body;
    assert!(
        matches!(&body.stmts[0].kind, StmtKind::Let {value: ast::Expr {kind: ExprKind::Union {name,..},..},..} if name=="U")
    );
    let names = body.stmts[1..5]
        .iter()
        .map(|stmt| match &stmt.kind {
            StmtKind::Expr(ast::Expr {
                kind: ExprKind::Call { name, .. },
                ..
            }) => name.as_str(),
            other => panic!("ordinary call: {other:?}"),
        })
        .collect::<Vec<_>>();
    assert_eq!(
        names,
        [
            "local",
            "module::function",
            "Trait::method",
            "module::Trait::method"
        ]
    );
}

#[test]
fn type_parameter_shadowing_checks_forward_declarations_but_keeps_value_names_separate() {
    for source in [
        "fn f<T,T>(x:T)->T{return x;}",
        "fn f<Self>(x:Self)->Self{return x;}",
        "fn f<R>(x:R)->R{return x;} record R{n:i64}",
        "fn f<U>(x:U)->U{return x;} union U{Empty{}}",
        "fn f<Contract>(x:Contract)->Contract{return x;} trait Contract{fn use(x:Self)->Self;}",
        "fn f<module>(x:module)->module{return x;} import module;",
        "record unit{n:i64} fn f<unit>(x:unit)->unit{return x;}",
    ] {
        rejects(source, "G001");
    }
    // `unit` is an ordinary identifier; the primitive's source spelling is ().
    let program =
        parser::parse("fn f<unit>(unit:unit)->unit{return unit;} fn g<T>(T:T)->T{return T;}")
            .unwrap();
    assert_eq!(program.generic_functions.len(), 2);
    for spelling in ["i64", "f64", "bool", "tensor", "tensor_error"] {
        rejects(
            &format!("fn f<{spelling}>()->i64{{return 0;}}"),
            "E_PARSE_EXPECTED",
        );
    }
}

#[test]
fn parameter_scope_is_restored_after_success_and_error_recovery() {
    rejects(
        "fn first<T>(x:T)->T{return x;} fn ordinary(x:T)->i64{return 0;}",
        "E_PARSE_TYPE",
    );
    let source = "fn broken<T>(x:T)->T{let n:T=;} fn after()->i64{let x:T=0;return 0;}";
    let errors = parser::parse(source).unwrap_err();
    assert!(
        errors.iter().any(|error| error.code == "E_PARSE_TYPE"
            && error.span.start > source.find("fn after").unwrap()),
        "{errors:?}"
    );
}

#[test]
fn declaration_parameter_bound_and_call_limits_accept_exact_boundaries() {
    let parameters = (0..8)
        .map(|index| format!("T{index}"))
        .collect::<Vec<_>>()
        .join(",");
    let bounds = (0..8)
        .map(|index| format!("Contract{index}"))
        .collect::<Vec<_>>()
        .join("+");
    assert!(parser::parse(&format!(
        "fn f<{parameters}>()->i64{{return 0;}} fn g<T:{bounds}>(x:T)->T{{return x;}}"
    ))
    .is_ok());
    rejects(
        &format!("fn f<{parameters},Extra>()->i64{{return 0;}}"),
        "G002",
    );
    rejects(
        &format!("fn g<T:{bounds}+Extra>(x:T)->T{{return x;}}"),
        "G002",
    );
    rejects(
        "fn main()->i64{return f::<i64,i64,i64,i64,i64,i64,i64,i64,i64>(0);}",
        "G003",
    );
    let mut declarations = String::new();
    for i in 0..128 {
        write!(declarations, "fn generic_{i}<T>(x:T)->T{{return x;}} ").unwrap();
    }
    assert_eq!(
        parser::parse(&declarations)
            .unwrap()
            .generic_functions
            .len(),
        128
    );
    rejects(
        &(declarations.clone() + "fn extra<T>(x:T)->T{return x;}"),
        "G002",
    );
    let mut ordinary = String::new();
    for i in 0..1024 {
        write!(ordinary, "fn ordinary_{i}()->i64{{return 0;}} ").unwrap();
    }
    let combined = parser::parse(&(ordinary + &declarations)).unwrap();
    assert_eq!(combined.functions.len(), 1024);
    assert_eq!(combined.generic_functions.len(), 128);
}

#[test]
fn malformed_and_truncated_generic_syntax_terminates_with_bounded_spans() {
    for source in [
        "fn f<>() -> i64{return 0;}",
        "fn f<T:>(x:T)->T{return x;}",
        "fn f<T:cap::>(x:T)->T{return x;}",
        "fn f<T:cap::Trait+>(x:T)->T{return x;}",
        "fn f<T=i64>(x:T)->T{return x;}",
        "fn f<T>(x:[T;2])->T{return x;}",
        "fn main()->i64{return f::<>(1);}",
        "fn main()->i64{return f::<i64>;}",
        "fn main()->i64{return module::f::<i64(1);}",
        "fn main()->i64{return module::Trait::method::<i64>(1);}",
    ] {
        let errors = parser::parse(source).unwrap_err();
        assert!(!errors.is_empty() && errors.len() <= 64, "{source}");
        assert!(
            errors
                .iter()
                .all(|error| error.span.start <= error.span.end && error.span.end <= source.len()),
            "{errors:?}"
        );
    }
    let valid = "fn identity<T:cap::Forward,U>(x:T,y:U)->T{return helper::<T,U>(x,y);}";
    for boundary in 1..valid.len() {
        let errors = parser::parse(&valid[..boundary]).unwrap_err();
        assert!(errors.len() <= 64);
        assert!(errors.iter().all(|error| error.span.end <= boundary));
    }
    parser::parse(valid).unwrap();
}

mod modules {
    use super::*;
    use axeon::project::{Project, ProjectError};
    use std::fs;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "axeon-generic-frontend-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
        fn write(&self, name: &str, source: &str) {
            fs::write(self.0.join(name), source).unwrap();
        }
        fn load(&self) -> Result<Project, ProjectError> {
            Project::load(&self.0.join("main.ax"), Some(&self.0))
        }
        fn rejects(&self, code: &str) {
            let ProjectError::Diagnostics {
                sources,
                diagnostics,
                module_mode,
            } = self.load().unwrap_err()
            else {
                panic!("language diagnostics")
            };
            assert!(module_mode);
            assert!(
                diagnostics.iter().any(|error| error.code == code),
                "{diagnostics:?}"
            );
            for error in diagnostics {
                let source = &sources.get(error.span.source).unwrap().text;
                assert!(error.span.start <= error.span.end && error.span.end <= source.len());
            }
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn exported_generics_alone_enable_module_mode_and_frozen_body_descriptors() {
        let fixture = Fixture::new();
        let source="export fn zebra<T>(x:T)->T{return x;} fn private<T>(x:T)->T{return x;} export fn alpha<U>(x:U)->U{ // body stays exact\nreturn x;} fn main()->i64{return 0;}";
        fixture.write("main.ax", source);
        let project = fixture.load().unwrap();
        assert!(project.is_module_project());
        let module = &project.modules[0];
        assert!(module.exports.is_empty());
        assert_eq!(
            module
                .generic_exports
                .iter()
                .map(|export| export.name.as_str())
                .collect::<Vec<_>>(),
            ["alpha", "zebra"]
        );
        let export = &module.generic_exports[0];
        assert_eq!(export.params, [export.return_type]);
        assert_eq!(export.parameters[0].name, "U");
        let span = export.body_span;
        assert_eq!(
            &project.sources.get(span.source).unwrap().text[span.start..span.end],
            "{ // body stays exact\nreturn x;}"
        );
        fixture.write("main.ax", "fn main()->i64{return 99;}");
        assert_eq!(
            &project.sources.get(span.source).unwrap().text[span.start..span.end],
            "{ // body stays exact\nreturn x;}"
        );
    }

    #[test]
    fn direct_import_types_bounds_and_calls_share_nominal_source_identity() {
        let fixture = Fixture::new();
        fixture.write("main.ax","import types; import helpers; fn main()->i64{let packet:types::Packet=types::Packet{n:42};let next:types::Packet=helpers::forward::<types::Packet>(packet);return next.n;}");
        fixture.write(
            "types.ax",
            "export record Packet{n:i64} export trait Forward{fn forward(value:Self)->Self;}",
        );
        fixture.write("helpers.ax","import types; export fn forward<T:types::Forward>(x:T)->T{return types::Forward::forward(x);} export fn signature<T>(x:T,p:types::Packet)->T{return x;}");
        let project = fixture.load().unwrap();
        let helper = project
            .modules
            .iter()
            .find(|module| project.sources.get(module.source).unwrap().logical_path == "helpers.ax")
            .unwrap();
        let types = project
            .modules
            .iter()
            .find(|module| project.sources.get(module.source).unwrap().logical_path == "types.ax")
            .unwrap();
        let forward = &helper.generic_exports[0];
        assert_eq!(forward.parameters[0].bounds[0].name, "types::Forward");
        let bound = project
            .trait_definition(helper.source, &forward.parameters[0].bounds[0].name)
            .unwrap();
        assert_eq!(bound.id.source, types.source);
        let signature = &helper.generic_exports[1];
        assert_eq!(
            signature.params[1],
            TypeExpr::Concrete(types.type_exports[0].ty)
        );
        assert_eq!(signature.params[0], signature.return_type);
    }

    #[test]
    fn strict_parse_rejects_private_and_transitive_type_arguments_after_header_discovery() {
        let fixture = Fixture::new();
        fixture.write(
            "main.ax",
            "import types; fn main()->i64{return identity::<types::Hidden>(0);}",
        );
        fixture.write("types.ax", "record Hidden{n:i64}");
        fixture.rejects("E_PARSE_TYPE");
        fixture.write(
            "main.ax",
            "import bridge; fn main()->i64{return identity::<types::Packet>(0);}",
        );
        fixture.write(
            "bridge.ax",
            "import types; export fn value()->i64{return 0;}",
        );
        fixture.write("types.ax", "export record Packet{n:i64}");
        fixture.rejects("E_PARSE_TYPE");
    }

    #[test]
    fn project_template_budget_counts_each_frozen_module_once_and_separately() {
        let fixture = Fixture::new();
        fixture.write(
            "main.ax",
            "import left; import right; fn main()->i64{return 0;}",
        );
        fixture.write("left.ax", "import shared; export fn left()->i64{return 0;}");
        fixture.write(
            "right.ax",
            "import shared; export fn right()->i64{return 0;}",
        );
        let mut shared = String::new();
        for i in 0..128 {
            write!(shared, "export fn f_{i}<T>(x:T)->T{{return x;}} ").unwrap();
        }
        fixture.write("shared.ax", &shared);
        let project = fixture.load().unwrap();
        assert_eq!(project.modules.len(), 4);
        assert_eq!(
            project
                .modules
                .iter()
                .map(|module| module.generic_exports.len())
                .sum::<usize>(),
            128
        );
        fixture.write("left.ax", "import shared; fn extra<T>(x:T)->T{return x;}");
        fixture.rejects("E_PROJECT_GENERICS");
    }

    #[test]
    fn generic_body_discovery_checks_complete_grammar_and_graph_rules() {
        let fixture = Fixture::new();
        fixture.write("main.ax", "import api; fn main()->i64{return 0;}");
        fixture.write("api.ax", "export fn broken<T>(x:T)->T{let y:T=;return x;}");
        fixture.rejects("E_PARSE_EXPRESSION");
        fixture.write(
            "api.ax",
            "import main; export fn good<T>(x:T)->T{return x;}",
        );
        fixture.rejects("E_MODULE_CYCLE");
    }
}
