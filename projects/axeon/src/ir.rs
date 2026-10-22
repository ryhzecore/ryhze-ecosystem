//! Typed, name-resolved structured IR. It preserves source spans for every operation.
pub use crate::ast::{
    BinaryOp, ElementType, FloatIntrinsic, NominalId, Record, RecordId, TensorErrorKind,
    TensorIntrinsic, Type, UnaryOp, Union,
};
use crate::diagnostic::Span;
pub type LocalId = usize;
pub type FunctionId = usize;
#[derive(Debug, Clone)]
pub struct Program {
    pub records: Vec<Record>,
    pub unions: Vec<Union>,
    pub functions: Vec<Function>,
    pub main: FunctionId,
}
impl Program {
    pub fn uses_tensors(&self) -> bool {
        self.functions.iter().any(|function| {
            function
                .body
                .stmts
                .iter()
                .any(|stmt| matches!(stmt.kind, StmtKind::TensorScope { .. }))
        })
    }
}
#[derive(Debug, Clone)]
pub struct Function {
    pub name: String,
    pub params: Vec<LocalId>,
    pub return_type: Type,
    pub locals: Vec<Local>,
    pub body: Block,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub struct Local {
    pub name: String,
    pub ty: Type,
    pub mutable: bool,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
    /// Tensor owners and views declared directly here, in declaration order.
    /// Release available values in reverse order on every block exit.
    pub cleanup: Vec<LocalId>,
}
#[derive(Debug, Clone)]
pub struct Stmt {
    pub kind: StmtKind,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub enum StmtKind {
    Let {
        local: LocalId,
        value: Expr,
    },
    Assign {
        local: LocalId,
        value: Expr,
    },
    IndexAssign {
        base: LocalId,
        index: Expr,
        value: Expr,
    },
    FieldAssign {
        base: LocalId,
        field: Vec<usize>,
        value: Expr,
    },
    Match {
        owner: LocalId,
        arms: Vec<MatchArm>,
    },
    If {
        condition: Expr,
        then_block: Block,
        else_block: Option<Block>,
    },
    While {
        condition: Expr,
        body: Block,
    },
    Return(Expr),
    Print(Expr),
    Expr(Expr),
    Block(Block),
    TensorScope {
        budget: Expr,
        body: Block,
        error_local: LocalId,
        handler: Block,
    },
    TensorSet {
        base: LocalId,
        index: Expr,
        value: Expr,
    },
    Break,
    Continue,
}
#[derive(Debug, Clone)]
pub struct Expr {
    pub kind: ExprKind,
    pub ty: Type,
    pub span: Span,
}
#[derive(Debug, Clone)]
pub struct MatchArm {
    pub variant: usize,
    pub bindings: Vec<(usize, LocalId)>,
    pub body: Block,
}
#[derive(Debug, Clone)]
pub enum ExprKind {
    Int(i64),
    Float(u64),
    Bool(bool),
    Unit,
    Var(LocalId),
    Array(Vec<Expr>),
    /// Entries remain in source evaluation order; indices name declaration-order slots.
    Record(Vec<(usize, Expr)>),
    Union {
        /// Checked payload layout is independent of the stored runtime tag, so
        /// defensive invalid-tag handling does not index with a corrupted tag.
        layout_variant: usize,
        variant: usize,
        fields: Vec<(usize, Expr)>,
    },
    Field {
        base: LocalId,
        field: Vec<usize>,
    },
    Move(LocalId),
    /// Transfer a complete aggregate subplace without copying its owner.
    MoveField {
        base: LocalId,
        field: Vec<usize>,
    },
    Borrow {
        owner: LocalId,
        mutable: bool,
        start: Box<Expr>,
        end: Box<Expr>,
    },
    Index {
        base: LocalId,
        index: Box<Expr>,
    },
    Len(LocalId),
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
        function: FunctionId,
        args: Vec<Expr>,
    },
    Intrinsic {
        op: FloatIntrinsic,
        value: Box<Expr>,
    },
    /// Fallible calls propagate their typed error to the active TensorScope.
    /// The type is the successful result; there are no escaping Result values.
    TensorCall {
        op: TensorIntrinsic,
        args: Vec<TensorArg>,
    },
}

#[derive(Debug, Clone)]
pub enum TensorArg {
    Local { local: LocalId, span: Span },
    Scalar(Expr),
    List(Vec<Expr>),
}
