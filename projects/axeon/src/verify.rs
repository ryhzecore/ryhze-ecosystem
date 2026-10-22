//! Independent structural/type checks for the current structured IR.
//!
//! This is not an ownership, effect, lifetime, source-map or optimization
//! equivalence proof. Raw backend APIs deliberately remain available for the
//! runtime's malformed-tag defense tests. See `docs/ir-verifier-design.md`.
use crate::ast::{InlineLayouts, RecordField};
use crate::diagnostic::Span;
use crate::ir::{self, BinaryOp, ExprKind, StmtKind, TensorArg, TensorIntrinsic, Type};
use std::collections::{HashMap, HashSet};

pub const MAX_ERRORS: usize = 64;
/// The source limit is 2048 nodes. Lowering adds implicit slice bounds; this
/// deliberately conservative IR limit is separate from the source contract.
pub const MAX_IR_NODES: usize = 8192;
pub const MAX_IR_DEPTH: usize = 256;
const MAX_LOCALS: usize = 256;
const NEXT: u8 = 1;
const RETURN: u8 = 2;
const BREAK: u8 = 4;
const CONTINUE: u8 = 8;

/// The span is untrusted provenance, not a validated source-map location.
/// Callers must use a known-valid fallback location when rendering failures.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerificationError {
    pub code: &'static str,
    pub function: Option<usize>,
    pub span: Span,
    pub message: String,
}

/// Validate without mutating or cloning the program. Errors are returned in
/// declaration/traversal order and capped at `MAX_ERRORS` in every profile.
pub fn verify(program: &ir::Program) -> Result<(), Vec<VerificationError>> {
    let mut verifier = Verifier {
        program,
        errors: Vec::new(),
        function: None,
        records: HashMap::new(),
        unions: HashMap::new(),
        layouts: None,
    };
    verifier.declarations();
    // Layout construction must never receive unresolved or ambiguous IDs.
    if verifier.errors.is_empty() {
        match InlineLayouts::build(&program.records, &program.unions) {
            Ok(layouts) => verifier.layouts = Some(layouts),
            Err(error) => verifier.error("IR008", error.span, error.message),
        }
    }
    if verifier.errors.is_empty() {
        for (index, function) in program.functions.iter().enumerate() {
            if verifier.full() {
                break;
            }
            verifier.function = Some(index);
            FunctionVerifier {
                verifier: &mut verifier,
                function,
                active: vec![false; function.locals.len().min(MAX_LOCALS)],
                declared: vec![false; function.locals.len().min(MAX_LOCALS)],
                nodes: 0,
            }
            .run();
        }
    }
    if verifier.errors.is_empty() {
        Ok(())
    } else {
        Err(verifier.errors)
    }
}

