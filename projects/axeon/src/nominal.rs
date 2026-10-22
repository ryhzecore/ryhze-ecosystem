//! Finite source nominal preparation and one compilation-owned normal form.
//! Source arenas are immutable; generated identities and symbolic applications
//! are admitted only by this session. No header is evidence of a trait bound.
use crate::ast::*;
use crate::diagnostic::{Diagnostic, SourceId, Span};
use std::{
    cell::RefCell,
    collections::{HashMap, HashSet},
};

pub const MAX_NOMINAL_NODES: usize = 131_072;
pub const MAX_NOMINAL_LAYOUTS: usize = 256;
pub const MAX_APPLICATION_DEPTH: usize = 32;

#[derive(Debug, Clone)]
pub struct NominalDeclaration {
    pub header: NominalHeader,
    pub source: usize,
    pub name: String,
    pub exported: bool,
    pub parameters: Vec<TypeParameter>,
    pub fields: Vec<RecordField<TypeExpr>>,
    pub variants: Vec<UnionVariant<TypeExpr>>,
}
#[derive(Debug, Clone)]
pub struct InstanceOrigin {
    pub declaration: RecordId,
    pub kind: NominalKind,
    pub arguments: Vec<TypeExpr>,
}
#[derive(Debug, Clone)]
pub struct NominalShape {
    pub owner: TypeExpr,
    pub declaration: RecordId,
    pub kind: NominalKind,
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<RecordField<TypeExpr>>,
    pub variants: Vec<UnionVariant<TypeExpr>>,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LowerBound {
    pub slots: usize,
    pub depth: usize,
}

pub(crate) struct Prepared {
    pub programs: Vec<Program>,
    pub session: NominalSession,
}
#[derive(Debug, Clone)]
struct Binder {
    bounds: Vec<Vec<RecordId>>,
}
#[derive(Debug, Clone)]
struct Obligation {
    declaration: RecordId,
    arguments: Vec<TypeExpr>,
    span: Span,
}
#[derive(Debug, Clone)]
struct Instance {
    origin: InstanceOrigin,
    shape: Option<NominalShape>,
    application_depth: usize,
}
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
struct Substitution {
    ty: TypeExpr,
    binder: RecordId,
    arguments: Box<[TypeExpr]>,
}
#[derive(Debug)]
struct Pool {
    applications: TypeApplicationsBuilder,
    keys: HashMap<TypeApplication, TypeExpr>,
    depths: HashMap<TypeExpr, usize>,
    instances: Vec<Instance>,
    shapes: HashMap<TypeExpr, NominalShape>,
    resolving: HashSet<TypeExpr>,
    failures: HashMap<TypeExpr, Diagnostic>,
    substitutions: HashMap<Substitution, TypeExpr>,
    normalized: HashMap<(TypeApplicationId, Option<RecordId>), TypeExpr>,
    source_checks: HashMap<(usize, TypeApplicationId, Option<RecordId>, bool), usize>,
    validated_types: HashSet<(TypeExpr, Option<RecordId>)>,
    public_types: HashMap<TypeExpr, bool>,
    bounds: HashMap<TypeExpr, LowerBound>,
    obligations: Vec<Obligation>,
    implementations: Vec<(RecordId, Type)>,
    contracts_validated: bool,
    nodes: usize,
}
impl Pool {
    fn new() -> Self {
        Self {
            applications: TypeApplicationsBuilder::new(),
            keys: HashMap::new(),
            depths: HashMap::new(),
            instances: Vec::new(),
            shapes: HashMap::new(),
            resolving: HashSet::new(),
            failures: HashMap::new(),
            substitutions: HashMap::new(),
            normalized: HashMap::new(),
            source_checks: HashMap::new(),
            validated_types: HashSet::new(),
            public_types: HashMap::new(),
            bounds: HashMap::new(),
            obligations: Vec::new(),
            implementations: Vec::new(),
            contracts_validated: false,
            nodes: 0,
        }
    }
    fn charge(&mut self, count: usize, span: Span) -> Result<(), Diagnostic> {
        self.nodes = self
            .nodes
            .checked_add(count)
            .filter(|n| *n <= MAX_NOMINAL_NODES)
            .ok_or_else(|| {
                Diagnostic::new(
                    "G002",
                    span,
                    "nominal expansion exceeds 131072 normalized nodes and field descriptors",
                )
            })?;
        Ok(())
    }
}
#[derive(Debug)]
pub struct NominalSession {
    declarations: HashMap<RecordId, NominalDeclaration>,
    order: Vec<RecordId>,
    names: Vec<HashMap<String, RecordId>>,
    binders: HashMap<RecordId, Binder>,
    arenas: HashMap<u64, TypeApplications>,
    ordinary_count: usize,
    new_content: bool,
    pool: RefCell<Pool>,
}

impl NominalSession {
    pub fn has_new_content(&self) -> bool {
        self.new_content
    }
    pub fn declaration(&self, id: RecordId) -> Option<NominalDeclaration> {
        self.declarations.get(&id).cloned()
    }
    pub fn nominal_header(&self, source: usize, name: &str) -> Option<NominalHeader> {
        self.declarations
            .get(self.names.get(source)?.get(name)?)
            .map(|d| d.header)
    }
    pub fn visible_declaration(&self, source: usize, name: &str) -> Option<RecordId> {
        self.names.get(source)?.get(name).copied()
    }
    pub fn application(&self, id: TypeApplicationId) -> Option<TypeApplication> {
        self.pool
            .borrow()
            .applications
            .get(id)
            .cloned()
            .or_else(|| {
                self.arenas
                    .get(&id.owner)
                    .and_then(|arena| arena.get(id))
                    .cloned()
            })
    }
    pub fn origin(&self, id: NominalId) -> Option<InstanceOrigin> {
        match id {
            NominalId::Declared(id) => self
                .declarations
                .get(&id)
                .filter(|d| d.parameters.is_empty())
                .map(|d| InstanceOrigin {
                    declaration: id,
                    kind: d.header.kind,
                    arguments: Vec::new(),
                }),
            NominalId::Instance(index) => self
                .pool
                .borrow()
                .instances
                .get(index as usize)
                .filter(|i| i.shape.is_some())
                .map(|i| i.origin.clone()),
        }
    }
    pub fn kind(&self, ty: TypeExpr) -> Option<NominalKind> {
        if self.pool.borrow().failures.contains_key(&ty) {
            return None;
        }
        match ty {
            TypeExpr::Concrete(Type::Record(id)) => self
                .origin(id)
                .filter(|o| o.kind == NominalKind::Record)
                .map(|o| o.kind),
            TypeExpr::Concrete(Type::Union(id)) => self
                .origin(id)
                .filter(|o| o.kind == NominalKind::Union)
                .map(|o| o.kind),
            TypeExpr::Application(id) => self.application(id).and_then(|a| {
                self.declarations
                    .get(&a.declaration)
                    .filter(|d| d.header.kind == a.kind)
                    .map(|d| d.header.kind)
            }),
            _ => None,
        }
    }
    pub fn owns_type(&self, ty: TypeExpr, binder: Option<RecordId>) -> bool {
        self.validate_type(ty, binder, Span::default()).is_ok()
    }
    pub fn validate_type(
        &self,
        ty: TypeExpr,
        binder: Option<RecordId>,
        span: Span,
    ) -> Result<(), Diagnostic> {
        self.validate_normal(ty, binder, span, 0)
    }
    fn validate_normal(
        &self,
        ty: TypeExpr,
        binder: Option<RecordId>,
        span: Span,
        depth: usize,
    ) -> Result<(), Diagnostic> {
        if let Some(error) = self.pool.borrow().failures.get(&ty) {
            return Err(error.clone());
        }
        match ty {
            TypeExpr::Parameter(id) => {
                if binder == Some(id.binder) && self.binders.get(&id.binder).is_some_and(|b| id.index < b.bounds.len()) { Ok(()) }
                else { Err(Diagnostic::new("G001", span, "annotation uses an undeclared or foreign parameter identity")) }
            }
            TypeExpr::Application(id) => {
                if depth >= MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
                if self.application_depth(ty)? + depth > MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
                if self.pool.borrow().validated_types.contains(&(ty, binder)) { return Ok(()); }
                let node = self.pool.borrow().applications.get(id).cloned().ok_or_else(|| invalid_handle(span))?;
                let declaration = self.declarations.get(&node.declaration).ok_or_else(|| invalid_handle(span))?;
                if declaration.header.kind != node.kind || declaration.parameters.len() != node.arguments.len() || node.arguments.is_empty() {
                    return Err(invalid_handle(span));
                }
                for &argument in &node.arguments { self.validate_normal(argument, binder, span, depth + 1)?; }
                self.pool.borrow_mut().validated_types.insert((ty, binder));
                Ok(())
            }
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
                let origin = self.origin(id).ok_or_else(|| invalid_handle(span))?;
                let kind = if matches!(ty, TypeExpr::Concrete(Type::Record(_))) { NominalKind::Record } else { NominalKind::Union };
                if origin.kind != kind { return Err(invalid_handle(span)); }
                if self.application_depth(ty)? + depth > MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
                Ok(())
            }
            TypeExpr::Concrete(Type::Array { length, .. }) => {
                if !(1..=crate::check::MAX_ARRAY_LEN).contains(&length) { Err(Diagnostic::new("O001", span, "array length must be in 1..=32")) } else { Ok(()) }
            }
            TypeExpr::Concrete(Type::I64 | Type::F64 | Type::Bool | Type::Unit) => Ok(()),
            TypeExpr::Concrete(_) => Err(Diagnostic::new("G003", span, "type arguments require a scalar, unit, declared parameter, or owned finite inline value")),
        }
    }
    fn application_depth(&self, ty: TypeExpr) -> Result<usize, Diagnostic> {
        match ty {
            TypeExpr::Application(_) => self
                .pool
                .borrow()
                .depths
                .get(&ty)
                .copied()
                .ok_or_else(|| invalid_handle(Span::default())),
            TypeExpr::Concrete(
                Type::Record(NominalId::Instance(i)) | Type::Union(NominalId::Instance(i)),
            ) => self
                .pool
                .borrow()
                .instances
                .get(i as usize)
                .map(|i| i.application_depth)
                .ok_or_else(|| invalid_handle(Span::default())),
            _ => Ok(0),
        }
    }
    pub fn normalize(
        &self,
        ty: TypeExpr,
        binder: Option<RecordId>,
        span: Span,
    ) -> Result<TypeExpr, Diagnostic> {
        self.normalize_at(ty, binder, span, 0)
    }
    fn normalize_at(
        &self,
        ty: TypeExpr,
        binder: Option<RecordId>,
        span: Span,
        depth: usize,
    ) -> Result<TypeExpr, Diagnostic> {
        match ty {
            TypeExpr::Application(id) => {
                if depth >= MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                if let Some(normalized) = self.pool.borrow().normalized.get(&(id, binder)).copied()
                {
                    if depth + self.application_depth(normalized)? > MAX_APPLICATION_DEPTH {
                        return Err(depth_error(span));
                    }
                    return Ok(normalized);
                }
                let node = self.application(id).ok_or_else(|| invalid_handle(span))?;
                let mut arguments = Vec::with_capacity(node.arguments.len().min(8));
                if node.arguments.len() > 8 {
                    return Err(Diagnostic::new(
                        "G002",
                        span,
                        "nominal application supports at most 8 arguments",
                    ));
                }
                for &argument in &node.arguments {
                    arguments.push(self.normalize_at(argument, binder, span, depth + 1)?);
                }
                let normalized =
                    self.admit(node.declaration, node.kind, &arguments, binder, span)?;
                if depth + self.application_depth(normalized)? > MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                self.pool
                    .borrow_mut()
                    .normalized
                    .insert((id, binder), normalized);
                Ok(normalized)
            }
            TypeExpr::Parameter(_) => {
                self.validate_type(ty, binder, span)?;
                Ok(ty)
            }
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
                let declared = id.declaration().and_then(|id| self.declarations.get(&id));
                if declared.is_some_and(|d| !d.parameters.is_empty()) {
                    return Err(Diagnostic::new(
                        "G003",
                        span,
                        "a generic nominal name requires all explicit type arguments",
                    ));
                }
                if self.kind(ty).is_none() {
                    return Err(invalid_handle(span));
                }
                if depth + self.application_depth(ty)? > MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                Ok(ty)
            }
            TypeExpr::Concrete(_) => Ok(ty), // ordinary tensor/slice locals retain their existing semantics
        }
    }
    pub fn apply(
        &self,
        source: usize,
        name: &str,
        args: &[TypeExpr],
        caller: Option<RecordId>,
        span: Span,
    ) -> Result<TypeExpr, Diagnostic> {
        let id = self
            .names
            .get(source)
            .and_then(|n| n.get(name))
            .copied()
            .ok_or_else(|| {
                Diagnostic::new(
                    "G006",
                    span,
                    format!("unknown or inaccessible nominal type `{name}`"),
                )
            })?;
        let declaration = &self.declarations[&id];
        let args = args
            .iter()
            .map(|&a| self.normalize(a, caller, span))
            .collect::<Result<Vec<_>, _>>()?;
        self.admit(id, declaration.header.kind, &args, caller, span)
    }
    fn admit(
        &self,
        id: RecordId,
        kind: NominalKind,
        arguments: &[TypeExpr],
        caller: Option<RecordId>,
        span: Span,
    ) -> Result<TypeExpr, Diagnostic> {
        let declaration = self
            .declarations
            .get(&id)
            .ok_or_else(|| invalid_handle(span))?;
        if declaration.header.kind != kind
            || declaration.parameters.is_empty()
            || declaration.parameters.len() != arguments.len()
        {
            return Err(Diagnostic::new(
                "G003",
                span,
                format!(
                    "nominal `{}` expects {} explicit arguments, found {}",
                    declaration.name,
                    declaration.parameters.len(),
                    arguments.len()
                ),
            )
            .with_related(declaration.header.name_span, "nominal declaration"));
        }
        let mut application_depth = 1;
        for &argument in arguments {
            self.validate_type(argument, caller, span)?;
            application_depth = application_depth.max(1 + self.application_depth(argument)?);
        }
        if application_depth > MAX_APPLICATION_DEPTH {
            return Err(depth_error(span));
        }
        if self.pool.borrow().contracts_validated {
            self.check_bounds(id, arguments, span, None)?;
        }
        let key = TypeApplication {
            declaration: id,
            kind,
            arguments: arguments.into(),
        };
        if let Some(&existing) = self.pool.borrow().keys.get(&key) {
            if self.pool.borrow().resolving.contains(&existing) {
                return Err(Diagnostic::new(
                    "A001",
                    span,
                    "nominal instance layout is already being resolved",
                )
                .with_related(
                    declaration.header.name_span,
                    "recursive instance declaration",
                ));
            }
            if let Some(error) = self.pool.borrow().failures.get(&existing) {
                return Err(error.clone());
            }
            if !self.pool.borrow().shapes.contains_key(&existing)
                || !self.pool.borrow().bounds.contains_key(&existing)
            {
                return Err(invalid_handle(span));
            }
            return Ok(existing);
        }
        let concrete = arguments.iter().all(|a| a.concrete().is_some());
        let owner = {
            let mut pool = self.pool.borrow_mut();
            if concrete && self.ordinary_count + pool.instances.len() >= MAX_NOMINAL_LAYOUTS {
                return Err(Diagnostic::new(
                    "G002",
                    span,
                    "project exceeds 256 ordinary and instantiated nominal layouts",
                ));
            }
            pool.charge(1, span)?;
            let owner = if concrete {
                let index = pool.instances.len() as u32;
                pool.instances.push(Instance {
                    origin: InstanceOrigin {
                        declaration: id,
                        kind,
                        arguments: arguments.to_vec(),
                    },
                    shape: None,
                    application_depth,
                });
                TypeExpr::Concrete(match kind {
                    NominalKind::Record => Type::Record(NominalId::Instance(index)),
                    NominalKind::Union => Type::Union(NominalId::Instance(index)),
                })
            } else {
                TypeExpr::Application(pool.applications.insert(key.clone()).ok_or_else(|| {
                    Diagnostic::new(
                        "G002",
                        span,
                        "nominal application arena exceeds its node limit",
                    )
                })?)
            };
            pool.keys.insert(key, owner);
            pool.depths.insert(owner, application_depth);
            pool.obligations.push(Obligation {
                declaration: id,
                arguments: arguments.to_vec(),
                span,
            });
            owner
        };
        // The key is reserved before resolving fields; an active key never acts
        // as a zero-sized layout or a successfully proven instance.
        self.build_shape(owner, id, arguments, span)?;
        if let Err(error) = self.lower_bound(owner, span) {
            self.remember_failure(owner, &error);
            return Err(error);
        }
        Ok(owner)
    }
    fn remember_failure(&self, owner: TypeExpr, error: &Diagnostic) {
        let mut pool = self.pool.borrow_mut();
        // Keep the reserved identity and its original failure. Retrying its key
        // cannot reuse an incomplete layout or reset the expansion budget.
        pool.failures.entry(owner).or_insert_with(|| error.clone());
        pool.shapes.remove(&owner);
        pool.bounds.remove(&owner);
        pool.public_types.remove(&owner);
        if let TypeExpr::Concrete(
            Type::Record(NominalId::Instance(i)) | Type::Union(NominalId::Instance(i)),
        ) = owner
        {
            if let Some(instance) = pool.instances.get_mut(i as usize) {
                instance.shape = None;
            }
        }
    }
    pub fn substitute(
        &self,
        ty: TypeExpr,
        binder: RecordId,
        args: &[TypeExpr],
        caller: Option<RecordId>,
        span: Span,
    ) -> Result<TypeExpr, Diagnostic> {
        let Some(expected) = self.binders.get(&binder) else {
            return Err(invalid_handle(span));
        };
        if args.len() != expected.bounds.len() {
            return Err(Diagnostic::new(
                "G003",
                span,
                "substitution argument count does not match its binder",
            ));
        }
        for &argument in args {
            self.validate_type(argument, caller, span)?;
        }
        self.substitute_at(ty, binder, args, caller, span, 0)
    }
    fn substitute_at(
        &self,
        ty: TypeExpr,
        binder: RecordId,
        args: &[TypeExpr],
        caller: Option<RecordId>,
        span: Span,
        depth: usize,
    ) -> Result<TypeExpr, Diagnostic> {
        if depth > MAX_APPLICATION_DEPTH {
            return Err(depth_error(span));
        }
        match ty {
            TypeExpr::Concrete(_) => self.normalize(ty, caller, span),
            TypeExpr::Parameter(id) => {
                if id.binder != binder {
                    return Err(Diagnostic::new(
                        "G001",
                        span,
                        "substitution encountered a foreign parameter binder",
                    ));
                }
                args.get(id.index)
                    .copied()
                    .ok_or_else(|| invalid_handle(span))
            }
            TypeExpr::Application(id) => {
                let key = Substitution {
                    ty,
                    binder,
                    arguments: args.into(),
                };
                if let Some(&result) = self.pool.borrow().substitutions.get(&key) {
                    return Ok(result);
                }
                let node = self.application(id).ok_or_else(|| invalid_handle(span))?;
                if depth >= MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                let normalized_args = node
                    .arguments
                    .iter()
                    .map(|&ty| self.substitute_at(ty, binder, args, caller, span, depth + 1))
                    .collect::<Result<Vec<_>, _>>()?;
                let result =
                    self.admit(node.declaration, node.kind, &normalized_args, caller, span)?;
                let mut pool = self.pool.borrow_mut();
                pool.charge(1, span)?;
                pool.substitutions.insert(key, result);
                Ok(result)
            }
        }
    }
    pub fn entails(&self, ty: TypeExpr, contract: RecordId, caller: Option<RecordId>) -> bool {
        match ty {
            TypeExpr::Parameter(id) => {
                caller == Some(id.binder)
                    && self
                        .binders
                        .get(&id.binder)
                        .and_then(|b| b.bounds.get(id.index))
                        .is_some_and(|b| b.contains(&contract))
            }
            TypeExpr::Concrete(ty) => {
                let pool = self.pool.borrow();
                pool.contracts_validated && pool.implementations.contains(&(contract, ty))
            }
            TypeExpr::Application(_) => false,
        }
    }
    fn check_bounds(
        &self,
        id: RecordId,
        arguments: &[TypeExpr],
        span: Span,
        implementations: Option<&[(RecordId, Type)]>,
    ) -> Result<(), Diagnostic> {
        let declaration = &self.declarations[&id];
        let bounds = &self.binders[&id].bounds;
        for (&argument, bounds) in arguments.iter().zip(bounds) {
            for &contract in bounds {
                let proved = match argument {
                    TypeExpr::Parameter(parameter) => self
                        .binders
                        .get(&parameter.binder)
                        .and_then(|b| b.bounds.get(parameter.index))
                        .is_some_and(|b| b.contains(&contract)),
                    TypeExpr::Concrete(ty) => implementations.map_or_else(
                        || {
                            let pool = self.pool.borrow();
                            pool.contracts_validated
                                && pool.implementations.contains(&(contract, ty))
                        },
                        |impls| impls.contains(&(contract, ty)),
                    ),
                    TypeExpr::Application(_) => false,
                };
                if !proved {
                    return Err(Diagnostic::new(
                        "G004",
                        span,
                        "nominal type argument does not satisfy the exact declared trait bound",
                    )
                    .with_related(
                        declaration.header.name_span,
                        "required by this nominal declaration",
                    ));
                }
            }
        }
        Ok(())
    }
    pub fn validate_contracts(
        &self,
        implementations: &[(RecordId, Type)],
        limit: usize,
    ) -> Result<(), Vec<Diagnostic>> {
        {
            let mut pool = self.pool.borrow_mut();
            pool.contracts_validated = false;
            pool.implementations.clear();
        }
        let obligations = self.pool.borrow().obligations.clone();
        let mut errors = Vec::new();
        for obligation in obligations {
            if let Err(error) = self.check_bounds(
                obligation.declaration,
                &obligation.arguments,
                obligation.span,
                Some(implementations),
            ) {
                errors.push(error);
                if errors.len() >= limit.max(1) {
                    break;
                }
            }
        }
        if !errors.is_empty() {
            return Err(errors);
        }
        let mut pool = self.pool.borrow_mut();
        pool.implementations = implementations.to_vec();
        pool.contracts_validated = true;
        Ok(())
    }
    pub fn public_type(&self, ty: TypeExpr) -> bool {
        self.public_at(ty, 0)
    }
    fn public_at(&self, ty: TypeExpr, depth: usize) -> bool {
        if depth > MAX_APPLICATION_DEPTH || self.pool.borrow().failures.contains_key(&ty) {
            return false;
        }
        if let Some(public) = self.pool.borrow().public_types.get(&ty).copied() {
            return public;
        }
        let (id, arguments) = match ty {
            TypeExpr::Parameter(id) => {
                return self
                    .binders
                    .get(&id.binder)
                    .is_some_and(|b| id.index < b.bounds.len())
            }
            TypeExpr::Application(id) => match self.application(id) {
                Some(a) => (a.declaration, a.arguments.to_vec()),
                None => return false,
            },
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => match self.origin(id) {
                Some(o) => (o.declaration, o.arguments),
                None => return false,
            },
            TypeExpr::Concrete(_) => return true,
        };
        let public = self.declarations.get(&id).is_some_and(|d| d.exported)
            && arguments.iter().all(|&a| self.public_at(a, depth + 1));
        self.pool.borrow_mut().public_types.insert(ty, public);
        public
    }
    pub fn shape(&self, ty: TypeExpr, span: Span) -> Result<NominalShape, Diagnostic> {
        if let Some(error) = self.pool.borrow().failures.get(&ty) {
            return Err(error.clone());
        }
        if matches!(
            ty,
            TypeExpr::Concrete(Type::Record(_) | Type::Union(_)) | TypeExpr::Application(_)
        ) && self.kind(ty).is_none()
        {
            return Err(invalid_handle(span));
        }
        if let Some(shape) = self.pool.borrow().shapes.get(&ty).cloned() {
            return Ok(shape);
        }
        let origin = match ty {
            TypeExpr::Application(id) => {
                let node = self
                    .pool
                    .borrow()
                    .applications
                    .get(id)
                    .cloned()
                    .ok_or_else(|| invalid_handle(span))?;
                InstanceOrigin {
                    declaration: node.declaration,
                    kind: node.kind,
                    arguments: node.arguments.to_vec(),
                }
            }
            TypeExpr::Concrete(
                Type::Record(NominalId::Declared(id)) | Type::Union(NominalId::Declared(id)),
            ) => {
                let declaration = self
                    .declarations
                    .get(&id)
                    .filter(|d| d.parameters.is_empty())
                    .ok_or_else(|| invalid_handle(span))?;
                InstanceOrigin {
                    declaration: id,
                    kind: declaration.header.kind,
                    arguments: Vec::new(),
                }
            }
            TypeExpr::Concrete(
                Type::Record(NominalId::Instance(_)) | Type::Union(NominalId::Instance(_)),
            ) => return Err(invalid_handle(span)),
            _ => {
                return Err(Diagnostic::new(
                    "R010",
                    span,
                    "type does not have a known nominal record or union shape",
                ))
            }
        };
        self.build_shape(ty, origin.declaration, &origin.arguments, span)
    }
    fn build_shape(
        &self,
        owner: TypeExpr,
        id: RecordId,
        arguments: &[TypeExpr],
        span: Span,
    ) -> Result<NominalShape, Diagnostic> {
        if let Some(error) = self.pool.borrow().failures.get(&owner) {
            return Err(error.clone());
        }
        if let Some(shape) = self.pool.borrow().shapes.get(&owner).cloned() {
            return Ok(shape);
        }
        let declaration = self
            .declarations
            .get(&id)
            .ok_or_else(|| invalid_handle(span))?;
        if !self.pool.borrow_mut().resolving.insert(owner) {
            return Err(Diagnostic::new(
                "A001",
                span,
                "inline nominal layout is already being resolved",
            )
            .with_related(declaration.header.name_span, "recursive layout declaration"));
        }
        let result: Result<NominalShape, Diagnostic> = (|| {
            let caller = self.argument_binder(arguments)?;
            let resolve =
                |field: &RecordField<TypeExpr>| -> Result<RecordField<TypeExpr>, Diagnostic> {
                    self.pool.borrow_mut().charge(1, field.name_span)?;
                    let binder = (!declaration.parameters.is_empty()).then_some(id);
                    let ty = self.normalize(field.ty, binder, field.name_span)?;
                    let ty = if binder.is_some() {
                        self.substitute(ty, id, arguments, caller, field.name_span)?
                    } else {
                        ty
                    };
                    self.validate_type(ty, caller, field.name_span)?;
                    Ok(RecordField {
                        name: field.name.clone(),
                        name_span: field.name_span,
                        ty,
                    })
                };
            let fields = declaration
                .fields
                .iter()
                .map(resolve)
                .collect::<Result<Vec<_>, _>>()?;
            let variants = declaration
                .variants
                .iter()
                .map(|variant| {
                    Ok(UnionVariant {
                        name: variant.name.clone(),
                        name_span: variant.name_span,
                        fields: variant
                            .fields
                            .iter()
                            .map(resolve)
                            .collect::<Result<Vec<_>, Diagnostic>>()?,
                    })
                })
                .collect::<Result<Vec<_>, Diagnostic>>()?;
            Ok(NominalShape {
                owner,
                declaration: id,
                kind: declaration.header.kind,
                name: declaration.name.clone(),
                name_span: declaration.header.name_span,
                fields,
                variants,
            })
        })();
        let mut pool = self.pool.borrow_mut();
        pool.resolving.remove(&owner);
        match &result {
            Ok(shape) => {
                pool.shapes.insert(owner, shape.clone());
                if let TypeExpr::Concrete(
                    Type::Record(NominalId::Instance(i)) | Type::Union(NominalId::Instance(i)),
                ) = owner
                {
                    pool.instances[i as usize].shape = Some(shape.clone());
                }
            }
            Err(error) => {
                pool.failures.insert(owner, error.clone());
            }
        }
        result
    }
    fn argument_binder(&self, arguments: &[TypeExpr]) -> Result<Option<RecordId>, Diagnostic> {
        let mut binder = None;
        let mut pending = arguments.to_vec();
        let mut seen = HashSet::new();
        while let Some(ty) = pending.pop() {
            if !seen.insert(ty) {
                continue;
            }
            match ty {
                TypeExpr::Parameter(id) => {
                    if binder.is_some_and(|b| b != id.binder) {
                        return Err(Diagnostic::new(
                            "G001",
                            Span::default(),
                            "application contains parameters from unrelated binders",
                        ));
                    }
                    binder = Some(id.binder);
                }
                TypeExpr::Application(id) => pending.extend(
                    self.application(id)
                        .ok_or_else(|| invalid_handle(Span::default()))?
                        .arguments
                        .iter()
                        .copied(),
                ),
                _ => (),
            }
        }
        Ok(binder)
    }
    pub fn lower_bound(&self, ty: TypeExpr, span: Span) -> Result<LowerBound, Diagnostic> {
        self.lower_at(ty, span, &mut HashSet::new())
    }
    fn lower_at(
        &self,
        ty: TypeExpr,
        span: Span,
        active: &mut HashSet<TypeExpr>,
    ) -> Result<LowerBound, Diagnostic> {
        match ty {
            TypeExpr::Concrete(Type::Array { length, .. }) => {
                return Ok(LowerBound {
                    slots: length,
                    depth: 1,
                })
            }
            TypeExpr::Parameter(_) => return Ok(LowerBound { slots: 1, depth: 0 }),
            TypeExpr::Concrete(Type::Record(_) | Type::Union(_)) | TypeExpr::Application(_) => (),
            TypeExpr::Concrete(_) => return Ok(LowerBound { slots: 1, depth: 0 }),
        }
        if let Some(bound) = self.pool.borrow().bounds.get(&ty).copied() {
            return Ok(bound);
        }
        if active.len() >= MAX_INLINE_DEPTH {
            return Err(Diagnostic::new(
                "A002",
                span,
                "inline aggregate composition exceeds depth 32",
            ));
        }
        if !active.insert(ty) {
            return Err(Diagnostic::new(
                "A001",
                span,
                "inline aggregate composition contains a value cycle",
            ));
        }
        let shape = self.shape(ty, span)?;
        let groups: Vec<&[RecordField<TypeExpr>]> = match shape.kind {
            NominalKind::Record => vec![shape.fields.as_slice()],
            NominalKind::Union => shape.variants.iter().map(|v| v.fields.as_slice()).collect(),
        };
        let tag = usize::from(shape.kind == NominalKind::Union);
        let mut result = LowerBound {
            slots: tag,
            depth: 1,
        };
        for fields in groups {
            let mut slots = tag;
            for field in fields {
                let child = self.lower_at(field.ty, field.name_span, active)?;
                result.depth = result.depth.max(1 + child.depth);
                if result.depth > MAX_INLINE_DEPTH {
                    return Err(Diagnostic::new(
                        "A002",
                        field.name_span,
                        "inline aggregate composition exceeds depth 32",
                    )
                    .with_related(shape.name_span, "enclosing nominal declaration"));
                }
                slots = slots
                    .checked_add(child.slots)
                    .filter(|n| *n <= MAX_INLINE_SLOTS)
                    .ok_or_else(|| {
                        Diagnostic::new(
                            "A003",
                            field.name_span,
                            "inline aggregate layout exceeds 256 scalar/tag slots",
                        )
                        .with_related(shape.name_span, "enclosing nominal declaration")
                    })?;
            }
            result.slots = result.slots.max(slots);
        }
        active.remove(&ty);
        self.pool.borrow_mut().bounds.insert(ty, result);
        Ok(result)
    }
    pub fn storage_slots(&self, ty: TypeExpr, span: Span) -> Result<usize, Diagnostic> {
        if matches!(ty, TypeExpr::Parameter(_)) {
            Ok(0)
        } else {
            self.lower_bound(ty, span).map(|b| b.slots)
        }
    }
    pub fn concrete_definitions(&self) -> (Vec<Record>, Vec<Union>) {
        let pool = self.pool.borrow();
        let mut shapes = Vec::new();
        for id in &self.order {
            let declaration = &self.declarations[id];
            if declaration.parameters.is_empty() {
                let ty = TypeExpr::Concrete(match declaration.header.kind {
                    NominalKind::Record => Type::Record((*id).into()),
                    NominalKind::Union => Type::Union((*id).into()),
                });
                if let Some(shape) = pool.shapes.get(&ty) {
                    shapes.push(shape);
                }
            }
        }
        shapes.extend(pool.instances.iter().filter_map(|i| i.shape.as_ref()));
        let mut records = Vec::new();
        let mut unions = Vec::new();
        for shape in shapes {
            let declaration = &self.declarations[&shape.declaration];
            let Some(Type::Record(id) | Type::Union(id)) = shape.owner.concrete() else {
                continue;
            };
            let fields = |fields: &[RecordField<TypeExpr>]| {
                fields
                    .iter()
                    .map(|f| RecordField {
                        name: f.name.clone(),
                        name_span: f.name_span,
                        ty: f.ty.concrete().expect("complete concrete nominal field"),
                    })
                    .collect()
            };
            match shape.kind {
                NominalKind::Record => records.push(Record {
                    exported: declaration.exported,
                    id,
                    name: shape.name.clone(),
                    name_span: shape.name_span,
                    fields: fields(&shape.fields),
                }),
                NominalKind::Union => unions.push(Union {
                    exported: declaration.exported,
                    id,
                    name: shape.name.clone(),
                    name_span: shape.name_span,
                    variants: shape
                        .variants
                        .iter()
                        .map(|v| UnionVariant {
                            name: v.name.clone(),
                            name_span: v.name_span,
                            fields: fields(&v.fields),
                        })
                        .collect(),
                }),
            }
        }
        (records, unions)
    }
    pub fn layouts(&self) -> Result<InlineLayouts, Diagnostic> {
        let (records, unions) = self.concrete_definitions();
        InlineLayouts::build(&records, &unions).map_err(|e| {
            let diagnostic = Diagnostic::new(e.code, e.span, e.message);
            if let Some(span) = e.related {
                diagnostic.with_related(span, "enclosing inline layout")
            } else {
                diagnostic
            }
        })
    }
}

