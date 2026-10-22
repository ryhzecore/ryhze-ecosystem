use crate::diagnostic::{SourceId, Span};
#[path = "ast/annotations.rs"]
mod annotations;
/// Nominal identity is independent of declaration order and spelling in other modules.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RecordId {
    pub source: SourceId,
    pub offset: usize,
}
/// Source declarations and generated layouts occupy disjoint namespaces.
/// Instance numbers are shared by records and unions within one compilation.
/// Trait and type-parameter binders continue to use declaration RecordId.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NominalId {
    Declared(RecordId),
    Instance(u32),
}
impl NominalId {
    pub fn declaration(self) -> Option<RecordId> {
        match self {
            Self::Declared(id) => Some(id),
            Self::Instance(_) => None,
        }
    }
}
impl From<RecordId> for NominalId {
    fn from(id: RecordId) -> Self {
        Self::Declared(id)
    }
}
impl std::fmt::Display for NominalId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Declared(id) => write!(f, "{}:{}", id.source.0, id.offset),
            Self::Instance(index) => write!(f, "instance:{index}"),
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum ElementType {
    I64,
    F64,
}
impl ElementType {
    pub fn scalar_type(self) -> Type {
        match self {
            Self::I64 => Type::I64,
            Self::F64 => Type::F64,
        }
    }
}
impl std::fmt::Display for ElementType {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::I64 => "i64",
            Self::F64 => "f64",
        })
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Type {
    I64,
    F64,
    Bool,
    Unit,
    Record(NominalId),
    Union(NominalId),
    Array { element: ElementType, length: usize },
    Slice { element: ElementType, mutable: bool },
    Tensor,
    TensorView { mutable: bool },
    TensorError,
}
impl Type {
    pub fn inline_owned(self) -> bool {
        matches!(self, Self::Array { .. } | Self::Record(_) | Self::Union(_))
    }
}

