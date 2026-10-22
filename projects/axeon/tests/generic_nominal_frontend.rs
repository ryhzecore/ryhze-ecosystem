//! Complete source parsing, frozen module loading and nominal arena ownership.
use axeon::ast::{self, ExprKind, NominalKind, StmtKind, Type, TypeExpr};
use axeon::{parser, tooling};
use std::fmt::Write;

fn application(program: &ast::SourceProgram, ty: TypeExpr) -> &ast::TypeApplication {
    let TypeExpr::Application(id) = ty else {
        panic!("expected nominal application: {ty:?}")
    };
    program
        .applications
        .get(id)
        .expect("application owned by source arena")
}

fn rejects(source: &str, code: &str) {
    let errors = parser::parse_source(source).expect_err("invalid source must reject");
    assert!(errors.iter().any(|error| error.code == code), "{errors:?}");
    assert!(errors.len() <= 64);
    assert!(errors
        .iter()
        .all(|error| error.span.start <= error.span.end && error.span.end <= source.len()));
}

#[test]
fn forward_declarations_nested_applications_and_parameter_binders_are_preserved() {
    let source = "fn relay<T>(x:Choice<Cell<T>>)->Choice<Cell<T>>{return x;} union Choice<T>{Present{value:T},Empty{}} record Cell<T>{value:T}";
    let program = parser::parse_source(source).unwrap();
    let function = &program.generic_functions[0];
    let choice = &program.generic_unions[0];
    let cell = &program.generic_records[0];
    let outer = application(&program, function.function.params[0].ty);
    assert_eq!(outer.declaration, choice.union.id.declaration().unwrap());
    assert_eq!(outer.kind, NominalKind::Union);
    let inner = application(&program, outer.arguments[0]);
    assert_eq!(inner.declaration, cell.record.id.declaration().unwrap());
    let TypeExpr::Parameter(parameter) = inner.arguments[0] else {
        panic!("opaque payload")
    };
    assert_eq!(parameter.binder.offset, function.function.name_span.start);
    assert_ne!(parameter.binder.offset, cell.record.name_span.start);
    assert_ne!(
        cell.record.fields[0].ty,
        choice.union.variants[0].fields[0].ty
    );
    assert_eq!(
        &source[cell.record.name_span.start..cell.record.name_span.end],
        "Cell"
    );
}

#[test]
fn bounds_are_scoped_to_each_nominal_and_parameter_environment_is_restored() {
    let program = parser::parse_source("import contracts; record First<T:contracts::Read+contracts::Send>{value:T} union Second<T>{Some{value:T},None{}} record unit{value:i64} fn identity<U>(x:U)->U{return x;}").unwrap();
    assert_eq!(
        program.generic_records[0].parameters[0]
            .bounds
            .iter()
            .map(|bound| bound.name.as_str())
            .collect::<Vec<_>>(),
        ["contracts::Read", "contracts::Send"]
    );
    assert_eq!(program.records[0].name, "unit");
    rejects(
        "record Box<T>{value:T} fn broken(x:T)->i64{return 0;}",
        "E_PARSE_TYPE",
    );
    let errors = parser::parse_source("record Broken<T,T>{value:T} fn later(x:T)->i64{return 0;}")
        .unwrap_err();
    assert!(errors.iter().any(|error| error.code == "G001"));
    assert!(
        errors.iter().any(|error| error.code == "E_PARSE_TYPE"),
        "failed nominal parameter scopes must also be restored: {errors:?}"
    );
    for source in [
        "record Box<T,T>{value:T}",
        "record Box<Self>{value:Self}",
        "record Box<Later>{value:Later} record Later{value:i64}",
        "record Box<Bound>{value:Bound} trait Bound{fn get(x:Self)->i64;}",
        "import data; record Box<data>{value:data}",
    ] {
        rejects(source, "G001");
    }
}

