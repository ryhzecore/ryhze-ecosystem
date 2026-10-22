use super::output::{self as ir, Output, Type};
use super::semantic::{Flow, Loan, Ownership, BREAK, CONTINUE, NEXT, RETURN};
use super::{semantic, Signature};
use crate::ast::{self, BinaryOp, FloatIntrinsic, TensorIntrinsic, UnaryOp};
use crate::diagnostic::{Diagnostic, Span};
use std::{collections::HashMap, marker::PhantomData};
fn scalar(ty: Type) -> bool {
    ty.scalar()
}
fn numeric(ty: Type) -> bool {
    ty.numeric()
}

pub(super) type ResolvedSignature = Signature<Type, ir::Callable>;
pub(super) type TraitMethods = HashMap<String, Vec<(Type, ResolvedSignature)>>;

// Ordinary declarations remain borrowed directly. Prepared applications carry
// substituted source types, so opaque payloads use the exact same field and
// ownership operations without pretending to have a concrete layout.
enum NominalShape<'a> {
    Record(&'a ast::Record),
    Union(&'a ast::Union),
    Prepared(crate::nominal::NominalShape),
}

#[derive(Clone, Copy)]
struct NominalField<'a> {
    name: &'a str,
    name_span: Span,
    ty: Type,
}

#[derive(Clone, Copy)]
enum NominalFields<'a> {
    Concrete(&'a [ast::RecordField]),
    Symbolic(&'a [ast::RecordField<Type>]),
}

impl<'a> NominalFields<'a> {
    fn len(self) -> usize {
        match self {
            Self::Concrete(fields) => fields.len(),
            Self::Symbolic(fields) => fields.len(),
        }
    }

    fn get(self, index: usize) -> NominalField<'a> {
        match self {
            Self::Concrete(fields) => NominalField {
                name: &fields[index].name,
                name_span: fields[index].name_span,
                ty: fields[index].ty.into(),
            },
            Self::Symbolic(fields) => NominalField {
                name: &fields[index].name,
                name_span: fields[index].name_span,
                ty: fields[index].ty,
            },
        }
    }

    fn iter(self) -> impl Iterator<Item = NominalField<'a>> {
        (0..self.len()).map(move |index| self.get(index))
    }
}

struct NominalVariant<'a> {
    name: &'a str,
    name_span: Span,
    fields: NominalFields<'a>,
}

impl<'a> NominalShape<'a> {
    fn owner(&self) -> Type {
        match self {
            Self::Record(record) => ast::Type::Record(record.id).into(),
            Self::Union(union) => ast::Type::Union(union.id).into(),
            Self::Prepared(shape) => shape.owner,
        }
    }

    fn kind(&self) -> ast::NominalKind {
        match self {
            Self::Record(_) => ast::NominalKind::Record,
            Self::Union(_) => ast::NominalKind::Union,
            Self::Prepared(shape) => shape.kind,
        }
    }

    fn name(&self) -> &str {
        match self {
            Self::Record(record) => &record.name,
            Self::Union(union) => &union.name,
            Self::Prepared(shape) => &shape.name,
        }
    }

    fn name_span(&self) -> Span {
        match self {
            Self::Record(record) => record.name_span,
            Self::Union(union) => union.name_span,
            Self::Prepared(shape) => shape.name_span,
        }
    }

    fn fields(&self) -> NominalFields<'_> {
        match self {
            Self::Record(record) => NominalFields::Concrete(&record.fields),
            Self::Union(_) => NominalFields::Concrete(&[]),
            Self::Prepared(shape) => NominalFields::Symbolic(&shape.fields),
        }
    }

    fn variant_count(&self) -> usize {
        match self {
            Self::Record(_) => 0,
            Self::Union(union) => union.variants.len(),
            Self::Prepared(shape) => shape.variants.len(),
        }
    }

    fn variants<'s>(&'s self) -> impl Iterator<Item = NominalVariant<'s>> + use<'s, 'a> {
        (0..self.variant_count()).map(move |index| match self {
            Self::Record(_) => unreachable!("record has no variants"),
            Self::Union(union) => {
                let variant = &union.variants[index];
                NominalVariant {
                    name: &variant.name,
                    name_span: variant.name_span,
                    fields: NominalFields::Concrete(&variant.fields),
                }
            }
            Self::Prepared(shape) => {
                let variant = &shape.variants[index];
                NominalVariant {
                    name: &variant.name,
                    name_span: variant.name_span,
                    fields: NominalFields::Symbolic(&variant.fields),
                }
            }
        })
    }
}

pub(super) struct FunctionChecker<'a, B: Output> {
    pub(super) generics: super::generics::Context<'a>,
    pub(super) nominal: Option<&'a crate::nominal::NominalSession>,
    pub(super) signatures: &'a HashMap<String, ResolvedSignature>,
    pub(super) trait_methods: &'a TraitMethods,
    pub(super) records: &'a [ast::Record],
    pub(super) type_names: &'a HashMap<String, ast::Type>,
    pub(super) unions: &'a [ast::Union],
    pub(super) errors: &'a mut Vec<Diagnostic>,
    pub(super) state: semantic::State<ir::Local>,
    pub(super) return_type: Type,
    pub(super) function_span: Span,
    pub(super) output: PhantomData<B>,
}

impl<'a, B: Output> FunctionChecker<'a, B> {
    fn nominal_kind(&self, ty: Type) -> Option<ast::NominalKind> {
        match ty {
            Type::Concrete(ast::Type::Record(_)) => Some(ast::NominalKind::Record),
            Type::Concrete(ast::Type::Union(_)) => Some(ast::NominalKind::Union),
            Type::Application(_) => self.nominal.and_then(|session| session.kind(ty)),
            _ => None,
        }
    }