/// Symbolic annotations are source-level contracts and never physical IR types.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TypeParameterId {
    pub binder: RecordId,
    pub index: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum TypeExpr {
    Concrete(Type),
    Parameter(TypeParameterId),
    Application(TypeApplicationId),
}
impl From<Type> for TypeExpr {
    fn from(value: Type) -> Self {
        Self::Concrete(value)
    }
}
#[allow(non_upper_case_globals)]
impl TypeExpr {
    pub const I64: Self = Self::Concrete(Type::I64);
    pub const F64: Self = Self::Concrete(Type::F64);
    pub const Bool: Self = Self::Concrete(Type::Bool);
    pub const Unit: Self = Self::Concrete(Type::Unit);
    pub const Tensor: Self = Self::Concrete(Type::Tensor);
    pub const TensorError: Self = Self::Concrete(Type::TensorError);
    pub fn concrete(self) -> Option<Type> {
        match self {
            Self::Concrete(ty) => Some(ty),
            Self::Parameter(_) | Self::Application(_) => None,
        }
    }
    pub fn inline_owned(self) -> bool {
        matches!(self, Self::Application(_)) || self.concrete().is_some_and(Type::inline_owned)
    }
    pub fn transferred(self) -> bool {
        matches!(self, Self::Parameter(_)) || self.inline_owned()
    }
    pub fn scalar(self) -> bool {
        matches!(self, Self::I64 | Self::F64 | Self::Bool | Self::Unit)
    }
    pub fn numeric(self) -> bool {
        matches!(self, Self::I64 | Self::F64)
    }
}
impl std::fmt::Display for TypeExpr {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Concrete(ty) => ty.fmt(f),
            Self::Parameter(id) => write!(f, "type parameter #{}", id.index),
            Self::Application(id) => write!(f, "nominal application #{}", id.index),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NominalKind {
    Record,
    Union,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NominalHeader {
    pub id: RecordId,
    pub kind: NominalKind,
    pub arity: usize,
    pub name_span: Span,
}
/// An application handle is valid only in its immutable source arena or the
/// compilation session that created it. Numeric indices alone are not identity.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TypeApplicationId {
    pub owner: u64,
    pub index: u32,
}
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct TypeApplication {
    pub declaration: RecordId,
    pub kind: NominalKind,
    pub arguments: Box<[TypeExpr]>,
}
pub const MAX_TYPE_APPLICATION_NODES: usize = 131_072;
#[derive(Debug, Clone, Default)]
pub struct TypeApplications {
    owner: u64,
    nodes: std::sync::Arc<[TypeApplication]>,
}
impl TypeApplications {
    pub fn get(&self, id: TypeApplicationId) -> Option<&TypeApplication> {
        (id.owner == self.owner)
            .then(|| self.nodes.get(id.index as usize))
            .flatten()
    }
    pub fn owner(&self) -> u64 {
        self.owner
    }
    pub fn len(&self) -> usize {
        self.nodes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.nodes.is_empty()
    }
}
/// Deliberately not Clone: mutable arenas may never fork under one owner ID.
#[derive(Debug)]
pub struct TypeApplicationsBuilder {
    owner: u64,
    nodes: Vec<TypeApplication>,
}
impl Default for TypeApplicationsBuilder {
    fn default() -> Self {
        Self::new()
    }
}
impl TypeApplicationsBuilder {
    pub fn new() -> Self {
        static NEXT_OWNER: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        let owner = NEXT_OWNER
            .fetch_update(
                std::sync::atomic::Ordering::Relaxed,
                std::sync::atomic::Ordering::Relaxed,
                |n| n.checked_add(1),
            )
            .expect("type application arena identity space exhausted");
        Self {
            owner,
            nodes: Vec::new(),
        }
    }
    pub fn insert(&mut self, node: TypeApplication) -> Option<TypeApplicationId> {
        if self.nodes.len() >= MAX_TYPE_APPLICATION_NODES || node.arguments.len() > 8 {
            return None;
        }
        let id = TypeApplicationId {
            owner: self.owner,
            index: self.nodes.len() as u32,
        };
        self.nodes.push(node);
        Some(id)
    }
    pub fn get(&self, id: TypeApplicationId) -> Option<&TypeApplication> {
        (id.owner == self.owner)
            .then(|| self.nodes.get(id.index as usize))
            .flatten()
    }
    pub fn finish(self) -> TypeApplications {
        TypeApplications {
            owner: self.owner,
            nodes: self.nodes.into(),
        }
    }
}

/// Checked private inline layouts. Scalar leaves and union tags occupy one
/// eight-byte slot; arrays and nominal fields recursively contribute their size.
/// Unit is a positive-sized scalar leaf here, initialized to zero, not a
/// zero-sized aggregate that could bypass storage or structural-node bounds.
/// Nonempty records and every union's tag make every owned type positive-sized.
pub const MAX_INLINE_DEPTH: usize = 32;
pub const MAX_INLINE_SLOTS: usize = 256;
#[derive(Debug, Clone, Default)]
pub struct InlineLayouts {
    sizes: std::collections::HashMap<NominalId, (usize, usize)>,
    record_offsets: std::collections::HashMap<NominalId, Vec<usize>>,
    union_offsets: std::collections::HashMap<NominalId, Vec<Vec<usize>>>,
}
#[derive(Debug)]
pub struct LayoutError {
    pub code: &'static str,
    pub span: Span,
    pub related: Option<Span>,
    pub message: String,
}
impl InlineLayouts {
    pub fn build(records: &[Record], unions: &[Union]) -> Result<Self, LayoutError> {
        let mut result = Self::default();
        for (ty, span) in records
            .iter()
            .map(|r| (Type::Record(r.id), r.name_span))
            .chain(unions.iter().map(|u| (Type::Union(u.id), u.name_span)))
        {
            result.visit(ty, span, records, unions, &mut Vec::new())?;
        }
        Ok(result)
    }
    fn visit(
        &mut self,
        ty: Type,
        span: Span,
        records: &[Record],
        unions: &[Union],
        stack: &mut Vec<(NominalId, Span)>,
    ) -> Result<(usize, usize), LayoutError> {
        let id = match ty {
            Type::Record(id) | Type::Union(id) => id,
            Type::Array { length, .. } => return Ok((length, 1)),
            _ => return Ok((1, 0)),
        };
        if let Some((_, previous)) = stack.iter().find(|(active, _)| *active == id) {
            return Err(LayoutError { code: "A001", span, related: Some(*previous), message: "inline aggregate layout contains a value cycle; recursive value storage requires indirection, which is unsupported".into() });
        }
        if let Some(&layout) = self.sizes.get(&id) {
            return Ok(layout);
        }
        if stack.len() >= MAX_INLINE_DEPTH {
            return Err(LayoutError {
                code: "A002",
                span,
                related: stack.first().map(|(_, span)| *span),
                message: format!("inline aggregate composition exceeds depth {MAX_INLINE_DEPTH}"),
            });
        }
        stack.push((id, span));
        let groups: Vec<&[RecordField]> = match ty {
            Type::Record(_) => vec![
                &records
                    .iter()
                    .find(|r| r.id == id)
                    .expect("resolved record")
                    .fields,
            ],
            Type::Union(_) => unions
                .iter()
                .find(|u| u.id == id)
                .expect("resolved union")
                .variants
                .iter()
                .map(|v| v.fields.as_slice())
                .collect(),
            _ => unreachable!(),
        };
        let tag = usize::from(matches!(ty, Type::Union(_)));
        let mut largest = tag;
        let mut depth = 1;
        let mut offsets = Vec::new();
        for fields in groups {
            let mut size = tag;
            let mut group_offsets = Vec::new();
            for field in fields {
                group_offsets.push(size);
                let (child_size, child_depth) =
                    self.visit(field.ty, field.name_span, records, unions, stack)?;
                depth = depth.max(1 + child_depth);
                if depth > MAX_INLINE_DEPTH {
                    return Err(LayoutError {
                        code: "A002",
                        span: field.name_span,
                        related: Some(span),
                        message: format!(
                            "inline aggregate composition exceeds depth {MAX_INLINE_DEPTH}"
                        ),
                    });
                }
                size = size
                    .checked_add(child_size)
                    .filter(|size| *size <= MAX_INLINE_SLOTS)
                    .ok_or_else(|| LayoutError {
                        code: "A003",
                        span: field.name_span,
                        related: Some(span),
                        message: format!(
                            "inline aggregate layout exceeds {MAX_INLINE_SLOTS} scalar/tag slots"
                        ),
                    })?;
            }
            largest = largest.max(size);
            offsets.push(group_offsets);
        }
        stack.pop();
        if matches!(ty, Type::Record(_)) {
            self.record_offsets.insert(id, offsets.remove(0));
        } else {
            self.union_offsets.insert(id, offsets);
        }
        self.sizes.insert(id, (largest, depth));
        Ok((largest, depth))
    }
    pub fn slots(&self, ty: Type) -> usize {
        match ty {
            Type::Record(id) | Type::Union(id) => self.sizes[&id].0,
            Type::Array { length, .. } => length,
            _ => 1,
        }
    }
    pub fn record_offset(&self, id: NominalId, field: usize) -> usize {
        self.record_offsets[&id][field]
    }
    pub fn union_offset(&self, id: NominalId, variant: usize, field: usize) -> usize {
        self.union_offsets[&id][variant][field]
    }
}
impl std::fmt::Display for Type {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::I64 => f.write_str("i64"),
            Self::F64 => f.write_str("f64"),
            Self::Bool => f.write_str("bool"),
            Self::Unit => f.write_str("()"),
            Self::Record(id) => write!(f, "record@{id}"),
            Self::Union(id) => write!(f, "union@{id}"),
            Self::Array { element, length } => write!(f, "[{element}; {length}]"),
            Self::Slice { element, mutable } => {
                write!(f, "&{}[{element}]", if *mutable { "mut " } else { "" })
            }
            Self::Tensor => f.write_str("tensor"),
            Self::TensorView { mutable } => {
                write!(f, "&{}tensor", if *mutable { "mut " } else { "" })
            }
            Self::TensorError => f.write_str("tensor_error"),
        }
    }
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatIntrinsic {
    FromI64,
    ToBits,
    FromBits,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TensorIntrinsic {
    Zeros,
    FromArray,
    View,
    Transpose,
    Slice,
    Add,
    Mul,
    Matmul,
    Sigmoid,
    Softmax,
    Sum,
    Logsumexp,
    Get,
    Set,
    Rank,
    Dim,
    Len,
    ErrorCode,
    ErrorStart,
    ErrorEnd,
}
impl TensorIntrinsic {
    pub fn from_name(name: &str) -> Option<Self> {
        Some(match name {
            "tensor_zeros" => Self::Zeros,
            "tensor_from_array" => Self::FromArray,
            "tensor_view" => Self::View,
            "tensor_transpose" => Self::Transpose,
            "tensor_slice" => Self::Slice,
            "tensor_add" => Self::Add,
            "tensor_mul" => Self::Mul,
            "tensor_matmul" => Self::Matmul,
            "tensor_sigmoid" => Self::Sigmoid,
            "tensor_softmax" => Self::Softmax,
            "tensor_sum" => Self::Sum,
            "tensor_logsumexp" => Self::Logsumexp,
            "tensor_get" => Self::Get,
            "tensor_set" => Self::Set,
            "tensor_rank" => Self::Rank,
            "tensor_dim" => Self::Dim,
            "tensor_len" => Self::Len,
            "tensor_error_code" => Self::ErrorCode,
            "tensor_error_start" => Self::ErrorStart,
            "tensor_error_end" => Self::ErrorEnd,
            _ => return None,
        })
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Zeros => "tensor_zeros",
            Self::FromArray => "tensor_from_array",
            Self::View => "tensor_view",
            Self::Transpose => "tensor_transpose",
            Self::Slice => "tensor_slice",
            Self::Add => "tensor_add",
            Self::Mul => "tensor_mul",
            Self::Matmul => "tensor_matmul",
            Self::Sigmoid => "tensor_sigmoid",
            Self::Softmax => "tensor_softmax",
            Self::Sum => "tensor_sum",
            Self::Logsumexp => "tensor_logsumexp",
            Self::Get => "tensor_get",
            Self::Set => "tensor_set",
            Self::Rank => "tensor_rank",
            Self::Dim => "tensor_dim",
            Self::Len => "tensor_len",
            Self::ErrorCode => "tensor_error_code",
            Self::ErrorStart => "tensor_error_start",
            Self::ErrorEnd => "tensor_error_end",
        }
    }
    pub fn fallible(self) -> bool {
        !matches!(
            self,
            Self::Rank | Self::Len | Self::ErrorCode | Self::ErrorStart | Self::ErrorEnd
        )
    }
}

