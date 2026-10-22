//! Type checking and lexical name resolution.
//!
//! Parameters and the outermost function block share a scope. Nested blocks may
//! shadow bindings; an initializer resolves names before its new binding exists.
//! All syntactically reachable function paths must return. This is deliberately
//! conservative: every `while` can finish without entering its body, including
//! `while true`. Statements after an unconditional return, break, continue, or
//! terminating conditional/block are rejected rather than silently discarded.
//!
//! Stable diagnostics: T001 duplicate function; T002 missing main; T003 invalid
//! main signature; T004 duplicate binding; T005 unknown variable; T006 immutable
//! assignment; T007 unknown function; T008 wrong argument count; T009 type
//! mismatch; T010 missing return; T011 unreachable statement; T012 invalid
//! break; T013 invalid continue; T014 parameter budget; T015 local budget;
//! T016 syntax-node budget; T020 unit print rejection. Any diagnostic prevents IR emission.
//!
//! Arrays are affine local owners. Slice loans last until their declaration's
//! lexical scope ends and cover the entire owner even for disjoint ranges.
//! Branch joins include only branches that can continue. An enclosing loop's
//! owner cannot be moved in its body; iteration-local owners may be moved.
//! This conservative restriction makes each loop backedge preserve all owners
//! and loans that existed at loop entry, without needing a fixed-point solver.
//!
//! Ownership diagnostics: O001 invalid array length; O002 array-storage budget;
//! O003 aggregate function signature; O004 aggregate in scalar context; O005
//! unsupported aggregate initializer; O006 moved owner; O007 loan conflict;
//! O008 aggregate reassignment; O009 invalid index/len target; O010 missing
//! mutable capability; O011 move of an owner from outside the current loop.
//!
//! Numeric operators require matching i64 or f64 operands, with no implicit
//! conversions. Array and slice element types are part of their exact types.
//! The three explicit f64 intrinsics use T008/T009 for arity/type failures.

use std::collections::HashMap;

use crate::ast::{self, Type};
use crate::diagnostic::{Diagnostic, SourceId, Span};
use crate::ir;

#[path = "check/body.rs"]
mod body;
#[path = "check/generics.rs"]
mod generics;
#[path = "check/output.rs"]
mod output;
#[path = "check/symbolic.rs"]
mod symbolic;
pub use symbolic::check_templates;
#[path = "check/semantic.rs"]
mod semantic;
#[path = "check/source_ids.rs"]
mod source_ids;
use body::FunctionChecker;
use semantic::NEXT;

pub const MAX_PARAMS: usize = 32;
pub const MAX_LOCALS: usize = 256;
pub const MAX_FUNCTION_NODES: usize = 2048;
pub const MAX_ARRAY_LEN: usize = 32;
pub const MAX_ARRAY_ELEMENTS: usize = 256;

fn scalar(ty: Type) -> bool {
    matches!(ty, Type::I64 | Type::F64 | Type::Bool | Type::Unit)
}

fn check_array_length(ty: Type, span: Span, errors: &mut Vec<Diagnostic>) {
    if let Type::Array { length, .. } = ty {
        if !(1..=MAX_ARRAY_LEN).contains(&length) {
            errors.push(Diagnostic::new(
                "O001",
                span,
                format!("array length must be in 1..={MAX_ARRAY_LEN}, found {length}"),
            ));
        }
    }
}