    fn nominal_shape(&self, ty: Type, span: Span) -> Result<NominalShape<'a>, Diagnostic> {
        match ty {
            Type::Concrete(ast::Type::Record(id)) => {
                if let Some(record) = self.records.iter().find(|record| record.id == id) {
                    return Ok(NominalShape::Record(record));
                }
            }
            Type::Concrete(ast::Type::Union(id)) => {
                if let Some(union) = self.unions.iter().find(|union| union.id == id) {
                    return Ok(NominalShape::Union(union));
                }
            }
            _ => {}
        }
        self.nominal
            .ok_or_else(|| {
                Diagnostic::new("G007", span, "nominal type has no prepared definition")
            })?
            .shape(ty, span)
            .map(NominalShape::Prepared)
    }

    fn constructor_type(
        &mut self,
        name: &str,
        application: Option<Type>,
        span: Span,
    ) -> Option<Type> {
        match application {
            Some(ty) => {
                let ty = self.generics.normalize(ty, span, self.errors)?;
                let Some(session) = self.nominal else {
                    self.errors.push(Diagnostic::new(
                        "G007",
                        span,
                        "explicit nominal application has no prepared definition",
                    ));
                    return None;
                };
                let shape = match session.shape(ty, span) {
                    Ok(shape) => shape,
                    Err(error) => {
                        self.errors.push(error);
                        return None;
                    }
                };
                if session.visible_declaration(self.generics.source, name)
                    != Some(shape.declaration)
                {
                    self.errors.push(Diagnostic::new(
                        "G007",
                        span,
                        format!(
                            "nominal application does not resolve to visible declaration `{name}`"
                        ),
                    ));
                    return None;
                }
                Some(ty)
            }
            None => self.type_names.get(name).copied().map(Type::from),
        }
    }

    pub(super) fn declare(
        &mut self,
        name: &str,
        span: Span,
        ty: impl Into<Type>,
        mutable: bool,
    ) -> ir::LocalId {
        let (id, previous) = self.state.declare(ir::Local {
            name: name.to_owned(),
            ty: ty.into(),
            mutable,
            span,
        });
        if let Some(previous) = previous {
            self.errors.push(
                Diagnostic::new(
                    "T004",
                    span,
                    format!("binding `{name}` is already declared in this scope"),
                )
                .with_related(
                    self.state.locals[previous].span,
                    "previous binding declaration",
                ),
            );
        }
        id
    }

    fn resolve(&self, name: &str) -> Option<ir::LocalId> {
        self.state.resolve(name)
    }

    fn named_local(&mut self, name: &str, span: Span) -> Option<ir::LocalId> {
        match self.resolve(name) {
            Some(local) => Some(local),
            None => {
                self.errors.push(Diagnostic::new(
                    "T005",
                    span,
                    format!("unknown variable `{name}`"),
                ));
                None
            }
        }
    }

    fn owner_available(&mut self, owner: ir::LocalId, span: Span) -> bool {
        self.place_available(owner, &[], span)
    }

    fn place_available(&mut self, owner: ir::LocalId, path: &[usize], span: Span) -> bool {
        let noun = match self.state.locals[owner].ty {
            Type::Tensor => "tensor",
            Type::Concrete(ast::Type::Record(_)) => "record",
            Type::Concrete(ast::Type::Union(_)) => "union",
            Type::Application(_) => "aggregate",
            _ => "array",
        };
        let moved = self.state.ownership.move_site(owner, path);
        if let Some(move_span) = moved {
            self.errors.push(
                Diagnostic::new(
                    "O006",
                    span,
                    format!(
                        "{noun} `{}` has been moved{}",
                        self.state.locals[owner].name,
                        if self.state.ownership.moved.contains_key(&owner) {
                            ""
                        } else {
                            " in whole or in the accessed field"
                        }
                    ),
                )
                .with_related(move_span, "ownership moved here")
                .with_related(
                    self.state.locals[owner].span,
                    format!("{noun} declared here"),
                ),
            );
            return false;
        }
        true
    }

    // Shared access includes reads and new shared loans. Exclusive access
    // includes writes, moves, and new mutable loans.
    fn owner_access(&mut self, owner: ir::LocalId, exclusive: bool, span: Span) -> bool {
        self.place_access(owner, &[], exclusive, span)
    }

    fn place_access(
        &mut self,
        owner: ir::LocalId,
        path: &[usize],
        exclusive: bool,
        span: Span,
    ) -> bool {
        if !self.place_available(owner, path, span) {
            return false;
        }
        if let Some(loan) = self.state.ownership.conflicting_loan(owner, exclusive) {
            self.errors.push(
                Diagnostic::new(
                    "O007",
                    span,
                    format!(
                        "cannot {} {} `{}` while it is {} borrowed",
                        if exclusive {
                            "exclusively access"
                        } else {
                            "access"
                        },
                        if self.state.locals[owner].ty == Type::Tensor {
                            "tensor"
                        } else {
                            "array"
                        },
                        self.state.locals[owner].name,
                        if loan.mutable { "mutably" } else { "shared" }
                    ),
                )
                .with_related(
                    loan.span,
                    "this loan lasts until its slice binding's scope ends",
                )
                .with_related(
                    self.state.locals[owner].span,
                    if self.state.locals[owner].ty == Type::Tensor {
                        "tensor declared here"
                    } else {
                        "array declared here"
                    },
                ),
            );
            return false;
        }
        true
    }

    fn collection(&mut self, name: &str, span: Span, write: bool) -> Option<ir::LocalId> {
        let local = self.named_local(name, span)?;
        match self.state.locals[local].ty {
            Type::Concrete(ast::Type::Array { .. }) => {
                let accessible = self.owner_access(local, write, span);
                let mutable = !write || self.state.locals[local].mutable;
                if !mutable {
                    self.errors.push(
                        Diagnostic::new(
                            "O010",
                            span,
                            format!("cannot write through immutable array `{name}`"),
                        )
                        .with_related(
                            self.state.locals[local].span,
                            "declare this owner with `let mut` to permit writes",
                        ),
                    );
                }
                (accessible && mutable).then_some(local)
            }
            Type::Concrete(ast::Type::Slice { mutable, .. }) => {
                if write && !mutable {
                    self.errors.push(
                        Diagnostic::new(
                            "O010",
                            span,
                            format!("cannot write through shared slice `{name}`"),
                        )
                        .with_related(
                            self.state.locals[local].span,
                            "a mutable slice capability is required",
                        ),
                    );
                    None
                } else {
                    Some(local)
                }
            }
            _ => {
                self.errors.push(
                    Diagnostic::new("O009", span, format!("`{name}` is not an array or slice"))
                        .with_related(self.state.locals[local].span, "binding declared here"),
                );
                None
            }
        }
    }

    fn element_type(&self, collection: ir::LocalId) -> Type {
        match self.state.locals[collection].ty {
            Type::Concrete(ast::Type::Array { element, .. })
            | Type::Concrete(ast::Type::Slice { element, .. }) => element.scalar_type().into(),
            _ => unreachable!("collection validation precedes element type lookup"),
        }
    }

    fn initializer(
        &mut self,
        expr: &ast::Expr,
        expected: Type,
        declaration: Span,
    ) -> Option<ir::Expr<B>> {
        if expected.transferred() {
            if let ast::ExprKind::Field {
                name,
                field,
                field_span,
            } = &expr.kind
            {
                let (base, path, ty, _) =
                    self.record_field(name, expr.span, field, *field_span, false)?;
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::MoveField {
                            base,
                            field: path.clone(),
                        },
                        ty,
                        expr.span,
                    )),
                    expected,
                    "aggregate field move",
                    Some(declaration),
                );
                let accessible = self.place_access(base, &path, true, expr.span);
                let protected = self.protected_move_allowed(base, expr.span);
                let loop_safe = self.state.loop_move_allowed(base);
                if !loop_safe {
                    self.errors.push(
                        Diagnostic::new(
                            "O011",
                            expr.span,
                            "cannot move a field of an owner declared outside the current loop",
                        )
                        .with_related(
                            self.state.locals[base].span,
                            "this owner must remain available across the loop backedge",
                        ),
                    );
                }
                if value.is_some() && accessible && protected && loop_safe {
                    self.state
                        .ownership
                        .partial
                        .entry(base)
                        .or_default()
                        .push((path, expr.span));
                    return value;
                }
                return None;
            }
            if let ast::ExprKind::GenericCall {
                name,
                type_args,
                args,
            } = &expr.kind
            {
                let value = self.generic_call(expr.span, name, type_args, args, true);
                return self.expect_type(
                    value,
                    expected,
                    "aggregate-producing call",
                    Some(declaration),
                );
            }
            if let ast::ExprKind::Call { name, args } = &expr.kind {
                let value = self.function_call(expr.span, name, args, true);
                return self.expect_type(
                    value,
                    expected,
                    "aggregate-producing call",
                    Some(declaration),
                );
            }
        }
        match expected {
            Type::Parameter(_) => {
                let ast::ExprKind::Var(name) = &expr.kind else {
                    self.errors.push(Diagnostic::new("O005", expr.span,
                        "an opaque parameter value requires a bare owner or an immediately captured producing call"));
                    return None;
                };
                let owner = self.named_local(name, expr.span)?;
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::Move(owner),
                        self.state.locals[owner].ty,
                        expr.span,
                    )),
                    expected,
                    "parameter move",
                    Some(declaration),
                );
                let available = self.consume_union(owner, expr.span);
                if value.is_some() && available {
                    value
                } else {
                    None
                }
            }
            Type::Concrete(ast::Type::Union(_)) => {
                self.union_initializer(expr, expected, declaration)
            }
            Type::Concrete(ast::Type::Record(_)) => {
                self.record_initializer(expr, expected, declaration)
            }
            Type::Application(_) => match self.nominal_kind(expected) {
                Some(ast::NominalKind::Record) => {
                    self.record_initializer(expr, expected, declaration)
                }
                Some(ast::NominalKind::Union) => {
                    self.union_initializer(expr, expected, declaration)
                }
                None => {
                    self.errors.push(Diagnostic::new(
                        "G007",
                        declaration,
                        "nominal application has no prepared definition",
                    ));
                    None
                }
            },
            Type::Tensor | Type::Concrete(ast::Type::TensorView { .. }) => {
                self.tensor_initializer(expr, expected, declaration)
            }
            Type::TensorError => {
                self.errors.push(Diagnostic::new("N008", declaration,
                    "tensor_error is permitted only as a catch binding; error copying is unsupported"));
                None
            }
            Type::I64 | Type::F64 | Type::Bool | Type::Unit => {
                let value = self.expression(expr);
                self.expect_type(value, expected, "initializer", Some(declaration))
            }
            Type::Concrete(ast::Type::Array { element, .. }) => match &expr.kind {
                ast::ExprKind::Array(elements) => {
                    let checked: Vec<_> = elements
                        .iter()
                        .map(|value| {
                            let value = self.expression(value);
                            self.expect_type(
                                value,
                                element.scalar_type().into(),
                                "array element",
                                Some(declaration),
                            )
                        })
                        .collect();
                    // Visit all elements before propagating a failed element.
                    let checked: Option<Vec<_>> = checked.into_iter().collect();
                    let value = checked.map(|elements| {
                        let length = elements.len();
                        ir::Expr::new(
                            ir::ExprKind::Array(
                                elements.into_iter().map(|value| value.payload).collect(),
                            ),
                            Type::Concrete(ast::Type::Array { element, length }),
                            expr.span,
                        )
                    });
                    self.expect_type(value, expected, "array initializer", Some(declaration))
                }
                ast::ExprKind::Var(name) => {
                    let owner = self.named_local(name, expr.span)?;
                    if !matches!(
                        self.state.locals[owner].ty,
                        Type::Concrete(ast::Type::Array { .. })
                    ) {
                        self.errors.push(Diagnostic::new("O005", expr.span, "an array initializer requires an array literal or an array owner to move")
                            .with_related(self.state.locals[owner].span, "this binding is not an array owner"));
                        return None;
                    }
                    let value = Some(ir::Expr::new(
                        ir::ExprKind::Move(owner),
                        self.state.locals[owner].ty,
                        expr.span,
                    ));
                    let value = self.expect_type(value, expected, "array move", Some(declaration));
                    let accessible = self.owner_access(owner, true, expr.span);
                    let scope_safe = self.protected_move_allowed(owner, expr.span);
                    let loop_safe = self.state.loop_move_allowed(owner);
                    if !loop_safe {
                        self.errors.push(
                            Diagnostic::new(
                                "O011",
                                expr.span,
                                "cannot move an array declared outside the current loop",
                            )
                            .with_related(
                                self.state.locals[owner].span,
                                "this owner must remain available across the loop backedge",
                            ),
                        );
                    }
                    if value.is_some() && accessible && loop_safe && scope_safe {
                        self.state.ownership.moved.insert(owner, expr.span);
                        value
                    } else {
                        None
                    }
                }
                _ => {
                    self.errors.push(Diagnostic::new("O005", expr.span, "an array initializer requires an array literal or a bare array owner to move"));
                    None
                }
            },
            Type::Concrete(ast::Type::Slice { .. }) => {
                self.slice_initializer(expr, expected, declaration)
            }
        }
    }

    fn consume_union(&mut self, owner: ir::LocalId, span: Span) -> bool {
        let accessible = self.owner_access(owner, true, span);
        let protected = self.protected_move_allowed(owner, span);
        let loop_safe = self.state.loop_move_allowed(owner);
        if !loop_safe {
            self.errors.push(
                Diagnostic::new(
                    "O011",
                    span,
                    if matches!(self.state.locals[owner].ty, Type::Parameter(_)) {
                        "cannot consume an opaque parameter declared outside the current loop"
                    } else {
                        "cannot consume a union declared outside the current loop"
                    },
                )
                .with_related(
                    self.state.locals[owner].span,
                    "this owner must remain available across the loop backedge",
                ),
            );
        }
        if accessible && protected && loop_safe {
            self.state.ownership.moved.insert(owner, span);
            true
        } else {
            false
        }
    }

    fn union_initializer(
        &mut self,
        expr: &ast::Expr,
        expected: Type,
        declaration: Span,
    ) -> Option<ir::Expr<B>> {
        match &expr.kind {
            ast::ExprKind::Union {
                name,
                application,
                variant,
                variant_span,
                fields,
            } => {
                let ty = self.constructor_type(name, *application, expr.span);
                let Some(ty) =
                    ty.filter(|ty| self.nominal_kind(*ty) == Some(ast::NominalKind::Union))
                else {
                    self.errors.push(Diagnostic::new(
                        "U006",
                        expr.span,
                        format!("unknown module-local union `{name}`"),
                    ));
                    for field in fields {
                        self.expression(&field.value);
                    }
                    return None;
                };
                let union = match self.nominal_shape(ty, expr.span) {
                    Ok(union) => union,
                    Err(error) => {
                        self.errors.push(error);
                        return None;
                    }
                };
                let Some((tag, variant)) = union
                    .variants()
                    .enumerate()
                    .find(|(_, declared)| declared.name == variant.as_str())
                else {
                    self.errors.push(
                        Diagnostic::new(
                            "U007",
                            *variant_span,
                            format!("union `{name}` has no variant `{variant}`"),
                        )
                        .with_related(union.name_span(), "union declared here"),
                    );
                    for field in fields {
                        self.expression(&field.value);
                    }
                    return None;
                };
                let mut seen = vec![None; variant.fields.len()];
                let mut values = Vec::new();
                let mut valid = true;
                for field in fields {
                    let Some(index) = variant
                        .fields
                        .iter()
                        .position(|declared| declared.name == field.name)
                    else {
                        self.expression(&field.value);
                        self.errors.push(Diagnostic::new(
                            "U008",
                            field.name_span,
                            format!("variant `{}` has no field `{}`", variant.name, field.name),
                        ));
                        valid = false;
                        continue;
                    };
                    let declared = variant.fields.get(index);
                    let value = if declared.ty.transferred() {
                        self.initializer(&field.value, declared.ty, declared.name_span)
                    } else {
                        self.expression(&field.value)
                    };
                    if let Some(previous) = seen[index] {
                        self.errors.push(
                            Diagnostic::new(
                                "U009",
                                field.name_span,
                                format!("variant field `{}` occurs more than once", field.name),
                            )
                            .with_related(previous, "previous field initializer"),
                        );
                        valid = false;
                    }
                    seen[index] = Some(field.name_span);
                    if let Some(value) = self.expect_type(
                        value,
                        declared.ty,
                        "union payload initializer",
                        Some(declared.name_span),
                    ) {
                        values.push((index, value.payload));
                    } else {
                        valid = false;
                    }
                }
                for (index, field) in variant.fields.iter().enumerate() {
                    if seen[index].is_none() {
                        self.errors.push(
                            Diagnostic::new(
                                "U010",
                                expr.span,
                                format!("missing union payload field `{}`", field.name),
                            )
                            .with_related(field.name_span, "required payload field"),
                        );
                        valid = false;
                    }
                }
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::Union {
                            layout_variant: tag,
                            variant: tag,
                            fields: values,
                        },
                        union.owner(),
                        expr.span,
                    )),
                    expected,
                    "union initializer",
                    Some(declaration),
                );
                if valid {
                    value
                } else {
                    None
                }
            }
            ast::ExprKind::Var(name) => {
                let owner = self.named_local(name, expr.span)?;
                if self.nominal_kind(self.state.locals[owner].ty) != Some(ast::NominalKind::Union) {
                    self.errors.push(Diagnostic::new("O005", expr.span, "a union initializer requires variant construction, a bare union owner, or a private union-producing call"));
                    return None;
                }
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::Move(owner),
                        self.state.locals[owner].ty,
                        expr.span,
                    )),
                    expected,
                    "union move",
                    Some(declaration),
                );
                let available = self.consume_union(owner, expr.span);
                if available {
                    value
                } else {
                    None
                }
            }
            ast::ExprKind::Call { name, args } => {
                let value = self.function_call(expr.span, name, args, true);
                self.expect_type(value, expected, "union-producing call", Some(declaration))
            }
            _ => {
                self.errors.push(Diagnostic::new("O005", expr.span, "a union initializer requires variant construction, a bare union owner, or a private union-producing call"));
                None
            }
        }
    }

    fn match_statement<A: Copy + Into<Type>>(
        &mut self,
        name: &str,
        name_span: Span,
        arms: &[ast::MatchArm<A>],
        span: Span,
    ) -> (Option<ir::StmtKind<B>>, Flow) {
        let Some(owner) = self.named_local(name, name_span) else {
            return (None, NEXT);
        };
        let owner_type = self.state.locals[owner].ty;
        if self.nominal_kind(owner_type) != Some(ast::NominalKind::Union) {
            self.errors.push(Diagnostic::new(
                "U011",
                name_span,
                "match consumes a bare available union owner",
            ));
            return (None, NEXT);
        }
        let union = match self.nominal_shape(owner_type, name_span) {
            Ok(union) => union,
            Err(error) => {
                self.errors.push(error);
                return (None, NEXT);
            }
        };
        let mut valid = self.consume_union(owner, name_span);
        let entry = self.state.ownership.clone();
        let mut seen = vec![None; union.variant_count()];
        let mut checked = Vec::new();
        let mut merged: Option<Ownership> = None;
        let mut flow = 0;
        for arm in arms {
            if self.constructor_type(&arm.union_name, arm.application, arm.union_span)
                != Some(owner_type)
            {
                self.errors.push(
                    Diagnostic::new(
                        "U012",
                        arm.union_span,
                        format!(
                            "match pattern requires union `{}`, found `{}`",
                            union.name(),
                            arm.union_name
                        ),
                    )
                    .with_related(union.name_span(), "matched union declared here"),
                );
                valid = false;
            }
            let Some((tag, variant)) = union
                .variants()
                .enumerate()
                .find(|(_, variant)| variant.name == arm.variant)
            else {
                self.errors.push(Diagnostic::new(
                    "U007",
                    arm.variant_span,
                    format!("union `{}` has no variant `{}`", union.name(), arm.variant),
                ));
                valid = false;
                continue;
            };
            if let Some(previous) = seen[tag] {
                self.errors.push(
                    Diagnostic::new(
                        "U013",
                        arm.variant_span,
                        format!("variant `{}` is matched more than once", variant.name),
                    )
                    .with_related(previous, "previous match arm"),
                );
                valid = false;
            }
            seen[tag] = Some(arm.variant_span);
            self.state.ownership = entry.clone();
            self.state.enter_scope();
            let mut field_seen = vec![None; variant.fields.len()];
            let mut bindings = Vec::new();
            for field in &arm.fields {
                let Some(index) = variant
                    .fields
                    .iter()
                    .position(|declared| declared.name == field.name)
                else {
                    self.errors.push(Diagnostic::new(
                        "U008",
                        field.name_span,
                        format!("variant `{}` has no field `{}`", variant.name, field.name),
                    ));
                    valid = false;
                    continue;
                };
                if let Some(previous) = field_seen[index] {
                    self.errors.push(
                        Diagnostic::new(
                            "U009",
                            field.name_span,
                            format!("pattern field `{}` occurs more than once", field.name),
                        )
                        .with_related(previous, "previous field pattern"),
                    );
                    valid = false;
                }
                field_seen[index] = Some(field.name_span);
                let local = self.declare(
                    &field.binding,
                    field.binding_span,
                    variant.fields.get(index).ty,
                    false,
                );
                bindings.push((index, local));
            }
            for (index, field) in variant.fields.iter().enumerate() {
                if field_seen[index].is_none() {
                    self.errors.push(
                        Diagnostic::new(
                            "U010",
                            arm.variant_span,
                            format!("missing union payload pattern field `{}`", field.name),
                        )
                        .with_related(field.name_span, "required payload field"),
                    );
                    valid = false;
                }
            }
            let (body, arm_flow) = self.block(&arm.body, false);
            self.leave_scope();
            if arm_flow & NEXT != 0 {
                merged = Some(match merged {
                    Some(previous) => Ownership::join(previous, self.state.ownership.clone()),
                    None => self.state.ownership.clone(),
                });
            }
            flow |= arm_flow;
            checked.push(B::arm(tag, bindings, body));
        }
        for (index, variant) in union.variants().enumerate() {
            if seen[index].is_none() {
                self.errors.push(
                    Diagnostic::new(
                        "U014",
                        span,
                        format!("non-exhaustive match: missing variant `{}`", variant.name),
                    )
                    .with_related(variant.name_span, "required variant"),
                );
                valid = false;
            }
        }
        self.state.ownership = merged.unwrap_or(entry);
        if valid {
            (
                Some(ir::StmtKind::Match {
                    owner,
                    arms: checked,
                }),
                flow,
            )
        } else {
            (None, flow | NEXT)
        }
    }

    fn record_initializer(
        &mut self,
        expr: &ast::Expr,
        expected: Type,
        declaration: Span,
    ) -> Option<ir::Expr<B>> {
        // Two-segment construction is grammatically shared with a local union
        // variant. The explicit expected type resolves m::Record{...} without
        // changing the meaning of existing Union::Variant{...} construction.
        if let ast::ExprKind::Union {
            name,
            application: None,
            variant,
            fields,
            ..
        } = &expr.kind
        {
            let qualified = ast::Expr {
                kind: ast::ExprKind::Record {
                    name: format!("{name}::{variant}"),
                    application: None,
                    fields: fields.clone(),
                },
                span: expr.span,
            };
            return self.record_initializer(&qualified, expected, declaration);
        }
        match &expr.kind {
            ast::ExprKind::Record {
                name,
                application,
                fields,
            } => {
                let ty = self.constructor_type(name, *application, expr.span);
                let Some(ty) =
                    ty.filter(|ty| self.nominal_kind(*ty) == Some(ast::NominalKind::Record))
                else {
                    self.errors.push(Diagnostic::new(
                        "R006",
                        expr.span,
                        format!("unknown module-local record `{name}`"),
                    ));
                    for field in fields {
                        self.expression(&field.value);
                    }
                    return None;
                };
                let record = match self.nominal_shape(ty, expr.span) {
                    Ok(record) => record,
                    Err(error) => {
                        self.errors.push(error);
                        return None;
                    }
                };
                let declared_fields = record.fields();
                let mut seen = vec![None; declared_fields.len()];
                let mut values = Vec::with_capacity(fields.len());
                let mut valid = true;
                for field in fields {
                    // Checking and lowering preserve written order, not layout order.
                    let Some(index) = declared_fields
                        .iter()
                        .position(|decl| decl.name == field.name)
                    else {
                        self.expression(&field.value);
                        self.errors.push(
                            Diagnostic::new(
                                "R007",
                                field.name_span,
                                format!("record `{name}` has no field `{}`", field.name),
                            )
                            .with_related(record.name_span(), "record declared here"),
                        );
                        valid = false;
                        continue;
                    };
                    let declared = declared_fields.get(index);
                    let value = if declared.ty.transferred() {
                        self.initializer(&field.value, declared.ty, declared.name_span)
                    } else {
                        self.expression(&field.value)
                    };
                    if let Some(previous) = seen[index] {
                        self.errors.push(
                            Diagnostic::new(
                                "R008",
                                field.name_span,
                                format!("field `{}` is initialized more than once", field.name),
                            )
                            .with_related(previous, "previous initializer"),
                        );
                        valid = false;
                    }
                    seen[index] = Some(field.name_span);
                    if let Some(value) = self.expect_type(
                        value,
                        declared.ty,
                        "record field initializer",
                        Some(declared.name_span),
                    ) {
                        values.push((index, value.payload));
                    } else {
                        valid = false;
                    }
                }
                for (index, field) in declared_fields.iter().enumerate() {
                    if seen[index].is_none() {
                        self.errors.push(
                            Diagnostic::new(
                                "R009",
                                expr.span,
                                format!(
                                    "missing initializer for field `{}` of record `{name}`",
                                    field.name
                                ),
                            )
                            .with_related(field.name_span, "required field declared here"),
                        );
                        valid = false;
                    }
                }
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::Record(values),
                        record.owner(),
                        expr.span,
                    )),
                    expected,
                    "record initializer",
                    Some(declaration),
                );
                if valid {
                    value
                } else {
                    None
                }
            }
            ast::ExprKind::Var(name) => {
                let owner = self.named_local(name, expr.span)?;
                if self.nominal_kind(self.state.locals[owner].ty) != Some(ast::NominalKind::Record)
                {
                    self.errors.push(Diagnostic::new("O005", expr.span,
                        "a record initializer requires named construction or a bare record owner to move"));
                    return None;
                }
                let value = self.expect_type(
                    Some(ir::Expr::new(
                        ir::ExprKind::Move(owner),
                        self.state.locals[owner].ty,
                        expr.span,
                    )),
                    expected,
                    "record move",
                    Some(declaration),
                );
                let accessible = self.owner_access(owner, true, expr.span);
                let scope_safe = self.protected_move_allowed(owner, expr.span);
                let loop_safe = self.state.loop_move_allowed(owner);
                if !loop_safe {
                    self.errors.push(
                        Diagnostic::new(
                            "O011",
                            expr.span,
                            "cannot move a record declared outside the current loop",
                        )
                        .with_related(
                            self.state.locals[owner].span,
                            "this owner must remain available across the loop backedge",
                        ),
                    );
                }
                if value.is_some() && accessible && scope_safe && loop_safe {
                    self.state.ownership.moved.insert(owner, expr.span);
                    value
                } else {
                    None
                }
            }
            _ => {
                self.errors.push(Diagnostic::new("O005", expr.span,
                    "a record initializer requires named construction or a bare record owner to move"));
                None
            }
        }
    }

    fn record_field(
        &mut self,
        name: &str,
        span: Span,
        field: &str,
        field_span: Span,
        write: bool,
    ) -> Option<(ir::LocalId, Vec<usize>, Type, Span)> {
        let base = self.named_local(name, span)?;
        let mut ty = self.state.locals[base].ty;
        let mut path = Vec::new();
        let mut declaration = self.state.locals[base].span;
        for part in field.split('.') {
            if self.nominal_kind(ty) != Some(ast::NominalKind::Record) {
                self.errors.push(Diagnostic::new(
                    "R010",
                    field_span,
                    format!(
                        "field path `{name}.{field}` crosses a value that is not a record owner"
                    ),
                ));
                return None;
            }
            let record = match self.nominal_shape(ty, field_span) {
                Ok(record) => record,
                Err(error) => {
                    self.errors.push(error);
                    return None;
                }
            };
            let Some((index, next)) = record
                .fields()
                .iter()
                .enumerate()
                .find(|(_, f)| f.name == part)
            else {
                self.errors.push(
                    Diagnostic::new(
                        "R007",
                        field_span,
                        format!("record `{}` has no field `{part}`", record.name()),
                    )
                    .with_related(record.name_span(), "record declared here"),
                );
                return None;
            };
            path.push(index);
            ty = next.ty;
            declaration = next.name_span;
        }
        let accessible = self.place_access(base, &path, write, span);
        let mutable = !write || self.state.locals[base].mutable;
        if !mutable {
            self.errors.push(
                Diagnostic::new(
                    "O010",
                    span,
                    format!("cannot write through immutable record `{name}`"),
                )
                .with_related(
                    self.state.locals[base].span,
                    "declare this owner with `let mut` to permit writes",
                ),
            );
        }
        (accessible && mutable).then_some((base, path, ty, declaration))
    }

    fn type_name(&self, ty: Type) -> String {
        if let Type::Concrete(ast::Type::Union(id)) = ty {
            if let Some(union) = self.unions.iter().find(|union| union.id == id) {
                return format!("union `{}`", union.name);
            }
        }
        if let Type::Concrete(ast::Type::Record(id)) = ty {
            if let Some(record) = self.records.iter().find(|record| record.id == id) {
                return format!("record `{}`", record.name);
            }
        }
        if self.nominal_kind(ty).is_some() {
            if let Ok(shape) = self.nominal_shape(ty, self.function_span) {
                let kind = match shape.kind() {
                    ast::NominalKind::Record => "record",
                    ast::NominalKind::Union => "union",
                };
                return format!("{kind} `{}` ({ty})", shape.name());
            }
        }
        ty.to_string()
    }

    fn slice_initializer(
        &mut self,
        expr: &ast::Expr,
        expected: Type,
        declaration: Span,
    ) -> Option<ir::Expr<B>> {
        let ast::ExprKind::Borrow {
            name,
            mutable,
            range,
        } = &expr.kind
        else {
            self.errors.push(Diagnostic::new("O005", expr.span, "a slice initializer must be a fresh borrow directly from an array owner; slice copying is unsupported"));
            return None;
        };

        // Bounds are evaluated in order before the new loan is acquired, so
        // `&mut a[a[0]..len(a)]` may read an otherwise available candidate owner.
        let bounds = range.as_ref().map(|(start, end)| {
            let start = self.expression(start);
            let start = self.expect_type(start, Type::I64, "slice start", None);
            let end = self.expression(end);
            let end = self.expect_type(end, Type::I64, "slice end", None);
            (start, end)
        });
        let owner = self.named_local(name, expr.span)?;
        let Type::Concrete(ast::Type::Array { element, length }) = self.state.locals[owner].ty
        else {
            self.errors.push(
                Diagnostic::new("O005", expr.span, "slice borrows require a direct array owner; reborrowing a slice is unsupported")
                    .with_related(self.state.locals[owner].span, "borrow target declared here"),
            );
            return None;
        };
        let accessible = self.owner_access(owner, *mutable, expr.span);
        let writable = !*mutable || self.state.locals[owner].mutable;
        if !writable {
            self.errors.push(
                Diagnostic::new(
                    "O010",
                    expr.span,
                    format!("cannot mutably borrow immutable array `{name}`"),
                )
                .with_related(
                    self.state.locals[owner].span,
                    "declare this owner with `let mut` to permit mutable borrowing",
                ),
            );
        }
        let (start, end) = match bounds {
            Some((start, end)) => (start?, end?),
            None => (
                ir::Expr::new(ir::ExprKind::Int(0), Type::I64, expr.span),
                ir::Expr::new(ir::ExprKind::Int(length as i64), Type::I64, expr.span),
            ),
        };
        let value = Some(ir::Expr::new(
            ir::ExprKind::Borrow {
                owner,
                mutable: *mutable,
                start: Box::new(start.payload),
                end: Box::new(end.payload),
            },
            Type::Concrete(ast::Type::Slice {
                element,
                mutable: *mutable,
            }),
            expr.span,
        ));
        let value = self.expect_type(value, expected, "slice initializer", Some(declaration));
        if accessible && writable {
            value
        } else {
            None
        }
    }

    fn leave_scope(&mut self) {
        self.state.leave_scope();
    }

    fn protected_move_allowed(&mut self, owner: ir::LocalId, span: Span) -> bool {
        if !self.state.protected_move_allowed(owner) {
            self.errors.push(
                Diagnostic::new(
                    "N007",
                    span,
                    "cannot move an aggregate declared outside the protected tensor scope body",
                )
                .with_related(
                    self.state.locals[owner].span,
                    "this owner must remain available on a tensor error path",
                ),
            );
            false
        } else {
            true
        }
    }

    fn require_tensor_body(&mut self, span: Span) -> bool {
        if self.state.tensor_body.is_none() {
            self.errors.push(Diagnostic::new(
                "N002",
                span,
                "tensor owners, views, and operations require a protected tensor_scope body",
            ));
            false
        } else {
            true
        }
    }

    fn tensor_initializer(
        &mut self,
        expr: &ast::Expr,
        expected: Type,
        declaration: Span,
    ) -> Option<ir::Expr<B>> {
        if !self.require_tensor_body(expr.span) {
            return None;
        }
        if let ast::ExprKind::Var(name) = &expr.kind {
            if expected != Type::Tensor {
                self.errors.push(Diagnostic::new("O005", expr.span,
                    "tensor views require a fresh checked view constructor; view copying is unsupported"));
                return None;
            }
            let owner = self.named_local(name, expr.span)?;
            if self.state.locals[owner].ty != Type::Tensor {
                self.errors.push(Diagnostic::new(
                    "T009",
                    expr.span,
                    "a tensor move requires a tensor owner",
                ));
                return None;
            }
            let accessible = self.owner_access(owner, true, expr.span);
            let scope_safe = self.protected_move_allowed(owner, expr.span);
            let loop_safe = self.state.loop_move_allowed(owner);
            if !loop_safe {
                self.errors.push(
                    Diagnostic::new(
                        "O011",
                        expr.span,
                        "cannot move a tensor declared outside the current loop",
                    )
                    .with_related(
                        self.state.locals[owner].span,
                        "this owner must remain available across the loop backedge",
                    ),
                );
            }
            if !(accessible && scope_safe && loop_safe) {
                return None;
            }
            self.state.ownership.moved.insert(owner, expr.span);
            return Some(ir::Expr::new(
                ir::ExprKind::Move(owner),
                Type::Tensor,
                expr.span,
            ));
        }
        let ast::ExprKind::Try(call) = &expr.kind else {
            self.errors.push(Diagnostic::new("N003", expr.span,
                "tensor and view initializers require try followed by a checked tensor builtin, or a bare tensor owner move"));
            return None;
        };
        let ast::ExprKind::TensorCall { op, args } = &call.kind else {
            self.errors.push(Diagnostic::new(
                "N003",
                call.span,
                "try requires a fallible tensor builtin call",
            ));
            return None;
        };
        let value = self.tensor_call(call.span, *op, args, true);
        self.expect_type(value, expected, "tensor initializer", Some(declaration))
    }

    fn tensor_local(&mut self, expr: &ast::Expr, write: bool) -> Option<ir::LocalId> {
        let ast::ExprKind::Var(name) = &expr.kind else {
            self.errors.push(Diagnostic::new("N004", expr.span,
                "tensor operands must be named owners or views; unnamed tensor temporaries are unsupported"));
            return None;
        };
        let local = self.named_local(name, expr.span)?;
        match self.state.locals[local].ty {
            Type::Tensor => {
                let access = self.owner_access(local, write, expr.span);
                if write && !self.state.locals[local].mutable {
                    self.errors.push(
                        Diagnostic::new(
                            "O010",
                            expr.span,
                            "tensor mutation requires a mutable owner",
                        )
                        .with_related(
                            self.state.locals[local].span,
                            "declare this tensor with let mut",
                        ),
                    );
                    return None;
                }
                access.then_some(local)
            }
            Type::Concrete(ast::Type::TensorView { mutable }) => {
                if write && !mutable {
                    self.errors.push(Diagnostic::new(
                        "O010",
                        expr.span,
                        "tensor mutation requires an exclusive view",
                    ));
                    None
                } else {
                    Some(local)
                }
            }
            _ => {
                self.errors.push(
                    Diagnostic::new("T009", expr.span, "expected a named tensor owner or view")
                        .with_related(self.state.locals[local].span, "binding declared here"),
                );
                None
            }
        }
    }

    fn tensor_scalar(&mut self, expr: &ast::Expr, ty: Type) -> Option<ir::Expr<B>> {
        let value = self.expression(expr);
        self.expect_type(value, ty, "tensor builtin argument", None)
    }

    fn tensor_list(&mut self, expr: &ast::Expr) -> Option<ir::TensorArg<B>> {
        let ast::ExprKind::Array(items) = &expr.kind else {
            self.errors.push(Diagnostic::new("N005", expr.span,
                "tensor shapes and permutations require an inline list of at most 8 i64 expressions"));
            return None;
        };
        let mut valid = true;
        if items.len() > 8 {
            self.errors.push(Diagnostic::new(
                "N005",
                expr.span,
                "tensor shape or permutation lists have at most 8 entries",
            ));
            valid = false;
        }
        let checked: Vec<_> = items
            .iter()
            .map(|item| self.tensor_scalar(item, Type::I64))
            .collect();
        let checked: Option<Vec<_>> = checked.into_iter().collect();
        if valid {
            checked.map(|values: Vec<ir::Expr<B>>| {
                ir::TensorArg::List(values.into_iter().map(|value| value.payload).collect())
            })
        } else {
            None
        }
    }

    fn tensor_arity(&mut self, span: Span, op: TensorIntrinsic, args: &[ast::Expr]) -> bool {
        use TensorIntrinsic::*;
        let expected = match op {
            Zeros | View | Sigmoid | Softmax | Sum | Logsumexp | Rank | Len | ErrorCode
            | ErrorStart | ErrorEnd => 1,
            FromArray | Transpose | Add | Mul | Matmul | Get | Dim => 2,
            Set => 3,
            Slice => 5,
        };
        if args.len() != expected {
            self.errors.push(Diagnostic::new(
                "T008",
                span,
                format!(
                    "builtin `{}` expects {expected} arguments, found {}",
                    op.name(),
                    args.len()
                ),
            ));
            false
        } else {
            true
        }
    }

    fn tensor_call(
        &mut self,
        span: Span,
        op: TensorIntrinsic,
        args: &[ast::Expr],
        tried: bool,
    ) -> Option<ir::Expr<B>> {
        use ir::TensorArg as Arg;
        use TensorIntrinsic::*;
        if tried != op.fallible() {
            self.errors.push(Diagnostic::new(
                "N003",
                span,
                if op.fallible() {
                    "fallible tensor calls require explicit try"
                } else {
                    "try applies only to fallible tensor calls"
                },
            ));
            return None;
        }
        let error_observer = matches!(op, ErrorCode | ErrorStart | ErrorEnd);
        if !error_observer && !self.require_tensor_body(span) {
            return None;
        }
        if !self.tensor_arity(span, op, args) {
            return None;
        }
        let mut checked = Vec::new();
        let ty = match op {
            Zeros | FromArray => {
                checked.push(self.tensor_list(&args[0])?);
                if op == FromArray {
                    let ast::ExprKind::Var(name) = &args[1].kind else {
                        self.errors.push(Diagnostic::new(
                            "N004",
                            args[1].span,
                            "tensor_from_array requires a named f64 array or slice",
                        ));
                        return None;
                    };
                    let local = self.collection(name, args[1].span, false)?;
                    if self.element_type(local) != Type::F64 {
                        self.errors.push(Diagnostic::new(
                            "T009",
                            args[1].span,
                            "tensor_from_array requires f64 elements",
                        ));
                        return None;
                    }
                    checked.push(Arg::Local {
                        local,
                        span: args[1].span,
                    });
                }
                Type::Tensor
            }
            View | Transpose | Slice => {
                let ast::ExprKind::Borrow {
                    name,
                    mutable,
                    range: None,
                } = &args[0].kind
                else {
                    self.errors.push(Diagnostic::new("N004", args[0].span,
                        "a tensor view constructor requires &owner or &mut owner directly; reborrowing is unsupported"));
                    return None;
                };
                let owner = self.named_local(name, args[0].span)?;
                if self.state.locals[owner].ty != Type::Tensor {
                    self.errors.push(Diagnostic::new(
                        "T009",
                        args[0].span,
                        "tensor views must borrow a direct tensor owner",
                    ));
                    return None;
                }
                checked.push(Arg::Local {
                    local: owner,
                    span: args[0].span,
                });
                // All descriptor operands are checked before the new loan exists.
                if op == Transpose {
                    checked.push(self.tensor_list(&args[1])?);
                }
                if op == Slice {
                    for arg in &args[1..] {
                        checked.push(Arg::Scalar(self.tensor_scalar(arg, Type::I64)?.payload));
                    }
                }
                let access = self.owner_access(owner, *mutable, args[0].span);
                if *mutable && !self.state.locals[owner].mutable {
                    self.errors.push(
                        Diagnostic::new(
                            "O010",
                            args[0].span,
                            "an exclusive tensor view requires a mutable owner",
                        )
                        .with_related(
                            self.state.locals[owner].span,
                            "declare this tensor with let mut",
                        ),
                    );
                    return None;
                }
                if !access {
                    return None;
                }
                Type::Concrete(ast::Type::TensorView { mutable: *mutable })
            }
            Set => {
                self.errors.push(Diagnostic::new(
                    "N004",
                    span,
                    "tensor_set is permitted only as a try statement",
                ));
                return None;
            }
            ErrorCode | ErrorStart | ErrorEnd => {
                let ast::ExprKind::Var(name) = &args[0].kind else {
                    self.errors.push(Diagnostic::new(
                        "N008",
                        args[0].span,
                        "a tensor error observer requires a catch binding",
                    ));
                    return None;
                };
                let local = self.named_local(name, args[0].span)?;
                if self.state.locals[local].ty != Type::TensorError {
                    self.errors.push(Diagnostic::new(
                        "T009",
                        args[0].span,
                        "expected a tensor_error catch binding",
                    ));
                    return None;
                }
                checked.push(Arg::Local {
                    local,
                    span: args[0].span,
                });
                Type::I64
            }
            Add | Mul | Matmul | Sigmoid | Softmax | Sum | Logsumexp | Get | Rank | Dim | Len => {
                let local = self.tensor_local(&args[0], false)?;
                checked.push(Arg::Local {
                    local,
                    span: args[0].span,
                });
                if matches!(op, Add | Mul | Matmul) {
                    let local = self.tensor_local(&args[1], false)?;
                    checked.push(Arg::Local {
                        local,
                        span: args[1].span,
                    });
                } else if matches!(op, Get | Dim) {
                    checked.push(Arg::Scalar(
                        self.tensor_scalar(&args[1], Type::I64)?.payload,
                    ));
                }
                match op {
                    Add | Mul | Matmul | Sigmoid | Softmax => Type::Tensor,
                    Get | Sum | Logsumexp => Type::F64,
                    _ => Type::I64,
                }
            }
        };
        Some(ir::Expr::new(
            ir::ExprKind::TensorCall { op, args: checked },
            ty,
            span,
        ))
    }

    fn tensor_set(&mut self, span: Span, args: &[ast::Expr]) -> Option<ir::StmtKind<B>> {
        if !self.require_tensor_body(span) || !self.tensor_arity(span, TensorIntrinsic::Set, args) {
            return None;
        }
        let base = self.tensor_local(&args[0], true);
        let index = self.tensor_scalar(&args[1], Type::I64);
        let value = self.tensor_scalar(&args[2], Type::F64);
        Some(ir::StmtKind::TensorSet {
            base: base?,
            index: index?.payload,
            value: value?.payload,
        })
    }

    pub(super) fn block<A: Copy + Into<Type>>(
        &mut self,
        block: &ast::Block<A>,
        new_scope: bool,
    ) -> (B::Block, Flow) {
        if new_scope {
            self.state.enter_scope();
        }
        let mut stmts = Vec::with_capacity(block.stmts.len());
        let mut cleanup = Vec::new();
        let mut flow = NEXT;
        for stmt in &block.stmts {
            let reachable = flow & NEXT != 0;
            if !reachable {
                self.errors
                    .push(Diagnostic::new("T011", stmt.span, "unreachable statement"));
            }
            // Check unreachable statements too, but never use their exits to
            // change the reachable control-flow result.
            let (checked, stmt_flow) = self.statement(stmt);
            if let Some(checked) = checked {
                if let Some(local) = checked.binding {
                    if matches!(
                        self.state.locals[local].ty,
                        Type::Tensor | Type::Concrete(ast::Type::TensorView { .. })
                    ) {
                        cleanup.push(local);
                    }
                }
                stmts.push(checked.payload);
            }
            if reachable {
                flow = (flow & !NEXT) | stmt_flow;
            }
        }
        if new_scope {
            self.leave_scope();
        }
        (B::block(stmts, block.span, cleanup), flow)
    }

    fn statement<A: Copy + Into<Type>>(
        &mut self,
        stmt: &ast::Stmt<A>,
    ) -> (Option<ir::Stmt<B>>, Flow) {
        use ast::StmtKind as A;
        use ir::StmtKind as I;
        let (kind, flow) = match &stmt.kind {
            A::Match {
                name,
                name_span,
                arms,
            } => self.match_statement(name, *name_span, arms, stmt.span),
            A::TensorScope {
                budget,
                body,
                error_name,
                error_span,
                handler,
            } => {
                if self.state.scope_depth() != 1 || self.state.tensor_body.is_some() {
                    self.errors.push(Diagnostic::new(
                        "N001",
                        stmt.span,
                        "tensor_scope is permitted only directly in a function body",
                    ));
                    return (None, NEXT);
                }
                let budget = self.tensor_scalar(budget, Type::I64);
                let entry = self.state.ownership.clone();
                self.state.tensor_body = Some((self.state.locals.len(), self.state.loop_depth));
                let (body, body_flow) = self.block(body, true);
                self.state.tensor_body = None;
                let success = self.state.ownership.clone();
                self.state.ownership = entry.clone();
                self.state.enter_scope();
                let error_local = self.declare(error_name, *error_span, Type::TensorError, false);
                let (handler, handler_flow) = self.block(handler, false);
                self.leave_scope();
                let failure = self.state.ownership.clone();
                self.state.ownership =
                    Ownership::join_continuing(entry, success, body_flow, failure, handler_flow);
                (
                    budget.map(|budget| I::TensorScope {
                        budget: budget.payload,
                        body,
                        error_local,
                        handler,
                    }),
                    body_flow | handler_flow,
                )
            }
            A::Let {
                name,
                name_span,
                mutable,
                ty,
                value,
            } => {
                let Some(ty) = self
                    .generics
                    .normalize((*ty).into(), *name_span, self.errors)
                else {
                    return (None, NEXT);
                };
                let value = self.initializer(value, ty, *name_span);
                let local = self.declare(name, *name_span, ty, *mutable);
                if let Some(value) = &value {
                    if let Some((owner, mutable)) = value.loan {
                        self.state.ownership.loans.push(Loan {
                            owner,
                            slice: local,
                            mutable,
                            span: value.span,
                        });
                    }
                }
                (
                    value.map(|value| I::Let {
                        local,
                        value: value.payload,
                    }),
                    NEXT,
                )
            }
            A::Assign {
                name,
                name_span,
                value,
            } => {
                let value = self.expression(value);
                match self.resolve(name) {
                    Some(local) => {
                        let declaration = &self.state.locals[local];
                        if !scalar(declaration.ty) {
                            self.errors.push(
                                Diagnostic::new(
                                    "O008",
                                    *name_span,
                                    "whole-aggregate and tensor_error reassignment are unsupported",
                                )
                                .with_related(declaration.span, "aggregate binding declared here"),
                            );
                            return (None, NEXT);
                        }
                        if !declaration.mutable {
                            self.errors.push(
                                Diagnostic::new(
                                    "T006",
                                    *name_span,
                                    format!("cannot assign to immutable binding `{name}`"),
                                )
                                .with_related(declaration.span, "immutable binding declared here"),
                            );
                        }
                        let value = self.expect_type(
                            value,
                            declaration.ty,
                            "assignment",
                            Some(declaration.span),
                        );
                        (
                            value.map(|value| I::Assign {
                                local,
                                value: value.payload,
                            }),
                            NEXT,
                        )
                    }
                    None => {
                        self.errors.push(Diagnostic::new(
                            "T005",
                            *name_span,
                            format!("unknown variable `{name}`"),
                        ));
                        (None, NEXT)
                    }
                }
            }
            A::FieldAssign {
                name,
                name_span,
                field,
                field_span,
                value,
            } => {
                let target = self.record_field(name, *name_span, field, *field_span, true);
                let value = self.expression(value);
                let kind = target.and_then(|(base, field, ty, declaration)| {
                    // A scalar-producing call in the RHS can now consume an
                    // inline owner. The previously resolved write target must
                    // still be available when the actual store occurs.
                    if !self.place_access(base, &field, true, *name_span) {
                        return None;
                    }
                    if ty.transferred() {
                        self.errors.push(Diagnostic::new("O008", *field_span, "aggregate field reassignment and moved-field reinitialization are unsupported; construct a new owner"));
                        return None;
                    }
                    self.expect_type(value, ty, "record field assignment", Some(declaration))
                        .map(|value| I::FieldAssign { base, field, value: value.payload })
                });
                (kind, NEXT)
            }
            A::IndexAssign {
                name,
                name_span,
                index,
                value,
            } => {
                let index = self.expression(index);
                let index = self.expect_type(index, Type::I64, "array or slice index", None);
                let value = self.expression(value);
                let base = self.collection(name, *name_span, true);
                let value = base.and_then(|base| {
                    self.expect_type(
                        value,
                        self.element_type(base),
                        "array or slice element",
                        None,
                    )
                });
                let kind = match (base, index, value) {
                    (Some(base), Some(index), Some(value)) => Some(I::IndexAssign {
                        base,
                        index: index.payload,
                        value: value.payload,
                    }),
                    _ => None,
                };
                (kind, NEXT)
            }
            A::If {
                condition,
                then_block,
                else_block,
            } => {
                let condition = self.expression(condition);
                let condition = self.expect_type(condition, Type::Bool, "if condition", None);
                let entry = self.state.ownership.clone();
                let (then_block, then_flow) = self.block(then_block, true);
                let then_state = self.state.ownership.clone();
                self.state.ownership = entry.clone();
                let (else_block, else_flow) = match else_block {
                    Some(block) => {
                        let (block, flow) = self.block(block, true);
                        (Some(block), flow)
                    }
                    None => (None, NEXT),
                };
                let else_state = self.state.ownership.clone();
                self.state.ownership =
                    Ownership::join_continuing(entry, then_state, then_flow, else_state, else_flow);
                (
                    condition.map(|condition| I::If {
                        condition: condition.payload,
                        then_block,
                        else_block,
                    }),
                    then_flow | else_flow,
                )
            }
            A::While { condition, body } => {
                let entry = self.state.ownership.clone();
                // The condition repeats on every backedge too. It cannot
                // consume an owner declared outside this loop any more than
                // the body can.
                self.state.loop_depth += 1;
                let condition = self.expression(condition);
                let condition = self.expect_type(condition, Type::Bool, "while condition", None);
                let (body, body_flow) = self.block(body, true);
                self.state.loop_depth -= 1;
                // No valid body can move an entry owner or retain a new loan
                // beyond this body scope. Therefore zero iterations, backedges,
                // breaks, and condition rechecks all have the same ownership.
                self.state.ownership = entry;
                (
                    condition.map(|condition| I::While {
                        condition: condition.payload,
                        body,
                    }),
                    NEXT | (body_flow & RETURN),
                )
            }
            A::Return(value) => {
                if self.state.tensor_body.is_some() {
                    self.errors.push(Diagnostic::new(
                        "N006",
                        stmt.span,
                        "return cannot cross the protected tensor_scope body boundary",
                    ));
                }
                let value = if self.return_type.transferred() {
                    self.initializer(value, self.return_type, self.function_span)
                } else {
                    let value = self.expression(value);
                    self.expect_type(
                        value,
                        self.return_type,
                        "return value",
                        Some(self.function_span),
                    )
                };
                (value.map(|value| I::Return(value.payload)), RETURN)
            }
            A::Print(value) => {
                let value = self.expression(value).and_then(|value| {
                    if value.ty == Type::Unit {
                        self.errors.push(Diagnostic::new(
                            "T020",
                            value.span,
                            "print requires i64, f64 or bool; unit () has no print representation",
                        ));
                        None
                    } else {
                        Some(value)
                    }
                });
                (value.map(|value| I::Print(value.payload)), NEXT)
            }
            A::Expr(value) => {
                if let ast::ExprKind::Try(call) = &value.kind {
                    if let ast::ExprKind::TensorCall {
                        op: TensorIntrinsic::Set,
                        args,
                    } = &call.kind
                    {
                        (self.tensor_set(stmt.span, args), NEXT)
                    } else {
                        (
                            self.expression(value).map(|value| I::Expr(value.payload)),
                            NEXT,
                        )
                    }
                } else {
                    (
                        self.expression(value).map(|value| I::Expr(value.payload)),
                        NEXT,
                    )
                }
            }
            A::Block(block) => {
                let (block, flow) = self.block(block, true);
                (Some(I::Block(block)), flow)
            }
            A::Break => {
                if self
                    .state
                    .tensor_body
                    .is_some_and(|(_, depth)| self.state.loop_depth <= depth)
                {
                    self.errors.push(Diagnostic::new(
                        "N006",
                        stmt.span,
                        "break cannot cross the protected tensor_scope body boundary",
                    ));
                }
                if self.state.loop_depth == 0 {
                    self.errors.push(Diagnostic::new(
                        "T012",
                        stmt.span,
                        "`break` requires an enclosing loop",
                    ));
                }
                (Some(I::Break), BREAK)
            }
            A::Continue => {
                if self
                    .state
                    .tensor_body
                    .is_some_and(|(_, depth)| self.state.loop_depth <= depth)
                {
                    self.errors.push(Diagnostic::new(
                        "N006",
                        stmt.span,
                        "continue cannot cross the protected tensor_scope body boundary",
                    ));
                }
                if self.state.loop_depth == 0 {
                    self.errors.push(Diagnostic::new(
                        "T013",
                        stmt.span,
                        "`continue` requires an enclosing loop",
                    ));
                }
                (Some(I::Continue), CONTINUE)
            }
        };
        (kind.map(|kind| ir::Stmt::new(kind, stmt.span)), flow)
    }

    fn expect_type(
        &mut self,
        expr: Option<ir::Expr<B>>,
        expected: Type,
        context: &str,
        declaration: Option<Span>,
    ) -> Option<ir::Expr<B>> {
        let expr = expr?;
        if expr.ty == expected {
            return Some(expr);
        }
        let mut error = Diagnostic::new(
            "T009",
            expr.span,
            format!(
                "{context} requires {}, found {}",
                self.type_name(expected),
                self.type_name(expr.ty)
            ),
        );
        if let Some(span) = declaration {
            error = error.with_related(span, "required type declared here");
        }
        self.errors.push(error);
        None
    }

    fn expect_numeric(&mut self, expr: Option<ir::Expr<B>>, context: &str) -> Option<ir::Expr<B>> {
        let expr = expr?;
        if numeric(expr.ty) {
            Some(expr)
        } else {
            self.errors.push(Diagnostic::new(
                "T009",
                expr.span,
                format!("{context} requires i64 or f64, found {}", expr.ty),
            ));
            None
        }
    }

    fn function_call(
        &mut self,
        span: Span,
        name: &str,
        args: &[ast::Expr],
        allow_owned: bool,
    ) -> Option<ir::Expr<B>> {
        // Trait dispatch examines only the receiver binding's declared type.
        // The normal argument path below performs each read/move exactly once.
        let selected = if let Some(implementations) = self.trait_methods.get(name) {
            let Some(ast::Expr {
                kind: ast::ExprKind::Var(receiver),
                span: receiver_span,
            }) = args.first()
            else {
                self.errors.push(Diagnostic::new(
                    "TR008",
                    span,
                    "a static trait call requires a bare local receiver as its first argument",
                ));
                return None;
            };
            let local = self.named_local(receiver, *receiver_span)?;
            let ty = self.state.locals[local].ty;
            match implementations.iter().find(|(target, _)| *target == ty) {
                Some((_, signature)) => Some(signature.clone()),
                None => {
                    self.errors.push(Diagnostic::new(
                        if matches!(ty, Type::Parameter(_)) {
                            "G004"
                        } else {
                            "TR007"
                        },
                        span,
                        if matches!(ty, Type::Parameter(_)) {
                            format!(
                                "`{name}` requires an explicit trait bound on this type parameter"
                            )
                        } else {
                            format!("no concrete implementation of `{name}` for the receiver type")
                        },
                    ));
                    return None;
                }
            }
        } else {
            self.signatures.get(name).cloned()
        };
        let Some(signature) = selected else {
            if self.generics.contains(name) {
                self.errors.push(Diagnostic::new(
                    "G006",
                    span,
                    format!("generic function `{name}` requires explicit type arguments"),
                ));
                return None;
            }
            for arg in args {
                self.expression(arg);
            }
            self.errors.push(Diagnostic::new(
                "T007",
                span,
                format!("unknown function `{name}`"),
            ));
            return None;
        };
        self.selected_call(span, name, args, allow_owned, signature)
    }

    fn generic_call(
        &mut self,
        span: Span,
        name: &str,
        type_args: &[ast::TypeExpr],
        args: &[ast::Expr],
        allow_owned: bool,
    ) -> Option<ir::Expr<B>> {
        let signature = self.generics.resolve(name, type_args, span, self.errors)?;
        self.selected_call(span, name, args, allow_owned, signature)
    }

    fn selected_call(
        &mut self,
        span: Span,
        name: &str,
        args: &[ast::Expr],
        allow_owned: bool,
        signature: ResolvedSignature,
    ) -> Option<ir::Expr<B>> {
        let arity_ok = args.len() == signature.params.len();
        let mut checked = Vec::with_capacity(args.len());
        for (index, arg) in args.iter().enumerate() {
            let value = match signature.params.get(index) {
                Some(&(ty, declaration)) if ty.transferred() => {
                    if matches!(arg.kind, ast::ExprKind::Var(_)) {
                        self.initializer(arg, ty, declaration)
                    } else {
                        self.errors.push(Diagnostic::new("I001", arg.span, "an owned aggregate argument requires a bare available owner; construct or capture it in a let binding first"));
                        None
                    }
                }
                _ => self.expression(arg),
            };
            checked.push(value);
        }
        // Keep legacy scalar diagnostic order: expression errors, arity, then
        // argument type mismatches. Ownership transfers above still occur in
        // written order before checking any later expression.
        if !arity_ok {
            self.errors.push(
                Diagnostic::new(
                    "T008",
                    span,
                    format!(
                        "function `{name}` expects {} arguments, found {}",
                        signature.params.len(),
                        args.len()
                    ),
                )
                .with_related(signature.span, "function declared here"),
            );
        }
        for (arg, &(ty, declaration)) in checked.iter_mut().zip(&signature.params) {
            if !ty.transferred() {
                *arg = self.expect_type(arg.take(), ty, "argument", Some(declaration));
            }
        }
        let args: Option<Vec<ir::Expr<B>>> = checked.into_iter().collect();
        let args = args?;
        if !arity_ok {
            return None;
        }
        if !(scalar(signature.return_type) || allow_owned && signature.return_type.transferred()) {
            self.errors.push(Diagnostic::new(
                "O004",
                span,
                "aggregate function results are unsupported in expressions",
            ));
            return None;
        }
        Some(ir::Expr::new(
            ir::ExprKind::Call {
                function: signature.id,
                args: args.into_iter().map(|value| value.payload).collect(),
            },
            signature.return_type,
            span,
        ))
    }

    fn expression(&mut self, expr: &ast::Expr) -> Option<ir::Expr<B>> {
        use ast::ExprKind as A;
        use ir::ExprKind as I;
        let (kind, ty) = match &expr.kind {
            A::TensorCall { op, args } => {
                let checked = self.tensor_call(expr.span, *op, args, false)?;
                if !scalar(checked.ty) {
                    self.errors.push(Diagnostic::new(
                        "N004",
                        expr.span,
                        "tensor and view values require an immediate matching let binding",
                    ));
                    return None;
                }
                return Some(checked);
            }
            A::Try(call) => {
                let A::TensorCall { op, args } = &call.kind else {
                    self.errors.push(Diagnostic::new(
                        "N003",
                        expr.span,
                        "try requires one fallible tensor builtin call",
                    ));
                    return None;
                };
                let checked = self.tensor_call(call.span, *op, args, true)?;
                if !scalar(checked.ty) {
                    self.errors.push(Diagnostic::new(
                        "N004",
                        expr.span,
                        "tensor and view values require an immediate matching let binding",
                    ));
                    return None;
                }
                return Some(checked);
            }
            A::Int(value) => (I::Int(*value), Type::I64),
            A::Float(bits) => (I::Float(*bits), Type::F64),
            A::Bool(value) => (I::Bool(*value), Type::Bool),
            A::Unit => (I::Unit, Type::Unit),
            A::Var(name) => {
                let local = self.named_local(name, expr.span)?;
                let ty = self.state.locals[local].ty;
                if !scalar(ty) {
                    if matches!(
                        ty,
                        Type::Concrete(ast::Type::Array { .. })
                            | Type::Tensor
                            | Type::Concrete(ast::Type::Record(_))
                            | Type::Concrete(ast::Type::Union(_))
                            | Type::Parameter(_)
                            | Type::Application(_)
                    ) && !self.owner_available(local, expr.span)
                    {
                        return None;
                    }
                    self.errors.push(
                        Diagnostic::new(
                            "O004",
                            expr.span,
                            "an aggregate or tensor_error cannot be used as a scalar value",
                        )
                        .with_related(
                            self.state.locals[local].span,
                            "use a field, index, or supported scalar operation to obtain a scalar",
                        ),
                    );
                    return None;
                }
                (I::Var(local), ty)
            }
            A::Field {
                name,
                field,
                field_span,
            } => {
                let (base, field, ty, _) =
                    self.record_field(name, expr.span, field, *field_span, false)?;
                if ty.transferred() {
                    self.errors.push(Diagnostic::new("O004", expr.span, "an aggregate field requires an explicit owned initializer or return to transfer it"));
                    return None;
                }
                (I::Field { base, field }, ty)
            }
            A::Record { fields, .. } => {
                for field in fields {
                    self.expression(&field.value);
                }
                self.errors.push(Diagnostic::new(
                    "O004",
                    expr.span,
                    "record construction is permitted only as a record let initializer",
                ));
                return None;
            }
            A::Union { fields, .. } => {
                for field in fields {
                    self.expression(&field.value);
                }
                self.errors.push(Diagnostic::new("O004", expr.span,
                    "union construction requires an immediate union let binding or private union return"));
                return None;
            }
            A::Array(elements) => {
                for element in elements {
                    self.expression(element);
                }
                self.errors.push(Diagnostic::new(
                    "O004",
                    expr.span,
                    "array literals are permitted only as array let initializers",
                ));
                return None;
            }
            A::Borrow { range, .. } => {
                if let Some((start, end)) = range {
                    self.expression(start);
                    self.expression(end);
                }
                self.errors.push(Diagnostic::new(
                    "O004",
                    expr.span,
                    "borrows are permitted only as slice let initializers",
                ));
                return None;
            }
            A::Index { name, index } => {
                let index = self.expression(index);
                let index = self.expect_type(index, Type::I64, "array or slice index", None);
                let base = self.collection(name, expr.span, false)?;
                (
                    I::Index {
                        base,
                        index: Box::new(index?.payload),
                    },
                    self.element_type(base),
                )
            }
            A::Len(name) => {
                let local = self.collection(name, expr.span, false)?;
                (I::Len(local), Type::I64)
            }
            A::Unary { op, value } => {
                let value = self.expression(value);
                let value = match op {
                    UnaryOp::Neg => self.expect_numeric(value, "unary operand"),
                    UnaryOp::Not => self.expect_type(value, Type::Bool, "unary operand", None),
                    UnaryOp::BitNot => self.expect_type(value, Type::I64, "unary operand", None),
                };
                let value = value?;
                let ty = value.ty;
                (
                    I::Unary {
                        op: *op,
                        value: Box::new(value.payload),
                    },
                    ty,
                )
            }
            A::Binary { op, left, right } => {
                let left = self.expression(left);
                let right = self.expression(right);
                let (left, right, ty) = match op {
                    BinaryOp::Eq | BinaryOp::Ne => {
                        let left = left?;
                        let right = self.expect_type(right, left.ty, "equality operand", None)?;
                        (left, right, Type::Bool)
                    }
                    BinaryOp::Add
                    | BinaryOp::Sub
                    | BinaryOp::Mul
                    | BinaryOp::Div
                    | BinaryOp::Lt
                    | BinaryOp::Le
                    | BinaryOp::Gt
                    | BinaryOp::Ge => {
                        let left = self.expect_numeric(left, "left operand");
                        let right = self.expect_numeric(right, "right operand");
                        let left = left?;
                        let right = self.expect_type(right, left.ty, "right operand", None)?;
                        let result_type = match op {
                            BinaryOp::Lt | BinaryOp::Le | BinaryOp::Gt | BinaryOp::Ge => Type::Bool,
                            _ => left.ty,
                        };
                        (left, right, result_type)
                    }
                    _ => {
                        let operand_type = match op {
                            BinaryOp::And | BinaryOp::Or => Type::Bool,
                            _ => Type::I64,
                        };
                        // Both sides are checked before propagating either failure.
                        let left = self.expect_type(left, operand_type, "left operand", None);
                        let right = self.expect_type(right, operand_type, "right operand", None);
                        let result_type = match op {
                            BinaryOp::And | BinaryOp::Or => Type::Bool,
                            _ => Type::I64,
                        };
                        (left?, right?, result_type)
                    }
                };
                (
                    I::Binary {
                        op: *op,
                        left: Box::new(left.payload),
                        right: Box::new(right.payload),
                    },
                    ty,
                )
            }
            A::Intrinsic { op, args } => {
                let mut checked: Vec<_> = args.iter().map(|arg| self.expression(arg)).collect();
                let (name, input, output) = match op {
                    FloatIntrinsic::FromI64 => ("f64_from_i64", Type::I64, Type::F64),
                    FloatIntrinsic::ToBits => ("f64_bits", Type::F64, Type::I64),
                    FloatIntrinsic::FromBits => ("f64_from_bits", Type::I64, Type::F64),
                };
                let arity_ok = args.len() == 1;
                if !arity_ok {
                    self.errors.push(Diagnostic::new(
                        "T008",
                        expr.span,
                        format!("builtin `{name}` expects 1 argument, found {}", args.len()),
                    ));
                }
                if let Some(value) = checked.first_mut() {
                    *value = self.expect_type(value.take(), input, "builtin argument", None);
                }
                if !arity_ok {
                    return None;
                }
                let value = checked.into_iter().next().flatten()?;
                (
                    I::Intrinsic {
                        op: *op,
                        value: Box::new(value.payload),
                    },
                    output,
                )
            }
            A::Call { name, args } => {
                return self.function_call(expr.span, name, args, false);
            }
            A::GenericCall {
                name,
                type_args,
                args,
            } => {
                return self.generic_call(expr.span, name, type_args, args, false);
            }
        };
        Some(ir::Expr::new(kind, ty, expr.span))
    }
}
