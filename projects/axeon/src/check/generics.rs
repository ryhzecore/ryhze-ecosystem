//! Finite template registry and deterministic, definition-scoped instances.
//! Opaque proof and concrete emission both resolve calls through this registry.
use std::{cell::RefCell, collections::HashMap};

use super::{body, output, Signature};
use crate::ast::{self, Type, TypeExpr};
use crate::diagnostic::{Diagnostic, SourceId, Span};

pub(super) const MAX_INSTANCES: usize = 256;
pub(super) const MAX_EXPANSION_DEPTH: usize = 32;
pub(super) const MAX_EXPANDED_NODES: usize = 131_072;
pub(super) const MAX_EMITTED_FUNCTIONS: usize = 1024;

pub(super) struct Scope {
    pub signatures: HashMap<String, body::ResolvedSignature>,
    pub methods: body::TraitMethods,
    pub type_names: HashMap<String, Type>,
}

pub(super) struct Template<'a> {
    pub declaration: &'a ast::GenericFunction,
    pub source: usize,
    pub bounds: Vec<Vec<ast::RecordId>>,
    pub nodes: usize,
}
pub(super) struct Registry<'a> {
    pub programs: &'a [ast::Program],
    pub imports: &'a [Vec<(String, SourceId)>],
    pub templates: Vec<Template<'a>>,
    pub names: Vec<HashMap<String, usize>>,
    implementations: Vec<(ast::RecordId, Type)>,
    pub nominal: Option<&'a crate::nominal::NominalSession>,
}
impl<'a> Registry<'a> {
    pub fn new(
        programs: &'a [ast::Program],
        imports: &'a [Vec<(String, SourceId)>],
        nominal: Option<&'a crate::nominal::NominalSession>,
    ) -> Self {
        let mut templates = Vec::new();
        let mut names = vec![HashMap::new(); programs.len()];
        let mut implementations = Vec::new();
        for (source, program) in programs.iter().enumerate() {
            for declaration in &program.generic_functions {
                names[source]
                    .entry(declaration.function.name.clone())
                    .or_insert(templates.len());
                templates.push(Template {
                    declaration,
                    source,
                    bounds: Vec::new(),
                    nodes: 0,
                });
            }
            // The ordinary trait pass validates coherence and every binding first.
            for implementation in &program.implementations {
                if let Some(contract) =
                    super::resolve_trait(programs, imports, source, &implementation.trait_name)
                {
                    implementations.push((contract.id, implementation.target));
                }
            }
        }
        for (source, imports) in imports.iter().enumerate() {
            for (alias, target) in imports {
                for (index, template) in templates.iter().enumerate() {
                    let f = &template.declaration.function;
                    if template.source == target.0 && f.exported {
                        names[source].insert(format!("{alias}::{}", f.name), index);
                    }
                }
            }
        }
        Self {
            programs,
            imports,
            templates,
            names,
            implementations,
            nominal,
        }
    }
    pub fn valid_type(&self, ty: TypeExpr, template: Option<usize>) -> bool {
        if let Some(session) = self.nominal {
            return session.owns_type(ty, template.map(|i| binder(self.templates[i].declaration)));
        }
        match ty {
            TypeExpr::Application(_) => false,
            TypeExpr::Parameter(id) => template.is_some_and(|index| {
                let t = self.templates[index].declaration;
                id.binder == binder(t) && id.index < t.parameters.len()
            }),
            TypeExpr::Concrete(Type::Record(id)) => self
                .programs
                .iter()
                .any(|p| p.records.iter().any(|r| r.id == id)),
            TypeExpr::Concrete(Type::Union(id)) => self
                .programs
                .iter()
                .any(|p| p.unions.iter().any(|u| u.id == id)),
            TypeExpr::Concrete(ty) => super::scalar(ty) || ty.inline_owned(),
        }
    }
    fn entails(&self, ty: TypeExpr, contract: ast::RecordId, caller: Option<usize>) -> bool {
        if let Some(session) = self.nominal {
            return session.entails(
                ty,
                contract,
                caller.map(|i| binder(self.templates[i].declaration)),
            );
        }
        match ty {
            TypeExpr::Application(_) => false,
            TypeExpr::Concrete(ty) => self.implementations.contains(&(contract, ty)),
            TypeExpr::Parameter(id) => caller.is_some_and(|index| {
                let template = &self.templates[index];
                id.binder == binder(template.declaration)
                    && template
                        .bounds
                        .get(id.index)
                        .is_some_and(|bounds| bounds.contains(&contract))
            }),
        }
    }
}

