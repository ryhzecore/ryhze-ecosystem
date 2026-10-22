//! Every generic declaration is proved with opaque identities before any instance.
use super::{
    body::FunctionChecker,
    generics::{self, Registry, Scope},
    output, semantic, Signature,
};
use crate::ast::{self, Type, TypeExpr};
use crate::diagnostic::{Diagnostic, Span};
use std::{collections::HashMap, marker::PhantomData};

/// Prove supplied templates using the same integrated project checking path.
/// Opaque parameters are never replaced by representative concrete types.
pub fn check_templates(
    program: &ast::Program,
    templates: &[ast::GenericFunction],
) -> Result<(), Vec<Diagnostic>> {
    let mut program = program.clone();
    program.generic_functions.extend_from_slice(templates);
    super::check(&program).map(|_| ())
}

pub(super) fn validate(
    registry: &mut Registry<'_>,
    scopes: &[Scope],
    records: &[ast::Record],
    unions: &[ast::Union],
    layouts: &ast::InlineLayouts,
    diagnostic_limit: usize,
) -> Result<(), Vec<Diagnostic>> {
    let mut errors = Vec::new();
    if registry.templates.len() > 128 {
        return Err(vec![Diagnostic::new(
            "G002",
            registry.templates[128].declaration.function.name_span,
            "project exceeds 128 generic function declarations",
        )]);
    }
    // Validate every signature/bound first so recursion and mutual recursion
    // consult finite proven declarations rather than expand proof bodies.
    let mut declared: Vec<HashMap<String, Span>> = registry
        .programs
        .iter()
        .map(|p| {
            p.functions
                .iter()
                .map(|f| (f.name.clone(), f.name_span))
                .collect()
        })
        .collect();
    for index in 0..registry.templates.len() {
        if errors.len() >= diagnostic_limit {
            break;
        }
        let template = registry.templates[index].declaration;
        let source = registry.templates[index].source;
        let program = &registry.programs[source];
        let f = &template.function;
        let binder = generics::binder(template);
        if let Some(previous) = declared[source].insert(f.name.clone(), f.name_span) {
            errors.push(
                Diagnostic::new(
                    "T001",
                    f.name_span,
                    format!("function `{}` is already declared", f.name),
                )
                .with_related(previous, "previous function declaration"),
            );
        }
        if f.name == "main" || f.name.starts_with("test_") {
            errors.push(Diagnostic::new(
                "G005",
                f.name_span,
                "generic main and test_ entry functions require an ordinary concrete wrapper",
            ));
        }
        if !(1..=8).contains(&template.parameters.len()) {
            errors.push(Diagnostic::new(
                "G002",
                f.name_span,
                "a generic function requires 1..=8 type parameters",
            ));
        }
        let mut names = HashMap::new();
        let mut resolved_bounds = Vec::new();
        for parameter in &template.parameters {
            if let Some(previous) = names.insert(&parameter.name, parameter.name_span) {
                errors.push(
                    Diagnostic::new("G001", parameter.name_span, "duplicate type parameter name")
                        .with_related(previous, "previous type parameter"),
                );
            }
            if matches!(
                parameter.name.as_str(),
                "i64" | "f64" | "bool" | "Self" | "tensor" | "tensor_error"
            ) || scopes[source].type_names.contains_key(&parameter.name)
                || program
                    .generic_records
                    .iter()
                    .any(|r| r.record.name == parameter.name)
                || program
                    .generic_unions
                    .iter()
                    .any(|u| u.union.name == parameter.name)
                || program.traits.iter().any(|t| t.name == parameter.name)
                || program.imports.iter().any(|i| i.name == parameter.name)
            {
                errors.push(Diagnostic::new(
                    "G001",
                    parameter.name_span,
                    "type parameter shadows a reserved or declared type, trait, or import name",
                ));
            }
            if parameter.bounds.len() > 8 {
                errors.push(Diagnostic::new(
                    "G002",
                    parameter.name_span,
                    "a type parameter supports at most 8 explicit trait bounds",
                ));
            }
            let mut bounds = Vec::new();
            for bound in &parameter.bounds {
                match super::resolve_trait(registry.programs, registry.imports, source, &bound.name)
                {
                    Some(contract) => {
                        if bounds.contains(&contract.id) {
                            errors.push(Diagnostic::new(
                                "G004",
                                bound.span,
                                "duplicate trait bound",
                            ));
                        }
                        bounds.push(contract.id);
                        if f.exported && !contract.exported {
                            errors.push(Diagnostic::new(
                                "G004",
                                bound.span,
                                "exported generic function bounds must name public traits",
                            ));
                        }
                    }
                    None => errors.push(Diagnostic::new(
                        "TR004",
                        bound.span,
                        format!("unknown or inaccessible trait `{}`", bound.name),
                    )),
                }
            }
            resolved_bounds.push(bounds);
        }
        registry.templates[index].bounds = resolved_bounds;
        let annotations = match annotations(f) {
            Ok(a) => a,
            Err(e) => {
                errors.push(e);
                continue;
            }
        };
        let valid_annotation = |ty| match ty {
            TypeExpr::Application(_) => registry
                .nominal
                .is_some_and(|session| session.owns_type(ty, Some(binder))),
            TypeExpr::Parameter(id) => id.binder == binder && id.index < template.parameters.len(),
            TypeExpr::Concrete(Type::Record(id)) => records.iter().any(|r| r.id == id),
            TypeExpr::Concrete(Type::Union(id)) => unions.iter().any(|u| u.id == id),
            TypeExpr::Concrete(_) => true,
        };
        // Validate identities before any layout lookup, including AST API input.
        for (ty, span) in annotations {
            if !super::source_ids::check_annotation_in(
                ty,
                span,
                &mut errors,
                registry.nominal,
                Some(binder),
            ) {
                continue;
            }
            if !valid_annotation(ty) {
                errors.push(Diagnostic::new(
                    "G001",
                    span,
                    "annotation uses an undeclared parameter identity or nominal type",
                ));
            }
        }
        if !errors.is_empty() {
            continue;
        }
        errors.extend(super::body_resource_limits(
            std::iter::once(f),
            layouts,
            registry.nominal,
            Some(binder),
        ));
        if !errors.is_empty() {
            continue;
        }
        let nodes = generics::expressions(&f.body, &mut |expression| {
            let type_args: &[TypeExpr] = match &expression.kind {
                ast::ExprKind::GenericCall { type_args, .. } => type_args,
                ast::ExprKind::Record {
                    application: Some(ty),
                    ..
                }
                | ast::ExprKind::Union {
                    application: Some(ty),
                    ..
                } => std::slice::from_ref(ty),
                _ => &[],
            };
            for &ty in type_args {
                if !valid_annotation(ty) {
                    errors.push(Diagnostic::new(
                        "G001",
                        expression.span,
                        "annotation uses an undeclared parameter identity or nominal type",
                    ));
                }
            }
        });
        registry.templates[index].nodes = nodes + f.params.len();
        for (ty, span) in f
            .params
            .iter()
            .map(|p| (p.ty, p.name_span))
            .chain(std::iter::once((f.return_type, f.name_span)))
        {
            if let TypeExpr::Concrete(ty) = ty {
                if !(super::scalar(ty) || ty.inline_owned()) {
                    errors.push(Diagnostic::new("O003", span, "generic function interfaces require an opaque parameter, scalar, or owned inline value"));
                }
                if f.exported
                    && !registry.nominal.map_or_else(
                        || super::public_type(registry.programs, ty),
                        |session| session.public_type(ty.into()),
                    )
                {
                    errors.push(Diagnostic::new(
                        "I002",
                        span,
                        "exported generic function signatures cannot expose a private nominal type",
                    ));
                }
            }
            if matches!(ty, TypeExpr::Application(_))
                && f.exported
                && !registry
                    .nominal
                    .is_some_and(|session| session.public_type(ty))
            {
                errors.push(Diagnostic::new("I002", span, "exported generic function signatures cannot expose private nominal applications"));
            }
        }
        if errors.len() >= diagnostic_limit {
            break;
        }
    }
    if !errors.is_empty() {
        errors.truncate(diagnostic_limit);
        return Err(errors);
    }
    for (index, template) in registry.templates.iter().enumerate() {
        let f = &template.declaration.function;
        let scope = &scopes[template.source];
        let mut methods = scope.methods.clone();
        for (parameter_index, parameter) in template.declaration.parameters.iter().enumerate() {
            let id = ast::TypeParameterId {
                binder: generics::binder(template.declaration),
                index: parameter_index,
            };
            let receiver = TypeExpr::Parameter(id);
            for bound in &parameter.bounds {
                let contract = super::resolve_trait(
                    registry.programs,
                    registry.imports,
                    template.source,
                    &bound.name,
                )
                .expect("validated bound");
                for (method, declaration) in contract.methods.iter().enumerate() {
                    let substitute = |ty| match ty {
                        ast::TraitType::Concrete(ty) => TypeExpr::Concrete(ty),
                        ast::TraitType::SelfType => receiver,
                    };
                    // Bounds are nominal identities, not a particular import spelling.
                    let spellings: Vec<_> = methods
                        .keys()
                        .filter(|name| {
                            name.rsplit_once("::").is_some_and(|(prefix, member)| {
                                member == declaration.name
                                    && super::resolve_trait(
                                        registry.programs,
                                        registry.imports,
                                        template.source,
                                        prefix,
                                    )
                                    .is_some_and(|visible| visible.id == contract.id)
                            })
                        })
                        .cloned()
                        .collect();
                    for spelling in spellings {
                        methods.entry(spelling).or_default().push((
                            receiver,
                            Signature {
                                id: output::Callable::Bound {
                                    contract: contract.id,
                                    parameter: id,
                                    method,
                                },
                                params: declaration
                                    .params
                                    .iter()
                                    .map(|p| (substitute(p.ty), p.name_span))
                                    .collect(),
                                return_type: substitute(declaration.return_type),
                                span: declaration.name_span,
                            },
                        ));
                    }
                }
            }
        }
        let mut checker = FunctionChecker::<output::Proof> {
            nominal: registry.nominal,
            signatures: &scope.signatures,
            trait_methods: &methods,
            records,
            unions,
            type_names: &scope.type_names,
            errors: &mut errors,
            state: semantic::State::new(),
            return_type: f.return_type,
            function_span: f.name_span,
            output: PhantomData,
            generics: generics::Context {
                registry,
                source: template.source,
                template: Some(index),
                instances: None,
                depth: 0,
            },
        };
        for p in &f.params {
            checker.declare(&p.name, p.name_span, p.ty, false);
        }
        let (_, flow) = checker.block(&f.body, false);
        if checker
            .state
            .locals
            .iter()
            .map(|local| {
                super::semantic_storage_slots(
                    local.ty,
                    local.span,
                    layouts,
                    registry.nominal,
                    checker.errors,
                )
            })
            .sum::<usize>()
            > super::MAX_ARRAY_ELEMENTS
        {
            checker.errors.push(Diagnostic::new("O002", f.name_span, format!("function declares more than {} inline scalar/tag slots, including parameters, pattern owners and move destinations", super::MAX_ARRAY_ELEMENTS)));
        }
        if flow & semantic::NEXT != 0 {
            checker.errors.push(Diagnostic::new(
                "T010",
                f.name_span,
                format!(
                    "function `{}` may finish without returning {}",
                    f.name, f.return_type
                ),
            ));
        }
        if errors.len() >= diagnostic_limit {
            break;
        }
    }
    errors.truncate(diagnostic_limit);
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors)
    }
}