// These limits bound native O0 frame sizes as well as frontend allocations.
// They are a subset contract, checked before creating any executable IR.
fn resource_limits(
    program: &ast::Program,
    layouts: &ast::InlineLayouts,
    nominal: Option<&crate::nominal::NominalSession>,
) -> Vec<Diagnostic> {
    body_resource_limits(program.functions.iter(), layouts, nominal, None)
}
fn body_resource_limits<'a, A: Copy + Into<output::Type> + 'a>(
    functions: impl IntoIterator<Item = &'a ast::Function<A>>,
    layouts: &ast::InlineLayouts,
    nominal: Option<&crate::nominal::NominalSession>,
    binder: Option<ast::RecordId>,
) -> Vec<Diagnostic> {
    enum Node<'a, A> {
        Block(&'a ast::Block<A>),
        Stmt(&'a ast::Stmt<A>),
        Expr(&'a ast::Expr),
    }
    let mut errors = Vec::new();
    for function in functions {
        if function.params.len() > MAX_PARAMS {
            errors.push(Diagnostic::new(
                "T014",
                function.name_span,
                format!("function exceeds {MAX_PARAMS} parameters"),
            ));
        }
        let mut nodes = 0;
        let mut locals = function.params.len();
        let mut array_elements = 0usize;
        if source_ids::check_annotation_in(
            function.return_type.into(),
            function.name_span,
            &mut errors,
            nominal,
            binder,
        ) {
            if let Some(ty) = function.return_type.into().concrete() {
                check_array_length(ty, function.name_span, &mut errors);
            }
        }
        for param in &function.params {
            if source_ids::check_annotation_in(
                param.ty.into(),
                param.name_span,
                &mut errors,
                nominal,
                binder,
            ) {
                if let Some(ty) = param.ty.into().concrete() {
                    check_array_length(ty, param.name_span, &mut errors);
                }
                array_elements = array_elements.saturating_add(semantic_storage_slots(
                    param.ty.into(),
                    param.name_span,
                    layouts,
                    nominal,
                    &mut errors,
                ));
            }
        }
        let mut stack = vec![Node::Block(&function.body)];
        while let Some(node) = stack.pop() {
            nodes += 1;
            if nodes > MAX_FUNCTION_NODES {
                errors.push(Diagnostic::new(
                    "T016",
                    function.name_span,
                    format!("function exceeds {MAX_FUNCTION_NODES} syntax nodes"),
                ));
                break;
            }
            match node {
                Node::Block(block) => stack.extend(block.stmts.iter().map(Node::Stmt)),
                Node::Stmt(stmt) => match &stmt.kind {
                    ast::StmtKind::Let {
                        value,
                        ty,
                        name_span,
                        ..
                    } => {
                        locals += 1;
                        if source_ids::check_annotation_in(
                            (*ty).into(),
                            *name_span,
                            &mut errors,
                            nominal,
                            binder,
                        ) {
                            if let Some(ty) = (*ty).into().concrete() {
                                check_array_length(ty, *name_span, &mut errors);
                            }
                            array_elements = array_elements.saturating_add(semantic_storage_slots(
                                (*ty).into(),
                                *name_span,
                                layouts,
                                nominal,
                                &mut errors,
                            ));
                        }
                        stack.push(Node::Expr(value));
                    }
                    ast::StmtKind::IndexAssign { index, value, .. } => {
                        stack.push(Node::Expr(index));
                        stack.push(Node::Expr(value));
                    }
                    ast::StmtKind::Assign { value, .. }
                    | ast::StmtKind::FieldAssign { value, .. }
                    | ast::StmtKind::Return(value)
                    | ast::StmtKind::Print(value)
                    | ast::StmtKind::Expr(value) => stack.push(Node::Expr(value)),
                    ast::StmtKind::If {
                        condition,
                        then_block,
                        else_block,
                    } => {
                        stack.push(Node::Expr(condition));
                        stack.push(Node::Block(then_block));
                        if let Some(block) = else_block {
                            stack.push(Node::Block(block));
                        }
                    }
                    ast::StmtKind::Match { arms, .. } => {
                        for arm in arms {
                            if let Some(ty) = arm.application {
                                source_ids::check_annotation_in(
                                    ty,
                                    arm.union_span,
                                    &mut errors,
                                    nominal,
                                    binder,
                                );
                            }
                            locals += arm.fields.len();
                            nodes += 1 + arm.fields.len();
                            stack.push(Node::Block(&arm.body));
                        }
                    }
                    ast::StmtKind::While { condition, body } => {
                        stack.push(Node::Expr(condition));
                        stack.push(Node::Block(body));
                    }
                    ast::StmtKind::Block(block) => stack.push(Node::Block(block)),
                    ast::StmtKind::TensorScope {
                        budget,
                        body,
                        handler,
                        ..
                    } => {
                        locals += 1;
                        stack.push(Node::Expr(budget));
                        stack.push(Node::Block(body));
                        stack.push(Node::Block(handler));
                    }
                    ast::StmtKind::Break | ast::StmtKind::Continue => (),
                },
                Node::Expr(expr) => match &expr.kind {
                    ast::ExprKind::Record {
                        fields,
                        application,
                        ..
                    }
                    | ast::ExprKind::Union {
                        fields,
                        application,
                        ..
                    } => {
                        if let Some(ty) = application {
                            source_ids::check_annotation_in(
                                *ty,
                                expr.span,
                                &mut errors,
                                nominal,
                                binder,
                            );
                        }
                        stack.extend(fields.iter().map(|field| Node::Expr(&field.value)));
                    }
                    ast::ExprKind::Unary { value, .. } | ast::ExprKind::Try(value) => {
                        stack.push(Node::Expr(value))
                    }
                    ast::ExprKind::Binary { left, right, .. } => {
                        stack.push(Node::Expr(left));
                        stack.push(Node::Expr(right));
                    }
                    ast::ExprKind::GenericCall {
                        type_args, args, ..
                    } => {
                        for ty in type_args {
                            source_ids::check_annotation_in(
                                *ty,
                                expr.span,
                                &mut errors,
                                nominal,
                                binder,
                            );
                        }
                        stack.extend(args.iter().map(Node::Expr));
                    }
                    ast::ExprKind::Call { args, .. }
                    | ast::ExprKind::Intrinsic { args, .. }
                    | ast::ExprKind::TensorCall { args, .. }
                    | ast::ExprKind::Array(args) => {
                        stack.extend(args.iter().map(Node::Expr));
                    }
                    ast::ExprKind::Borrow {
                        range: Some((start, end)),
                        ..
                    } => {
                        stack.push(Node::Expr(start));
                        stack.push(Node::Expr(end));
                    }
                    ast::ExprKind::Index { index, .. } => stack.push(Node::Expr(index)),
                    _ => (),
                },
            }
        }
        if locals > MAX_LOCALS {
            errors.push(Diagnostic::new(
                "T015",
                function.name_span,
                format!("function exceeds {MAX_LOCALS} local bindings including parameters"),
            ));
        }
        if array_elements > MAX_ARRAY_ELEMENTS {
            errors.push(Diagnostic::new(
                "O002", function.name_span,
                format!("function declares more than {MAX_ARRAY_ELEMENTS} array elements, record fields and union tag/payload slots, including move destinations"),
            ));
        }
    }
    errors
}

fn storage_slots(ty: Type, layouts: &ast::InlineLayouts) -> usize {
    if ty.inline_owned() {
        layouts.slots(ty)
    } else {
        0
    }
}

fn semantic_storage_slots(
    ty: ast::TypeExpr,
    span: Span,
    layouts: &ast::InlineLayouts,
    nominal: Option<&crate::nominal::NominalSession>,
    errors: &mut Vec<Diagnostic>,
) -> usize {
    // A bare opaque parameter retains the previous proof accounting. Opaque
    // leaves inside a known nominal shape contribute its positive lower bound.
    if !ty.inline_owned() {
        return 0;
    }
    if let Some(session) = nominal {
        match session.storage_slots(ty, span) {
            Ok(slots) => slots,
            Err(error) => {
                errors.push(error);
                0
            }
        }
    } else {
        ty.concrete().map_or(0, |ty| storage_slots(ty, layouts))
    }
}

#[derive(Clone)]
struct Signature<T = Type, F = ir::FunctionId> {
    id: F,
    params: Vec<(T, Span)>,
    return_type: T,
    span: Span,
}

impl Signature {
    fn semantic(self) -> Signature<output::Type, output::Callable> {
        Signature {
            id: output::Callable::Concrete(self.id),
            params: self
                .params
                .into_iter()
                .map(|(ty, span)| (ty.into(), span))
                .collect(),
            return_type: self.return_type.into(),
            span: self.span,
        }
    }
}

/// Check the complete program before exposing typed, resolved IR to a backend.
pub fn check(program: &ast::Program) -> Result<ir::Program, Vec<Diagnostic>> {
    if !program.imports.is_empty() {
        return Err(program
            .imports
            .iter()
            .map(|import| {
                Diagnostic::new(
                    "E_MODULE_CONTEXT",
                    import.span,
                    "imports require the project loader",
                )
            })
            .collect());
    }
    check_units(
        std::slice::from_ref(program),
        &[Vec::new()],
        SourceId(0),
        usize::MAX,
        None,
    )
}

/// Each program is one namespace. Imported exports are resolved to original
/// FunctionIds; user-facing names and source-local spans are never mangled.
pub(crate) fn check_modules(
    programs: &[ast::Program],
    imports: &[Vec<(String, SourceId)>],
    entry: SourceId,
) -> Result<ir::Program, Vec<Diagnostic>> {
    check_units(
        programs,
        imports,
        entry,
        crate::project::MAX_PROJECT_DIAGNOSTICS,
        None,
    )
}

/// Prepare source applications without replacing opaque parameters with
/// representative concrete values. Each invocation owns a fresh instance pool.
pub fn check_source(program: &ast::SourceProgram) -> Result<ir::Program, Vec<Diagnostic>> {
    if !program.imports.is_empty() {
        return Err(program
            .imports
            .iter()
            .map(|import| {
                Diagnostic::new(
                    "E_MODULE_CONTEXT",
                    import.span,
                    "imports require the project loader",
                )
            })
            .collect());
    }
    check_modules_source(std::slice::from_ref(program), &[Vec::new()], SourceId(0))
}

pub(crate) fn check_modules_source(
    programs: &[ast::SourceProgram],
    imports: &[Vec<(String, SourceId)>],
    entry: SourceId,
) -> Result<ir::Program, Vec<Diagnostic>> {
    if programs.iter().all(|p| {
        p.applications.is_empty() && p.generic_records.is_empty() && p.generic_unions.is_empty()
    }) {
        crate::nominal::preflight_sources(programs, crate::project::MAX_PROJECT_DIAGNOSTICS)?;
        let ordinary = programs
            .iter()
            .cloned()
            .map(ast::SourceProgram::resolve_ordinary_annotations)
            .collect::<Result<Vec<_>, _>>()?;
        return check_modules(&ordinary, imports, entry);
    }
    let prepared = crate::nominal::prepare_sources(
        programs,
        imports,
        crate::project::MAX_PROJECT_DIAGNOSTICS,
    )?;
    check_units(
        &prepared.programs,
        imports,
        entry,
        crate::project::MAX_PROJECT_DIAGNOSTICS,
        Some(&prepared.session),
    )
}

fn check_units(
    programs: &[ast::Program],
    imports: &[Vec<(String, SourceId)>],
    entry: SourceId,
    diagnostic_limit: usize,
    nominal: Option<&crate::nominal::NominalSession>,
) -> Result<ir::Program, Vec<Diagnostic>> {
    let identities = source_ids::validate(programs, diagnostic_limit, nominal);
    if !identities.is_empty() {
        return Err(identities);
    }
    let mut resource_errors = Vec::new();
    let records: Vec<_> = programs
        .iter()
        .flat_map(|program| program.records.iter().cloned())
        .collect();
    let unions: Vec<_> = programs
        .iter()
        .flat_map(|program| program.unions.iter().cloned())
        .collect();
    let (records, unions) =
        nominal.map_or((records, unions), |session| session.concrete_definitions());
    let record_count: usize = programs
        .iter()
        .map(|program| {
            program.records.len()
                + program.unions.len()
                + program.generic_records.len()
                + program.generic_unions.len()
        })
        .sum();
    if record_count > crate::parser::MAX_RECORDS {
        resource_errors.push(Diagnostic::new(
            "R001",
            Span::in_source(entry, 0, 0),
            format!(
                "project exceeds {} record and union declarations",
                crate::parser::MAX_RECORDS
            ),
        ));
    }
    for program in programs {
        let mut names = HashMap::new();
        for record in &program.records {
            if let Some(previous) = names.insert(&record.name, record.name_span) {
                resource_errors.push(
                    Diagnostic::new(
                        "R003",
                        record.name_span,
                        format!(
                            "record `{}` is already declared in this module",
                            record.name
                        ),
                    )
                    .with_related(previous, "previous record declaration"),
                );
            }
            if !(1..=crate::parser::MAX_RECORD_FIELDS).contains(&record.fields.len()) {
                resource_errors.push(Diagnostic::new(
                    "R002",
                    record.name_span,
                    format!(
                        "a record requires 1..={} fields",
                        crate::parser::MAX_RECORD_FIELDS
                    ),
                ));
            }
            let mut fields = HashMap::new();
            for field in &record.fields {
                if let Some(previous) = fields.insert(&field.name, field.name_span) {
                    resource_errors.push(
                        Diagnostic::new(
                            "R004",
                            field.name_span,
                            format!("field `{}` is already declared", field.name),
                        )
                        .with_related(previous, "previous field declaration"),
                    );
                }
                check_array_length(field.ty, field.name_span, &mut resource_errors);
                if !(scalar(field.ty) || field.ty.inline_owned()) {
                    resource_errors.push(Diagnostic::new("R005", field.name_span,
                        "record fields require scalars or owned inline arrays, records, or unions; borrowed and tensor fields are unsupported"));
                }
            }
        }
        for union in &program.unions {
            if let Some(previous) = names.insert(&union.name, union.name_span) {
                resource_errors.push(
                    Diagnostic::new(
                        "R003",
                        union.name_span,
                        format!(
                            "nominal type `{}` is already declared in this module",
                            union.name
                        ),
                    )
                    .with_related(previous, "previous nominal type declaration"),
                );
            }
            if !(1..=crate::parser::MAX_UNION_VARIANTS).contains(&union.variants.len()) {
                resource_errors.push(Diagnostic::new(
                    "U001",
                    union.name_span,
                    "a union requires 1..=32 variants",
                ));
            }
            let mut variants = HashMap::new();
            for variant in &union.variants {
                if let Some(previous) = variants.insert(&variant.name, variant.name_span) {
                    resource_errors.push(
                        Diagnostic::new(
                            "U003",
                            variant.name_span,
                            format!("variant `{}` is already declared", variant.name),
                        )
                        .with_related(previous, "previous variant declaration"),
                    );
                }
                if variant.fields.len() > crate::parser::MAX_RECORD_FIELDS {
                    resource_errors.push(Diagnostic::new(
                        "U002",
                        variant.name_span,
                        "a union variant supports at most 32 fields",
                    ));
                }
                let mut fields = HashMap::new();
                for field in &variant.fields {
                    if let Some(previous) = fields.insert(&field.name, field.name_span) {
                        resource_errors.push(
                            Diagnostic::new(
                                "U004",
                                field.name_span,
                                format!("variant field `{}` is already declared", field.name),
                            )
                            .with_related(previous, "previous field declaration"),
                        );
                    }
                    check_array_length(field.ty, field.name_span, &mut resource_errors);
                    if !(scalar(field.ty) || field.ty.inline_owned()) {
                        resource_errors.push(Diagnostic::new("U005", field.name_span, "union payload fields require scalars or owned inline arrays, records, or unions; borrowed and tensor fields are unsupported"));
                    }
                }
            }
        }
        if resource_errors.len() >= diagnostic_limit {
            break;
        }
    }
    resource_errors.truncate(diagnostic_limit);
    if !resource_errors.is_empty() {
        return Err(resource_errors);
    }
    let layouts = ast::InlineLayouts::build(&records, &unions).map_err(|error| {
        let mut diagnostic = Diagnostic::new(error.code, error.span, error.message);
        if let Some(span) = error.related {
            diagnostic = diagnostic.with_related(span, "enclosing inline layout");
        }
        vec![diagnostic]
    })?;
    for program in programs {
        resource_errors.extend(resource_limits(program, &layouts, nominal));
        for (exported, fields) in program
            .records
            .iter()
            .map(|r| (r.exported, r.fields.as_slice()))
            .chain(program.unions.iter().flat_map(|u| {
                u.variants
                    .iter()
                    .map(move |v| (u.exported, v.fields.as_slice()))
            }))
        {
            if !exported {
                continue;
            }
            for field in fields {
                let private = match field.ty {
                    Type::Record(id) => records
                        .iter()
                        .find(|r| r.id == id && !r.exported)
                        .map(|r| r.name_span),
                    Type::Union(id) => unions
                        .iter()
                        .find(|u| u.id == id && !u.exported)
                        .map(|u| u.name_span),
                    _ => None,
                };
                if let Some(span) = private {
                    resource_errors.push(
                        Diagnostic::new(
                            "I002",
                            field.name_span,
                            "exported aggregate fields cannot expose a private nominal type",
                        )
                        .with_related(
                            span,
                            "export this nested type to expose its fields and variants",
                        ),
                    );
                }
            }
        }
        if resource_errors.len() >= diagnostic_limit {
            break;
        }
    }
    resource_errors.truncate(diagnostic_limit);
    if !resource_errors.is_empty() {
        return Err(resource_errors);
    }
    let mut errors = Vec::new();
    let mut all_signatures = Vec::new();
    let mut next_id = 0;
    for program in programs {
        let mut signatures: HashMap<String, Signature> = HashMap::new();
        for function in &program.functions {
            let id = next_id;
            next_id += 1;
            if !(scalar(function.return_type) || function.return_type.inline_owned()) {
                errors.push(Diagnostic::new(
                "O003",
                function.name_span,
                "function results require a scalar or owned inline array, record, or union; borrowed and tensor results are unsupported",
            ));
            }
            for param in &function.params {
                if !(scalar(param.ty) || param.ty.inline_owned()) {
                    errors.push(Diagnostic::new(
                    "O003",
                    param.name_span,
                    "function parameters require a scalar or owned inline array, record, or union; borrowed and tensor parameters are unsupported",
                ));
                }
            }
            if function.exported {
                for (ty, span) in function
                    .params
                    .iter()
                    .map(|param| (param.ty, param.name_span))
                    .chain(std::iter::once((function.return_type, function.name_span)))
                {
                    let private = match ty {
                        Type::Record(id) => records
                            .iter()
                            .find(|record| record.id == id)
                            .filter(|record| !record.exported)
                            .map(|record| record.name_span),
                        Type::Union(id) => unions
                            .iter()
                            .find(|union| union.id == id)
                            .filter(|union| !union.exported)
                            .map(|union| union.name_span),
                        _ => None,
                    };
                    if let Some(declaration) = private {
                        errors.push(
                            Diagnostic::new(
                                "I002",
                                span,
                                "exported function signatures cannot expose a private nominal type",
                            )
                            .with_related(
                                declaration,
                                "export this type to expose its scalar fields and variants",
                            ),
                        );
                    }
                }
            }
            if let Some(previous) = signatures.get(&function.name) {
                errors.push(
                    Diagnostic::new(
                        "T001",
                        function.name_span,
                        format!("function `{}` is already declared", function.name),
                    )
                    .with_related(previous.span, "previous function declaration"),
                );
            } else {
                signatures.insert(
                    function.name.clone(),
                    Signature {
                        id,
                        params: function
                            .params
                            .iter()
                            .map(|p| (p.ty, p.name_span))
                            .collect(),
                        return_type: function.return_type,
                        span: function.name_span,
                    },
                );
            }
        }

        all_signatures.push(signatures);
        if errors.len() >= diagnostic_limit {
            errors.truncate(diagnostic_limit);
            return Err(errors);
        }
    }
    let trait_methods = trait_dispatch(
        programs,
        imports,
        &all_signatures,
        &mut errors,
        diagnostic_limit,
        nominal,
    );
    if errors.is_empty() {
        if let Some(session) = nominal {
            let implementations: Vec<_> = programs
                .iter()
                .enumerate()
                .flat_map(|(source, program)| {
                    program
                        .implementations
                        .iter()
                        .filter_map(move |implementation| {
                            resolve_trait(programs, imports, source, &implementation.trait_name)
                                .map(|contract| (contract.id, implementation.target))
                        })
                })
                .collect();
            if let Err(failures) = session.validate_contracts(&implementations, diagnostic_limit) {
                errors.extend(failures);
            }
        }
    }
    if nominal.is_some() && !errors.is_empty() {
        errors.truncate(diagnostic_limit);
        return Err(errors);
    }
    let main = match all_signatures[entry.0].get("main") {
        Some(signature) => {
            if !signature.params.is_empty() || signature.return_type != Type::I64 {
                errors.push(Diagnostic::new(
                    "T003",
                    signature.span,
                    "entry function must have the signature `main() -> i64`",
                ));
            }
            signature.id
        }
        None => {
            errors.push(Diagnostic::new(
                "T002",
                Span::in_source(entry, 0, 0),
                "program requires an entry function `main() -> i64`",
            ));
            0
        }
    };

    let mut scopes = Vec::with_capacity(programs.len());
    for (source, program) in programs.iter().enumerate() {
        let mut signatures = all_signatures[source].clone();
        let mut type_names: HashMap<_, _> = program
            .records
            .iter()
            .map(|record| (record.name.clone(), Type::Record(record.id)))
            .chain(
                program
                    .unions
                    .iter()
                    .map(|union| (union.name.clone(), Type::Union(union.id))),
            )
            .collect();
        for (name, imported) in &imports[source] {
            for record in &programs[imported.0].records {
                if record.exported {
                    type_names.insert(format!("{name}::{}", record.name), Type::Record(record.id));
                }
            }
            for union in &programs[imported.0].unions {
                if union.exported {
                    type_names.insert(format!("{name}::{}", union.name), Type::Union(union.id));
                }
            }
            for function in &programs[imported.0].functions {
                if function.exported {
                    if let Some(signature) = all_signatures[imported.0].get(&function.name) {
                        signatures.insert(format!("{name}::{}", function.name), signature.clone());
                    }
                }
            }
        }
        let signatures: HashMap<_, _> = signatures
            .into_iter()
            .map(|(name, signature)| (name, signature.semantic()))
            .collect();
        let methods: HashMap<_, _> = trait_methods[source]
            .iter()
            .map(|(name, candidates)| {
                (
                    name.clone(),
                    candidates
                        .iter()
                        .map(|(ty, signature)| ((*ty).into(), signature.clone().semantic()))
                        .collect(),
                )
            })
            .collect();
        scopes.push(generics::Scope {
            signatures,
            methods,
            type_names,
        });
    }
    let mut registry = generics::Registry::new(programs, imports, nominal);
    if !registry.templates.is_empty() {
        // No concrete specialization can serve as proof of an opaque contract.
        // Validate all templates, including unused ones, before queuing instances.
        if let Err(proof_errors) = symbolic::validate(
            &mut registry,
            &scopes,
            &records,
            &unions,
            &layouts,
            diagnostic_limit,
        ) {
            errors.extend(proof_errors);
        }
        if !errors.is_empty() {
            errors.truncate(diagnostic_limit);
            return Err(errors);
        }
    }
    let instances = std::cell::RefCell::new(generics::Instances::new(next_id));
    let types = BodyTypes {
        records: &records,
        unions: &unions,
        layouts: &layouts,
        nominal,
    };
    let mut functions = Vec::with_capacity(next_id);
    let mut function_spans = Vec::with_capacity(next_id);
    for (source, program) in programs.iter().enumerate() {
        for function in &program.functions {
            if errors.len() >= diagnostic_limit {
                break;
            }
            let context = generics::Context {
                registry: &registry,
                source,
                template: None,
                instances: Some(&instances),
                depth: 0,
            };
            functions.push(emit_function(
                function,
                &scopes[source],
                context,
                &types,
                &mut errors,
            ));
            function_spans.push(function.name_span);
        }
        if errors.len() >= diagnostic_limit {
            break;
        }
    }
    // Only valid ordinary bodies can cause emission of specialized bodies. Each
    // queue entry was already charged and interned by the common call checker.
    let mut index = 0;
    while errors.is_empty() {
        let Some(instance) = instances.borrow().pending.get(index).cloned() else {
            break;
        };
        let template = &registry.templates[instance.template];
        let function =
            match generics::instantiate(template.declaration, &instance.arguments, nominal) {
                Ok(function) => function,
                Err(error) => {
                    errors
                        .push(error.with_related(instance.span, "generic instance requested here"));
                    break;
                }
            };
        let preflight = body_resource_limits(std::iter::once(&function), &layouts, nominal, None);
        if !preflight.is_empty() {
            errors.extend(preflight);
            break;
        }
        let context = generics::Context {
            registry: &registry,
            source: template.source,
            template: None,
            instances: Some(&instances),
            depth: instance.depth,
        };
        let before = errors.len();
        functions.push(emit_function(
            &function,
            &scopes[template.source],
            context,
            &types,
            &mut errors,
        ));
        function_spans.push(function.name_span);
        let instance_errors: Vec<_> = errors
            .drain(before..)
            .map(|diagnostic| {
                diagnostic.with_related(instance.span, "generic instance requested here")
            })
            .collect();
        errors.extend(instance_errors);
        index += 1;
    }
    errors.truncate(diagnostic_limit);
    if errors.is_empty() {
        let (records, unions) =
            nominal.map_or((records, unions), |session| session.concrete_definitions());
        let checked = ir::Program {
            records,
            unions,
            functions,
            main,
        };
        // Validate the shared frontend boundary in both compiler profiles.
        // Verifier spans are untrusted IR provenance; render an invariant
        // failure at a location retained from the checked source AST instead.
        crate::verify::verify(&checked).map_err(|failures| {
            let fallback = function_spans[main];
            failures
                .into_iter()
                .map(|failure| {
                    let location = failure
                        .function
                        .and_then(|id| function_spans.get(id).copied())
                        .unwrap_or(fallback);
                    Diagnostic::new(
                        "E_IR_INVARIANT",
                        location,
                        format!(
                            "internal typed IR verification failed [{}]: {} (IR source {}, bytes {}..{})",
                            failure.code, failure.message, failure.span.source.0,
                            failure.span.start, failure.span.end
                        ),
                    )
                })
                .collect::<Vec<_>>()
        })?;
        Ok(checked)
    } else {
        Err(errors)
    }
}

struct BodyTypes<'a> {
    records: &'a [ast::Record],
    unions: &'a [ast::Union],
    layouts: &'a ast::InlineLayouts,
    nominal: Option<&'a crate::nominal::NominalSession>,
}
fn emit_function<A: Copy + Into<output::Type>>(
    function: &ast::Function<A>,
    scope: &generics::Scope,
    context: generics::Context<'_>,
    types: &BodyTypes<'_>,
    errors: &mut Vec<Diagnostic>,
) -> ir::Function {
    let return_type = function.return_type.into();
    let mut checker = FunctionChecker::<output::Concrete> {
        signatures: &scope.signatures,
        trait_methods: &scope.methods,
        records: types.records,
        unions: types.unions,
        type_names: &scope.type_names,
        errors,
        state: semantic::State::new(),
        return_type,
        output: std::marker::PhantomData,
        function_span: function.name_span,
        generics: context,
        nominal: types.nominal,
    };
    let params = function
        .params
        .iter()
        .map(|p| checker.declare(&p.name, p.name_span, p.ty, false))
        .collect();
    let (body, flow) = checker.block(&function.body, false);
    if checker
        .state
        .locals
        .iter()
        .map(|l| semantic_storage_slots(l.ty, l.span, types.layouts, types.nominal, checker.errors))
        .sum::<usize>()
        > MAX_ARRAY_ELEMENTS
    {
        checker.errors.push(Diagnostic::new("O002", function.name_span, format!("function declares more than {MAX_ARRAY_ELEMENTS} inline scalar/tag slots, including parameters, pattern owners and move destinations")));
    }
    if flow & NEXT != 0 {
        checker.errors.push(Diagnostic::new(
            "T010",
            function.name_span,
            format!(
                "function `{}` may finish without returning {}",
                function.name, return_type
            ),
        ));
    }
    ir::Function {
        name: function.name.clone(),
        params,
        return_type: return_type.concrete().expect("concrete result"),
        locals: checker
            .state
            .locals
            .into_iter()
            .map(output::Local::into_concrete)
            .collect(),
        body,
        span: function.span,
    }
}

/// Resolve only local or directly imported exported trait names. This same
/// frozen lookup is used by canonical metadata after successful checking.
pub(crate) fn resolve_trait<'a>(
    programs: &'a [ast::Program],
    imports: &[Vec<(String, SourceId)>],
    source: usize,
    name: &str,
) -> Option<&'a ast::Trait> {
    if let Some((module, member)) = name.split_once("::") {
        let (_, target) = imports
            .get(source)?
            .iter()
            .find(|(alias, _)| alias == module)?;
        programs
            .get(target.0)?
            .traits
            .iter()
            .find(|contract| contract.exported && contract.name == member)
    } else {
        programs
            .get(source)?
            .traits
            .iter()
            .find(|contract| contract.name == name)
    }
}