pub(super) fn binder(template: &ast::GenericFunction) -> ast::RecordId {
    ast::RecordId {
        source: template.function.name_span.source,
        offset: template.function.name_span.start,
    }
}

#[derive(Clone, Copy)]
pub(super) struct Context<'a> {
    pub registry: &'a Registry<'a>,
    pub source: usize,
    pub template: Option<usize>,
    pub instances: Option<&'a RefCell<Instances>>,
    pub depth: usize,
}
impl Context<'_> {
    pub fn active_binder(self) -> Option<ast::RecordId> {
        self.template
            .map(|i| binder(self.registry.templates[i].declaration))
    }
    pub fn normalize(
        self,
        ty: TypeExpr,
        span: Span,
        errors: &mut Vec<Diagnostic>,
    ) -> Option<TypeExpr> {
        // Ordinary local tensors and borrowed slices are not generic arguments;
        // their admission remains with the existing ownership checker.
        if !matches!(
            ty,
            TypeExpr::Application(_)
                | TypeExpr::Concrete(
                    Type::Record(ast::NominalId::Instance(_))
                        | Type::Union(ast::NominalId::Instance(_))
                )
        ) {
            return Some(ty);
        }
        let Some(session) = self.registry.nominal else {
            errors.push(Diagnostic::new(
                "G007",
                span,
                "nominal applications require source preparation",
            ));
            return None;
        };
        match session.normalize(ty, self.active_binder(), span) {
            Ok(ty) => Some(ty),
            Err(error) => {
                errors.push(error);
                None
            }
        }
    }
    pub fn contains(self, name: &str) -> bool {
        self.registry.names[self.source].contains_key(name)
    }
    pub fn resolve(
        self,
        name: &str,
        arguments: &[TypeExpr],
        span: Span,
        errors: &mut Vec<Diagnostic>,
    ) -> Option<body::ResolvedSignature> {
        let normalized;
        let arguments = if self.registry.nominal.is_some() {
            normalized = arguments
                .iter()
                .map(|ty| self.normalize(*ty, span, errors))
                .collect::<Option<Vec<_>>>()?;
            normalized.as_slice()
        } else {
            arguments
        };
        let Some(&index) = self.registry.names[self.source].get(name) else {
            errors.push(Diagnostic::new("G006", span, format!("unknown or inaccessible generic function `{name}`; explicit type arguments require a generic function")));
            return None;
        };
        let template = &self.registry.templates[index];
        let declaration = template.declaration;
        if arguments.len() != declaration.parameters.len() {
            errors.push(
                Diagnostic::new(
                    "G003",
                    span,
                    format!(
                        "generic function `{name}` expects {} type arguments, found {}",
                        declaration.parameters.len(),
                        arguments.len()
                    ),
                )
                .with_related(
                    declaration.function.name_span,
                    "generic function declared here",
                ),
            );
            return None;
        }
        let mut valid = true;
        for (argument, bounds) in arguments.iter().zip(&template.bounds) {
            if !self.registry.valid_type(*argument, self.template) {
                errors.push(Diagnostic::new("G003", span, "type arguments require a declared type parameter, scalar, unit, or owned inline array, record, or union"));
                valid = false;
                continue;
            }
            if let TypeExpr::Concrete(ty) = argument {
                super::check_array_length(*ty, span, errors);
                if matches!(ty, Type::Array { length, .. } if !(1..=super::MAX_ARRAY_LEN).contains(length))
                {
                    valid = false;
                }
            }
            for &contract in bounds {
                if !self.registry.entails(*argument, contract, self.template) {
                    errors.push(
                        Diagnostic::new(
                            "G004",
                            span,
                            "type argument does not satisfy the exact declared trait bound",
                        )
                        .with_related(
                            declaration.function.name_span,
                            "required by this generic function",
                        ),
                    );
                    valid = false;
                }
            }
        }
        if !valid {
            return None;
        }
        let callable = if let Some(pool) = self.instances {
            let concrete: Option<Vec<_>> = arguments.iter().map(|ty| ty.concrete()).collect();
            let Some(concrete) = concrete else {
                errors.push(Diagnostic::new(
                    "G007",
                    span,
                    "internal generic instantiation retained an opaque argument",
                ));
                return None;
            };
            output::Callable::Concrete(pool.borrow_mut().intern(
                index,
                concrete,
                self.depth + 1,
                template.nodes,
                span,
                errors,
            )?)
        } else {
            output::Callable::Template(binder(declaration))
        };
        let substitute = |ty| -> Result<TypeExpr, Diagnostic> {
            if let Some(session) = self.registry.nominal {
                return session.substitute(
                    ty,
                    binder(declaration),
                    arguments,
                    self.active_binder(),
                    span,
                );
            }
            match ty {
                TypeExpr::Concrete(_) => Ok(ty),
                TypeExpr::Parameter(id) => arguments
                    .get(id.index)
                    .copied()
                    .ok_or_else(|| Diagnostic::new("G007", span, "invalid substitution parameter")),
                TypeExpr::Application(_) => Err(Diagnostic::new(
                    "G007",
                    span,
                    "nominal substitution requires source preparation",
                )),
            }
        };
        let params = declaration
            .function
            .params
            .iter()
            .map(|p| substitute(p.ty).map(|ty| (ty, p.name_span)))
            .collect::<Result<Vec<_>, _>>();
        let return_type = substitute(declaration.function.return_type);
        let (params, return_type) = match (params, return_type) {
            (Ok(params), Ok(return_type)) => (params, return_type),
            (Err(error), _) | (_, Err(error)) => {
                errors.push(error);
                return None;
            }
        };
        Some(Signature {
            id: callable,
            params,
            return_type,
            span: declaration.function.name_span,
        })
    }
}

