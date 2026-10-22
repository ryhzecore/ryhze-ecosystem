//! Source syntax cannot supply an ID reserved for a generated concrete layout.
//! Declaration edges are checked before building layouts. Function annotations
//! are checked by the existing iterative resource walk before each layout lookup;
//! symbolic annotation validation uses the same check before proof.
use crate::ast::{self, NominalId, Type, TypeExpr};
use crate::diagnostic::{Diagnostic, Span};

pub(super) fn validate(
    programs: &[ast::Program],
    limit: usize,
    nominal: Option<&crate::nominal::NominalSession>,
) -> Vec<Diagnostic> {
    let mut errors = Vec::new();
    let mut reject = |error| {
        if errors.len() < limit {
            errors.push(error);
        }
    };
    for program in programs {
        if nominal.is_none()
            && (!program.applications.is_empty()
                || !program.generic_records.is_empty()
                || !program.generic_unions.is_empty())
        {
            reject(Diagnostic::new(
                "G007",
                Span::default(),
                "generic nominal syntax requires the source preparation entry point",
            ));
        }
        for record in &program.records {
            annotation(
                Type::Record(record.id).into(),
                record.name_span,
                nominal,
                None,
                &mut reject,
            );
            for field in &record.fields {
                annotation(field.ty.into(), field.name_span, nominal, None, &mut reject);
            }
        }
        for union in &program.unions {
            annotation(
                Type::Union(union.id).into(),
                union.name_span,
                nominal,
                None,
                &mut reject,
            );
            for variant in &union.variants {
                for field in &variant.fields {
                    annotation(field.ty.into(), field.name_span, nominal, None, &mut reject);
                }
            }
        }
        for contract in &program.traits {
            for method in &contract.methods {
                for param in &method.params {
                    if let ast::TraitType::Concrete(ty) = param.ty {
                        annotation(ty.into(), param.name_span, nominal, None, &mut reject);
                    }
                }
                if let ast::TraitType::Concrete(ty) = method.return_type {
                    annotation(ty.into(), method.name_span, nominal, None, &mut reject);
                }
            }
        }
        for implementation in &program.implementations {
            annotation(
                implementation.target.into(),
                implementation.trait_span,
                nominal,
                None,
                &mut reject,
            );
        }
    }
    errors
}

fn identity(id: NominalId, span: Span, reject: &mut impl FnMut(Diagnostic)) {
    if matches!(id, NominalId::Instance(_)) {
        reject(Diagnostic::new(
            "G007",
            span,
            "a generated nominal layout identity cannot appear in source AST before preparation",
        ));
    }
}

fn annotation(
    ty: TypeExpr,
    span: Span,
    nominal: Option<&crate::nominal::NominalSession>,
    binder: Option<ast::RecordId>,
    reject: &mut impl FnMut(Diagnostic),
) {
    if matches!(
        ty,
        TypeExpr::Application(_)
            | TypeExpr::Concrete(
                Type::Record(NominalId::Instance(_)) | Type::Union(NominalId::Instance(_))
            )
    ) {
        if let Some(session) = nominal {
            if let Err(error) = session.validate_type(ty, binder, span) {
                reject(error);
            }
            return;
        }
        if matches!(ty, TypeExpr::Application(_)) {
            reject(Diagnostic::new(
                "G007",
                span,
                "an application identity requires source preparation",
            ));
            return;
        }
    }
    if let TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) = ty {
        identity(id, span, reject);
    }
}