fn invalid_handle(span: Span) -> Diagnostic {
    Diagnostic::new(
        "G007",
        span,
        "nominal type identity or application handle does not belong to this prepared compilation",
    )
}
fn depth_error(span: Span) -> Diagnostic {
    Diagnostic::new(
        "G002",
        span,
        "nominal type application exceeds depth 32, including phantom arguments",
    )
}

pub(crate) fn prepare_sources(
    programs: &[SourceProgram],
    imports: &[Vec<(String, SourceId)>],
    diagnostic_limit: usize,
) -> Result<Prepared, Vec<Diagnostic>> {
    preflight_sources(programs, diagnostic_limit)?;
    prepare(programs, imports).map_err(|error| {
        let mut errors = vec![error];
        errors.truncate(diagnostic_limit.max(1));
        errors
    })
}

fn prepare(
    programs: &[SourceProgram],
    imports: &[Vec<(String, SourceId)>],
) -> Result<Prepared, Diagnostic> {
    let new_content = programs.iter().any(|p| {
        !p.applications.is_empty() || !p.generic_records.is_empty() || !p.generic_unions.is_empty()
    });
    let mut session = NominalSession {
        declarations: HashMap::new(),
        order: Vec::new(),
        names: vec![HashMap::new(); programs.len()],
        binders: HashMap::new(),
        arenas: HashMap::new(),
        ordinary_count: 0,
        new_content,
        pool: RefCell::new(Pool::new()),
    };
    if imports.len() != programs.len() {
        return Err(Diagnostic::new(
            "G007",
            Span::default(),
            "module import table does not match source programs",
        ));
    }
    let mut generic_count = 0;
    for (source, program) in programs.iter().enumerate() {
        if program.records.len()
            + program.unions.len()
            + program.generic_records.len()
            + program.generic_unions.len()
            > MAX_NOMINAL_LAYOUTS
        {
            return Err(Diagnostic::new(
                "R001",
                Span::in_source(SourceId(source), 0, 0),
                "project exceeds 256 record and union declarations",
            ));
        }
        session
            .arenas
            .insert(program.applications.owner(), program.applications.clone());
        let ordinary_records = program
            .records
            .iter()
            .map(|r| declaration_record(source, r, &[]));
        let ordinary_unions = program
            .unions
            .iter()
            .map(|u| declaration_union(source, u, &[]));
        let generic_records = program.generic_records.iter().map(|r| {
            if r.parameters.is_empty() {
                Err(Diagnostic::new(
                    "G002",
                    r.record.name_span,
                    "a generic nominal declaration requires 1..=8 parameters",
                ))
            } else {
                declaration_record(source, &r.record, &r.parameters)
            }
        });
        let generic_unions = program.generic_unions.iter().map(|u| {
            if u.parameters.is_empty() {
                Err(Diagnostic::new(
                    "G002",
                    u.union.name_span,
                    "a generic nominal declaration requires 1..=8 parameters",
                ))
            } else {
                declaration_union(source, &u.union, &u.parameters)
            }
        });
        for declaration in ordinary_records
            .chain(ordinary_unions)
            .chain(generic_records)
            .chain(generic_unions)
        {
            let declaration = declaration?;
            validate_declaration(&declaration)?;
            if session.order.len() >= MAX_NOMINAL_LAYOUTS {
                return Err(Diagnostic::new(
                    "R001",
                    declaration.header.name_span,
                    "project exceeds 256 record and union declarations",
                ));
            }
            if let Some(previous) =
                session.names[source].insert(declaration.name.clone(), declaration.header.id)
            {
                return Err(Diagnostic::new(
                    "R003",
                    declaration.header.name_span,
                    format!("nominal type `{}` is already declared", declaration.name),
                )
                .with_related(
                    session.declarations[&previous].header.name_span,
                    "previous nominal declaration",
                ));
            }
            if let Some(previous) = session.declarations.get(&declaration.header.id) {
                return Err(Diagnostic::new(
                    "G007",
                    declaration.header.name_span,
                    "source nominal declarations have colliding identities",
                )
                .with_related(previous.header.name_span, "previous nominal identity"));
            }
            if declaration.parameters.is_empty() {
                session.ordinary_count += 1;
            } else {
                generic_count += 1;
            }
            session.order.push(declaration.header.id);
            session
                .declarations
                .insert(declaration.header.id, declaration);
        }
        if generic_count > 128 {
            return Err(Diagnostic::new(
                "G002",
                Span::in_source(SourceId(source), 0, 0),
                "project exceeds 128 generic nominal declarations",
            ));
        }
    }
    for (source, edges) in imports.iter().enumerate() {
        for (alias, target) in edges {
            if target.0 >= programs.len() {
                return Err(Diagnostic::new(
                    "G007",
                    Span::in_source(SourceId(source), 0, 0),
                    "import references an unknown source module",
                ));
            }
            for id in &session.order {
                let declaration = &session.declarations[id];
                if declaration.source == target.0 && declaration.exported {
                    session.names[source].insert(format!("{alias}::{}", declaration.name), *id);
                }
            }
        }
    }
    for id in &session.order {
        let declaration = &session.declarations[id];
        if !declaration.parameters.is_empty() {
            let binder = resolve_binder(
                programs,
                imports,
                &session.names,
                declaration.source,
                *id,
                declaration.exported,
                &declaration.parameters,
                declaration.header.name_span,
            )?;
            session.binders.insert(*id, binder);
        }
    }
    for (source, program) in programs.iter().enumerate() {
        if program.generic_functions.len() > 128 {
            return Err(Diagnostic::new(
                "G002",
                Span::in_source(SourceId(source), 0, 0),
                "project exceeds 128 generic function declarations",
            ));
        }
        for template in &program.generic_functions {
            let id = RecordId {
                source: template.function.name_span.source,
                offset: template.function.name_span.start,
            };
            let binder = resolve_binder(
                programs,
                imports,
                &session.names,
                source,
                id,
                template.function.exported,
                &template.parameters,
                template.function.name_span,
            )?;
            if session.binders.insert(id, binder).is_some() {
                return Err(Diagnostic::new(
                    "G007",
                    template.function.name_span,
                    "generic declarations have colliding binder identities",
                ));
            }
        }
    }
    // All AST API input is admitted iteratively before any recursive AST clone
    // or normalizer. The guard does not perform or replace storage accounting.
    for (source, program) in programs.iter().enumerate() {
        preflight(&session, source, program)?;
    }
    check_cycles(&session)?;

    // Normalize fields in definition scope, including unused nominal templates.
    let mut normalized_declarations = HashMap::new();
    for id in &session.order {
        let declaration = &session.declarations[id];
        let binder = (!declaration.parameters.is_empty()).then_some(*id);
        let mut normalized = declaration.clone();
        for field in &mut normalized.fields {
            field.ty = session.normalize(field.ty, binder, field.name_span)?;
            session.validate_type(field.ty, binder, field.name_span)?;
        }
        for variant in &mut normalized.variants {
            for field in &mut variant.fields {
                field.ty = session.normalize(field.ty, binder, field.name_span)?;
                session.validate_type(field.ty, binder, field.name_span)?;
            }
        }
        if declaration.exported {
            for field in normalized
                .fields
                .iter()
                .chain(normalized.variants.iter().flat_map(|v| &v.fields))
            {
                if !session.public_type(field.ty) {
                    return Err(Diagnostic::new(
                        "I002",
                        field.name_span,
                        "exported aggregate fields cannot expose a private nominal type",
                    ));
                }
            }
        }
        normalized_declarations.insert(*id, normalized);
    }
    session.declarations = normalized_declarations;
    // Known lower bounds validate unused generic declarations without fabricating
    // a physical instance or promising that every later argument fits.
    for id in &session.order {
        let declaration = &session.declarations[id];
        let ty = if declaration.parameters.is_empty() {
            TypeExpr::Concrete(match declaration.header.kind {
                NominalKind::Record => Type::Record((*id).into()),
                NominalKind::Union => Type::Union((*id).into()),
            })
        } else {
            let args: Vec<_> = (0..declaration.parameters.len())
                .map(|index| TypeExpr::Parameter(TypeParameterId { binder: *id, index }))
                .collect();
            session.admit(
                *id,
                declaration.header.kind,
                &args,
                Some(*id),
                declaration.header.name_span,
            )?
        };
        session.shape(ty, declaration.header.name_span)?;
        session.lower_bound(ty, declaration.header.name_span)?;
    }
    session.layouts()?;
    let mut prepared = Vec::with_capacity(programs.len());
    for program in programs {
        let mut normalized = program.clone();
        for function in &mut normalized.functions {
            normalize_function(&session, function, None)?;
        }
        for template in &mut normalized.generic_functions {
            let binder = RecordId {
                source: template.function.name_span.source,
                offset: template.function.name_span.start,
            };
            normalize_function(&session, &mut template.function, Some(binder))?;
        }
        for template in &mut normalized.generic_records {
            let id = template
                .record
                .id
                .declaration()
                .ok_or_else(|| invalid_handle(template.record.name_span))?;
            template.record.fields = session.declarations[&id].fields.clone();
        }
        for template in &mut normalized.generic_unions {
            let id = template
                .union
                .id
                .declaration()
                .ok_or_else(|| invalid_handle(template.union.name_span))?;
            template.union.variants = session.declarations[&id].variants.clone();
        }
        prepared.push(normalized.try_map_annotations(|ty, span| {
            session
                .normalize(ty, None, span)?
                .concrete()
                .ok_or_else(|| {
                    Diagnostic::new(
                        "G007",
                        span,
                        "ordinary source annotation retained an opaque type after preparation",
                    )
                })
        })?);
    }
    session.layouts()?;
    Ok(Prepared {
        programs: prepared,
        session,
    })
}