#[derive(Clone)]
pub(super) struct Instance {
    pub template: usize,
    pub arguments: Vec<Type>,
    pub depth: usize,
    pub span: Span,
}
pub(super) struct Instances {
    pub pending: Vec<Instance>,
    ordinary: usize,
    expanded_nodes: usize,
}
impl Instances {
    pub fn new(ordinary: usize) -> Self {
        Self {
            pending: Vec::new(),
            ordinary,
            expanded_nodes: 0,
        }
    }
    fn intern(
        &mut self,
        template: usize,
        arguments: Vec<Type>,
        depth: usize,
        nodes: usize,
        span: Span,
        errors: &mut Vec<Diagnostic>,
    ) -> Option<usize> {
        if let Some(index) = self
            .pending
            .iter()
            .position(|instance| instance.template == template && instance.arguments == arguments)
        {
            return Some(self.ordinary + index);
        }
        let message = if depth > MAX_EXPANSION_DEPTH {
            Some("generic instantiation exceeds expansion depth 32")
        } else if self.pending.len() >= MAX_INSTANCES {
            Some("project exceeds 256 distinct generic instances")
        } else if self.ordinary + self.pending.len() >= MAX_EMITTED_FUNCTIONS {
            Some("project exceeds 1024 emitted ordinary and instantiated functions")
        } else if self.expanded_nodes.saturating_add(nodes) > MAX_EXPANDED_NODES {
            Some("generic instantiation exceeds 131072 expanded syntax nodes")
        } else {
            None
        };
        if let Some(message) = message {
            errors.push(Diagnostic::new("G002", span, message));
            return None;
        }
        // Intern before checking the body: recursive calls reuse this exact key.
        let id = self.ordinary + self.pending.len();
        self.expanded_nodes += nodes;
        self.pending.push(Instance {
            template,
            arguments,
            depth,
            span,
        });
        Some(id)
    }
}