#[test]
fn constructor_and_pattern_nodes_keep_full_explicit_applications() {
    let source = "record Cell<T>{value:T} union Choice<T>{Present{value:T},Empty{}} fn pass<T>(x:T)->T{let c:Cell<T>=Cell::<T>{value:x};let u:Choice<Cell<T>>=Choice::<Cell<T>>::Present{value:c};match u{Choice::<Cell<T>>::Present{value:moved}=>{return moved.value;},Choice::<Cell<T>>::Empty{}=>{return fail::<T>();}}}";
    let program = parser::parse_source(source).unwrap();
    let body = &program.generic_functions[0].function.body;
    let StmtKind::Let { value, .. } = &body.stmts[0].kind else {
        panic!("record local")
    };
    let ExprKind::Record {
        application: Some(ty),
        name,
        ..
    } = &value.kind
    else {
        panic!("explicit record")
    };
    assert_eq!(name, "Cell");
    assert_eq!(application(&program, *ty).kind, NominalKind::Record);
    let StmtKind::Let { value, .. } = &body.stmts[1].kind else {
        panic!("union local")
    };
    let ExprKind::Union {
        application: Some(ty),
        variant,
        ..
    } = &value.kind
    else {
        panic!("explicit union")
    };
    assert_eq!(variant, "Present");
    assert_eq!(application(&program, *ty).kind, NominalKind::Union);
    let StmtKind::Match { arms, .. } = &body.stmts[2].kind else {
        panic!("consuming match")
    };
    for arm in arms {
        let node = application(&program, arm.application.unwrap());
        assert_eq!(
            node.declaration,
            program.generic_unions[0].union.id.declaration().unwrap()
        );
        assert!(matches!(node.arguments[0], TypeExpr::Application(_)));
        assert!(source[arm.union_span.start..arm.union_span.end].starts_with("Choice::<Cell<T>>"));
    }
}

#[test]
fn virtual_gt_and_eq_tokens_preserve_error_spans_and_following_statements() {
    let prefix = "// λ\nrecord Cell<T>{value:T} ";
    let source = format!("{prefix}fn main()->i64{{let x:Cell<i64>=;return 0;}}");
    let errors = parser::parse_source(&source).unwrap_err();
    let error = errors
        .iter()
        .find(|error| error.code == "E_PARSE_EXPRESSION")
        .unwrap();
    assert_eq!(&source[error.span.start..error.span.end], ";");
    let source = format!("{prefix}fn main()->i64{{let x:Cell<Cell<i64>>=;return 0;}}");
    let errors = parser::parse_source(&source).unwrap_err();
    let error = errors
        .iter()
        .find(|error| error.code == "E_PARSE_EXPRESSION")
        .unwrap();
    assert_eq!(&source[error.span.start..error.span.end], ";");
    // The second byte of >> remains an actual unexpected token for one level.
    let source = format!("{prefix}fn f(x:Cell<i64>>)->i64{{return 0;}}");
    let errors = parser::parse_source(&source).unwrap_err();
    let error = errors
        .iter()
        .find(|error| error.code == "E_PARSE_EXPECTED")
        .unwrap();
    assert_eq!(error.span.start, source.find(">>").unwrap() + 1);
    assert_eq!(&source[error.span.start..error.span.end], ">");
}

#[test]
fn parser_preserves_legacy_comparisons_shifts_and_concrete_api() {
    let source = "fn main()->i64{let n:i64=64 >> 2;if n>=16 && 2<3{return n << 1;}return 0;}";
    let program: ast::Program<Type> = parser::parse(source).unwrap();
    assert_eq!(
        axeon::interpreter::run(&axeon::check::check(&program).unwrap(), 1000)
            .result
            .unwrap(),
        32
    );
    let source_program = parser::parse_source(source).unwrap();
    assert!(source_program.applications.is_empty());
    let errors = parser::parse("record Cell<T>{value:T} fn main()->i64{return 0;}").unwrap_err();
    assert!(errors.iter().any(|error| error.code == "G007"));
}