/// Return false before a generated reference can reach source layout lookup.
pub(super) fn check_annotation_in(
    ty: TypeExpr,
    span: Span,
    errors: &mut Vec<Diagnostic>,
    nominal: Option<&crate::nominal::NominalSession>,
    binder: Option<ast::RecordId>,
) -> bool {
    let mut valid = true;
    annotation(ty, span, nominal, binder, &mut |error| {
        valid = false;
        errors.push(error);
    });
    valid
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rejected(program: &ast::Program) {
        let errors = crate::check::check(program).expect_err("source cannot forge a generated ID");
        assert!(!errors.is_empty());
        assert!(
            errors.iter().all(|error| error.code == "G007"),
            "{errors:#?}"
        );
        assert_eq!(crate::check::check(program).unwrap_err(), errors);
    }

    #[test]
    fn source_declarations_and_fields_cannot_supply_generated_layout_ids() {
        let original =
            crate::parser::parse("record R{x:i64}union U{Value{x:i64}}fn main()->i64{return 0;}")
                .unwrap();
        let mut program = original.clone();
        program.records[0].id = NominalId::Instance(0);
        rejected(&program);
        let mut program = original.clone();
        program.unions[0].id = NominalId::Instance(0);
        rejected(&program);
        let mut program = original.clone();
        program.records[0].fields[0].ty = Type::Union(NominalId::Instance(0));
        rejected(&program);
        let mut program = original;
        program.unions[0].variants[0].fields[0].ty = Type::Record(NominalId::Instance(0));
        rejected(&program);
    }

    #[test]
    fn undeclared_generated_local_is_rejected_before_layout_lookup() {
        let mut program = crate::parser::parse("fn main()->i64{{let x:i64=1;}return 0;}").unwrap();
        let ast::StmtKind::Block(block) = &mut program.functions[0].body.stmts[0].kind else {
            panic!("block")
        };
        let ast::StmtKind::Let { ty, .. } = &mut block.stmts[0].kind else {
            panic!("local")
        };
        *ty = Type::Record(NominalId::Instance(u32::MAX));
        rejected(&program);
    }

    #[test]
    fn unused_template_and_nested_explicit_type_arguments_are_checked() {
        let original = crate::parser::parse("fn identity<T>(x:T)->T{return x;}fn unused<T>(x:T)->T{return identity::<T>(x);}fn main()->i64{return identity::<i64>(1);}").unwrap();
        let mut program = original.clone();
        program.generic_functions[1].function.params[0].ty =
            Type::Record(NominalId::Instance(0)).into();
        rejected(&program);
        let mut program = original;
        let ast::StmtKind::Return(value) =
            &mut program.generic_functions[1].function.body.stmts[0].kind
        else {
            panic!("return")
        };
        let ast::ExprKind::GenericCall { type_args, .. } = &mut value.kind else {
            panic!("generic call")
        };
        type_args[0] = Type::Union(NominalId::Instance(0)).into();
        rejected(&program);
    }

    #[test]
    fn signatures_trait_methods_and_implementation_targets_are_checked() {
        let original = crate::parser::parse("trait Echo{fn echo(x:Self)->Self;}impl Echo for i64{echo=echo;}fn echo(x:i64)->i64{return x;}fn main()->i64{return 0;}").unwrap();
        let generated = Type::Record(NominalId::Instance(0));
        let mut program = original.clone();
        program.functions[0].params[0].ty = generated;
        rejected(&program);
        let mut program = original.clone();
        program.functions[0].return_type = generated;
        rejected(&program);
        let mut program = original.clone();
        program.traits[0].methods[0].params[0].ty = ast::TraitType::Concrete(generated);
        rejected(&program);
        let mut program = original.clone();
        program.traits[0].methods[0].return_type = ast::TraitType::Concrete(generated);
        rejected(&program);
        let mut program = original;
        program.implementations[0].target = generated;
        rejected(&program);
    }

    #[test]
    fn ast_api_syntax_is_bounded_before_recursive_semantic_visitors() {
        let mut program = crate::parser::parse("fn main()->i64{return 0;}").unwrap();
        let stmt = program.functions[0].body.stmts[0].clone();
        program.functions[0].body.stmts = vec![stmt; super::super::MAX_FUNCTION_NODES + 1];
        let errors = crate::check::check(&program).unwrap_err();
        assert_eq!(errors.len(), 1);
        assert_eq!(errors[0].code, "T016");

        let mut program = crate::parser::parse("fn main()->i64{return 0;}").unwrap();
        let mut body = std::mem::replace(
            &mut program.functions[0].body,
            ast::Block {
                stmts: vec![],
                span: Span::default(),
            },
        );
        for _ in 0..super::super::MAX_FUNCTION_NODES {
            body = ast::Block {
                stmts: vec![ast::Stmt {
                    kind: ast::StmtKind::Block(body),
                    span: Span::default(),
                }],
                span: Span::default(),
            };
        }
        program.functions[0].body = body;
        let errors = crate::check::check(&program).unwrap_err();
        assert_eq!(errors.len(), 1);
        assert_eq!(errors[0].code, "T016");
        // Iterative destruction keeps this deliberately unparseable test AST
        // from exercising recursive Rust drop instead of our source guard.
        let mut body = program.functions.pop().unwrap().body;
        while let Some(stmt) = body.stmts.pop() {
            if let ast::StmtKind::Block(nested) = stmt.kind {
                body = nested;
            }
        }
    }
}