/// Iterate every source expression, including unreachable syntax, without
/// evaluating it. This is used only for resource accounting and annotation IDs;
/// semantic proof still exclusively uses FunctionChecker's common traversal.
pub(super) fn expressions<A>(block: &ast::Block<A>, visit: &mut impl FnMut(&ast::Expr)) -> usize {
    fn expr(e: &ast::Expr, visit: &mut impl FnMut(&ast::Expr)) -> usize {
        use ast::ExprKind as E;
        visit(e);
        let mut nodes = 1;
        match &e.kind {
            E::Record { fields, .. } | E::Union { fields, .. } => {
                for f in fields {
                    nodes += expr(&f.value, visit);
                }
            }
            E::Unary { value, .. } | E::Try(value) => nodes += expr(value, visit),
            E::Binary { left, right, .. } => {
                nodes += expr(left, visit);
                nodes += expr(right, visit);
            }
            E::Call { args, .. }
            | E::GenericCall { args, .. }
            | E::Intrinsic { args, .. }
            | E::TensorCall { args, .. }
            | E::Array(args) => {
                for a in args {
                    nodes += expr(a, visit);
                }
            }
            E::Borrow {
                range: Some((start, end)),
                ..
            } => {
                nodes += expr(start, visit);
                nodes += expr(end, visit);
            }
            E::Index { index, .. } => nodes += expr(index, visit),
            _ => (),
        }
        nodes
    }
    use ast::StmtKind as S;
    let mut nodes = 1;
    for statement in &block.stmts {
        nodes += 1;
        match &statement.kind {
            S::Let { value, .. }
            | S::Assign { value, .. }
            | S::FieldAssign { value, .. }
            | S::Return(value)
            | S::Print(value)
            | S::Expr(value) => nodes += expr(value, visit),
            S::IndexAssign { index, value, .. } => {
                nodes += expr(index, visit);
                nodes += expr(value, visit);
            }
            S::If {
                condition,
                then_block,
                else_block,
            } => {
                nodes += expr(condition, visit) + expressions(then_block, visit);
                if let Some(b) = else_block {
                    nodes += expressions(b, visit);
                }
            }
            S::Match { arms, .. } => {
                for arm in arms {
                    nodes += 1 + arm.fields.len() + expressions(&arm.body, visit);
                }
            }
            S::While { condition, body } => {
                nodes += expr(condition, visit) + expressions(body, visit)
            }
            S::Block(b) => nodes += expressions(b, visit),
            S::TensorScope {
                budget,
                body,
                handler,
                ..
            } => {
                nodes +=
                    expr(budget, visit) + expressions(body, visit) + expressions(handler, visit)
            }
            S::Break | S::Continue => (),
        }
    }
    nodes
}