fn declaration_record(
    source: usize,
    record: &Record<TypeExpr>,
    parameters: &[TypeParameter],
) -> Result<NominalDeclaration, Diagnostic> {
    let id = record
        .id
        .declaration()
        .ok_or_else(|| invalid_handle(record.name_span))?;
    if id.source.0 != source || record.name_span.source.0 != source {
        return Err(invalid_handle(record.name_span));
    }
    if record.fields.len() > crate::parser::MAX_RECORD_FIELDS {
        return Err(Diagnostic::new(
            "R002",
            record.name_span,
            "a record supports at most 32 fields",
        ));
    }
    check_parameter_budget(parameters, record.name_span)?;
    Ok(NominalDeclaration {
        header: NominalHeader {
            id,
            kind: NominalKind::Record,
            arity: parameters.len(),
            name_span: record.name_span,
        },
        source,
        name: record.name.clone(),
        exported: record.exported,
        parameters: parameters.to_vec(),
        fields: record.fields.clone(),
        variants: Vec::new(),
    })
}
fn declaration_union(
    source: usize,
    union: &Union<TypeExpr>,
    parameters: &[TypeParameter],
) -> Result<NominalDeclaration, Diagnostic> {
    let id = union
        .id
        .declaration()
        .ok_or_else(|| invalid_handle(union.name_span))?;
    if id.source.0 != source || union.name_span.source.0 != source {
        return Err(invalid_handle(union.name_span));
    }
    if union.variants.len() > crate::parser::MAX_UNION_VARIANTS
        || union
            .variants
            .iter()
            .any(|v| v.fields.len() > crate::parser::MAX_RECORD_FIELDS)
    {
        return Err(Diagnostic::new(
            "U002",
            union.name_span,
            "union variants and fields exceed their 32-element admission limit",
        ));
    }
    check_parameter_budget(parameters, union.name_span)?;
    Ok(NominalDeclaration {
        header: NominalHeader {
            id,
            kind: NominalKind::Union,
            arity: parameters.len(),
            name_span: union.name_span,
        },
        source,
        name: union.name.clone(),
        exported: union.exported,
        parameters: parameters.to_vec(),
        fields: Vec::new(),
        variants: union.variants.clone(),
    })
}
fn validate_declaration(declaration: &NominalDeclaration) -> Result<(), Diagnostic> {
    if declaration.kind_is_record() && declaration.fields.is_empty() {
        return Err(Diagnostic::new(
            "R002",
            declaration.header.name_span,
            "a record requires 1..=32 fields",
        ));
    }
    if !declaration.kind_is_record() && declaration.variants.is_empty() {
        return Err(Diagnostic::new(
            "U001",
            declaration.header.name_span,
            "a union requires 1..=32 variants",
        ));
    }
    let mut variants = HashMap::new();
    for variant in &declaration.variants {
        if let Some(previous) = variants.insert(&variant.name, variant.name_span) {
            return Err(
                Diagnostic::new("U003", variant.name_span, "duplicate union variant")
                    .with_related(previous, "previous variant declaration"),
            );
        }
    }
    for fields in
        std::iter::once(&declaration.fields).chain(declaration.variants.iter().map(|v| &v.fields))
    {
        let mut names = HashMap::new();
        for field in fields {
            if let Some(previous) = names.insert(&field.name, field.name_span) {
                return Err(Diagnostic::new(
                    if declaration.kind_is_record() {
                        "R004"
                    } else {
                        "U004"
                    },
                    field.name_span,
                    "duplicate nominal field",
                )
                .with_related(previous, "previous field declaration"));
            }
        }
    }
    Ok(())
}
impl NominalDeclaration {
    fn kind_is_record(&self) -> bool {
        self.header.kind == NominalKind::Record
    }
}