pub(crate) fn public_type(programs: &[ast::Program], ty: Type) -> bool {
    match ty {
        Type::Record(id) => id
            .declaration()
            .and_then(|id| programs.get(id.source.0))
            .is_some_and(|p| {
                p.records
                    .iter()
                    .any(|record| record.id == id && record.exported)
            }),
        Type::Union(id) => id
            .declaration()
            .and_then(|id| programs.get(id.source.0))
            .is_some_and(|p| {
                p.unions
                    .iter()
                    .any(|union| union.id == id && union.exported)
            }),
        _ => scalar(ty) || ty.inline_owned(),
    }
}

type TraitDispatch = HashMap<String, Vec<(Type, Signature)>>;
fn trait_dispatch(
    programs: &[ast::Program],
    imports: &[Vec<(String, SourceId)>],
    signatures: &[HashMap<String, Signature>],
    errors: &mut Vec<Diagnostic>,
    diagnostic_limit: usize,
    nominal: Option<&crate::nominal::NominalSession>,
) -> Vec<TraitDispatch> {
    let span = programs
        .iter()
        .flat_map(|p| &p.traits)
        .next()
        .map(|t| t.name_span)
        .unwrap_or_default();
    if programs.iter().map(|p| p.traits.len()).sum::<usize>() > crate::parser::MAX_TRAITS
        || programs
            .iter()
            .map(|p| p.implementations.len())
            .sum::<usize>()
            > crate::parser::MAX_IMPLEMENTATIONS
    {
        errors.push(Diagnostic::new(
            "TR001",
            span,
            "project exceeds 64 traits or 256 concrete implementations",
        ));
        return vec![HashMap::new(); programs.len()];
    }
    // Validate each declared contract, including unused traits and signatures.
    for program in programs {
        let mut names: HashMap<&str, Span> = program
            .records
            .iter()
            .map(|r| (r.name.as_str(), r.name_span))
            .chain(
                program
                    .unions
                    .iter()
                    .map(|u| (u.name.as_str(), u.name_span)),
            )
            .chain(program.imports.iter().map(|i| (i.name.as_str(), i.span)))
            .chain(
                program
                    .generic_records
                    .iter()
                    .map(|r| (r.record.name.as_str(), r.record.name_span)),
            )
            .chain(
                program
                    .generic_unions
                    .iter()
                    .map(|u| (u.union.name.as_str(), u.union.name_span)),
            )
            .collect();
        for contract in &program.traits {
            if errors.len() >= diagnostic_limit {
                return vec![HashMap::new(); programs.len()];
            }
            if let Some(previous) = names.insert(&contract.name, contract.name_span) {
                errors.push(
                    Diagnostic::new(
                        "TR002",
                        contract.name_span,
                        format!(
                            "trait name `{}` conflicts with a trait, nominal type or import",
                            contract.name
                        ),
                    )
                    .with_related(previous, "previous declaration"),
                );
            }
            if !(1..=crate::parser::MAX_TRAIT_METHODS).contains(&contract.methods.len()) {
                errors.push(Diagnostic::new(
                    "TR001",
                    contract.name_span,
                    "a trait supports 1..=32 methods",
                ));
            }
            let mut methods = HashMap::new();
            for method in &contract.methods {
                if errors.len() >= diagnostic_limit {
                    return vec![HashMap::new(); programs.len()];
                }
                if let Some(previous) = methods.insert(&method.name, method.name_span) {
                    errors.push(
                        Diagnostic::new(
                            "TR002",
                            method.name_span,
                            "trait method is already declared",
                        )
                        .with_related(previous, "previous method declaration"),
                    );
                }
                if method.params.first().map(|p| p.ty) != Some(ast::TraitType::SelfType) {
                    errors.push(Diagnostic::new(
                        "TR003",
                        method.name_span,
                        "a trait method requires Self as its first parameter type",
                    ));
                }
                if method.params.len() > MAX_PARAMS {
                    errors.push(Diagnostic::new(
                        "T014",
                        method.name_span,
                        "trait method exceeds 32 parameters",
                    ));
                }
                let mut params = HashMap::new();
                for param in &method.params {
                    if let Some(previous) = params.insert(&param.name, param.name_span) {
                        errors.push(
                            Diagnostic::new(
                                "T004",
                                param.name_span,
                                "trait parameter is already declared",
                            )
                            .with_related(previous, "previous parameter declaration"),
                        );
                    }
                }
                for (ty, span) in method
                    .params
                    .iter()
                    .map(|p| (p.ty, p.name_span))
                    .chain(std::iter::once((method.return_type, method.name_span)))
                {
                    if let ast::TraitType::Concrete(ty) = ty {
                        check_array_length(ty, span, errors);
                        if !(scalar(ty) || ty.inline_owned()) {
                            errors.push(Diagnostic::new("TR003", span, "trait signatures require Self, scalars or owned inline aggregate types"));
                        } else if contract.exported
                            && !nominal.map_or_else(
                                || public_type(programs, ty),
                                |session| session.public_type(ty.into()),
                            )
                        {
                            errors.push(Diagnostic::new(
                                "I002",
                                span,
                                "exported trait signatures cannot expose a private nominal type",
                            ));
                        }
                    }
                }
            }
        }
    }
    let mut pairs: Vec<(ast::RecordId, Type, Span)> = Vec::new();
    let mut implementations: Vec<(ast::RecordId, Type, String, Signature)> = Vec::new();
    for (source, program) in programs.iter().enumerate() {
        for implementation in &program.implementations {
            if errors.len() >= diagnostic_limit {
                return vec![HashMap::new(); programs.len()];
            }
            let Some(contract) =
                resolve_trait(programs, imports, source, &implementation.trait_name)
            else {
                errors.push(Diagnostic::new(
                    "TR004",
                    implementation.trait_span,
                    format!(
                        "unknown or inaccessible trait `{}`",
                        implementation.trait_name
                    ),
                ));
                continue;
            };
            let target = implementation.target;
            check_array_length(target, implementation.trait_span, errors);
            if !(scalar(target) || target.inline_owned()) {
                errors.push(Diagnostic::new(
                    "TR003",
                    implementation.trait_span,
                    "concrete implementations require a scalar or owned inline aggregate type",
                ));
            }
            let owns_type = match target {
                Type::Record(id) | Type::Union(id) => id
                    .declaration()
                    .or_else(|| {
                        nominal
                            .and_then(|session| session.origin(id))
                            .map(|origin| origin.declaration)
                    })
                    .is_some_and(|declaration| declaration.source.0 == source),
                _ => false,
            };
            if contract.id.source.0 != source && !owns_type {
                errors.push(Diagnostic::new("TR005", implementation.trait_span, "an implementation must be declared in the trait's module or the nominal target type's module"));
            }
            if let Some((_, _, previous)) = pairs
                .iter()
                .find(|(id, ty, _)| *id == contract.id && *ty == target)
            {
                errors.push(
                    Diagnostic::new(
                        "TR005",
                        implementation.trait_span,
                        "a concrete implementation already exists for this trait and type",
                    )
                    .with_related(*previous, "previous concrete implementation"),
                );
            } else {
                pairs.push((contract.id, target, implementation.trait_span));
            }
            let mut bound = HashMap::new();
            for binding in &implementation.bindings {
                if errors.len() >= diagnostic_limit {
                    return vec![HashMap::new(); programs.len()];
                }
                if let Some(previous) = bound.insert(&binding.method, binding.method_span) {
                    errors.push(
                        Diagnostic::new(
                            "TR006",
                            binding.method_span,
                            "implementation method is bound more than once",
                        )
                        .with_related(previous, "previous method binding"),
                    );
                }
                let Some(method) = contract.methods.iter().find(|m| m.name == binding.method)
                else {
                    errors.push(Diagnostic::new(
                        "TR006",
                        binding.method_span,
                        "implementation binds a method not declared by the trait",
                    ));
                    continue;
                };
                let Some(signature) = signatures[source].get(&binding.function) else {
                    errors.push(Diagnostic::new(
                        "TR006",
                        binding.function_span,
                        "a method binding requires a local named function",
                    ));
                    continue;
                };
                if signature.params.len() != method.params.len()
                    || signature
                        .params
                        .iter()
                        .zip(&method.params)
                        .any(|((ty, _), param)| *ty != param.ty.substitute(target))
                    || signature.return_type != method.return_type.substitute(target)
                {
                    errors.push(Diagnostic::new("TR006", binding.function_span, "bound function signature does not exactly match the trait method after substituting Self").with_related(method.name_span, "trait method declared here").with_related(signature.span, "bound function declared here"));
                } else {
                    implementations.push((
                        contract.id,
                        target,
                        method.name.clone(),
                        signature.clone(),
                    ));
                }
            }
            for method in &contract.methods {
                if !bound.contains_key(&method.name) {
                    errors.push(
                        Diagnostic::new(
                            "TR006",
                            implementation.trait_span,
                            format!("implementation is missing method `{}`", method.name),
                        )
                        .with_related(method.name_span, "required trait method"),
                    );
                }
            }
        }
    }
    programs
        .iter()
        .enumerate()
        .map(|(source, program)| {
            let mut result = HashMap::new();
            let visible = program.traits.iter().map(|t| (t.name.clone(), t)).chain(
                imports[source].iter().flat_map(|(alias, target)| {
                    programs[target.0]
                        .traits
                        .iter()
                        .filter(|t| t.exported)
                        .map(move |t| (format!("{alias}::{}", t.name), t))
                }),
            );
            for (name, contract) in visible {
                for method in &contract.methods {
                    let candidates = implementations
                        .iter()
                        .filter(|(id, _, member, _)| *id == contract.id && member == &method.name)
                        .map(|(_, ty, _, signature)| (*ty, signature.clone()))
                        .collect();
                    result.insert(format!("{name}::{}", method.name), candidates);
                }
            }
            result
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn compile(source: &str) -> Result<ir::Program, Vec<Diagnostic>> {
        check(&crate::parser::parse(source).expect("valid test syntax"))
    }

    fn errors(source: &str) -> Vec<Diagnostic> {
        compile(source).expect_err("test must reject the program")
    }

    fn has_code(source: &str, code: &str) {
        let errors = errors(source);
        assert!(
            errors.iter().any(|error| error.code == code),
            "missing {code}: {errors:?}"
        );
    }

    #[test]
    fn resolves_forward_calls_and_mutual_recursion() {
        let program = compile("fn main() -> i64 { return even(4); } fn even(n: i64) -> i64 { if n == 0 { return 1; } else { return odd(n - 1); } } fn odd(n: i64) -> i64 { if n == 0 { return 0; } else { return even(n - 1); } }").unwrap();
        let ir::StmtKind::Return(value) = &program.functions[0].body.stmts[0].kind else {
            panic!("return expected")
        };
        assert!(matches!(
            &value.kind,
            ir::ExprKind::Call { function: 1, .. }
        ));
    }

    #[test]
    fn nested_shadowing_has_distinct_ids_and_initializes_from_outer_binding() {
        let program = compile(
            "fn main() -> i64 { let x: i64 = 10; { let x: i64 = x + 1; print(x); } return x; }",
        )
        .unwrap();
        let function = &program.functions[0];
        assert_eq!(function.locals.len(), 2);
        let ir::StmtKind::Block(block) = &function.body.stmts[1].kind else {
            panic!("block expected")
        };
        let ir::StmtKind::Let { local: 1, value } = &block.stmts[0].kind else {
            panic!("inner declaration expected")
        };
        let ir::ExprKind::Binary { left, .. } = &value.kind else {
            panic!("binary expected")
        };
        assert!(matches!(left.kind, ir::ExprKind::Var(0)));
        let ir::StmtKind::Return(value) = &function.body.stmts[2].kind else {
            panic!("return expected")
        };
        assert!(matches!(value.kind, ir::ExprKind::Var(0)));
    }

    #[test]
    fn duplicate_declarations_point_to_original_declaration() {
        for source in [
            "fn main() -> i64 { let x: i64 = 1; let x: i64 = 2; return x; }",
            "fn f(x: i64, x: i64) -> i64 { return x; } fn main() -> i64 { return 0; }",
            "fn f(x: i64) -> i64 { let x: i64 = 0; return x; } fn main() -> i64 { return 0; }",
        ] {
            let errors = errors(source);
            let duplicate = errors.iter().find(|error| error.code == "T004").unwrap();
            assert_eq!(duplicate.related.len(), 1);
            assert!(duplicate.related[0].span.start < duplicate.span.start);
        }
        has_code(
            "fn main() -> i64 { return 0; } fn main() -> i64 { return 1; }",
            "T001",
        );
    }

    #[test]
    fn parameters_are_immutable_and_nested_shadow_can_be_mutable() {
        has_code(
            "fn f(x: i64) -> i64 { x = 2; return x; } fn main() -> i64 { return 0; }",
            "T006",
        );
        compile("fn f(x: i64) -> i64 { { let mut x: i64 = x; x = 2; print(x); } return x; } fn main() -> i64 { return f(1); }").unwrap();
    }

    #[test]
    fn binding_scope_ends_at_block_boundary() {
        has_code("fn main() -> i64 { { let x: i64 = 1; } return x; }", "T005");
        has_code("fn main() -> i64 { let x: i64 = x; return x; }", "T005");
        has_code("fn main() -> i64 { y = 3; return 0; }", "T005");
        compile("fn main() -> i64 { if true { let x: i64 = 1; } else { let x: i64 = 2; } let x: i64 = 3; return x; }").unwrap();
    }

    #[test]
    fn entry_point_must_exist_and_have_exact_signature() {
        has_code("fn f() -> i64 { return 0; }", "T002");
        has_code("fn main(a: i64) -> i64 { return a; }", "T003");
        has_code("fn main() -> bool { return true; }", "T003");
    }

    #[test]
    fn rejects_wrong_call_target_arity_and_types() {
        has_code("fn main() -> i64 { return missing(1); }", "T007");
        has_code(
            "fn f(n: i64) -> i64 { return n; } fn main() -> i64 { return f(); }",
            "T008",
        );
        has_code(
            "fn f(n: i64) -> i64 { return n; } fn main() -> i64 { return f(1, 2); }",
            "T008",
        );
        has_code(
            "fn f(n: i64) -> i64 { return n; } fn main() -> i64 { return f(true); }",
            "T009",
        );
    }

    #[test]
    fn exact_types_apply_to_statements_and_operators() {
        for source in [
            "fn main() -> i64 { let x: bool = 1; return 0; }",
            "fn main() -> i64 { let mut x: bool = false; x = 1; return 0; }",
            "fn main() -> i64 { if 1 { return 1; } return 0; }",
            "fn main() -> i64 { while 1 {} return 0; }",
            "fn main() -> i64 { return true; }",
            "fn main() -> i64 { print(true + 1); return 0; }",
            "fn main() -> i64 { print(1 && 2); return 0; }",
            "fn main() -> i64 { print(true < false); return 0; }",
            "fn main() -> i64 { print(true == 1); return 0; }",
            "fn main() -> i64 { print(!1); return 0; }",
            "fn main() -> i64 { print(~true); return 0; }",
            "fn main() -> i64 { print(-false); return 0; }",
            "fn main() -> i64 { print(true << 1); return 0; }",
            "fn main() -> i64 { print(false | true); return 0; }",
        ] {
            has_code(source, "T009");
        }
        compile("fn main() -> i64 { print(true == false); print(1 != 2); print(!false); print(~0); return 0; }").unwrap();
    }

    #[test]
    fn checks_all_operands_and_preserves_related_type_spans() {
        let errors =
            errors("fn main() -> i64 { let a: i64 = true; print(true + false); return 0; }");
        assert_eq!(
            errors.iter().filter(|error| error.code == "T009").count(),
            3
        );
        assert_eq!(errors[0].related.len(), 1);
    }

    #[test]
    fn returns_are_conservative_and_must_cover_all_paths() {
        for source in [
            "fn main() -> i64 {}",
            "fn main() -> i64 { if true { return 1; } }",
            "fn main() -> i64 { while true { return 1; } }",
            "fn main() -> i64 { while true { continue; } }",
        ] {
            has_code(source, "T010");
        }
        compile("fn main() -> i64 { if true { return 1; } else { { return 2; } } }").unwrap();
        compile("fn main() -> i64 { if true { return 1; } return 2; }").unwrap();
        compile("fn main() -> i64 { while true { if false { return 1; } break; } return 2; }")
            .unwrap();
    }

    #[test]
    fn unreachable_statements_are_rejected_after_all_terminal_forms() {
        for source in [
            "fn main() -> i64 { return 0; print(1); }",
            "fn main() -> i64 { { return 0; } print(1); }",
            "fn main() -> i64 { if true { return 0; } else { return 1; } print(1); }",
            "fn main() -> i64 { while true { break; print(1); } return 0; }",
            "fn main() -> i64 { while true { continue; print(1); } return 0; }",
        ] {
            has_code(source, "T011");
        }
    }

    #[test]
    fn break_and_continue_must_be_lexically_inside_loops() {
        has_code("fn main() -> i64 { break; }", "T012");
        has_code("fn main() -> i64 { { continue; } }", "T013");
        has_code(
            "fn main() -> i64 { while false { break; } continue; }",
            "T013",
        );
        compile("fn main() -> i64 { while false { { if true { break; } else { continue; } } } return 0; }").unwrap();
    }

    #[test]
    fn functions_and_bindings_use_distinct_namespaces() {
        compile("fn f() -> i64 { return 9; } fn main() -> i64 { let f: i64 = 1; return f + f(); }")
            .unwrap();
    }

    #[test]
    fn diagnostics_are_stable_and_failed_check_never_emits_ir() {
        let source = "fn main() -> i64 { let x: i64 = true; return missing + unknown(); }";
        assert_eq!(errors(source), errors(source));
        assert_eq!(
            errors(source)
                .iter()
                .map(|error| error.code)
                .collect::<Vec<_>>(),
            ["T009", "T005", "T007"]
        );
    }
}

#[cfg(test)]
mod resource_tests {
    use super::*;
    fn checked(source: &str) -> Result<ir::Program, Vec<Diagnostic>> {
        check(&crate::parser::parse(source).unwrap())
    }
    #[test]
    fn parameter_and_local_limits_accept_boundaries() {
        for count in [32, 33] {
            let params = (0..count)
                .map(|i| format!("p{i}:i64"))
                .collect::<Vec<_>>()
                .join(",");
            let source = format!("fn f({params})->i64{{return 0;}} fn main()->i64{{return 0;}}");
            let result = checked(&source);
            if count == 32 {
                assert!(result.is_ok());
            } else {
                assert_eq!(result.unwrap_err()[0].code, "T014");
            }
        }
        for count in [256, 257] {
            let declarations = (0..count)
                .map(|i| format!("let v{i}:i64=0;"))
                .collect::<Vec<_>>()
                .join("");
            let result = checked(&format!("fn main()->i64{{{declarations} return 0;}}"));
            if count == 256 {
                assert!(result.is_ok());
            } else {
                assert!(result.unwrap_err().iter().any(|d| d.code == "T015"));
            }
        }
    }
    #[test]
    fn function_node_budget_rejects_wide_bodies() {
        assert!(checked(&format!(
            "fn main()->i64{{{} return 0;}}",
            "print(0);".repeat(1022)
        ))
        .is_ok());
        assert!(checked(&format!(
            "fn main()->i64{{{} return 0;}}",
            "print(0);".repeat(1023)
        ))
        .unwrap_err()
        .iter()
        .any(|d| d.code == "T016"));
    }
}

#[cfg(test)]
mod ownership_tests {
    use super::*;

    fn checked(body: &str) -> Result<ir::Program, Vec<Diagnostic>> {
        let source = format!("fn main() -> i64 {{ {body} }}");
        let ast = crate::parser::parse(&source).expect("ownership test syntax is valid");
        check(&ast)
    }

    fn accepts(body: &str) -> ir::Program {
        checked(body).unwrap_or_else(|errors| panic!("unexpected rejection: {errors:#?}\n{body}"))
    }

    fn rejects(body: &str, code: &str) -> Vec<Diagnostic> {
        let errors = checked(body).expect_err("ownership violation must reject IR emission");
        assert!(
            errors.iter().any(|error| error.code == code),
            "missing {code}: {errors:#?}\n{body}"
        );
        errors
    }

    #[test]
    fn array_initialization_indexing_and_moves_are_resolved_to_distinct_locals() {
        let program = accepts("let a: [i64; 2] = [10, 20]; let mut b: [i64; 2] = a; b[0] = b[1] + 1; return len(b) + b[0];");
        let function = &program.functions[0];
        assert_eq!(
            function.locals[0].ty,
            Type::Array {
                element: ast::ElementType::I64,
                length: 2
            }
        );
        let ir::StmtKind::Let { local: 1, value } = &function.body.stmts[1].kind else {
            panic!("moved array initializer")
        };
        assert!(matches!(value.kind, ir::ExprKind::Move(0)));
        assert!(matches!(
            function.body.stmts[2].kind,
            ir::StmtKind::IndexAssign { base: 1, .. }
        ));
    }

    #[test]
    fn array_elements_lengths_indices_and_values_require_exact_types() {
        for body in [
            "let a: [i64; 2] = [1]; return 0;",
            "let a: [i64; 1] = [1, 2]; return 0;",
            "let a: [i64; 2] = [true, false]; return 0;",
            "let a: [i64; 1] = [1]; let b: [i64; 2] = a; return 0;",
            "let a: [i64; 1] = [1]; return a[false];",
            "let mut a: [i64; 1] = [1]; a[0] = false; return 0;",
            "let a: [i64; 1] = [1]; let s: &[i64] = &a[true..false]; return 0;",
            "let mut a: [i64; 1] = [1]; let s: &[i64] = &mut a; return 0;",
            "let mut a: [i64; 1] = [1]; let s: &mut [i64] = &a; return 0;",
        ] {
            rejects(body, "T009");
        }
        let errors = rejects("let a: [i64; 2] = [true, false]; return 0;", "T009");
        assert_eq!(
            errors.iter().filter(|error| error.code == "T009").count(),
            2
        );
        rejects("let x: i64 = 0; return x[0];", "O009");
        rejects("let x: bool = true; return len(x);", "O009");
    }

    #[test]
    fn aggregates_cannot_escape_into_scalar_expressions_or_calls() {
        for body in [
            "let a: [i64; 1] = [1]; print(a); return 0;",
            "let a: [i64; 1] = [1]; return a;",
            "let a: [i64; 1] = [1]; print(a == a); return 0;",
            "let a: [i64; 1] = [1]; let x: i64 = a; return x;",
            "let a: [i64; 1] = [1]; let s: &[i64] = &a; print(s); return 0;",
            "let a: [i64; 1] = [1]; print(&a); return 0;",
            "print([1, 2]); return 0;",
            "let a: [i64; 1] = [[1]]; return 0;",
        ] {
            rejects(body, "O004");
        }
        let source = "fn f(x: i64) -> i64 { return x; } fn main() -> i64 { let a: [i64; 1] = [1]; return f(a); }";
        let errors = check(&crate::parser::parse(source).unwrap()).unwrap_err();
        assert!(errors.iter().any(|error| error.code == "O004"));
    }

    #[test]
    fn borrowed_function_signatures_are_explicitly_unsupported() {
        for signature in ["x: &[i64]", "x: &mut [i64]"] {
            let source = format!(
                "fn f({signature}) -> i64 {{ return 0; }} fn main() -> i64 {{ return 0; }}"
            );
            let errors = check(&crate::parser::parse(&source).unwrap()).unwrap_err();
            assert!(errors.iter().any(|error| error.code == "O003"));
        }
        for ty in ["&[i64]", "&mut [i64]"] {
            let source = format!("fn f() -> {ty} {{ return 0; }} fn main() -> i64 {{ return 0; }}");
            let errors = check(&crate::parser::parse(&source).unwrap()).unwrap_err();
            assert!(errors.iter().any(|error| error.code == "O003"));
        }
    }

    #[test]
    fn moves_invalidate_every_owner_access_with_related_move_site() {
        for access in [
            "print(a[0]);",
            "print(len(a));",
            "a[0] = 3;",
            "let c: [i64; 1] = a;",
            "let s: &[i64] = &a;",
            "let s: &mut [i64] = &mut a;",
        ] {
            let errors = rejects(
                &format!("let mut a: [i64; 1] = [1]; let b: [i64; 1] = a; {access} return 0;"),
                "O006",
            );
            let moved = errors.iter().find(|error| error.code == "O006").unwrap();
            assert_eq!(moved.related.len(), 2);
            assert!(moved.related[0].span.start < moved.span.start);
        }
        rejects(
            "let a: [i64; 1] = [1]; { let a: [i64; 1] = a; print(a[0]); } return a[0];",
            "O006",
        );
    }

    #[test]
    fn shared_borrows_coexist_with_reads_and_expire_before_move() {
        accepts("let a: [i64; 2] = [1, 2]; { let s: &[i64] = &a; let t: &[i64] = &a[0..1]; print(a[0] + len(a) + s[1] + t[0]); } let b: [i64; 2] = a; return b[0];");
        accepts("let mut a: [i64; 1] = [1]; { let s: &[i64] = &a; print(s[0]); } a[0] = 2; return a[0];");
    }

    #[test]
    fn shared_loans_block_owner_writes_moves_and_exclusive_borrows() {
        for access in [
            "a[0] = 3;",
            "let b: [i64; 1] = a;",
            "let t: &mut [i64] = &mut a;",
        ] {
            rejects(
                &format!("let mut a: [i64; 1] = [1]; let s: &[i64] = &a; {access} return 0;"),
                "O007",
            );
        }
    }

    #[test]
    fn exclusive_loans_block_every_direct_owner_access_and_new_loan() {
        for access in [
            "print(a[0]);",
            "print(len(a));",
            "a[0] = 3;",
            "let b: [i64; 2] = a;",
            "let t: &[i64] = &a;",
            "let t: &mut [i64] = &mut a[1..2];",
        ] {
            rejects(&format!("let mut a: [i64; 2] = [1, 2]; let s: &mut [i64] = &mut a[0..1]; {access} return 0;"), "O007");
        }
        accepts("let mut a: [i64; 2] = [0, 2]; { let s: &mut [i64] = &mut a; s[s[0]] = s[1]; print(len(s)); } return a[0];");
    }

    #[test]
    fn mutable_slice_capability_is_independent_of_binding_mutability() {
        accepts("let mut a: [i64; 1] = [0]; { let s: &mut [i64] = &mut a; s[0] = s[0] + 1; } return a[0];");
        rejects("let a: [i64; 1] = [1]; a[0] = 2; return 0;", "O010");
        rejects(
            "let a: [i64; 1] = [1]; let s: &mut [i64] = &mut a; return 0;",
            "O010",
        );
        rejects(
            "let mut a: [i64; 1] = [1]; let mut s: &[i64] = &a; s[0] = 2; return 0;",
            "O010",
        );
    }

    #[test]
    fn aggregate_reassignment_copying_and_reborrowing_are_unsupported() {
        for body in [
            "let mut a: [i64; 1] = [1]; a = [2]; return 0;",
            "let a: [i64; 1] = [1]; let mut s: &[i64] = &a; s = &a; return 0;",
        ] {
            rejects(body, "O008");
        }
        for body in [
            "let x: i64 = 1; let a: [i64; 1] = x; return 0;",
            "let a: [i64; 1] = [1]; let s: &[i64] = &a; let t: &[i64] = s; return 0;",
            "let a: [i64; 1] = [1]; let s: &[i64] = &a; let t: &[i64] = &s; return 0;",
            "let mut a: [i64; 1] = [1]; let s: &mut [i64] = &mut a; let t: &mut [i64] = &mut s; return 0;",
        ] {
            rejects(body, "O005");
        }
    }

    #[test]
    fn borrow_bounds_read_owner_before_new_loan_and_full_range_is_materialized() {
        accepts("let mut a: [i64; 2] = [0, 1]; { let s: &mut [i64] = &mut a[a[0]..len(a)]; s[0] = 2; } return a[0];");
        let program = accepts("let a: [i64; 2] = [1, 2]; let s: &[i64] = &a; return len(s);");
        let ir::StmtKind::Let { value, .. } = &program.functions[0].body.stmts[1].kind else {
            panic!("slice binding")
        };
        let ir::ExprKind::Borrow {
            owner,
            mutable,
            start,
            end,
        } = &value.kind
        else {
            panic!("borrow initializer")
        };
        assert_eq!(*owner, 0);
        assert!(!mutable);
        assert!(matches!(start.kind, ir::ExprKind::Int(0)));
        assert!(matches!(end.kind, ir::ExprKind::Int(2)));
        // Static ownership permits dynamic-invalid bounds; runtimes trap them.
        accepts("let a: [i64; 1] = [1]; let s: &[i64] = &a[-1..2]; return s[3];");
    }

    #[test]
    fn lexical_scope_cleanup_tracks_loan_identities_across_shadowing() {
        rejects("let mut a: [i64; 1] = [1]; let s: &[i64] = &a; { let s: &[i64] = &a; print(s[0]); } a[0] = 2; return 0;", "O007");
        rejects("let mut a: [i64; 1] = [1]; let s: &[i64] = &a; { let mut a: [i64; 1] = [3]; a[0] = 4; } a[0] = 2; return 0;", "O007");
        accepts(
            "let mut a: [i64; 1] = [1]; { let a: &mut [i64] = &mut a; a[0] = 2; } return a[0];",
        );
        accepts("let mut a: [i64; 1] = [1]; if true { let s: &mut [i64] = &mut a; s[0] = 2; } else { let s: &[i64] = &a; print(s[0]); } a[0] = 3; return a[0];");
    }

    #[test]
    fn branch_joins_intersect_only_fallthrough_owner_availability() {
        rejects(
            "let a: [i64; 1] = [1]; if true { let b: [i64; 1] = a; print(b[0]); } return a[0];",
            "O006",
        );
        rejects(
            "let a: [i64; 1] = [1]; if false {} else { let b: [i64; 1] = a; } return len(a);",
            "O006",
        );
        accepts(
            "let a: [i64; 1] = [1]; if true { let b: [i64; 1] = a; return b[0]; } return a[0];",
        );
        accepts("let a: [i64; 1] = [1]; if true {} else { let b: [i64; 1] = a; return b[0]; } return a[0];");
        accepts("let a: [i64; 1] = [1]; if true { let b: [i64; 1] = a; return b[0]; } else { let b: [i64; 1] = a; return b[0]; }");
    }

    #[test]
    fn loops_reject_moves_of_enclosing_owners_even_on_terminating_paths() {
        for body in [
            "let a: [i64; 1] = [1]; while false { let b: [i64; 1] = a; } return 0;",
            "let a: [i64; 1] = [1]; while true { let b: [i64; 1] = a; break; } return 0;",
            "let a: [i64; 1] = [1]; while true { let b: [i64; 1] = a; return b[0]; } return 0;",
            "while false { let a: [i64; 1] = [1]; while true { let b: [i64; 1] = a; break; } } return 0;",
        ] {
            rejects(body, "O011");
        }
        accepts("let mut i: i64 = 0; while i < 3 { let a: [i64; 1] = [i]; let b: [i64; 1] = a; print(b[0]); i = i + 1; continue; } return i;");
        accepts("while false { let a: [i64; 1] = [1]; if true { let b: [i64; 1] = a; break; } print(a[0]); break; } return 0;");
        accepts("while false { let a: [i64; 1] = [1]; if true { let b: [i64; 1] = a; continue; } print(a[0]); break; } return 0;");
    }

    #[test]
    fn loop_body_loans_expire_before_condition_rechecks_and_later_owner_use() {
        accepts("let mut a: [i64; 1] = [0]; while a[0] < len(a) { let s: &mut [i64] = &mut a; s[0] = s[0] + 1; continue; } let b: [i64; 1] = a; return b[0];");
        accepts("let mut a: [i64; 1] = [0]; while true { let s: &mut [i64] = &mut a; s[0] = 1; break; } a[0] = 2; return a[0];");
        rejects("let mut a: [i64; 1] = [0]; let s: &[i64] = &a; while false { let t: &[i64] = &a; print(t[0]); } a[0] = 1; return 0;", "O007");
    }

    #[test]
    fn array_length_and_total_storage_budgets_include_move_destinations() {
        rejects("let a: [i64; 0] = []; return 0;", "O001");
        rejects("let a: [i64; 33] = [1]; return 0;", "O001");
        let values = vec!["0"; MAX_ARRAY_LEN].join(",");
        for count in [8, 9] {
            let mut body = format!("let a0: [i64; 32] = [{values}];");
            for index in 1..count {
                body.push_str(&format!("let a{index}: [i64; 32] = a{};", index - 1));
            }
            body.push_str(&format!("return a{}[0];", count - 1));
            if count == 8 {
                accepts(&body);
            } else {
                rejects(&body, "O002");
            }
        }
    }

    #[test]
    fn aggregate_children_count_toward_the_syntax_node_budget() {
        let elements = vec!["0"; MAX_FUNCTION_NODES].join(",");
        rejects(
            &format!("let a: [i64; 1] = [{elements}]; return 0;"),
            "T016",
        );
        for statement in ["print(a[0]);", "a[0] = 1;"] {
            accepts(&format!(
                "let mut a: [i64; 1] = [0]; {} return 0;",
                statement.repeat(680)
            ));
            rejects(
                &format!(
                    "let mut a: [i64; 1] = [0]; {} return 0;",
                    statement.repeat(681)
                ),
                "T016",
            );
        }
        let bound = vec!["0"; 30].join("+");
        let slices = (0..18)
            .map(|index| format!("let s{index}: &[i64] = &a[{bound}..{bound}];"))
            .collect::<Vec<_>>()
            .join("");
        rejects(
            &format!("let a: [i64; 1] = [0]; {slices} return 0;"),
            "T016",
        );
    }
}

#[cfg(test)]
mod float_tests {
    use super::*;

    fn checked(source: &str) -> Result<ir::Program, Vec<Diagnostic>> {
        check(&crate::parser::parse(source).expect("valid f64 test syntax"))
    }

    fn body(body: &str) -> Result<ir::Program, Vec<Diagnostic>> {
        checked(&format!("fn main() -> i64 {{ {body} }}"))
    }

    fn rejects(body_source: &str, code: &str) {
        let errors =
            body(body_source).expect_err("invalid floating-point program must fail checking");
        assert!(
            errors.iter().any(|error| error.code == code),
            "missing {code}: {errors:#?}\n{body_source}"
        );
    }

    #[test]
    fn f64_function_parameters_results_and_explicit_conversions_are_typed() {
        let program = checked("fn add(x: f64, y: f64) -> f64 { return x + y; } fn main() -> i64 { let x: f64 = add(f64_from_i64(1), 2.0); print(x); return f64_bits(x); }").unwrap();
        let function = &program.functions[0];
        assert_eq!(function.return_type, Type::F64);
        assert_eq!(function.locals[0].ty, Type::F64);
        assert_eq!(function.locals[1].ty, Type::F64);
        let ir::StmtKind::Return(value) = &function.body.stmts[0].kind else {
            panic!("return expected")
        };
        assert_eq!(value.ty, Type::F64);
        let ir::StmtKind::Return(value) = &program.functions[1].body.stmts[2].kind else {
            panic!("return expected")
        };
        assert_eq!(value.ty, Type::I64);
        assert!(matches!(
            value.kind,
            ir::ExprKind::Intrinsic {
                op: ast::FloatIntrinsic::ToBits,
                ..
            }
        ));
    }

    #[test]
    fn float_literals_preserve_the_frontends_rounded_binary64_bits() {
        for (literal, expected_bits) in [
            ("0.0", 0),
            ("1.0", 0x3ff0_0000_0000_0000),
            ("5e-324", 1),
            ("1e-9999", 0),
            ("1.7976931348623157e308", 0x7fef_ffff_ffff_ffff),
        ] {
            let program = body(&format!("let x: f64 = {literal}; return f64_bits(x);")).unwrap();
            let ir::StmtKind::Let { value, .. } = &program.functions[0].body.stmts[0].kind else {
                panic!("literal declaration expected")
            };
            assert_eq!(value.ty, Type::F64);
            assert!(matches!(value.kind, ir::ExprKind::Float(bits) if bits == expected_bits));
        }
    }

    #[test]
    fn all_supported_f64_arithmetic_comparisons_and_negation_typecheck() {
        for operation in ["+", "-", "*", "/"] {
            body(&format!(
                "let x: f64 = 1.0 {operation} 2.0; return f64_bits(-x);"
            ))
            .unwrap();
        }
        for operation in ["==", "!=", "<", "<=", ">", ">="] {
            body(&format!(
                "let b: bool = -1.0 {operation} 2.0; print(b); return 0;"
            ))
            .unwrap();
        }
        // IEEE non-finite results remain runtime values, never integer traps.
        body("let inf: f64 = 1.0 / 0.0; let nan: f64 = 0.0 / 0.0; print(inf); print(nan != nan); return f64_bits(nan);").unwrap();
    }

    #[test]
    fn numeric_operators_never_convert_between_i64_and_f64() {
        for operation in ["+", "-", "*", "/", "==", "!=", "<", "<=", ">", ">="] {
            rejects(&format!("print(1.0 {operation} 1); return 0;"), "T009");
            rejects(&format!("print(1 {operation} 1.0); return 0;"), "T009");
        }
    }

    #[test]
    fn float_remainder_bitwise_shifts_and_boolean_operators_are_rejected() {
        for operation in ["%", "&", "|", "^", "<<", ">>"] {
            rejects(&format!("print(1.0 {operation} 2.0); return 0;"), "T009");
            rejects(&format!("print(1 {operation} 2.0); return 0;"), "T009");
        }
        for expression in [
            "~1.0",
            "!1.0",
            "1.0 && true",
            "false || 1.0",
            "true + 1.0",
            "false < 1.0",
        ] {
            rejects(&format!("print({expression}); return 0;"), "T009");
        }
    }

    #[test]
    fn statements_and_function_interfaces_require_exact_scalar_types() {
        for body_source in [
            "let x: f64 = 1; return 0;",
            "let x: i64 = 1.0; return 0;",
            "let mut x: f64 = 0.0; x = 1; return 0;",
            "if 1.0 { return 1; } return 0;",
            "while 1.0 {} return 0;",
            "return 1.0;",
        ] {
            rejects(body_source, "T009");
        }
        for source in [
            "fn f(x: f64) -> f64 { return x; } fn main() -> i64 { print(f(1)); return 0; }",
            "fn f() -> f64 { return 1; } fn main() -> i64 { return 0; }",
        ] {
            assert!(checked(source)
                .unwrap_err()
                .iter()
                .any(|error| error.code == "T009"));
        }
        let errors = checked("fn main() -> f64 { return 0.0; }").unwrap_err();
        assert!(errors.iter().any(|error| error.code == "T003"));
    }

    #[test]
    fn float_intrinsics_have_exact_arity_and_input_types() {
        for builtin in ["f64_from_i64", "f64_bits", "f64_from_bits"] {
            rejects(&format!("print({builtin}()); return 0;"), "T008");
            rejects(&format!("print({builtin}(1, 2)); return 0;"), "T008");
            rejects(&format!("print({builtin}(true)); return 0;"), "T009");
        }
        rejects("print(f64_from_i64(1.0)); return 0;", "T009");
        rejects("print(f64_from_bits(1.0)); return 0;", "T009");
        rejects("print(f64_bits(1)); return 0;", "T009");
        body("let x: f64 = f64_from_bits(-9223372036854775808); let y: f64 = f64_from_i64(9223372036854775807); print(x); print(y); return f64_bits(x);").unwrap();
        let errors = body("print(f64_from_i64(unknown, also_unknown)); return 0;").unwrap_err();
        assert_eq!(
            errors.iter().filter(|error| error.code == "T005").count(),
            2
        );
        assert!(errors.iter().any(|error| error.code == "T008"));
    }

    #[test]
    fn f64_arrays_slices_and_index_assignments_keep_their_element_type() {
        let program = body("let mut a: [f64; 2] = [1.0, f64_from_i64(2)]; { let s: &mut [f64] = &mut a[0..len(a)]; s[0] = s[1] / 2.0; print(s[0]); } let b: [f64; 2] = a; return f64_bits(b[0]);").unwrap();
        let function = &program.functions[0];
        assert_eq!(
            function.locals[0].ty,
            Type::Array {
                element: ast::ElementType::F64,
                length: 2
            }
        );
        assert_eq!(
            function.locals[1].ty,
            Type::Slice {
                element: ast::ElementType::F64,
                mutable: true
            }
        );
        let ir::StmtKind::Block(block) = &function.body.stmts[1].kind else {
            panic!("borrow scope expected")
        };
        let ir::StmtKind::IndexAssign { value, .. } = &block.stmts[1].kind else {
            panic!("slice element assignment expected")
        };
        assert_eq!(value.ty, Type::F64);
        body("let a: [f64; 1] = [0.0]; let s: &[f64] = &a; let x: f64 = s[0]; return len(s);")
            .unwrap();
    }

    #[test]
    fn array_and_slice_element_types_do_not_coerce() {
        for body_source in [
            "let a: [f64; 2] = [1, 2.0]; return 0;",
            "let a: [i64; 2] = [1.0, 2]; return 0;",
            "let a: [i64; 1] = [1]; let b: [f64; 1] = a; return 0;",
            "let a: [f64; 1] = [1.0]; let b: [i64; 1] = a; return 0;",
            "let a: [i64; 1] = [1]; let s: &[f64] = &a; return 0;",
            "let a: [f64; 1] = [1.0]; let s: &[i64] = &a; return 0;",
            "let mut a: [f64; 1] = [1.0]; a[0] = 2; return 0;",
            "let mut a: [i64; 1] = [1]; a[0] = 2.0; return 0;",
            "let mut a: [f64; 1] = [1.0]; let s: &mut [f64] = &mut a; s[0] = 2; return 0;",
        ] {
            rejects(body_source, "T009");
        }
    }

    #[test]
    fn array_indices_and_slice_bounds_remain_i64_even_for_f64_elements() {
        for body_source in [
            "let a: [f64; 1] = [1.0]; print(a[0.0]); return 0;",
            "let mut a: [f64; 1] = [1.0]; a[0.0] = 2.0; return 0;",
            "let a: [f64; 1] = [1.0]; let s: &[f64] = &a[0.0..1]; return 0;",
            "let a: [f64; 1] = [1.0]; let s: &[f64] = &a[0..1.0]; return 0;",
            "let a: [f64; 1] = [1.0]; let x: f64 = len(a); return 0;",
        ] {
            rejects(body_source, "T009");
        }
    }

    #[test]
    fn f64_owners_obey_the_existing_move_loan_and_loop_rules() {
        for (body_source, code) in [
            (
                "let a: [f64; 1] = [1.0]; let b: [f64; 1] = a; print(a[0]); return 0;",
                "O006",
            ),
            (
                "let mut a: [f64; 1] = [1.0]; let s: &[f64] = &a; a[0] = 2.0; return 0;",
                "O007",
            ),
            (
                "let mut a: [f64; 1] = [1.0]; let s: &mut [f64] = &mut a; print(a[0]); return 0;",
                "O007",
            ),
            (
                "let a: [f64; 1] = [1.0]; let s: &mut [f64] = &mut a; return 0;",
                "O010",
            ),
            (
                "let a: [f64; 1] = [1.0]; let s: &[f64] = &a; s[0] = 2.0; return 0;",
                "O010",
            ),
            (
                "let a: [f64; 1] = [1.0]; let s: &[f64] = &a; let t: &[f64] = s; return 0;",
                "O005",
            ),
            ("let mut a: [f64; 1] = [1.0]; a = [2.0]; return 0;", "O008"),
            (
                "let a: [f64; 1] = [1.0]; while false { let b: [f64; 1] = a; } return 0;",
                "O011",
            ),
        ] {
            rejects(body_source, code);
        }
        body("let a: [f64; 1] = [1.0]; if true { let b: [f64; 1] = a; return f64_bits(b[0]); } return f64_bits(a[0]);").unwrap();
        body(
            "while false { let a: [f64; 1] = [1.0]; let b: [f64; 1] = a; print(b[0]); } return 0;",
        )
        .unwrap();
    }

    #[test]
    fn f64_borrowed_interfaces_and_scalar_escape_remain_rejected() {
        for source in [
            "fn f(x: &mut [f64]) -> i64 { return 0; } fn main() -> i64 { return 0; }",
            "fn f(x: &[f64]) -> i64 { return 0; } fn main() -> i64 { return 0; }",
            "fn f() -> &[f64] { return 0; } fn main() -> i64 { return 0; }",
        ] {
            assert!(checked(source)
                .unwrap_err()
                .iter()
                .any(|error| error.code == "O003"));
        }
        rejects("let a: [f64; 1] = [1.0]; print(a); return 0;", "O004");
        rejects(
            "let a: [f64; 1] = [1.0]; print(f64_bits(a)); return 0;",
            "O004",
        );
    }

    #[test]
    fn i64_and_f64_arrays_share_the_total_element_storage_budget() {
        let integer_values = vec!["0"; 32].join(",");
        let float_values = vec!["0.0"; 32].join(",");
        let declarations = (0..8)
            .map(|index| {
                let (ty, values) = if index % 2 == 0 {
                    ("i64", &integer_values)
                } else {
                    ("f64", &float_values)
                };
                format!("let a{index}: [{ty}; 32] = [{values}];")
            })
            .collect::<Vec<_>>()
            .join("");
        body(&format!("{declarations} return 0;")).unwrap();
        rejects(
            &format!("{declarations} let extra: [f64; 1] = [0.0]; return 0;"),
            "O002",
        );
        rejects("let a: [f64; 0] = []; return 0;", "O001");
        rejects("let a: [f64; 33] = [0.0]; return 0;", "O001");
    }

    #[test]
    fn intrinsic_arguments_count_toward_the_function_node_budget() {
        body(&format!(
            "{} return 0;",
            "print(f64_from_i64(0));".repeat(681)
        ))
        .unwrap();
        rejects(
            &format!("{} return 0;", "print(f64_from_i64(0));".repeat(682)),
            "T016",
        );
    }
}