struct Verifier<'a> {
    program: &'a ir::Program,
    errors: Vec<VerificationError>,
    function: Option<usize>,
    records: HashMap<ir::NominalId, usize>,
    unions: HashMap<ir::NominalId, usize>,
    layouts: Option<InlineLayouts>,
}
impl Verifier<'_> {
    fn full(&self) -> bool {
        self.errors.len() >= MAX_ERRORS
    }
    fn error(&mut self, code: &'static str, span: Span, message: impl Into<String>) {
        if !self.full() {
            self.errors.push(VerificationError {
                code,
                function: self.function,
                span,
                message: message.into(),
            });
        }
    }
    fn require(&mut self, condition: bool, code: &'static str, span: Span, message: &str) {
        if !condition {
            self.error(code, span, message);
        }
    }
    fn span(&mut self, span: Span) {
        self.require(
            span.start <= span.end,
            "IR007",
            span,
            "reversed source span",
        );
    }
    fn ty(&mut self, ty: Type, span: Span) -> bool {
        let valid = match ty {
            Type::Record(id) => self.records.contains_key(&id),
            Type::Union(id) => self.unions.contains_key(&id),
            Type::Array { length, .. } => (1..=32).contains(&length),
            _ => true,
        };
        if !valid {
            self.error("IR003", span, format!("invalid or unresolved type {ty}"));
        }
        valid
    }
    fn declarations(&mut self) {
        let program = self.program;
        if program.records.len().saturating_add(program.unions.len()) > 256
            || program.functions.is_empty()
            || program.functions.len() > 1024
        {
            self.error(
                "IR001",
                Span::default(),
                "program declaration budget exceeded or no functions",
            );
            return;
        }
        let mut identities = HashSet::new();
        for (index, record) in program.records.iter().enumerate() {
            self.span(record.name_span);
            self.require(
                identities.insert(record.id),
                "IR002",
                record.name_span,
                "duplicate nominal identity",
            );
            self.records.insert(record.id, index);
            self.require(
                (1..=32).contains(&record.fields.len()),
                "IR001",
                record.name_span,
                "record requires 1..32 fields",
            );
        }
        for (index, union) in program.unions.iter().enumerate() {
            self.span(union.name_span);
            self.require(
                identities.insert(union.id),
                "IR002",
                union.name_span,
                "duplicate nominal identity",
            );
            self.unions.insert(union.id, index);
            self.require(
                (1..=32).contains(&union.variants.len()),
                "IR001",
                union.name_span,
                "union requires 1..32 variants",
            );
            let mut names = HashSet::new();
            for variant in union.variants.iter().take(32) {
                self.span(variant.name_span);
                self.require(
                    names.insert(variant.name.as_str()),
                    "IR002",
                    variant.name_span,
                    "duplicate variant name",
                );
                self.require(
                    variant.fields.len() <= 32,
                    "IR001",
                    variant.name_span,
                    "variant exceeds 32 fields",
                );
            }
        }
        for record in &program.records {
            self.fields(&record.fields);
        }
        for union in &program.unions {
            for variant in union.variants.iter().take(32) {
                self.fields(&variant.fields);
            }
        }
        match program.functions.get(program.main) {
            Some(main) => self.require(
                main.params.is_empty() && main.return_type == Type::I64,
                "IR003",
                main.span,
                "entry function must have no parameters and return i64",
            ),
            None => self.error(
                "IR002",
                Span::default(),
                "entry function index is out of bounds",
            ),
        }
    }
    fn fields(&mut self, fields: &[RecordField]) {
        let mut names = HashSet::new();
        for field in fields.iter().take(32) {
            self.span(field.name_span);
            self.ty(field.ty, field.name_span);
            self.require(
                value_type(field.ty),
                "IR003",
                field.name_span,
                "nominal field must be scalar or inline owned",
            );
            self.require(
                names.insert(field.name.as_str()),
                "IR002",
                field.name_span,
                "duplicate field name",
            );
        }
    }
    fn record(&self, id: ir::NominalId) -> Option<&ir::Record> {
        self.records
            .get(&id)
            .and_then(|index| self.program.records.get(*index))
    }
    fn union(&self, id: ir::NominalId) -> Option<&ir::Union> {
        self.unions
            .get(&id)
            .and_then(|index| self.program.unions.get(*index))
    }
}

fn scalar(ty: Type) -> bool {
    matches!(ty, Type::I64 | Type::F64 | Type::Bool | Type::Unit)
}
fn numeric(ty: Type) -> bool {
    matches!(ty, Type::I64 | Type::F64)
}
fn value_type(ty: Type) -> bool {
    scalar(ty) || ty.inline_owned()
}
fn tensor(ty: Type) -> bool {
    matches!(ty, Type::Tensor | Type::TensorView { .. })
}

#[derive(Clone, Copy, Default)]
struct Context {
    loops: usize,
    /// A protected tensor body cannot exit to a containing function/loop.
    protected: Option<usize>,
    root: bool,
}
#[derive(Clone, Copy)]
enum Use {
    Scalar,
    Initializer,
    Argument,
    Return,
}