fn annotations(f: &ast::Function<TypeExpr>) -> Result<Vec<(TypeExpr, Span)>, Diagnostic> {
    let mut result: Vec<_> = f.params.iter().map(|p| (p.ty, p.name_span)).collect();
    result.push((f.return_type, f.name_span));
    let mut blocks = vec![(&f.body, 1usize)];
    let mut nodes = 0usize;
    while let Some((block, depth)) = blocks.pop() {
        if depth > crate::parser::MAX_NESTING
            || nodes.saturating_add(block.stmts.len()) > super::MAX_FUNCTION_NODES
        {
            return Err(Diagnostic::new(
                "T016",
                f.name_span,
                "symbolic body exceeds syntax depth or node limits",
            ));
        }
        nodes += block.stmts.len();
        for stmt in &block.stmts {
            match &stmt.kind {
                ast::StmtKind::Let { ty, name_span, .. } => result.push((*ty, *name_span)),
                ast::StmtKind::Block(body) | ast::StmtKind::While { body, .. } => {
                    blocks.push((body, depth + 1))
                }
                ast::StmtKind::If {
                    then_block,
                    else_block,
                    ..
                } => {
                    blocks.push((then_block, depth + 1));
                    if let Some(block) = else_block {
                        blocks.push((block, depth + 1));
                    }
                }
                ast::StmtKind::TensorScope { body, handler, .. } => {
                    blocks.push((body, depth + 1));
                    blocks.push((handler, depth + 1));
                }
                ast::StmtKind::Match { arms, .. } => {
                    result.extend(
                        arms.iter()
                            .filter_map(|arm| arm.application.map(|ty| (ty, arm.union_span))),
                    );
                    blocks.extend(arms.iter().map(|a| (&a.body, depth + 1)))
                }
                _ => (),
            }
        }
    }
    Ok(result)
}