fn check_parameter_budget(parameters: &[TypeParameter], span: Span) -> Result<(), Diagnostic> {
    if parameters.len() > 8 || parameters.iter().any(|p| p.bounds.len() > 8) {
        return Err(Diagnostic::new(
            "G002",
            span,
            "generic nominal parameters or bounds exceed their admission limit of 8",
        ));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)] // one bounded declaration lookup context; never an expansion proof
fn resolve_binder(
    programs: &[SourceProgram],
    imports: &[Vec<(String, SourceId)>],
    names: &[HashMap<String, RecordId>],
    source: usize,
    _id: RecordId,
    exported: bool,
    parameters: &[TypeParameter],
    span: Span,
) -> Result<Binder, Diagnostic> {
    if !(1..=8).contains(&parameters.len()) {
        return Err(Diagnostic::new(
            "G002",
            span,
            "a generic declaration requires 1..=8 type parameters",
        ));
    }
    let mut declared = HashMap::new();
    let mut resolved = Vec::new();
    for parameter in parameters {
        if let Some(previous) = declared.insert(&parameter.name, parameter.name_span) {
            return Err(Diagnostic::new(
                "G001",
                parameter.name_span,
                "duplicate type parameter name",
            )
            .with_related(previous, "previous type parameter"));
        }
        if matches!(
            parameter.name.as_str(),
            "i64" | "f64" | "bool" | "Self" | "tensor" | "tensor_error"
        ) || names[source].contains_key(&parameter.name)
            || programs[source]
                .traits
                .iter()
                .any(|t| t.name == parameter.name)
            || programs[source]
                .imports
                .iter()
                .any(|i| i.name == parameter.name)
        {
            return Err(Diagnostic::new(
                "G001",
                parameter.name_span,
                "type parameter shadows a reserved or declared type, trait, or import name",
            ));
        }
        if parameter.bounds.len() > 8 {
            return Err(Diagnostic::new(
                "G002",
                parameter.name_span,
                "a type parameter supports at most 8 trait bounds",
            ));
        }
        let mut bounds = Vec::new();
        for bound in &parameter.bounds {
            let contract = if let Some((alias, name)) = bound.name.split_once("::") {
                imports[source]
                    .iter()
                    .find(|(a, _)| a == alias)
                    .and_then(|(_, target)| programs.get(target.0))
                    .and_then(|p| p.traits.iter().find(|t| t.exported && t.name == name))
            } else {
                programs[source]
                    .traits
                    .iter()
                    .find(|t| t.name == bound.name)
            };
            let Some(contract) = contract else {
                return Err(Diagnostic::new(
                    "TR004",
                    bound.span,
                    format!("unknown or inaccessible trait `{}`", bound.name),
                ));
            };
            if bounds.contains(&contract.id) {
                return Err(Diagnostic::new("G004", bound.span, "duplicate trait bound"));
            }
            if exported && !contract.exported {
                return Err(Diagnostic::new(
                    "G004",
                    bound.span,
                    "exported generic declaration bounds require public traits",
                ));
            }
            bounds.push(contract.id);
        }
        resolved.push(bounds);
    }
    Ok(Binder { bounds: resolved })
}