#[test]
fn clones_share_immutable_application_ownership_and_other_parses_do_not() {
    let source = "record Cell<T>{value:T} fn f(x:Cell<i64>)->i64{return x.value;}";
    let program = parser::parse_source(source).unwrap();
    let copy = program.clone();
    let other = parser::parse_source(source).unwrap();
    let TypeExpr::Application(id) = program.functions[0].params[0].ty else {
        panic!("application")
    };
    assert_eq!(program.applications.owner(), copy.applications.owner());
    assert!(std::ptr::eq(
        program.applications.get(id).unwrap(),
        copy.applications.get(id).unwrap()
    ));
    assert!(other.applications.get(id).is_none());
    assert!(program
        .applications
        .get(ast::TypeApplicationId {
            owner: id.owner,
            index: u32::MAX
        })
        .is_none());
}

#[test]
fn explicit_arguments_arity_and_unsupported_forms_reject_without_inference() {
    for source in [
        "record Cell<T>{value:T} fn f(x:Cell)->i64{return 0;}",
        "record Cell<T>{value:T} fn f(x:Cell<i64,bool>)->i64{return 0;}",
        "record Plain{value:i64} fn f(x:Plain<i64>)->i64{return 0;}",
    ] {
        rejects(source, "G003");
    }
    for source in [
        "record Box<>{value:i64}", "record Box<T=i64>{value:T}",
        "record Box<T>{value:[T;2]}", "record Box<T:>{value:T}",
        "record Box<T>{value:T} fn f()->i64{let x:Box<i64>=Box<i64>{value:1};return 0;}",
        "union Choice<T>{Empty{}} fn f()->i64{let x:Choice<i64>=Choice::<i64>::Empty{};match x{Choice::<>::Empty{}=>{return 0;}}}",
    ] {
        let errors = parser::parse_source(source).unwrap_err();
        assert!(!errors.is_empty() && errors.len() <= 64);
        assert!(errors.iter().all(|error| error.span.end <= source.len()));
    }
}

#[test]
fn depth_and_declaration_limits_are_separate_and_accept_the_boundary() {
    for depth in [1, 31, 32] {
        let ty = format!("{}i64{}", "Cell<".repeat(depth), ">".repeat(depth));
        let source = format!("record Cell<T>{{value:T}} fn f(x:{ty})->i64{{return 0;}}");
        parser::parse_source(&source).unwrap();
        parser::parse_source(&format!(
            "record Cell<T>{{value:T}} fn f()->i64{{return helper::<{ty}>();}}"
        ))
        .unwrap();
        // The continuation, not an equally spelled nominal header, determines
        // whether the outer turbofish contributes a nominal application level.
        parser::parse_source(&format!("record Cell<T>{{value:T}} fn Cell<U>(x:U)->U{{return x;}} fn f()->i64{{return Cell::<{ty}>(unknown);}}")).unwrap();
    }
    let ty = format!("{}i64{}", "Cell<".repeat(33), ">".repeat(33));
    rejects(
        &format!("record Cell<T>{{value:T}} fn f(x:{ty})->i64{{return 0;}}"),
        "G002",
    );
    let inner = format!("{}i64{}", "Cell<".repeat(32), ">".repeat(32));
    rejects(&format!("record Cell<T>{{value:T}} fn f()->i64{{let x:i64=Cell::<{inner}>{{value:unknown}};return 0;}}"), "G002");
    let mut declarations = String::new();
    for index in 0..128 {
        write!(declarations, "record Cell{index}<T>{{value:T}} ").unwrap();
    }
    assert_eq!(
        parser::parse_source(&declarations)
            .unwrap()
            .generic_records
            .len(),
        128
    );
    rejects(
        &format!("{declarations}union Extra<T>{{Empty{{}}}}"),
        "G002",
    );
    for index in 0..128 {
        write!(declarations, "record Plain{index}{{value:i64}} ").unwrap();
    }
    let program = parser::parse_source(&declarations).unwrap();
    assert_eq!(program.records.len() + program.generic_records.len(), 256);
    rejects(&format!("{declarations}record Extra{{value:i64}}"), "R001");
}