pub(super) fn instantiate(
    template: &ast::GenericFunction,
    arguments: &[Type],
    nominal: Option<&crate::nominal::NominalSession>,
) -> Result<ast::Function<TypeExpr>, Diagnostic> {
    type Substitution<'a> = dyn FnMut(TypeExpr, Span) -> Result<TypeExpr, Diagnostic> + 'a;
    fn expr(e: &mut ast::Expr, substitute: &mut Substitution<'_>) -> Result<(), Diagnostic> {
        use ast::ExprKind as E;
        match &mut e.kind {
            E::Record {
                fields,
                application,
                ..
            }
            | E::Union {
                fields,
                application,
                ..
            } => {
                if let Some(ty) = application {
                    *ty = substitute(*ty, e.span)?;
                }
                for f in fields {
                    expr(&mut f.value, substitute)?;
                }
            }
            E::Unary { value, .. } | E::Try(value) => expr(value, substitute)?,
            E::Binary { left, right, .. } => {
                expr(left, substitute)?;
                expr(right, substitute)?;
            }
            E::GenericCall {
                type_args,
                args: values,
                ..
            } => {
                for ty in type_args {
                    *ty = substitute(*ty, e.span)?;
                }
                for value in values {
                    expr(value, substitute)?;
                }
            }
            E::Call { args: values, .. }
            | E::Intrinsic { args: values, .. }
            | E::TensorCall { args: values, .. }
            | E::Array(values) => {
                for value in values {
                    expr(value, substitute)?;
                }
            }
            E::Borrow {
                range: Some((start, end)),
                ..
            } => {
                expr(start, substitute)?;
                expr(end, substitute)?;
            }
            E::Index { index, .. } => expr(index, substitute)?,
            _ => (),
        }
        Ok(())
    }
    fn block(
        b: &mut ast::Block<TypeExpr>,
        substitute: &mut Substitution<'_>,
    ) -> Result<(), Diagnostic> {
        use ast::StmtKind as S;
        for s in &mut b.stmts {
            match &mut s.kind {
                S::Let {
                    ty,
                    value,
                    name_span,
                    ..
                } => {
                    *ty = substitute(*ty, *name_span)?;
                    expr(value, substitute)?;
                }
                S::Assign { value, .. }
                | S::FieldAssign { value, .. }
                | S::Return(value)
                | S::Print(value)
                | S::Expr(value) => expr(value, substitute)?,
                S::IndexAssign { index, value, .. } => {
                    expr(index, substitute)?;
                    expr(value, substitute)?;
                }
                S::If {
                    condition,
                    then_block,
                    else_block,
                } => {
                    expr(condition, substitute)?;
                    block(then_block, substitute)?;
                    if let Some(b) = else_block {
                        block(b, substitute)?;
                    }
                }
                S::Match { arms, .. } => {
                    for arm in arms {
                        if let Some(ty) = &mut arm.application {
                            *ty = substitute(*ty, arm.union_span)?;
                        }
                        block(&mut arm.body, substitute)?;
                    }
                }
                S::While { condition, body } => {
                    expr(condition, substitute)?;
                    block(body, substitute)?;
                }
                S::Block(b) => block(b, substitute)?,
                S::TensorScope {
                    budget,
                    body,
                    handler,
                    ..
                } => {
                    expr(budget, substitute)?;
                    block(body, substitute)?;
                    block(handler, substitute)?;
                }
                S::Break | S::Continue => (),
            }
        }
        Ok(())
    }
    let argument_types: Vec<_> = arguments.iter().copied().map(TypeExpr::Concrete).collect();
    let mut substitute = |ty, span| {
        if let Some(session) = nominal {
            return session.substitute(ty, binder(template), &argument_types, None, span);
        }
        match ty {
            TypeExpr::Concrete(_) => Ok(ty),
            TypeExpr::Parameter(id) if id.binder == binder(template) => argument_types
                .get(id.index)
                .copied()
                .ok_or_else(|| Diagnostic::new("G007", span, "invalid substitution parameter")),
            _ => Err(Diagnostic::new(
                "G007",
                span,
                "unprepared nominal or parameter identity during specialization",
            )),
        }
    };
    // Callers reserve instance/node budgets before this one bounded AST clone.
    let mut function = template.function.clone();
    function.exported = false;
    function.return_type = substitute(function.return_type, function.name_span)?;
    for param in &mut function.params {
        param.ty = substitute(param.ty, param.name_span)?;
    }
    block(&mut function.body, &mut substitute)?;
    Ok(function)
}