// Source admission has already validated every arena handle and bounded each
// application DAG. Cache its declaration references once across all fields and
// declarations; a shared phantom DAG must not allocate one graph edge per use.
type DependencySet = [u64; MAX_NOMINAL_LAYOUTS.div_ceil(64)];

fn dependency_edges(
    session: &NominalSession,
) -> Result<HashMap<RecordId, Vec<(RecordId, Span)>>, Diagnostic> {
    fn references(
        session: &NominalSession,
        indices: &HashMap<RecordId, usize>,
        cached: &mut HashMap<TypeApplicationId, DependencySet>,
        ty: TypeExpr,
        span: Span,
    ) -> Result<DependencySet, Diagnostic> {
        let mut found = [0; MAX_NOMINAL_LAYOUTS.div_ceil(64)];
        let declaration = match ty {
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
                id.declaration().ok_or_else(|| invalid_handle(span))?
            }
            TypeExpr::Application(handle) => {
                if let Some(found) = cached.get(&handle) {
                    return Ok(*found);
                }
                let node = session
                    .application(handle)
                    .ok_or_else(|| invalid_handle(span))?;
                for &argument in &node.arguments {
                    let child = references(session, indices, cached, argument, span)?;
                    for (word, child) in found.iter_mut().zip(child) {
                        *word |= child;
                    }
                }
                let index = *indices
                    .get(&node.declaration)
                    .ok_or_else(|| invalid_handle(span))?;
                found[index / 64] |= 1u64 << (index % 64);
                cached.insert(handle, found);
                return Ok(found);
            }
            _ => return Ok(found),
        };
        let index = *indices
            .get(&declaration)
            .ok_or_else(|| invalid_handle(span))?;
        found[index / 64] |= 1u64 << (index % 64);
        Ok(found)
    }

    let indices: HashMap<_, _> = session
        .order
        .iter()
        .enumerate()
        .map(|(i, id)| (*id, i))
        .collect();
    let mut cached = HashMap::new();
    let mut edges: HashMap<RecordId, Vec<(RecordId, Span)>> = HashMap::new();
    for id in &session.order {
        let declaration = &session.declarations[id];
        let mut children = Vec::new();
        let mut seen = [0; MAX_NOMINAL_LAYOUTS.div_ceil(64)];
        for field in declaration
            .fields
            .iter()
            .chain(declaration.variants.iter().flat_map(|v| &v.fields))
        {
            let found = references(session, &indices, &mut cached, field.ty, field.name_span)?;
            for (word_index, found) in found.into_iter().enumerate() {
                let mut new = found & !seen[word_index];
                seen[word_index] |= found;
                while new != 0 {
                    let bit = new.trailing_zeros() as usize;
                    children.push((session.order[word_index * 64 + bit], field.name_span));
                    new &= new - 1;
                }
            }
        }
        edges.insert(*id, children);
    }
    Ok(edges)
}

fn check_cycles(session: &NominalSession) -> Result<(), Diagnostic> {
    let edges = dependency_edges(session)?;
    fn visit(
        id: RecordId,
        session: &NominalSession,
        edges: &HashMap<RecordId, Vec<(RecordId, Span)>>,
        active: &mut HashSet<RecordId>,
        complete: &mut HashSet<RecordId>,
    ) -> Result<(), Diagnostic> {
        if complete.contains(&id) {
            return Ok(());
        }
        active.insert(id);
        for &(child, span) in &edges[&id] {
            if active.contains(&child) {
                return Err(Diagnostic::new(
                    "A001",
                    span,
                    "nominal declaration dependency contains a cycle, including phantom arguments",
                )
                .with_related(
                    session.declarations[&child].header.name_span,
                    "referenced declaration in the cycle",
                ));
            }
            visit(child, session, edges, active, complete)?;
        }
        active.remove(&id);
        complete.insert(id);
        Ok(())
    }
    let mut active = HashSet::new();
    let mut complete = HashSet::new();
    for id in &session.order {
        visit(*id, session, &edges, &mut active, &mut complete)?;
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)] // source arena, module and binder scopes are independent trust boundaries
fn source_type(
    session: &NominalSession,
    program: &SourceProgram,
    source: usize,
    ty: TypeExpr,
    binder: Option<RecordId>,
    any_registered_binder: bool,
    restricted: bool,
    span: Span,
    depth: usize,
) -> Result<(), Diagnostic> {
    match ty {
        TypeExpr::Parameter(id) => {
            if (!any_registered_binder && binder != Some(id.binder)) || session.binders.get(&id.binder).is_none_or(|b| id.index >= b.bounds.len()) {
                return Err(Diagnostic::new("G001", span, "source type uses an undeclared or foreign parameter identity"));
            }
        }
        TypeExpr::Application(id) => {
            if depth >= MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
            let node = program.applications.get(id).ok_or_else(|| invalid_handle(span))?;
            let cache_key = (source, id, binder, any_registered_binder);
            if let Some(height) = session.pool.borrow().source_checks.get(&cache_key).copied() {
                if depth + height > MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
                return Ok(());
            }
            let declaration = session.declarations.get(&node.declaration).ok_or_else(|| invalid_handle(span))?;
            if !session.names[source].values().any(|id| *id == node.declaration) { return Err(Diagnostic::new("I002", span, "source application references an inaccessible nominal declaration")); }
            if declaration.header.kind != node.kind || declaration.parameters.is_empty() || node.arguments.len() != declaration.parameters.len() {
                return Err(Diagnostic::new("G003", span, "nominal application kind or argument count does not match its declaration").with_related(declaration.header.name_span, "nominal declaration"));
            }
            let mut height = 1;
            for &argument in &node.arguments {
                source_type(session, program, source, argument, binder, any_registered_binder, true, span, depth + 1)?;
                if let TypeExpr::Application(child) = argument {
                    let child_height = session.pool.borrow().source_checks[&(source, child, binder, any_registered_binder)];
                    height = height.max(1 + child_height);
                }
            }
            if depth + height > MAX_APPLICATION_DEPTH { return Err(depth_error(span)); }
            session.pool.borrow_mut().source_checks.insert(cache_key, height);
        }
        TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
            let id = id.declaration().ok_or_else(|| invalid_handle(span))?;
            let declaration = session.declarations.get(&id).ok_or_else(|| invalid_handle(span))?;
            if !session.names[source].values().any(|visible| *visible == id) { return Err(Diagnostic::new("I002", span, "source annotation references an inaccessible nominal declaration")); }
            let expected = if matches!(ty, TypeExpr::Concrete(Type::Record(_))) { NominalKind::Record } else { NominalKind::Union };
            if declaration.header.kind != expected { return Err(invalid_handle(span)); }
            if !declaration.parameters.is_empty() { return Err(Diagnostic::new("G003", span, "a generic nominal name requires all explicit arguments")); }
        }
        TypeExpr::Concrete(Type::Array { length, .. }) if restricted => {
            if !(1..=crate::check::MAX_ARRAY_LEN).contains(&length) { return Err(Diagnostic::new("O001", span, "array length must be in 1..=32")); }
        }
        TypeExpr::Concrete(Type::I64 | Type::F64 | Type::Bool | Type::Unit | Type::Array { .. }) => (),
        TypeExpr::Concrete(_) if restricted => return Err(Diagnostic::new("G003", span, "generic fields and arguments require a scalar, unit, parameter, or owned finite inline value")),
        TypeExpr::Concrete(_) => (),
    }
    Ok(())
}

fn preflight(
    session: &NominalSession,
    source: usize,
    program: &SourceProgram,
) -> Result<(), Diagnostic> {
    let span = Span::in_source(SourceId(source), 0, 0);
    if program.imports.len() > crate::project::MAX_PROJECT_EDGES
        || program.functions.len() + program.generic_functions.len() > crate::parser::MAX_FUNCTIONS
        || program.traits.len() > crate::parser::MAX_TRAITS
        || program.implementations.len() > crate::parser::MAX_IMPLEMENTATIONS
    {
        return Err(Diagnostic::new(
            "T016",
            span,
            "source declaration tables exceed their admission limits",
        ));
    }
    // Validate even arena nodes not reached by a function/declaration use.
    for index in 0..program.applications.len() {
        let id = TypeApplicationId {
            owner: program.applications.owner(),
            index: index as u32,
        };
        source_type(
            session,
            program,
            source,
            TypeExpr::Application(id),
            None,
            true,
            true,
            span,
            0,
        )?;
    }
    for declaration in session.declarations.values().filter(|d| d.source == source) {
        let binder = (!declaration.parameters.is_empty()).then_some(declaration.header.id);
        for field in declaration
            .fields
            .iter()
            .chain(declaration.variants.iter().flat_map(|v| &v.fields))
        {
            source_type(
                session,
                program,
                source,
                field.ty,
                binder,
                false,
                true,
                field.name_span,
                0,
            )?;
        }
    }
    for contract in &program.traits {
        if contract.methods.len() > crate::parser::MAX_TRAIT_METHODS {
            return Err(Diagnostic::new(
                "TR002",
                contract.name_span,
                "trait method table exceeds its admission limit",
            ));
        }
        for method in &contract.methods {
            if method.params.len() > crate::check::MAX_PARAMS {
                return Err(Diagnostic::new(
                    "T014",
                    method.name_span,
                    "trait method parameter table exceeds its admission limit",
                ));
            }
            for (ty, span) in method
                .params
                .iter()
                .map(|p| (p.ty, p.name_span))
                .chain(std::iter::once((method.return_type, method.name_span)))
            {
                if let TraitType::Concrete(ty) = ty {
                    source_type(session, program, source, ty, None, false, false, span, 0)?;
                }
            }
        }
    }
    for implementation in &program.implementations {
        if implementation.bindings.len() > crate::parser::MAX_TRAIT_METHODS {
            return Err(Diagnostic::new(
                "TR002",
                implementation.trait_span,
                "implementation method table exceeds its admission limit",
            ));
        }
        source_type(
            session,
            program,
            source,
            implementation.target,
            None,
            false,
            false,
            implementation.trait_span,
            0,
        )?;
    }
    for function in &program.functions {
        preflight_function(session, program, source, function, None)?;
    }
    for template in &program.generic_functions {
        let binder = RecordId {
            source: template.function.name_span.source,
            offset: template.function.name_span.start,
        };
        preflight_function(session, program, source, &template.function, Some(binder))?;
    }
    Ok(())
}

fn preflight_function(
    session: &NominalSession,
    program: &SourceProgram,
    source: usize,
    function: &Function<TypeExpr>,
    binder: Option<RecordId>,
) -> Result<(), Diagnostic> {
    for (ty, span) in function
        .params
        .iter()
        .map(|p| (p.ty, p.name_span))
        .chain(std::iter::once((function.return_type, function.name_span)))
    {
        if function.exported && !session.public_type(ty) {
            return Err(Diagnostic::new(
                "I002",
                span,
                "exported function signatures cannot expose a private nominal type",
            ));
        }
    }
    walk_function(function, &mut |ty, span, restricted| {
        source_type(
            session, program, source, ty, binder, false, restricted, span, 0,
        )
    })
}