#[test]
fn grammar_discovery_and_formatting_preserve_nested_closures_and_comments() {
    let source = "import types; export fn pass<T:types::Read>(x:types::Cell<types::Cell<T>>)->types::Cell<types::Cell<T>>{let next:types::Cell<types::Cell<T>>=helper::<types::Cell<types::Cell<T>>>(x);return next;}";
    parser::validate_syntax(source).unwrap();
    assert!(
        parser::parse_source(source).is_err(),
        "syntax placeholders cannot enter strict parsing"
    );
    for source in [
        source,
        "record Cell<T>{value:T}fn f(x:Cell<Cell<i64> // inner\n >)->i64{return 8>>1;}",
        "record Cell<T>{value:T}fn f()->i64{let x:Cell<i64>=Cell::<i64>{value:42};return x.value;}",
    ] {
        let formatted = tooling::format_source(source).unwrap();
        parser::validate_syntax(&formatted).unwrap();
        assert_eq!(tooling::format_source(&formatted).unwrap(), formatted);
        if source.contains("// inner") {
            assert!(formatted.contains("// inner"));
        }
    }
    // In `>>=`, the lexer produces Shr followed by Eq. Formatting may insert
    // whitespace between those tokens; a `>=` substring is not a token oracle.
    // Check both the type-context split and ordinary operators through execution.
    for (source, expected) in [
        ("record Cell<T>{value:T}fn main()->i64{let inner:Cell<i64>=Cell::<i64>{value:42};let outer:Cell<Cell<i64>>=Cell::<Cell<i64>>{value:inner};return outer.value.value;}", 42),
        ("record Cell<T>{value:T}fn f(x:Cell<Cell<i64> // inner\n >)->i64{return x.value.value>>1;}fn main()->i64{let inner:Cell<i64>=Cell::<i64>{value:8};let outer:Cell<Cell<i64>>=Cell::<Cell<i64>>{value:inner};return f(outer);}", 4),
        ("fn main()->i64{let n:i64=8>>1;if n>=4{return n;}return 0;}", 4),
    ] {
        let formatted = tooling::format_source(source).unwrap();
        for text in [source, formatted.as_str()] {
            let program = axeon::compile(text).unwrap();
            assert_eq!(
                axeon::interpreter::run(&program, 1000).result.unwrap(),
                expected
            );
        }
    }
}