/// Stable source tensor error tags. These are distinct from fatal scalar traps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i64)]
pub enum TensorErrorKind {
    RankLimit = 1,
    ExtentLimit = 2,
    ElementLimit = 3,
    ShapeMismatch = 4,
    RankMismatch = 5,
    Bounds = 6,
    InvalidPermutation = 7,
    InvalidStep = 8,
    BudgetLimit = 9,
    AllocationFailure = 10,
    WorkLimit = 11,
    NonFiniteInput = 12,
    EmptyInput = 13,
    NumericalFailure = 14,
    NestedScope = 15,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UnaryOp {
    Neg,
    Not,
    BitNot,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Shl,
    Shr,
    BitAnd,
    BitOr,
    BitXor,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or,
}
#[derive(Debug, Clone)]
pub struct Program<A = Type> {
    pub applications: TypeApplications,
    pub imports: Vec<Import>,
    pub records: Vec<Record<A>>,
    pub unions: Vec<Union<A>>,
    pub traits: Vec<Trait<A>>,
    pub implementations: Vec<Implementation<A>>,
    pub functions: Vec<Function<A>>,
    pub generic_functions: Vec<GenericFunction>,
    pub generic_records: Vec<GenericRecord>,
    pub generic_unions: Vec<GenericUnion>,
}
/// Source annotations may be opaque; executable IR continues to use concrete Type.
/// Even Program<Type> remains source AST: generic call arguments and templates
/// require the shared proof/instantiation pass before IR emission.
pub type SourceProgram = Program<TypeExpr>;
/// Trait signature placeholders never enter concrete checked IR.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TraitType<A = Type> {
    Concrete(A),
    SelfType,
}
impl TraitType {
    pub fn substitute(self, receiver: Type) -> Type {
        match self {
            Self::Concrete(ty) => ty,
            Self::SelfType => receiver,
        }
    }
}
#[derive(Debug, Clone)]
pub struct Trait<A = Type> {
    pub id: RecordId,
    pub exported: bool,
    pub name: String,
    pub name_span: Span,
    pub methods: Vec<TraitMethod<A>>,
}
#[derive(Debug, Clone)]
pub struct TraitMethod<A = Type> {
    pub name: String,
    pub name_span: Span,
    pub params: Vec<TraitParam<A>>,
    pub return_type: TraitType<A>,
}
#[derive(Debug, Clone)]
pub struct TraitParam<A = Type> {
    pub name: String,
    pub name_span: Span,
    pub ty: TraitType<A>,
}
#[derive(Debug, Clone)]
pub struct Implementation<A = Type> {
    pub trait_name: String,
    pub trait_span: Span,
    pub target: A,
    pub bindings: Vec<MethodBinding>,
}
#[derive(Debug, Clone)]
pub struct MethodBinding {
    pub method: String,
    pub method_span: Span,
    pub function: String,
    pub function_span: Span,
}
#[derive(Debug, Clone)]
pub struct Record<A = Type> {
    pub exported: bool,
    pub id: NominalId,
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<RecordField<A>>,
}
#[derive(Debug, Clone)]
pub struct RecordField<A = Type> {
    pub name: String,
    pub name_span: Span,
    pub ty: A,
}
#[derive(Debug, Clone)]
pub struct Union<A = Type> {
    pub exported: bool,
    pub id: NominalId,
    pub name: String,
    pub name_span: Span,
    pub variants: Vec<UnionVariant<A>>,
}
impl Union {
    /// Maximum direct field count, retained for scalar-only callers. Recursive
    /// physical storage must use InlineLayouts::slots instead.
    pub fn payload_slots(&self) -> usize {
        self.variants
            .iter()
            .map(|variant| variant.fields.len())
            .max()
            .unwrap_or(0)
    }
}
#[derive(Debug, Clone)]
pub struct UnionVariant<A = Type> {
    pub name: String,
    pub name_span: Span,
    pub fields: Vec<RecordField<A>>,
}
#[derive(Debug, Clone)]
pub struct MatchArm<A = Type> {
    pub application: Option<TypeExpr>,
    pub union_name: String,
    pub union_span: Span,
    pub variant: String,
    pub variant_span: Span,
    pub fields: Vec<PatternField>,
    pub body: Block<A>,
}
#[derive(Debug, Clone)]
pub struct PatternField {
    pub name: String,
    pub name_span: Span,
    pub binding: String,
    pub binding_span: Span,
}
#[derive(Debug, Clone)]
pub struct FieldInitializer {
    pub name: String,
    pub name_span: Span,
    pub value: Expr,
}
#[derive(Debug, Clone)]
pub struct Import {
    pub name: String,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub struct Function<A = Type> {
    pub exported: bool,
    pub name: String,
    pub name_span: Span,
    pub params: Vec<Param<A>>,
    pub return_type: A,
    pub body: Block<A>,
    pub span: Span,
}
/// An explicitly parameterized function body, before any concrete substitution.
#[derive(Debug, Clone)]
pub struct GenericFunction {
    pub parameters: Vec<TypeParameter>,
    pub function: Function<TypeExpr>,
}
#[derive(Debug, Clone)]
pub struct GenericRecord {
    pub parameters: Vec<TypeParameter>,
    pub record: Record<TypeExpr>,
}
#[derive(Debug, Clone)]
pub struct GenericUnion {
    pub parameters: Vec<TypeParameter>,
    pub union: Union<TypeExpr>,
}
#[derive(Debug, Clone)]
pub struct TypeParameter {
    pub name: String,
    pub name_span: Span,
    pub bounds: Vec<TraitBound>,
}
#[derive(Debug, Clone)]
pub struct TraitBound {
    pub name: String,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Param<A = Type> {
    pub name: String,
    pub name_span: Span,
    pub ty: A,
}
#[derive(Debug, Clone)]
pub struct Block<A = Type> {
    pub stmts: Vec<Stmt<A>>,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub struct Stmt<A = Type> {
    pub kind: StmtKind<A>,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub enum StmtKind<A = Type> {
    Let {
        name: String,
        name_span: Span,
        mutable: bool,
        ty: A,
        value: Expr,
    },
    Assign {
        name: String,
        name_span: Span,
        value: Expr,
    },
    IndexAssign {
        name: String,
        name_span: Span,
        index: Expr,
        value: Expr,
    },
    FieldAssign {
        name: String,
        name_span: Span,
        field: String,
        field_span: Span,
        value: Expr,
    },
    Match {
        name: String,
        name_span: Span,
        arms: Vec<MatchArm<A>>,
    },
    If {
        condition: Expr,
        then_block: Block<A>,
        else_block: Option<Block<A>>,
    },
    While {
        condition: Expr,
        body: Block<A>,
    },
    Return(Expr),
    Print(Expr),
    Expr(Expr),
    Block(Block<A>),
    TensorScope {
        budget: Expr,
        body: Block<A>,
        error_name: String,
        error_span: Span,
        handler: Block<A>,
    },
    Break,
    Continue,
}
#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(u64),
    Bool(bool),
    Unit,
    Var(String),
    Array(Vec<Expr>),
    Record {
        name: String,
        application: Option<TypeExpr>,
        fields: Vec<FieldInitializer>,
    },
    Union {
        name: String,
        application: Option<TypeExpr>,
        variant: String,
        variant_span: Span,
        fields: Vec<FieldInitializer>,
    },
    Field {
        name: String,
        field: String,
        field_span: Span,
    },
    Borrow {
        name: String,
        mutable: bool,
        range: Option<(Box<Expr>, Box<Expr>)>,
    },
    Index {
        name: String,
        index: Box<Expr>,
    },
    Len(String),
    Unary {
        op: UnaryOp,
        value: Box<Expr>,
    },
    Binary {
        op: BinaryOp,
        left: Box<Expr>,
        right: Box<Expr>,
    },
    Call {
        name: String,
        args: Vec<Expr>,
    },
    GenericCall {
        name: String,
        type_args: Vec<TypeExpr>,
        args: Vec<Expr>,
    },
    Intrinsic {
        op: FloatIntrinsic,
        args: Vec<Expr>,
    },
    TensorCall {
        op: TensorIntrinsic,
        args: Vec<Expr>,
    },
    Try(Box<Expr>),
}