/// A borrowed, iterative syntax walk; capacity is checked before extending the
/// traversal stack. This also guards the legacy source-to-concrete AST clone.
fn walk_function(
    function: &Function<TypeExpr>,
    check: &mut impl FnMut(TypeExpr, Span, bool) -> Result<(), Diagnostic>,
) -> Result<(), Diagnostic> {
    if function.params.len() > crate::check::MAX_PARAMS {
        return Err(Diagnostic::new(
            "T014",
            function.name_span,
            "function exceeds 32 parameters",
        ));
    }
    for (ty, span) in function
        .params
        .iter()
        .map(|p| (p.ty, p.name_span))
        .chain(std::iter::once((function.return_type, function.name_span)))
    {
        check(ty, span, false)?;
    }
    enum Node<'a> {
        Block(&'a Block<TypeExpr>),
        Stmt(&'a Stmt<TypeExpr>),
        Expr(&'a Expr),
    }
    let mut pending = vec![(Node::Block(&function.body), 0)];
    let mut nodes = 0;
    while let Some((node, depth)) = pending.pop() {
        nodes += 1;
        if nodes > crate::check::MAX_FUNCTION_NODES {
            return Err(Diagnostic::new(
                "T016",
                function.name_span,
                "function syntax exceeds its node admission limit",
            ));
        }
        match node {
            Node::Block(block) => {
                stack_room(nodes, pending.len(), block.stmts.len(), function.name_span)?;
                pending.extend(block.stmts.iter().rev().map(|s| (Node::Stmt(s), depth + 1)));
            }
            Node::Stmt(statement) => {
                use StmtKind as S;
                match &statement.kind {
                    S::Let {
                        ty,
                        name_span,
                        value,
                        ..
                    } => {
                        check(*ty, *name_span, false)?;
                        pending.push((Node::Expr(value), depth + 1));
                    }
                    S::Assign { value, .. }
                    | S::FieldAssign { value, .. }
                    | S::Return(value)
                    | S::Print(value)
                    | S::Expr(value) => pending.push((Node::Expr(value), depth + 1)),
                    S::IndexAssign { index, value, .. } => {
                        pending.push((Node::Expr(value), depth + 1));
                        pending.push((Node::Expr(index), depth + 1));
                    }
                    S::Match { arms, .. } => {
                        if arms.len() > crate::parser::MAX_UNION_VARIANTS {
                            return Err(Diagnostic::new(
                                "U001",
                                statement.span,
                                "match arm table exceeds its admission limit",
                            ));
                        }
                        for arm in arms.iter().rev() {
                            if arm.fields.len() > crate::parser::MAX_RECORD_FIELDS {
                                return Err(Diagnostic::new(
                                    "U002",
                                    arm.variant_span,
                                    "match pattern fields exceed their admission limit",
                                ));
                            }
                            nodes += 1 + arm.fields.len();
                            if let Some(ty) = arm.application {
                                check(ty, arm.union_span, true)?;
                            }
                            pending.push((Node::Block(&arm.body), depth + 1));
                        }
                    }
                    S::If {
                        condition,
                        then_block,
                        else_block,
                    } => {
                        if let Some(block) = else_block {
                            pending.push((Node::Block(block), depth + 1));
                        }
                        pending.push((Node::Block(then_block), depth + 1));
                        pending.push((Node::Expr(condition), depth + 1));
                    }
                    S::While { condition, body } => {
                        pending.push((Node::Block(body), depth + 1));
                        pending.push((Node::Expr(condition), depth + 1));
                    }
                    S::Block(block) => pending.push((Node::Block(block), depth + 1)),
                    S::TensorScope {
                        budget,
                        body,
                        handler,
                        ..
                    } => {
                        pending.push((Node::Block(handler), depth + 1));
                        pending.push((Node::Block(body), depth + 1));
                        pending.push((Node::Expr(budget), depth + 1));
                    }
                    S::Break | S::Continue => (),
                }
            }
            Node::Expr(expression) => {
                use ExprKind as E;
                match &expression.kind {
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
                        if fields.len() > crate::parser::MAX_RECORD_FIELDS {
                            return Err(Diagnostic::new(
                                "R002",
                                expression.span,
                                "constructor fields exceed their admission limit",
                            ));
                        }
                        if let Some(ty) = application {
                            check(*ty, expression.span, true)?;
                        }
                        stack_room(nodes, pending.len(), fields.len(), function.name_span)?;
                        pending.extend(
                            fields
                                .iter()
                                .rev()
                                .map(|f| (Node::Expr(&f.value), depth + 1)),
                        );
                    }
                    E::GenericCall {
                        type_args, args, ..
                    } => {
                        if type_args.len() > 8 {
                            return Err(Diagnostic::new(
                                "G002",
                                expression.span,
                                "explicit function type arguments exceed limit 8",
                            ));
                        }
                        for &ty in type_args {
                            check(ty, expression.span, true)?;
                        }
                        stack_room(nodes, pending.len(), args.len(), function.name_span)?;
                        pending.extend(args.iter().rev().map(|a| (Node::Expr(a), depth + 1)));
                    }
                    E::Call { args, .. }
                    | E::Intrinsic { args, .. }
                    | E::TensorCall { args, .. }
                    | E::Array(args) => {
                        stack_room(nodes, pending.len(), args.len(), function.name_span)?;
                        pending.extend(args.iter().rev().map(|a| (Node::Expr(a), depth + 1)));
                    }
                    E::Unary { value, .. } | E::Try(value) => {
                        pending.push((Node::Expr(value), depth + 1))
                    }
                    E::Binary { left, right, .. } => {
                        pending.push((Node::Expr(right), depth + 1));
                        pending.push((Node::Expr(left), depth + 1));
                    }
                    E::Borrow {
                        range: Some((start, end)),
                        ..
                    } => {
                        pending.push((Node::Expr(end), depth + 1));
                        pending.push((Node::Expr(start), depth + 1));
                    }
                    E::Index { index, .. } => pending.push((Node::Expr(index), depth + 1)),
                    _ => (),
                }
            }
        }
        if nodes + pending.len() > crate::check::MAX_FUNCTION_NODES {
            return Err(Diagnostic::new(
                "T016",
                function.name_span,
                "function exceeds 2048 source syntax nodes",
            ));
        }
    }
    Ok(())
}

fn stack_room(
    nodes: usize,
    pending: usize,
    additional: usize,
    span: Span,
) -> Result<(), Diagnostic> {
    if nodes
        .checked_add(pending)
        .and_then(|n| n.checked_add(additional))
        .is_none_or(|n| n > crate::check::MAX_FUNCTION_NODES)
    {
        Err(Diagnostic::new(
            "T016",
            span,
            "function exceeds 2048 source syntax nodes",
        ))
    } else {
        Ok(())
    }
}

/// Validate the source ownership and bounded syntax representation without
/// cloning AST bodies, normalizing nominal types, or granting semantic proof.
pub(crate) fn preflight_sources(
    programs: &[SourceProgram],
    limit: usize,
) -> Result<(), Vec<Diagnostic>> {
    admission(programs).map_err(|error| {
        let mut errors = vec![error];
        errors.truncate(limit.max(1));
        errors
    })
}
fn admission(programs: &[SourceProgram]) -> Result<(), Diagnostic> {
    if programs.len() > crate::project::MAX_PROJECT_FILES {
        return Err(Diagnostic::new(
            "G002",
            Span::default(),
            "source project exceeds 64 modules",
        ));
    }
    let mut declarations = HashMap::new();
    let mut binders = HashMap::new();
    let mut application_nodes = 0usize;
    for (source, program) in programs.iter().enumerate() {
        let span = Span::in_source(SourceId(source), 0, 0);
        if program.functions.len() + program.generic_functions.len() > crate::parser::MAX_FUNCTIONS
            || program.generic_functions.len() > 128
            || program.imports.len() > crate::project::MAX_PROJECT_EDGES
            || program.traits.len() > crate::parser::MAX_TRAITS
            || program.implementations.len() > crate::parser::MAX_IMPLEMENTATIONS
        {
            return Err(Diagnostic::new(
                "T016",
                span,
                "source declaration tables exceed their admission limits",
            ));
        }
        application_nodes = application_nodes
            .checked_add(program.applications.len())
            .filter(|n| *n <= MAX_NOMINAL_NODES)
            .ok_or_else(|| {
                Diagnostic::new(
                    "G002",
                    span,
                    "source application arenas exceed 131072 nodes",
                )
            })?;
        let records = program
            .records
            .iter()
            .map(|r| (r.id, NominalKind::Record, 0, r.name_span, &r.fields));
        let generic_records = program.generic_records.iter().map(|r| {
            (
                r.record.id,
                NominalKind::Record,
                r.parameters.len(),
                r.record.name_span,
                &r.record.fields,
            )
        });
        for (id, kind, arity, span, fields) in records.chain(generic_records) {
            if fields.len() > crate::parser::MAX_RECORD_FIELDS {
                return Err(Diagnostic::new(
                    "R002",
                    span,
                    "record fields exceed admission limit 32",
                ));
            }
            admit_header(&mut declarations, id, kind, arity, span)?;
        }
        let unions = program
            .unions
            .iter()
            .map(|u| (u.id, 0, u.name_span, &u.variants));
        let generic_unions = program.generic_unions.iter().map(|u| {
            (
                u.union.id,
                u.parameters.len(),
                u.union.name_span,
                &u.union.variants,
            )
        });
        for (id, arity, span, variants) in unions.chain(generic_unions) {
            if variants.len() > crate::parser::MAX_UNION_VARIANTS
                || variants
                    .iter()
                    .any(|v| v.fields.len() > crate::parser::MAX_RECORD_FIELDS)
            {
                return Err(Diagnostic::new(
                    "U002",
                    span,
                    "union variants or fields exceed admission limit 32",
                ));
            }
            admit_header(&mut declarations, id, NominalKind::Union, arity, span)?;
        }
        for record in &program.generic_records {
            check_parameter_budget(&record.parameters, record.record.name_span)?;
            let id = record
                .record
                .id
                .declaration()
                .ok_or_else(|| invalid_handle(record.record.name_span))?;
            if record.parameters.is_empty() {
                return Err(Diagnostic::new(
                    "G002",
                    record.record.name_span,
                    "generic record requires type parameters",
                ));
            }
            if binders.insert(id, record.parameters.len()).is_some() {
                return Err(invalid_handle(record.record.name_span));
            }
        }
        for union in &program.generic_unions {
            check_parameter_budget(&union.parameters, union.union.name_span)?;
            let id = union
                .union
                .id
                .declaration()
                .ok_or_else(|| invalid_handle(union.union.name_span))?;
            if union.parameters.is_empty() {
                return Err(Diagnostic::new(
                    "G002",
                    union.union.name_span,
                    "generic union requires type parameters",
                ));
            }
            if binders.insert(id, union.parameters.len()).is_some() {
                return Err(invalid_handle(union.union.name_span));
            }
        }
        for template in &program.generic_functions {
            check_parameter_budget(&template.parameters, template.function.name_span)?;
            if template.parameters.is_empty() {
                return Err(Diagnostic::new(
                    "G002",
                    template.function.name_span,
                    "generic function requires type parameters",
                ));
            }
            let id = RecordId {
                source: template.function.name_span.source,
                offset: template.function.name_span.start,
            };
            if binders.insert(id, template.parameters.len()).is_some() {
                return Err(invalid_handle(template.function.name_span));
            }
        }
    }
    for program in programs {
        let mut context = SourceAdmission {
            program,
            declarations: &declarations,
            binders: &binders,
            heights: HashMap::new(),
        };
        for index in 0..program.applications.len() {
            context.ty(
                TypeExpr::Application(TypeApplicationId {
                    owner: program.applications.owner(),
                    index: index as u32,
                }),
                None,
                true,
                Span::default(),
                0,
            )?;
        }
        for record in &program.records {
            for field in &record.fields {
                context.ty(field.ty, None, false, field.name_span, 0)?;
            }
        }
        for union in &program.unions {
            for field in union.variants.iter().flat_map(|v| &v.fields) {
                context.ty(field.ty, None, false, field.name_span, 0)?;
            }
        }
        for template in &program.generic_records {
            let binder = template.record.id.declaration();
            for field in &template.record.fields {
                context.ty(field.ty, binder, false, field.name_span, 0)?;
            }
        }
        for template in &program.generic_unions {
            let binder = template.union.id.declaration();
            for field in template.union.variants.iter().flat_map(|v| &v.fields) {
                context.ty(field.ty, binder, false, field.name_span, 0)?;
            }
        }
        for contract in &program.traits {
            if contract.methods.len() > crate::parser::MAX_TRAIT_METHODS {
                return Err(Diagnostic::new(
                    "TR002",
                    contract.name_span,
                    "trait methods exceed admission limit 32",
                ));
            }
            for method in &contract.methods {
                if method.params.len() > crate::check::MAX_PARAMS {
                    return Err(Diagnostic::new(
                        "T014",
                        method.name_span,
                        "trait parameters exceed admission limit 32",
                    ));
                }
                for (ty, span) in method
                    .params
                    .iter()
                    .map(|p| (p.ty, p.name_span))
                    .chain(std::iter::once((method.return_type, method.name_span)))
                {
                    if let TraitType::Concrete(ty) = ty {
                        context.ty(ty, None, false, span, 0)?;
                    }
                }
            }
        }
        for implementation in &program.implementations {
            if implementation.bindings.len() > crate::parser::MAX_TRAIT_METHODS {
                return Err(Diagnostic::new(
                    "TR002",
                    implementation.trait_span,
                    "implementation methods exceed admission limit 32",
                ));
            }
            context.ty(
                implementation.target,
                None,
                false,
                implementation.trait_span,
                0,
            )?;
        }
        for function in &program.functions {
            walk_function(function, &mut |ty, span, _| {
                context.ty(ty, None, false, span, 0).map(|_| ())
            })?;
        }
        for template in &program.generic_functions {
            let binder = RecordId {
                source: template.function.name_span.source,
                offset: template.function.name_span.start,
            };
            walk_function(&template.function, &mut |ty, span, _| {
                context.ty(ty, Some(binder), false, span, 0).map(|_| ())
            })?;
        }
    }
    Ok(())
}
fn admit_header(
    declarations: &mut HashMap<RecordId, (NominalKind, usize)>,
    id: NominalId,
    kind: NominalKind,
    arity: usize,
    span: Span,
) -> Result<(), Diagnostic> {
    let id = id.declaration().ok_or_else(|| invalid_handle(span))?;
    if declarations.len() >= MAX_NOMINAL_LAYOUTS {
        return Err(Diagnostic::new(
            "R001",
            span,
            "source project exceeds 256 nominal declarations",
        ));
    }
    if declarations.insert(id, (kind, arity)).is_some() {
        return Err(invalid_handle(span));
    }
    Ok(())
}
struct SourceAdmission<'a> {
    program: &'a SourceProgram,
    declarations: &'a HashMap<RecordId, (NominalKind, usize)>,
    binders: &'a HashMap<RecordId, usize>,
    heights: HashMap<(TypeApplicationId, Option<RecordId>, bool), usize>,
}
impl SourceAdmission<'_> {
    fn ty(
        &mut self,
        ty: TypeExpr,
        binder: Option<RecordId>,
        any_binder: bool,
        span: Span,
        depth: usize,
    ) -> Result<usize, Diagnostic> {
        match ty {
            TypeExpr::Parameter(id) => {
                if (!any_binder && Some(id.binder) != binder)
                    || self.binders.get(&id.binder).is_none_or(|n| id.index >= *n)
                {
                    return Err(Diagnostic::new(
                        "G001",
                        span,
                        "source annotation has an undeclared or foreign parameter identity",
                    ));
                }
                Ok(0)
            }
            TypeExpr::Application(id) => {
                if depth >= MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                let node = self
                    .program
                    .applications
                    .get(id)
                    .ok_or_else(|| invalid_handle(span))?;
                let key = (id, binder, any_binder);
                if let Some(&height) = self.heights.get(&key) {
                    if depth + height > MAX_APPLICATION_DEPTH {
                        return Err(depth_error(span));
                    }
                    return Ok(height);
                }
                if self.declarations.get(&node.declaration)
                    != Some(&(node.kind, node.arguments.len()))
                    || node.arguments.is_empty()
                    || node.arguments.len() > 8
                {
                    return Err(Diagnostic::new(
                        "G003",
                        span,
                        "source nominal application kind or arity does not match its declaration",
                    ));
                }
                let arguments = node.arguments.to_vec();
                let mut height = 1;
                for argument in arguments {
                    height =
                        height.max(1 + self.ty(argument, binder, any_binder, span, depth + 1)?);
                }
                if depth + height > MAX_APPLICATION_DEPTH {
                    return Err(depth_error(span));
                }
                self.heights.insert(key, height);
                Ok(height)
            }
            TypeExpr::Concrete(Type::Record(id) | Type::Union(id)) => {
                let id = id.declaration().ok_or_else(|| invalid_handle(span))?;
                let expected = if matches!(ty, TypeExpr::Concrete(Type::Record(_))) {
                    NominalKind::Record
                } else {
                    NominalKind::Union
                };
                if !self
                    .declarations
                    .get(&id)
                    .is_some_and(|(kind, _)| *kind == expected)
                {
                    return Err(invalid_handle(span));
                }
                Ok(0)
            }
            TypeExpr::Concrete(_) => Ok(0),
        }
    }
}

