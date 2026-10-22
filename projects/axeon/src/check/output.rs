//! Semantic facts are independent of output. Concrete checking builds the
//! existing IR directly; proof checking retains no expression or statement tree.
use crate::{ast, diagnostic::Span, ir};
pub(super) type LocalId = usize;

pub(super) use crate::ast::{TypeExpr as Type, TypeParameterId as ParameterId};

pub(super) struct Local {
    pub name: String,
    pub ty: Type,
    pub mutable: bool,
    pub span: Span,
}
impl super::semantic::LocalBinding for Local {
    fn name(&self) -> &str {
        &self.name
    }
}
impl Local {
    pub fn into_concrete(self) -> ir::Local {
        ir::Local {
            name: self.name,
            ty: self.ty.concrete().expect("concrete annotation domain"),
            mutable: self.mutable,
            span: self.span,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Callable {
    Concrete(usize),
    Template(ast::RecordId),
    Bound {
        contract: ast::RecordId,
        parameter: ParameterId,
        method: usize,
    },
}

pub(super) trait Output: Sized {
    type Expr;
    type Stmt;
    type Block;
    type Arm;
    fn expression(kind: ExprKind<Self>, ty: Type, span: Span) -> Self::Expr;
    fn statement(kind: StmtKind<Self>, span: Span) -> Self::Stmt;
    fn block(stmts: Vec<Self::Stmt>, span: Span, cleanup: Vec<usize>) -> Self::Block;
    fn arm(variant: usize, bindings: Vec<(usize, usize)>, body: Self::Block) -> Self::Arm;
}

pub(super) struct Expr<B: Output> {
    pub payload: B::Expr,
    pub ty: Type,
    pub span: Span,
    pub loan: Option<(usize, bool)>,
}
impl<B: Output> Expr<B> {
    pub fn new(kind: ExprKind<B>, ty: Type, span: Span) -> Self {
        let loan = match (&kind, ty) {
            (ExprKind::Borrow { owner, mutable, .. }, _) => Some((*owner, *mutable)),
            (
                ExprKind::TensorCall { args, .. },
                Type::Concrete(ast::Type::TensorView { mutable }),
            ) => match args.first() {
                Some(TensorArg::Local { local, .. }) => Some((*local, mutable)),
                _ => None,
            },
            _ => None,
        };
        Self {
            payload: B::expression(kind, ty, span),
            ty,
            span,
            loan,
        }
    }
}
pub(super) struct Stmt<B: Output> {
    pub payload: B::Stmt,
    pub binding: Option<usize>,
}
impl<B: Output> Stmt<B> {
    pub fn new(kind: StmtKind<B>, span: Span) -> Self {
        let binding = match &kind {
            StmtKind::Let { local, .. } => Some(*local),
            _ => None,
        };
        Self {
            payload: B::statement(kind, span),
            binding,
        }
    }
}

pub(super) enum ExprKind<B: Output> {
    Int(i64),
    Float(u64),
    Bool(bool),
    Unit,
    Var(usize),
    Array(Vec<B::Expr>),
    Record(Vec<(usize, B::Expr)>),
    Union {
        layout_variant: usize,
        variant: usize,
        fields: Vec<(usize, B::Expr)>,
    },
    Field {
        base: usize,
        field: Vec<usize>,
    },
    Move(usize),
    MoveField {
        base: usize,
        field: Vec<usize>,
    },
    Borrow {
        owner: usize,
        mutable: bool,
        start: Box<B::Expr>,
        end: Box<B::Expr>,
    },
    Index {
        base: usize,
        index: Box<B::Expr>,
    },
    Len(usize),
    Unary {
        op: ast::UnaryOp,
        value: Box<B::Expr>,
    },
    Binary {
        op: ast::BinaryOp,
        left: Box<B::Expr>,
        right: Box<B::Expr>,
    },
    Call {
        function: Callable,
        args: Vec<B::Expr>,
    },
    Intrinsic {
        op: ast::FloatIntrinsic,
        value: Box<B::Expr>,
    },
    TensorCall {
        op: ast::TensorIntrinsic,
        args: Vec<TensorArg<B>>,
    },
}
pub(super) enum TensorArg<B: Output> {
    Local { local: usize, span: Span },
    Scalar(B::Expr),
    List(Vec<B::Expr>),
}
pub(super) enum StmtKind<B: Output> {
    Let {
        local: usize,
        value: B::Expr,
    },
    Assign {
        local: usize,
        value: B::Expr,
    },
    IndexAssign {
        base: usize,
        index: B::Expr,
        value: B::Expr,
    },
    FieldAssign {
        base: usize,
        field: Vec<usize>,
        value: B::Expr,
    },
    Match {
        owner: usize,
        arms: Vec<B::Arm>,
    },
    If {
        condition: B::Expr,
        then_block: B::Block,
        else_block: Option<B::Block>,
    },
    While {
        condition: B::Expr,
        body: B::Block,
    },
    Return(B::Expr),
    Print(B::Expr),
    Expr(B::Expr),
    Block(B::Block),
    TensorScope {
        budget: B::Expr,
        body: B::Block,
        error_local: usize,
        handler: B::Block,
    },
    TensorSet {
        base: usize,
        index: B::Expr,
        value: B::Expr,
    },
    Break,
    Continue,
}

pub(super) struct Concrete;
impl Output for Concrete {
    type Expr = ir::Expr;
    type Stmt = ir::Stmt;
    type Block = ir::Block;
    type Arm = ir::MatchArm;
    fn expression(kind: ExprKind<Self>, ty: Type, span: Span) -> Self::Expr {
        use ir::ExprKind as I;
        use ExprKind as S;
        let kind = match kind {
            S::Int(v) => I::Int(v),
            S::Float(v) => I::Float(v),
            S::Bool(v) => I::Bool(v),
            S::Unit => I::Unit,
            S::Var(v) => I::Var(v),
            S::Array(v) => I::Array(v),
            S::Record(v) => I::Record(v),
            S::Union {
                layout_variant,
                variant,
                fields,
            } => I::Union {
                layout_variant,
                variant,
                fields,
            },
            S::Field { base, field } => I::Field { base, field },
            S::Move(v) => I::Move(v),
            S::MoveField { base, field } => I::MoveField { base, field },
            S::Borrow {
                owner,
                mutable,
                start,
                end,
            } => I::Borrow {
                owner,
                mutable,
                start,
                end,
            },
            S::Index { base, index } => I::Index { base, index },
            S::Len(v) => I::Len(v),
            S::Unary { op, value } => I::Unary { op, value },
            S::Binary { op, left, right } => I::Binary { op, left, right },
            S::Call { function, args } => {
                let Callable::Concrete(function) = function else {
                    panic!("abstract callable cannot enter concrete IR");
                };
                I::Call { function, args }
            }
            S::Intrinsic { op, value } => I::Intrinsic { op, value },
            S::TensorCall { op, args } => I::TensorCall {
                op,
                args: args
                    .into_iter()
                    .map(|arg| match arg {
                        TensorArg::Local { local, span } => ir::TensorArg::Local { local, span },
                        TensorArg::Scalar(value) => ir::TensorArg::Scalar(value),
                        TensorArg::List(values) => ir::TensorArg::List(values),
                    })
                    .collect(),
            },
        };
        ir::Expr {
            kind,
            ty: ty.concrete().expect("concrete annotation domain"),
            span,
        }
    }
    fn statement(kind: StmtKind<Self>, span: Span) -> Self::Stmt {
        use ir::StmtKind as I;
        use StmtKind as S;
        let kind = match kind {
            S::Let { local, value } => I::Let { local, value },
            S::Assign { local, value } => I::Assign { local, value },
            S::IndexAssign { base, index, value } => I::IndexAssign { base, index, value },
            S::FieldAssign { base, field, value } => I::FieldAssign { base, field, value },
            S::Match { owner, arms } => I::Match { owner, arms },
            S::If {
                condition,
                then_block,
                else_block,
            } => I::If {
                condition,
                then_block,
                else_block,
            },
            S::While { condition, body } => I::While { condition, body },
            S::Return(v) => I::Return(v),
            S::Print(v) => I::Print(v),
            S::Expr(v) => I::Expr(v),
            S::Block(v) => I::Block(v),
            S::TensorScope {
                budget,
                body,
                error_local,
                handler,
            } => I::TensorScope {
                budget,
                body,
                error_local,
                handler,
            },
            S::TensorSet { base, index, value } => I::TensorSet { base, index, value },
            S::Break => I::Break,
            S::Continue => I::Continue,
        };
        ir::Stmt { kind, span }
    }
    fn block(stmts: Vec<Self::Stmt>, span: Span, cleanup: Vec<usize>) -> Self::Block {
        ir::Block {
            stmts,
            span,
            cleanup,
        }
    }
    fn arm(variant: usize, bindings: Vec<(usize, usize)>, body: Self::Block) -> Self::Arm {
        ir::MatchArm {
            variant,
            bindings,
            body,
        }
    }
}

pub(super) struct Proof;
impl Output for Proof {
    type Expr = ();
    type Stmt = ();
    type Block = ();
    type Arm = ();
    fn expression(_: ExprKind<Self>, _: Type, _: Span) {}
    fn statement(_: StmtKind<Self>, _: Span) {}
    fn block(_: Vec<()>, _: Span, _: Vec<usize>) {}
    fn arm(_: usize, _: Vec<(usize, usize)>, _: ()) {}
}