struct FunctionVerifier<'a, 'b> {
    verifier: &'a mut Verifier<'b>,
    function: &'b ir::Function,
    active: Vec<bool>,
    declared: Vec<bool>,
    nodes: usize,
}
impl FunctionVerifier<'_, '_> {
    fn error(&mut self, code: &'static str, span: Span, message: impl Into<String>) {
        self.verifier.error(code, span, message);
    }
    fn require(&mut self, condition: bool, code: &'static str, span: Span, message: &str) {
        self.verifier.require(condition, code, span, message);
    }
    fn enter(&mut self, span: Span, depth: usize) -> bool {
        if self.verifier.full() || self.nodes > MAX_IR_NODES {
            return false;
        }
        self.nodes += 1;
        self.verifier.span(span);
        if self.nodes > MAX_IR_NODES || depth > MAX_IR_DEPTH {
            self.error("IR001", span, "function IR node/depth budget exceeded");
            return false;
        }
        true
    }
    fn local(&mut self, id: usize, span: Span) -> Option<&ir::Local> {
        if !self.active.get(id).copied().unwrap_or(false) {
            self.error(
                "IR004",
                span,
                format!("local {id} is out of bounds or not in scope"),
            );
            return None;
        }
        self.function.locals.get(id)
    }
    fn bind(&mut self, id: usize, span: Span) -> Option<Type> {
        if id >= self.declared.len() {
            self.error(
                "IR002",
                span,
                format!("binding local {id} is out of bounds"),
            );
            return None;
        }
        self.require(
            !self.declared[id],
            "IR004",
            span,
            "local has multiple binding sites",
        );
        self.declared[id] = true;
        self.active[id] = true;
        self.function.locals.get(id).map(|local| local.ty)
    }
    fn equal(&mut self, actual: Type, expected: Type, span: Span) {
        if actual != expected {
            self.error(
                "IR003",
                span,
                format!("expected {expected}, found {actual}"),
            );
        }
    }
    fn run(mut self) {
        let function = self.function;
        self.verifier.span(function.span);
        if function.locals.len() > MAX_LOCALS || function.params.len() > 32 {
            self.error(
                "IR001",
                function.span,
                "function local/parameter budget exceeded",
            );
            return;
        }
        self.verifier.ty(function.return_type, function.span);
        self.require(
            value_type(function.return_type),
            "IR003",
            function.span,
            "unsupported function return type",
        );
        let mut slots = 0usize;
        for local in &function.locals {
            self.verifier.span(local.span);
            if self.verifier.ty(local.ty, local.span) && local.ty.inline_owned() {
                if let Some(layouts) = &self.verifier.layouts {
                    slots = slots.saturating_add(layouts.slots(local.ty));
                }
            }
        }
        self.require(
            slots <= 256,
            "IR001",
            function.span,
            "function inline owned storage exceeds 256 slots",
        );
        for id in &function.params {
            if let Some(ty) = self.bind(*id, function.span) {
                self.require(
                    value_type(ty),
                    "IR003",
                    function.span,
                    "unsupported parameter type",
                );
                self.require(
                    !function.locals[*id].mutable,
                    "IR004",
                    function.span,
                    "parameter must be immutable",
                );
            }
        }
        let flow = self.block(
            &function.body,
            Context {
                root: true,
                ..Context::default()
            },
            0,
        );
        self.require(
            flow == RETURN,
            "IR005",
            function.span,
            "function has a fallthrough or escaping loop exit",
        );
        for (id, declared) in self.declared.iter().copied().enumerate() {
            if !declared {
                self.verifier.error(
                    "IR004",
                    function.locals[id].span,
                    format!("local {id} has no binding site"),
                );
            }
        }
    }
    fn block(&mut self, block: &ir::Block, context: Context, depth: usize) -> u8 {
        if !self.enter(block.span, depth) {
            return NEXT;
        }
        if block.stmts.len() > MAX_IR_NODES || block.cleanup.len() > MAX_LOCALS {
            self.error(
                "IR001",
                block.span,
                "block statement/cleanup budget exceeded",
            );
            return NEXT;
        }
        let saved = self.active.clone();
        let mut cleanup = Vec::new();
        let mut flow = NEXT;
        for stmt in &block.stmts {
            if self.verifier.full() || self.nodes > MAX_IR_NODES {
                break;
            }
            self.require(
                flow & NEXT != 0,
                "IR005",
                stmt.span,
                "unreachable statement",
            );
            let exits = self.statement(stmt, context, depth + 1);
            if flow & NEXT != 0 {
                flow = (flow & !NEXT) | exits;
            }
            if let StmtKind::Let { local, .. } = &stmt.kind {
                if self
                    .function
                    .locals
                    .get(*local)
                    .is_some_and(|local| tensor(local.ty))
                {
                    cleanup.push(*local);
                }
            }
        }
        self.require(
            block.cleanup == cleanup,
            "IR006",
            block.span,
            "cleanup must list direct tensor/view bindings once in declaration order",
        );
        self.active = saved;
        flow
    }
    fn statement(&mut self, stmt: &ir::Stmt, context: Context, depth: usize) -> u8 {
        if !self.enter(stmt.span, depth) {
            return NEXT;
        }
        let child = Context {
            root: false,
            ..context
        };
        match &stmt.kind {
            StmtKind::Let { local, value } => {
                self.expr(value, Use::Initializer, child, depth + 1);
                if let Some(ty) = self.bind(*local, stmt.span) {
                    self.equal(value.ty, ty, value.span);
                    self.require(
                        ty != Type::TensorError,
                        "IR003",
                        stmt.span,
                        "tensor errors bind only in handlers",
                    );
                }
            }
            StmtKind::Assign { local, value } => {
                let target = self
                    .local(*local, stmt.span)
                    .map(|local| (local.ty, local.mutable));
                self.expr(value, Use::Scalar, child, depth + 1);
                if let Some((ty, mutable)) = target {
                    self.require(
                        mutable && scalar(ty),
                        "IR003",
                        stmt.span,
                        "assignment requires a mutable scalar binding",
                    );
                    self.equal(value.ty, ty, value.span);
                }
            }
            StmtKind::IndexAssign { base, index, value } => {
                let element = self.collection(*base, true, stmt.span);
                self.scalar_expr(index, Type::I64, child, depth + 1);
                self.expr(value, Use::Scalar, child, depth + 1);
                if let Some(ty) = element {
                    self.equal(value.ty, ty, value.span);
                }
            }
            StmtKind::FieldAssign { base, field, value } => {
                let ty = self.path(*base, field, stmt.span);
                let mutable = self
                    .local(*base, stmt.span)
                    .is_some_and(|local| local.mutable);
                self.require(
                    mutable,
                    "IR004",
                    stmt.span,
                    "field assignment requires a mutable root",
                );
                self.expr(value, Use::Scalar, child, depth + 1);
                if let Some(ty) = ty {
                    self.require(
                        scalar(ty),
                        "IR003",
                        stmt.span,
                        "field assignment requires a scalar leaf",
                    );
                    self.equal(value.ty, ty, value.span);
                }
            }
            StmtKind::Match { owner, arms } => {
                return self.match_stmt(*owner, arms, stmt.span, child, depth + 1)
            }
            StmtKind::If {
                condition,
                then_block,
                else_block,
            } => {
                self.scalar_expr(condition, Type::Bool, child, depth + 1);
                let left = self.block(then_block, child, depth + 1);
                let right = else_block
                    .as_ref()
                    .map_or(NEXT, |block| self.block(block, child, depth + 1));
                return left | right;
            }
            StmtKind::While { condition, body } => {
                self.scalar_expr(condition, Type::Bool, child, depth + 1);
                let flow = self.block(
                    body,
                    Context {
                        loops: child.loops + 1,
                        ..child
                    },
                    depth + 1,
                );
                return NEXT | (flow & RETURN);
            }
            StmtKind::Return(value) => {
                self.require(
                    context.protected.is_none(),
                    "IR005",
                    stmt.span,
                    "return escapes a protected tensor body",
                );
                self.expr(value, Use::Return, child, depth + 1);
                self.equal(value.ty, self.function.return_type, value.span);
                return RETURN;
            }
            StmtKind::Print(value) => {
                self.expr(value, Use::Scalar, child, depth + 1);
                self.require(
                    matches!(value.ty, Type::I64 | Type::F64 | Type::Bool),
                    "IR003",
                    value.span,
                    "print requires i64, f64 or bool",
                );
            }
            StmtKind::Expr(value) => {
                self.expr(value, Use::Scalar, child, depth + 1);
                self.require(
                    matches!(
                        value.kind,
                        ExprKind::Call { .. }
                            | ExprKind::Intrinsic { .. }
                            | ExprKind::TensorCall { .. }
                    ),
                    "IR005",
                    value.span,
                    "expression statement requires a scalar call",
                );
            }
            StmtKind::Block(block) => return self.block(block, child, depth + 1),
            StmtKind::TensorScope {
                budget,
                body,
                error_local,
                handler,
            } => {
                self.require(
                    context.root && context.protected.is_none(),
                    "IR005",
                    stmt.span,
                    "tensor scope must be directly in the function body",
                );
                self.scalar_expr(budget, Type::I64, child, depth + 1);
                let body_flow = self.block(
                    body,
                    Context {
                        protected: Some(context.loops),
                        ..child
                    },
                    depth + 1,
                );
                self.require(
                    body_flow == NEXT,
                    "IR005",
                    body.span,
                    "protected tensor body must fall through normally",
                );
                let saved = self.active.clone();
                if let Some(ty) = self.bind(*error_local, stmt.span) {
                    self.equal(ty, Type::TensorError, stmt.span);
                    self.require(
                        !self.function.locals[*error_local].mutable,
                        "IR004",
                        stmt.span,
                        "catch binding must be immutable",
                    );
                }
                let handler_flow = self.block(handler, child, depth + 1);
                self.active = saved;
                return NEXT | handler_flow;
            }
            StmtKind::TensorSet { base, index, value } => {
                self.require(
                    context.protected.is_some(),
                    "IR005",
                    stmt.span,
                    "tensor write outside protected body",
                );
                self.tensor_local(*base, true, stmt.span);
                self.scalar_expr(index, Type::I64, child, depth + 1);
                self.scalar_expr(value, Type::F64, child, depth + 1);
            }
            StmtKind::Break | StmtKind::Continue => {
                self.require(
                    context.loops > context.protected.unwrap_or(0),
                    "IR005",
                    stmt.span,
                    "loop exit has no reachable enclosing loop",
                );
                return if matches!(stmt.kind, StmtKind::Break) {
                    BREAK
                } else {
                    CONTINUE
                };
            }
        }
        NEXT
    }
    fn match_stmt(
        &mut self,
        owner: usize,
        arms: &[ir::MatchArm],
        span: Span,
        context: Context,
        depth: usize,
    ) -> u8 {
        let ty = self.local(owner, span).map(|local| local.ty);
        let union = match ty {
            Some(Type::Union(id)) => self.verifier.union(id),
            _ => None,
        };
        let Some(union) = union else {
            self.error("IR003", span, "match owner must have a resolved union type");
            return NEXT;
        };
        // The declaration is independent of the mutable traversal state.
        let program = self.verifier.program;
        let id = union.id;
        let Some(union) = program.unions.iter().find(|union| union.id == id) else {
            return NEXT;
        };
        if arms.len() > 32 {
            self.error("IR001", span, "match exceeds 32 arms");
            return NEXT;
        }
        let mut variants = HashSet::new();
        let mut flow = 0;
        for arm in arms {
            self.require(
                variants.insert(arm.variant),
                "IR005",
                arm.body.span,
                "duplicate match variant",
            );
            let Some(variant) = union.variants.get(arm.variant) else {
                self.error("IR002", arm.body.span, "match variant is out of bounds");
                continue;
            };
            if arm.bindings.len() > 32 {
                self.error("IR001", arm.body.span, "match exceeds 32 bindings");
                continue;
            }
            let saved = self.active.clone();
            let mut fields = HashSet::new();
            for (field, local) in &arm.bindings {
                self.require(
                    fields.insert(*field),
                    "IR005",
                    arm.body.span,
                    "duplicate match field",
                );
                if let Some(ty) = self.bind(*local, arm.body.span) {
                    self.require(
                        !self.function.locals[*local].mutable,
                        "IR004",
                        arm.body.span,
                        "match bindings must be immutable",
                    );
                    if let Some(field) = variant.fields.get(*field) {
                        self.equal(ty, field.ty, arm.body.span);
                    } else {
                        self.error("IR002", arm.body.span, "match field index is out of bounds");
                    }
                }
            }
            self.require(
                fields.len() == variant.fields.len(),
                "IR005",
                arm.body.span,
                "match must bind every variant field",
            );
            flow |= self.block(&arm.body, context, depth + 1);
            self.active = saved;
        }
        self.require(
            variants.len() == union.variants.len(),
            "IR005",
            span,
            "match must cover every union variant",
        );
        flow
    }
    fn collection(&mut self, id: usize, write: bool, span: Span) -> Option<Type> {
        let local = self.local(id, span)?;
        let (ty, writable) = match local.ty {
            Type::Array { element, .. } => (element.scalar_type(), local.mutable),
            Type::Slice { element, mutable } => (element.scalar_type(), mutable),
            _ => {
                self.error("IR003", span, "collection must be an array or slice");
                return None;
            }
        };
        self.require(
            !write || writable,
            "IR004",
            span,
            "collection lacks write capability",
        );
        Some(ty)
    }
    fn path(&mut self, id: usize, path: &[usize], span: Span) -> Option<Type> {
        if path.is_empty() || path.len() > 32 {
            self.error("IR001", span, "field path must contain 1..32 indices");
            return None;
        }
        let mut ty = self.local(id, span)?.ty;
        for index in path {
            ty = match ty {
                Type::Record(id) => match self
                    .verifier
                    .record(id)
                    .and_then(|record| record.fields.get(*index))
                {
                    Some(field) => field.ty,
                    None => {
                        self.error("IR002", span, "field path index is out of bounds");
                        return None;
                    }
                },
                _ => {
                    self.error("IR003", span, "field path traverses a non-record value");
                    return None;
                }
            };
        }
        Some(ty)
    }
    fn scalar_expr(&mut self, expr: &ir::Expr, ty: Type, context: Context, depth: usize) {
        self.expr(expr, Use::Scalar, context, depth);
        self.equal(expr.ty, ty, expr.span);
    }
    fn expr(&mut self, expr: &ir::Expr, usage: Use, context: Context, depth: usize) {
        if !self.enter(expr.span, depth) {
            return;
        }
        self.verifier.ty(expr.ty, expr.span);
        if matches!(usage, Use::Scalar) {
            self.require(
                scalar(expr.ty),
                "IR003",
                expr.span,
                "scalar expression has a non-scalar type",
            );
        }
        if expr.ty.inline_owned() {
            self.require(
                !matches!(usage, Use::Scalar),
                "IR003",
                expr.span,
                "owned result must be captured or forwarded",
            );
            if matches!(usage, Use::Argument) {
                self.require(
                    matches!(expr.kind, ExprKind::Move(_)),
                    "IR005",
                    expr.span,
                    "owned argument must be a bare owner move",
                );
            }
        }
        if matches!(
            expr.ty,
            Type::Slice { .. } | Type::Tensor | Type::TensorView { .. }
        ) {
            self.require(
                matches!(usage, Use::Initializer),
                "IR005",
                expr.span,
                "borrowed/tensor value is only an initializer",
            );
        }
        let expected = match &expr.kind {
            ExprKind::Int(_) => Some(Type::I64),
            ExprKind::Float(_) => Some(Type::F64),
            ExprKind::Bool(_) => Some(Type::Bool),
            ExprKind::Unit => Some(Type::Unit),
            ExprKind::Var(id) => {
                let ty = self.local(*id, expr.span).map(|local| local.ty);
                self.require(
                    ty.is_some_and(scalar),
                    "IR003",
                    expr.span,
                    "Var requires a scalar binding",
                );
                ty
            }
            ExprKind::Move(id) => {
                let ty = self.local(*id, expr.span).map(|local| local.ty);
                self.require(
                    ty.is_some_and(|ty| ty.inline_owned() || ty == Type::Tensor),
                    "IR003",
                    expr.span,
                    "Move requires an inline or tensor owner",
                );
                ty
            }
            ExprKind::Field { base, field } | ExprKind::MoveField { base, field } => {
                let ty = self.path(*base, field, expr.span);
                let moving = matches!(expr.kind, ExprKind::MoveField { .. });
                self.require(
                    ty.is_some_and(|ty| {
                        if moving {
                            ty.inline_owned()
                        } else {
                            scalar(ty)
                        }
                    }),
                    "IR003",
                    expr.span,
                    "field access kind does not match leaf type",
                );
                ty
            }
            ExprKind::Array(elements) => {
                if elements.len() > 32 {
                    self.error("IR001", expr.span, "array constructor exceeds 32 elements");
                    return;
                }
                let Type::Array { element, length } = expr.ty else {
                    self.error("IR003", expr.span, "array constructor has non-array type");
                    return;
                };
                self.require(
                    elements.len() == length,
                    "IR009",
                    expr.span,
                    "array constructor length disagrees with type",
                );
                for value in elements {
                    self.scalar_expr(value, element.scalar_type(), context, depth + 1);
                }
                None
            }
            ExprKind::Record(fields) => {
                let program = self.verifier.program;
                let declaration = match expr.ty {
                    Type::Record(id) => self
                        .verifier
                        .records
                        .get(&id)
                        .and_then(|index| program.records.get(*index)),
                    _ => None,
                };
                if let Some(record) = declaration {
                    self.constructor(fields, &record.fields, expr.span, context, depth + 1);
                } else {
                    self.error(
                        "IR003",
                        expr.span,
                        "record constructor has unresolved/non-record type",
                    );
                }
                None
            }
            ExprKind::Union {
                layout_variant,
                variant,
                fields,
            } => {
                self.require(
                    layout_variant == variant,
                    "IR009",
                    expr.span,
                    "stored union tag differs from checked layout variant",
                );
                let program = self.verifier.program;
                let declaration = match expr.ty {
                    Type::Union(id) => self
                        .verifier
                        .unions
                        .get(&id)
                        .and_then(|index| program.unions.get(*index)),
                    _ => None,
                };
                if let Some(variant) =
                    declaration.and_then(|union| union.variants.get(*layout_variant))
                {
                    self.constructor(fields, &variant.fields, expr.span, context, depth + 1);
                } else {
                    self.error(
                        "IR002",
                        expr.span,
                        "union constructor type/layout variant is unresolved",
                    );
                }
                None
            }
            ExprKind::Borrow {
                owner,
                mutable,
                start,
                end,
            } => {
                let owner = self
                    .local(*owner, expr.span)
                    .map(|local| (local.ty, local.mutable));
                self.scalar_expr(start, Type::I64, context, depth + 1);
                self.scalar_expr(end, Type::I64, context, depth + 1);
                match owner {
                    Some((Type::Array { element, .. }, writable)) => {
                        self.require(
                            !*mutable || writable,
                            "IR004",
                            expr.span,
                            "mutable borrow requires a mutable array owner",
                        );
                        Some(Type::Slice {
                            element,
                            mutable: *mutable,
                        })
                    }
                    _ => {
                        self.error("IR003", expr.span, "borrow requires a direct array owner");
                        None
                    }
                }
            }
            ExprKind::Index { base, index } => {
                let ty = self.collection(*base, false, expr.span);
                self.scalar_expr(index, Type::I64, context, depth + 1);
                ty
            }
            ExprKind::Len(id) => {
                self.collection(*id, false, expr.span);
                Some(Type::I64)
            }
            ExprKind::Unary { op, value } => {
                self.expr(value, Use::Scalar, context, depth + 1);
                let valid = match op {
                    ir::UnaryOp::Neg => numeric(value.ty),
                    ir::UnaryOp::Not => value.ty == Type::Bool,
                    ir::UnaryOp::BitNot => value.ty == Type::I64,
                };
                self.require(valid, "IR003", expr.span, "invalid unary operand type");
                Some(value.ty)
            }
            ExprKind::Binary { op, left, right } => {
                self.expr(left, Use::Scalar, context, depth + 1);
                self.expr(right, Use::Scalar, context, depth + 1);
                self.equal(right.ty, left.ty, right.span);
                use BinaryOp::*;
                let valid = match op {
                    Add | Sub | Mul | Div | Lt | Le | Gt | Ge => numeric(left.ty),
                    Rem | Shl | Shr | BitAnd | BitOr | BitXor => left.ty == Type::I64,
                    Eq | Ne => scalar(left.ty),
                    And | Or => left.ty == Type::Bool,
                };
                self.require(valid, "IR003", expr.span, "invalid binary operand type");
                Some(if matches!(op, Eq | Ne | Lt | Le | Gt | Ge | And | Or) {
                    Type::Bool
                } else {
                    left.ty
                })
            }
            ExprKind::Call { function, args } => {
                if args.len() > 32 {
                    self.error("IR001", expr.span, "call exceeds 32 arguments");
                    return;
                }
                let program = self.verifier.program;
                let target = program.functions.get(*function);
                if let Some(target) = target {
                    self.require(
                        args.len() == target.params.len(),
                        "IR003",
                        expr.span,
                        "call arity does not match signature",
                    );
                } else {
                    self.error("IR002", expr.span, "call function index is out of bounds");
                }
                for (index, arg) in args.iter().enumerate() {
                    self.expr(arg, Use::Argument, context, depth + 1);
                    if let Some(ty) = target
                        .and_then(|target| {
                            target
                                .params
                                .get(index)
                                .and_then(|id| target.locals.get(*id))
                        })
                        .map(|local| local.ty)
                    {
                        self.equal(arg.ty, ty, arg.span);
                    }
                }
                target.map(|target| target.return_type)
            }
            ExprKind::Intrinsic { op, value } => {
                let (input, output) = match op {
                    ir::FloatIntrinsic::FromI64 => (Type::I64, Type::F64),
                    ir::FloatIntrinsic::ToBits => (Type::F64, Type::I64),
                    ir::FloatIntrinsic::FromBits => (Type::I64, Type::F64),
                };
                self.scalar_expr(value, input, context, depth + 1);
                Some(output)
            }
            ExprKind::TensorCall { op, args } => {
                self.tensor_call(*op, args, expr.ty, expr.span, context, depth + 1)
            }
        };
        if let Some(expected) = expected {
            self.equal(expr.ty, expected, expr.span);
        }
    }
    fn constructor(
        &mut self,
        values: &[(usize, ir::Expr)],
        fields: &[RecordField],
        span: Span,
        context: Context,
        depth: usize,
    ) {
        if values.len() > 32 {
            self.error("IR001", span, "constructor exceeds 32 fields");
            return;
        }
        let mut indices = HashSet::new();
        for (index, value) in values {
            self.require(
                indices.insert(*index),
                "IR009",
                value.span,
                "constructor repeats a field",
            );
            self.expr(value, Use::Initializer, context, depth);
            if let Some(field) = fields.get(*index) {
                self.equal(value.ty, field.ty, value.span);
            } else {
                self.error(
                    "IR002",
                    value.span,
                    "constructor field index is out of bounds",
                );
            }
        }
        self.require(
            indices.len() == fields.len(),
            "IR009",
            span,
            "constructor must initialize every field",
        );
    }
    fn tensor_local(&mut self, id: usize, write: bool, span: Span) {
        if let Some((ty, mutable)) = self.local(id, span).map(|local| (local.ty, local.mutable)) {
            self.require(
                tensor(ty),
                "IR003",
                span,
                "tensor operand is not a tensor or view",
            );
            self.require(
                !write
                    || (ty == Type::Tensor && mutable)
                    || ty == Type::TensorView { mutable: true },
                "IR004",
                span,
                "tensor operand lacks write capability",
            );
        }
    }
    fn tensor_call(
        &mut self,
        op: TensorIntrinsic,
        args: &[TensorArg],
        result: Type,
        span: Span,
        context: Context,
        depth: usize,
    ) -> Option<Type> {
        use TensorIntrinsic::*;
        let observer = matches!(op, ErrorCode | ErrorStart | ErrorEnd);
        self.require(
            observer || context.protected.is_some(),
            "IR005",
            span,
            "tensor operation outside protected body",
        );
        let arity = match op {
            Zeros | View | Sigmoid | Softmax | Sum | Logsumexp | Rank | Len | ErrorCode
            | ErrorStart | ErrorEnd => 1,
            FromArray | Transpose | Add | Mul | Matmul | Get | Dim => 2,
            Slice => 5,
            Set => 3,
        };
        self.require(
            args.len() == arity,
            "IR003",
            span,
            "tensor operation arity mismatch",
        );
        if args.len() > 5 {
            return None;
        }
        self.require(
            op != Set,
            "IR005",
            span,
            "tensor_set must be a TensorSet statement",
        );
        for (index, arg) in args.iter().enumerate() {
            let list =
                (matches!(op, Zeros | FromArray) && index == 0) || (op == Transpose && index == 1);
            let integer = (matches!(op, Get | Dim) && index == 1) || (op == Slice && index > 0);
            match arg {
                TensorArg::List(values) if list => {
                    self.require(
                        values.len() <= 8,
                        "IR001",
                        span,
                        "tensor dimension/permutation list exceeds rank 8",
                    );
                    for value in values.iter().take(8) {
                        self.scalar_expr(value, Type::I64, context, depth + 1);
                    }
                }
                TensorArg::Scalar(value) if integer => {
                    self.scalar_expr(value, Type::I64, context, depth + 1)
                }
                TensorArg::Local {
                    local,
                    span: local_span,
                } if !list && !integer => {
                    self.verifier.span(*local_span);
                    let local = self
                        .local(*local, *local_span)
                        .map(|local| (local.ty, local.mutable));
                    if let Some((ty, mutable)) = local {
                        let valid = if observer {
                            ty == Type::TensorError
                        } else if op == FromArray {
                            matches!(
                                ty,
                                Type::Array {
                                    element: ir::ElementType::F64,
                                    ..
                                } | Type::Slice {
                                    element: ir::ElementType::F64,
                                    ..
                                }
                            )
                        } else if matches!(op, View | Transpose | Slice) {
                            ty == Type::Tensor
                        } else {
                            tensor(ty)
                        };
                        self.require(
                            valid,
                            "IR003",
                            *local_span,
                            "tensor operation has invalid local operand type",
                        );
                        if matches!(op, View | Transpose | Slice) {
                            self.require(
                                result != Type::TensorView { mutable: true } || mutable,
                                "IR004",
                                *local_span,
                                "mutable view requires a mutable owner",
                            );
                        }
                    }
                }
                _ => self.error(
                    "IR003",
                    span,
                    "tensor argument has the wrong representation",
                ),
            }
        }
        Some(match op {
            Zeros | FromArray | Add | Mul | Matmul | Sigmoid | Softmax => Type::Tensor,
            View | Transpose | Slice => {
                self.require(
                    matches!(result, Type::TensorView { .. }),
                    "IR003",
                    span,
                    "view operation must return a tensor view",
                );
                result
            }
            Sum | Logsumexp | Get => Type::F64,
            _ => Type::I64,
        })
    }
}