fn normalize_function(
    session: &NominalSession,
    function: &mut Function<TypeExpr>,
    binder: Option<RecordId>,
) -> Result<(), Diagnostic> {
    for param in &mut function.params {
        param.ty = session.normalize(param.ty, binder, param.name_span)?;
    }
    function.return_type = session.normalize(function.return_type, binder, function.name_span)?;
    normalize_block(session, &mut function.body, binder)
}
fn normalize_block(
    session: &NominalSession,
    block: &mut Block<TypeExpr>,
    binder: Option<RecordId>,
) -> Result<(), Diagnostic> {
    use StmtKind as S;
    for statement in &mut block.stmts {
        match &mut statement.kind {
            S::Let {
                ty,
                name_span,
                value,
                ..
            } => {
                *ty = session.normalize(*ty, binder, *name_span)?;
                normalize_expr(session, value, binder)?;
            }
            S::Assign { value, .. }
            | S::FieldAssign { value, .. }
            | S::Return(value)
            | S::Print(value)
            | S::Expr(value) => normalize_expr(session, value, binder)?,
            S::IndexAssign { index, value, .. } => {
                normalize_expr(session, index, binder)?;
                normalize_expr(session, value, binder)?;
            }
            S::Match { arms, .. } => {
                for arm in arms {
                    if let Some(ty) = &mut arm.application {
                        *ty = session.normalize(*ty, binder, arm.union_span)?;
                    }
                    normalize_block(session, &mut arm.body, binder)?;
                }
            }
            S::If {
                condition,
                then_block,
                else_block,
            } => {
                normalize_expr(session, condition, binder)?;
                normalize_block(session, then_block, binder)?;
                if let Some(block) = else_block {
                    normalize_block(session, block, binder)?;
                }
            }
            S::While { condition, body } => {
                normalize_expr(session, condition, binder)?;
                normalize_block(session, body, binder)?;
            }
            S::Block(block) => normalize_block(session, block, binder)?,
            S::TensorScope {
                budget,
                body,
                handler,
                ..
            } => {
                normalize_expr(session, budget, binder)?;
                normalize_block(session, body, binder)?;
                normalize_block(session, handler, binder)?;
            }
            S::Break | S::Continue => (),
        }
    }
    Ok(())
}
fn normalize_expr(
    session: &NominalSession,
    expression: &mut Expr,
    binder: Option<RecordId>,
) -> Result<(), Diagnostic> {
    use ExprKind as E;
    match &mut expression.kind {
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
                *ty = session.normalize(*ty, binder, expression.span)?;
            }
            for field in fields {
                normalize_expr(session, &mut field.value, binder)?;
            }
        }
        E::GenericCall {
            type_args, args, ..
        } => {
            for ty in type_args {
                *ty = session.normalize(*ty, binder, expression.span)?;
            }
            for argument in args {
                normalize_expr(session, argument, binder)?;
            }
        }
        E::Call { args, .. }
        | E::Intrinsic { args, .. }
        | E::TensorCall { args, .. }
        | E::Array(args) => {
            for argument in args {
                normalize_expr(session, argument, binder)?;
            }
        }
        E::Unary { value, .. } | E::Try(value) => normalize_expr(session, value, binder)?,
        E::Binary { left, right, .. } => {
            normalize_expr(session, left, binder)?;
            normalize_expr(session, right, binder)?;
        }
        E::Borrow {
            range: Some((start, end)),
            ..
        } => {
            normalize_expr(session, start, binder)?;
            normalize_expr(session, end, binder)?;
        }
        E::Index { index, .. } => normalize_expr(session, index, binder)?,
        _ => (),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn prepared(source: &str) -> Prepared {
        let program = crate::parser::parse_source(source).unwrap();
        prepare_sources(&[program], &[Vec::new()], 256).unwrap()
    }

    #[test]
    fn immutable_source_arenas_validate_owner_and_range() {
        let declaration = RecordId {
            source: SourceId(0),
            offset: 1,
        };
        let mut builder = TypeApplicationsBuilder::new();
        let handle = builder
            .insert(TypeApplication {
                declaration,
                kind: NominalKind::Record,
                arguments: vec![TypeExpr::I64].into_boxed_slice(),
            })
            .unwrap();
        let arena = builder.finish();
        let shared = arena.clone();
        assert_eq!(arena.get(handle), shared.get(handle));
        let other = TypeApplicationsBuilder::new().finish();
        assert!(other.get(handle).is_none());
        assert!(arena
            .get(TypeApplicationId {
                index: u32::MAX,
                ..handle
            })
            .is_none());
    }

    #[test]
    fn direct_shape_queries_reject_mismatched_nominal_tags() {
        let prepared = prepared("record Plain{value:i64}union Flag{Empty{}}record Cell<T>{value:T}union Choice<T>{Present{value:T},Empty{}}fn main()->i64{return 0;}");
        let span = Span::default();
        let record = prepared.programs[0].records[0].id;
        let union = prepared.programs[0].unions[0].id;
        assert_eq!(
            prepared
                .session
                .shape(Type::Record(record).into(), span)
                .unwrap()
                .kind,
            NominalKind::Record
        );
        assert_eq!(
            prepared
                .session
                .shape(Type::Union(union).into(), span)
                .unwrap()
                .kind,
            NominalKind::Union
        );
        for wrong in [Type::Union(record), Type::Record(union)] {
            assert_eq!(
                prepared.session.shape(wrong.into(), span).unwrap_err().code,
                "G007"
            );
        }
        prepared.session.validate_contracts(&[], 256).unwrap();
        let cell = prepared
            .session
            .apply(0, "Cell", &[TypeExpr::I64], None, span)
            .unwrap();
        let choice = prepared
            .session
            .apply(0, "Choice", &[TypeExpr::I64], None, span)
            .unwrap();
        let Some(Type::Record(cell_id)) = cell.concrete() else {
            panic!("record instance")
        };
        let Some(Type::Union(choice_id)) = choice.concrete() else {
            panic!("union instance")
        };
        for wrong in [Type::Union(cell_id), Type::Record(choice_id)] {
            assert_eq!(
                prepared.session.shape(wrong.into(), span).unwrap_err().code,
                "G007"
            );
        }
        assert_eq!(
            prepared
                .session
                .shape(TypeExpr::I64, span)
                .unwrap_err()
                .code,
            "R010"
        );
    }

    #[test]
    fn expansion_budget_accepts_exact_limit_and_memoized_reuse() {
        let span = Span::default();
        let mut counter = Pool::new();
        counter.charge(MAX_NOMINAL_NODES, span).unwrap();
        assert_eq!(counter.nodes, 131072);
        assert_eq!(counter.charge(1, span).unwrap_err().code, "G002");
        assert_eq!(counter.nodes, 131072);

        let prepared = prepared("record Cell<T>{value:T}fn main()->i64{return 0;}");
        prepared.session.validate_contracts(&[], 256).unwrap();
        let owner = prepared
            .session
            .apply(0, "Cell", &[TypeExpr::I64], None, span)
            .unwrap();
        let definitions = prepared.session.concrete_definitions().0.len();
        // Model the remaining budget consumed by other admitted applications.
        prepared.session.pool.borrow_mut().nodes = MAX_NOMINAL_NODES;
        assert_eq!(
            prepared
                .session
                .apply(0, "Cell", &[TypeExpr::I64], None, span)
                .unwrap(),
            owner
        );
        assert_eq!(prepared.session.pool.borrow().nodes, 131072);
        assert_eq!(
            prepared
                .session
                .apply(0, "Cell", &[TypeExpr::Bool], None, span)
                .unwrap_err()
                .code,
            "G002"
        );
        assert_eq!(prepared.session.pool.borrow().nodes, 131072);
        assert_eq!(prepared.session.concrete_definitions().0.len(), definitions);
    }

    #[test]
    fn failed_field_expansion_never_becomes_a_successful_key_hit() {
        for symbolic in [false, true] {
            let prepared = prepared("record Cell<T>{value:T}fn identity<U>(value:U)->U{return value;}fn main()->i64{return 0;}");
            prepared.session.validate_contracts(&[], 256).unwrap();
            let span = Span::default();
            let function = &prepared.programs[0].generic_functions[0].function;
            let binder = RecordId {
                source: function.name_span.source,
                offset: function.name_span.start,
            };
            let argument = if symbolic {
                function.params[0].ty
            } else {
                TypeExpr::Bool
            };
            let caller = symbolic.then_some(binder);
            let declaration = prepared.session.visible_declaration(0, "Cell").unwrap();
            // Reserving the new key fits; its first field descriptor does not.
            prepared.session.pool.borrow_mut().nodes = MAX_NOMINAL_NODES - 1;
            let error = prepared
                .session
                .apply(0, "Cell", &[argument], caller, span)
                .unwrap_err();
            assert_eq!(error.code, "G002");
            assert_eq!(
                prepared
                    .session
                    .apply(0, "Cell", &[argument], caller, span)
                    .unwrap_err(),
                error
            );
            let owner = prepared.session.pool.borrow().keys[&TypeApplication {
                declaration,
                kind: NominalKind::Record,
                arguments: vec![argument].into_boxed_slice(),
            }];
            assert_eq!(prepared.session.shape(owner, span).unwrap_err(), error);
            assert!(!prepared.session.owns_type(owner, caller));
            assert!(prepared.session.kind(owner).is_none());
            assert_eq!(prepared.session.pool.borrow().nodes, 131072);
            assert!(prepared.session.concrete_definitions().0.is_empty());
            if let Some(Type::Record(id)) = owner.concrete() {
                assert!(prepared.session.origin(id).is_none());
            }
        }
    }

    #[test]
    fn failed_physical_layout_never_becomes_a_successful_key_hit() {
        let prepared = prepared(
            "record Wide<T>{a:T,b:T,c:T,d:T,e:T,f:T,g:T,h:T,i:T}fn main()->i64{return 0;}",
        );
        prepared.session.validate_contracts(&[], 256).unwrap();
        let span = Span::default();
        let argument = TypeExpr::Concrete(Type::Array {
            element: ElementType::I64,
            length: 32,
        });
        let error = prepared
            .session
            .apply(0, "Wide", &[argument], None, span)
            .unwrap_err();
        assert_eq!(error.code, "A003");
        let charged = prepared.session.pool.borrow().nodes;
        assert_eq!(
            prepared
                .session
                .apply(0, "Wide", &[argument], None, span)
                .unwrap_err(),
            error
        );
        assert_eq!(prepared.session.pool.borrow().nodes, charged);
        assert!(prepared.session.concrete_definitions().0.is_empty());
        let owner = TypeExpr::Concrete(Type::Record(NominalId::Instance(0)));
        assert_eq!(prepared.session.shape(owner, span).unwrap_err(), error);
        assert_eq!(
            prepared.session.lower_bound(owner, span).unwrap_err(),
            error
        );
        assert!(!prepared.session.owns_type(owner, None));
        assert!(prepared.session.origin(NominalId::Instance(0)).is_none());
    }

    #[test]
    fn dependency_graph_deduplicates_shared_dags_and_retains_phantom_cycles() {
        fn dag(
            builder: &mut TypeApplicationsBuilder,
            declaration: RecordId,
            depth: usize,
        ) -> TypeExpr {
            if depth == 0 {
                return TypeExpr::I64;
            }
            let arguments = (0..8)
                .map(|_| dag(builder, declaration, depth - 1))
                .collect::<Vec<_>>();
            TypeExpr::Application(
                builder
                    .insert(TypeApplication {
                        declaration,
                        kind: NominalKind::Record,
                        arguments: arguments.into_boxed_slice(),
                    })
                    .unwrap(),
            )
        }

        let fields = (0..32)
            .map(|i| format!("f{i}:Phantom<i64,i64,i64,i64,i64,i64,i64,i64>"))
            .collect::<Vec<_>>()
            .join(",");
        let source = format!("record Phantom<A,B,C,D,E,F,G,H>{{value:i64}}record Holder{{{fields}}}fn main()->i64{{return 0;}}");
        let mut program = crate::parser::parse_source(&source).unwrap();
        let phantom = declaration_record(
            0,
            &program.generic_records[0].record,
            &program.generic_records[0].parameters,
        )
        .unwrap();
        let phantom_id = phantom.header.id;
        let holder_id = program.records[0].id.declaration().unwrap();
        let mut builder = TypeApplicationsBuilder::new();
        let root = dag(&mut builder, phantom_id, 4);
        // This use is admitted as a finite type expression. Once placed in a
        // Holder field, its phantom argument still creates a declaration cycle.
        let mut arguments = vec![TypeExpr::I64; 8];
        arguments[0] = TypeExpr::Concrete(Type::Record(holder_id.into()));
        let phantom_cycle = TypeExpr::Application(
            builder
                .insert(TypeApplication {
                    declaration: phantom_id,
                    kind: NominalKind::Record,
                    arguments: arguments.into_boxed_slice(),
                })
                .unwrap(),
        );
        program.applications = builder.finish();
        assert_eq!(program.applications.len(), 586);
        for field in &mut program.records[0].fields {
            field.ty = root;
        }
        preflight_sources(std::slice::from_ref(&program), 256).unwrap();
        let holder = declaration_record(0, &program.records[0], &[]).unwrap();
        let first_field = holder.fields[0].name_span;
        let holder_span = holder.header.name_span;
        let mut session = NominalSession {
            declarations: [(phantom_id, phantom), (holder_id, holder)]
                .into_iter()
                .collect(),
            order: vec![phantom_id, holder_id],
            names: vec![HashMap::new()],
            binders: HashMap::new(),
            arenas: [(program.applications.owner(), program.applications.clone())]
                .into_iter()
                .collect(),
            ordinary_count: 1,
            new_content: true,
            pool: RefCell::new(Pool::new()),
        };
        let edges = dependency_edges(&session).unwrap();
        assert_eq!(edges[&holder_id], vec![(phantom_id, first_field)]);
        assert!(edges[&phantom_id].is_empty());
        check_cycles(&session).unwrap();

        session.declarations.get_mut(&holder_id).unwrap().fields[0].ty = phantom_cycle;
        let error = check_cycles(&session).unwrap_err();
        assert_eq!(error.code, "A001");
        assert_eq!(error.span, first_field);
        assert!(error.related.iter().any(|r| r.span == holder_span));
        program.records[0].fields[0].ty = phantom_cycle;
        let Err(errors) = prepare_sources(&[program], &[Vec::new()], 256) else {
            panic!("phantom declaration cycle")
        };
        assert_eq!(errors[0].code, "A001");
    }

    #[test]
    fn source_guard_rejects_foreign_handles_and_phantom_generated_ids() {
        let mut program = crate::parser::parse_source("record Cell<T>{value:T}fn main()->i64{let c:Cell<i64> = Cell::<i64>{value:1};return c.value;}").unwrap();
        let StmtKind::Let { ty, .. } = &mut program.functions[0].body.stmts[0].kind else {
            panic!("local")
        };
        let TypeExpr::Application(handle) = *ty else {
            panic!("application")
        };
        *ty = TypeExpr::Application(TypeApplicationId {
            owner: handle.owner + 1,
            ..handle
        });
        assert_eq!(
            preflight_sources(&[program], 256).unwrap_err()[0].code,
            "G007"
        );

        let mut program =
            crate::parser::parse_source("record Phantom<T>{value:i64}fn main()->i64{return 0;}")
                .unwrap();
        let declaration = program.generic_records[0].record.id.declaration().unwrap();
        let mut builder = TypeApplicationsBuilder::new();
        builder
            .insert(TypeApplication {
                declaration,
                kind: NominalKind::Record,
                arguments: vec![TypeExpr::Concrete(Type::Record(NominalId::Instance(0)))]
                    .into_boxed_slice(),
            })
            .unwrap();
        program.applications = builder.finish();
        assert_eq!(
            preflight_sources(&[program], 256).unwrap_err()[0].code,
            "G007"
        );
    }

    #[test]
    fn syntax_node_boundary_excludes_function_parameters() {
        let mut source = String::from("fn f(x:i64)->i64{");
        source.push_str(&"print(0);".repeat(1021));
        source.push_str("if true{}return x;}fn main()->i64{return f(1);}");
        let mut program = crate::parser::parse_source(&source).unwrap();
        preflight_sources(std::slice::from_ref(&program), 256).unwrap();
        crate::check::check_source(&program).unwrap();
        let body = &mut program.functions[0].body;
        body.stmts.push(Stmt {
            kind: StmtKind::Break,
            span: body.span,
        });
        assert_eq!(
            preflight_sources(&[program], 256).unwrap_err()[0].code,
            "T016"
        );
    }

    #[test]
    fn deeply_nested_source_api_input_is_bounded_before_cloning() {
        let mut program = crate::parser::parse_source("fn main()->i64{return 0;}").unwrap();
        let mut body = std::mem::replace(
            &mut program.functions[0].body,
            Block {
                stmts: Vec::new(),
                span: Span::default(),
            },
        );
        for _ in 0..crate::check::MAX_FUNCTION_NODES {
            body = Block {
                stmts: vec![Stmt {
                    kind: StmtKind::Block(body),
                    span: Span::default(),
                }],
                span: Span::default(),
            };
        }
        program.functions[0].body = body;
        assert_eq!(
            preflight_sources(std::slice::from_ref(&program), 256).unwrap_err()[0].code,
            "T016"
        );
        assert_eq!(
            crate::check::check_source(&program).unwrap_err()[0].code,
            "T016"
        );
        // Dispose this intentionally unparseable AST iteratively as well.
        let mut body = program.functions.pop().unwrap().body;
        while let Some(statement) = body.stmts.pop() {
            if let StmtKind::Block(child) = statement.kind {
                body = child;
            }
        }
    }

    #[test]
    fn symbolic_fields_preserve_exact_binder_and_substitute_to_one_instance() {
        let prepared = prepared("record Cell<T>{value:T}fn unwrap<T>(c:Cell<T>)->T{return c.value;}fn main()->i64{return 0;}");
        let function = &prepared.programs[0].generic_functions[0].function;
        let binder = RecordId {
            source: function.name_span.source,
            offset: function.name_span.start,
        };
        let owner = function.params[0].ty;
        let shape = prepared.session.shape(owner, function.name_span).unwrap();
        assert_eq!(shape.fields[0].ty, function.return_type);
        assert_eq!(
            shape.fields[0].ty,
            TypeExpr::Parameter(TypeParameterId { binder, index: 0 })
        );
        prepared.session.validate_contracts(&[], 256).unwrap();
        let first = prepared
            .session
            .substitute(owner, binder, &[TypeExpr::I64], None, function.name_span)
            .unwrap();
        let second = prepared
            .session
            .apply(0, "Cell", &[TypeExpr::I64], None, function.name_span)
            .unwrap();
        assert_eq!(first, second);
        assert!(matches!(
            first,
            TypeExpr::Concrete(Type::Record(NominalId::Instance(_)))
        ));
        assert_eq!(
            prepared
                .session
                .shape(first, function.name_span)
                .unwrap()
                .fields[0]
                .ty,
            TypeExpr::I64
        );
        assert!(!prepared.session.owns_type(owner, None));
        assert!(prepared.session.owns_type(owner, Some(binder)));
    }

    #[test]
    fn concrete_bounds_wait_for_coherence_and_remain_enforced_afterwards() {
        let prepared = prepared("trait Mark{fn mark(x:Self)->i64;}record Cell<T:Mark>{value:T}fn marker(x:i64)->i64{return x;}impl Mark for i64{mark=marker;}fn main()->i64{let c:Cell<i64> = Cell::<i64>{value:1};return c.value;}");
        let contract = prepared.programs[0].traits[0].id;
        assert!(!prepared.session.entails(TypeExpr::I64, contract, None));
        assert_eq!(
            prepared.session.validate_contracts(&[], 256).unwrap_err()[0].code,
            "G004"
        );
        prepared
            .session
            .validate_contracts(&[(contract, Type::I64)], 256)
            .unwrap();
        assert!(prepared.session.entails(TypeExpr::I64, contract, None));
        assert_eq!(
            prepared
                .session
                .apply(0, "Cell", &[TypeExpr::Bool], None, Span::default())
                .unwrap_err()
                .code,
            "G004"
        );
    }

    #[test]
    fn shared_phantom_argument_dag_keeps_depth_and_bounded_normalization() {
        fn source(depth: usize) -> SourceProgram {
            let mut program = crate::parser::parse_source("record Phantom<A,B,C,D,E,F,G,H>{value:i64}fn main()->i64{let p:Phantom<i64,i64,i64,i64,i64,i64,i64,i64> = Phantom::<i64,i64,i64,i64,i64,i64,i64,i64>{value:1};return p.value;}").unwrap();
            let declaration = program.generic_records[0].record.id.declaration().unwrap();
            let mut builder = TypeApplicationsBuilder::new();
            let mut ty = TypeExpr::I64;
            for _ in 0..depth {
                ty = TypeExpr::Application(
                    builder
                        .insert(TypeApplication {
                            declaration,
                            kind: NominalKind::Record,
                            arguments: vec![ty; 8].into_boxed_slice(),
                        })
                        .unwrap(),
                );
            }
            program.applications = builder.finish();
            let StmtKind::Let {
                ty: annotation,
                value,
                ..
            } = &mut program.functions[0].body.stmts[0].kind
            else {
                panic!("local")
            };
            *annotation = ty;
            let ExprKind::Record { application, .. } = &mut value.kind else {
                panic!("constructor")
            };
            *application = Some(ty);
            program
        }
        let program = source(32);
        let prepared = prepare_sources(&[program], &[Vec::new()], 256).unwrap();
        prepared.session.validate_contracts(&[], 256).unwrap();
        assert_eq!(prepared.session.concrete_definitions().0.len(), 32);
        assert_eq!(
            preflight_sources(&[source(33)], 256).unwrap_err()[0].code,
            "G002"
        );
    }
}