#[test]
fn truncated_type_and_constructor_syntax_has_bounded_recoverable_spans() {
    let prefix = "record Cell<T>{value:T} union Choice<T>{Some{value:T},None{}} ";
    let function = "fn f<T>(x:Cell<Choice<T>>)->Cell<Choice<T>>{return Cell::<Choice<T>>{value:Choice::<T>::None{}};}";
    for boundary in 1..function.len() {
        let source = format!("{prefix}{}", &function[..boundary]);
        let errors = parser::parse_source(&source).unwrap_err();
        assert!(!errors.is_empty() && errors.len() <= 64);
        assert!(errors
            .iter()
            .all(|error| error.span.start <= error.span.end && error.span.end <= source.len()));
    }
    parser::parse_source(&format!("{prefix}{function}")).unwrap();
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
                "axeon-generic-nominal-frontend-{}-{}",
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
            Project::load(&self.0.join("main.ax"), None)
        }
        fn rejects(&self, code: &str) {
            let ProjectError::Diagnostics { diagnostics, .. } = self.load().unwrap_err() else {
                panic!("language error")
            };
            assert!(
                diagnostics.iter().any(|error| error.code == code),
                "{diagnostics:?}"
            );
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn both_project_paths_retain_source_and_recheck_fresh_frozen_sessions() {
        let fixture = Fixture::new();
        let source = "record Cell<T>{value:T} fn main()->i64{let x:Cell<i64>=Cell::<i64>{value:42};return x.value;}";
        fixture.write("main.ax", source);
        let project = fixture.load().unwrap();
        assert!(!project.is_module_project());
        assert!(project.nominal_session().is_some());
        assert!(project.source_program(project.entry).is_some());
        fixture.write("main.ax", "broken input after the frozen read");
        for _ in 0..2 {
            assert_eq!(
                axeon::interpreter::run(&project.check().unwrap(), 1000)
                    .result
                    .unwrap(),
                42
            );
        }
        fixture.write(
            "types.ax",
            "export record Cell<T>{value:T} export union Choice<T>{Some{value:T},None{}}",
        );
        fixture.write("main.ax", "import types; fn main()->i64{let cell:types::Cell<i64>=types::Cell::<i64>{value:42};let choice:types::Choice<types::Cell<i64>>=types::Choice::<types::Cell<i64>>::Some{value:cell};match choice{types::Choice::<types::Cell<i64>>::Some{value:answer}=>{return answer.value;},types::Choice::<types::Cell<i64>>::None{}=>{return 0;}}}");
        let project = fixture.load().unwrap();
        assert!(project.is_module_project());
        let types = project
            .modules
            .iter()
            .find(|module| project.sources.get(module.source).unwrap().logical_path == "types.ax")
            .unwrap();
        assert_eq!(types.generic_nominal_exports.len(), 2);
        assert!(
            types.type_exports.is_empty(),
            "generated layouts are not source declarations"
        );
        for _ in 0..2 {
            assert_eq!(
                axeon::interpreter::run(&project.check().unwrap(), 1000)
                    .result
                    .unwrap(),
                42
            );
        }
    }

    #[test]
    fn ordinary_projects_keep_the_preexisting_non_nominal_path() {
        let fixture = Fixture::new();
        fixture.write("main.ax", "fn main()->i64{return 42;}");
        let project = fixture.load().unwrap();
        assert!(project.nominal_session().is_none());
        assert!(project.source_program(project.entry).is_none());
        assert_eq!(
            axeon::interpreter::run(&project.check().unwrap(), 1000)
                .result
                .unwrap(),
            42
        );
    }

    #[test]
    fn nominal_exports_alone_enable_module_mode_and_preserve_bound_definition_scope() {
        let fixture = Fixture::new();
        fixture.write(
            "main.ax",
            "export record Cell<T>{value:T} fn main()->i64{return 0;}",
        );
        let project = fixture.load().unwrap();
        assert!(project.is_module_project());
        assert_eq!(project.modules[0].generic_nominal_exports.len(), 1);
        fixture.write(
            "contracts.ax",
            "export trait Read{fn read(value:Self)->i64;}",
        );
        fixture.write(
            "types.ax",
            "import contracts; export record Checked<T:contracts::Read>{value:T}",
        );
        fixture.write("main.ax", "import types; fn main()->i64{return 0;}");
        let project = fixture.load().unwrap();
        let types = project
            .modules
            .iter()
            .find(|module| project.sources.get(module.source).unwrap().logical_path == "types.ax")
            .unwrap();
        let source = project.source_program(types.source).unwrap();
        let bound = &source.generic_records[0].parameters[0].bounds[0];
        assert_eq!(bound.name, "contracts::Read");
        let definition = project.trait_definition(types.source, &bound.name).unwrap();
        assert_eq!(
            project
                .sources
                .get(definition.id.source)
                .unwrap()
                .logical_path,
            "contracts.ax"
        );
    }

    #[test]
    fn project_nominal_budget_counts_diamond_sources_once_and_separately() {
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
        let mut declarations = String::new();
        for index in 0..128 {
            write!(declarations, "export record Cell{index}<T>{{value:T}} ").unwrap();
        }
        fixture.write("shared.ax", &declarations);
        let project = fixture.load().unwrap();
        assert_eq!(project.modules.len(), 4);
        assert_eq!(
            project
                .modules
                .iter()
                .map(|module| module.generic_nominal_exports.len())
                .sum::<usize>(),
            128
        );
        fixture.write("left.ax", "import shared; record Extra<T>{value:T}");
        fixture.rejects("G002");
    }

    fn declarations(prefix: &str, count: usize, generic: bool) -> String {
        let mut source = String::new();
        let parameters = if generic { "<T>" } else { "" };
        let payload = if generic { "T" } else { "i64" };
        for index in 0..count {
            if index % 2 == 0 {
                write!(
                    source,
                    "record {prefix}{index}{parameters}{{value:{payload}}} "
                )
                .unwrap();
            } else {
                write!(
                    source,
                    "union {prefix}{index}{parameters}{{Present{{value:{payload}}},Empty{{}}}} "
                )
                .unwrap();
            }
        }
        source
    }

    #[test]
    fn mixed_module_nominal_totals_accept_256_and_reject_257_with_r001() {
        for generic_count in [1, 128] {
            let fixture = Fixture::new();
            fixture.write("types.ax", &declarations("Generic", generic_count, true));
            let ordinary = declarations("Plain", 256 - generic_count, false);
            fixture.write(
                "main.ax",
                &format!("import types; {ordinary} fn main()->i64{{return 0;}}"),
            );
            let project = fixture.load().unwrap();
            assert!(project.nominal_session().is_some());
            project.check().unwrap();
            fixture.write(
                "main.ax",
                &format!(
                    "import types; {ordinary} record Extra{{value:i64}} fn main()->i64{{return 0;}}"
                ),
            );
            let ProjectError::Diagnostics { diagnostics, .. } = fixture.load().unwrap_err() else {
                panic!("mixed project total must reject during loading")
            };
            assert!(diagnostics.iter().any(|error| error.code == "R001"));
            assert!(!diagnostics.iter().any(|error| error.code == "G002"));
        }
    }

    #[test]
    fn generic_nominal_limit_is_independent_of_total_module_declarations() {
        let fixture = Fixture::new();
        fixture.write("types.ax", &declarations("Generic", 128, true));
        fixture.write("main.ax", "import types; fn main()->i64{return 0;}");
        fixture.load().unwrap().check().unwrap();
        fixture.write(
            "main.ax",
            "import types; union Extra<T>{Some{value:T},None{}} fn main()->i64{return 0;}",
        );
        let ProjectError::Diagnostics { diagnostics, .. } = fixture.load().unwrap_err() else {
            panic!("generic count must reject during loading")
        };
        assert!(diagnostics.iter().any(|error| error.code == "G002"));
        assert!(!diagnostics.iter().any(|error| error.code == "R001"));
    }

    #[test]
    fn late_generic_module_checks_already_loaded_ordinary_overflow_before_preparation() {
        let fixture = Fixture::new();
        let ordinary = declarations("Plain", 129, false);
        fixture.write("alpha.ax", &ordinary);
        fixture.write(
            "main.ax",
            &format!("import alpha; {ordinary} fn main()->i64{{return 0;}}"),
        );
        // Keep the accepted ordinary-project boundary: loading succeeds and
        // the project checker reports the combined record/union count.
        let project = fixture.load().unwrap();
        assert!(project.nominal_session().is_none());
        let errors = project.check().unwrap_err();
        assert!(errors.iter().any(|error| error.code == "R001"));
        fixture.write(
            "main.ax",
            &format!("import zeta; import alpha; {ordinary} fn main()->i64{{return 0;}}"),
        );
        // Imports are discovered lexically: alpha overflows the ordinary
        // total before zeta introduces the first generic nominal. Its cycle
        // would fail during preparation, which must not be reached here.
        fixture.write("zeta.ax", "record Recursive<T>{value:Recursive<T>}");
        let ProjectError::Diagnostics { diagnostics, .. } = fixture.load().unwrap_err() else {
            panic!("late generic nominal must enforce the complete graph budget")
        };
        assert_eq!(diagnostics.len(), 1, "{diagnostics:?}");
        assert_eq!(diagnostics[0].code, "R001");
    }

    #[test]
    fn discovery_never_exposes_private_or_transitive_nominal_templates() {
        let fixture = Fixture::new();
        fixture.write(
            "types.ax",
            "record Hidden<T>{value:T} export record Cell<T>{value:T}",
        );
        fixture.write(
            "main.ax",
            "import types; fn f(x:types::Hidden<i64>)->i64{return 0;} fn main()->i64{return 0;}",
        );
        fixture.rejects("E_PARSE_TYPE");
        fixture.write("bridge.ax", "import types; export fn f()->i64{return 0;}");
        fixture.write(
            "main.ax",
            "import bridge; fn f(x:types::Cell<i64>)->i64{return 0;} fn main()->i64{return 0;}",
        );
        fixture.rejects("E_PARSE_TYPE");
        fixture.write("main.ax", "import types; fn main()->i64{return 0;}");
        fixture.write("types.ax", "export record Cell<T>{value:Missing<T>}");
        fixture.rejects("E_PARSE_TYPE");
    }
}
