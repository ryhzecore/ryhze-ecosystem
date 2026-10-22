//! The reference interpreter for checked, name-resolved IR.
//!
//! One unit of fuel is charged on each function entry and before each statement
//! and expression actually evaluated. Short-circuited operands consume no fuel.
use crate::diagnostic::{line_col, SourceTable, Span};
use crate::ir::{
    self, BinaryOp, ElementType, ExprKind, FloatIntrinsic, StmtKind, TensorArg, TensorErrorKind,
    TensorIntrinsic, Type, UnaryOp,
};
mod tensor;

pub const DEFAULT_FUEL: u64 = 1_000_000;
pub const MAX_CALL_DEPTH: usize = 64;
pub const MAX_OUTPUT_BYTES: usize = 1 << 20;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum TrapKind {
    Overflow = 1,
    DivisionByZero = 2,
    ShiftCount = 3,
    CallDepth = 4,
    FuelExhausted = 5,
    OutputLimit = 6,
    Bounds = 7,
    InvalidUnionTag = 8,
}

impl TrapKind {
    pub const fn code(self) -> i32 {
        self as i32
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Overflow => "overflow",
            Self::DivisionByZero => "division_by_zero",
            Self::ShiftCount => "shift_count",
            Self::CallDepth => "call_depth",
            Self::FuelExhausted => "fuel_exhausted",
            Self::OutputLimit => "output_limit",
            Self::Bounds => "bounds",
            Self::InvalidUnionTag => "invalid_union_tag",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Trap {
    pub kind: TrapKind,
    pub span: Span,
}

impl Trap {
    pub fn render(&self, source: &str) -> String {
        let (line, column) = line_col(source, self.span.start);
        format!("trap[{}] at {line}:{column}\n", self.kind.name())
    }

    /// Render a trap using the logical path and local span of its source file.
    pub fn render_modules(&self, sources: &SourceTable) -> String {
        let file = sources.get(self.span.source).expect("valid trap source");
        let (line, column) = sources.line_col(self.span);
        format!(
            "trap[{}] at {}:{line}:{column}\n",
            self.kind.name(),
            file.logical_path
        )
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Execution {
    pub stdout: String,
    pub result: Result<i64, Trap>,
}

/// Execute a checked program. The main function must have signature `() -> i64`.
pub fn run(program: &ir::Program, fuel: u64) -> Execution {
    let mut interpreter = Interpreter {
        program,
        fuel,
        call_depth: 0,
        tensor_context: None,
        stdout: String::new(),
    };
    let result = interpreter
        .call(program.main, Vec::new())
        .map(|value| value.integer())
        .map_err(Failure::fatal);
    assert!(
        interpreter.tensor_context.is_none(),
        "tensor context survived run cleanup"
    );
    Execution {
        stdout: interpreter.stdout,
        result,
    }
}

#[derive(Debug)]
enum Value {
    Int(i64),
    Float(f64),
    Bool(bool),
    Unit,
    I64Array(Vec<i64>),
    F64Array(Vec<f64>),
    Record(Vec<Value>),
    Union {
        tag: usize,
        fields: Vec<Value>,
    },
    Slice {
        owner: ir::LocalId,
        start: usize,
        len: usize,
    },
    Tensor(tensor::Indirect<tensor::Owned>),
    TensorView(tensor::Indirect<TensorView>),
    TensorError {
        kind: TensorErrorKind,
        span: Span,
    },
    // A local has no value before initialization or after transferring ownership.
    Unavailable,
}

#[derive(Debug)]
struct TensorView {
    owner: ir::LocalId,
    descriptor: tensor::Descriptor,
}

impl Value {
    fn field(&self, path: &[usize]) -> &Self {
        let mut value = self;
        for &index in path {
            let Self::Record(fields) = value else {
                unreachable!("checked record path")
            };
            value = &fields[index];
        }
        value
    }
    fn field_mut(&mut self, path: &[usize]) -> &mut Self {
        let mut value = self;
        for &index in path {
            let Self::Record(fields) = value else {
                unreachable!("checked record path")
            };
            value = &mut fields[index];
        }
        value
    }
    fn integer(&self) -> i64 {
        match self {
            Self::Int(value) => *value,
            _ => unreachable!("checked IR requires an i64 value"),
        }
    }

    fn boolean(&self) -> bool {
        match self {
            Self::Bool(value) => *value,
            _ => unreachable!("checked IR requires a bool value"),
        }
    }

    fn float(&self) -> f64 {
        match self {
            Self::Float(value) => *value,
            _ => unreachable!("checked IR requires an f64 value"),
        }
    }

    fn scalar_copy(&self) -> Self {
        match self {
            Self::Int(value) => Self::Int(*value),
            Self::Float(value) => Self::Float(*value),
            Self::Bool(value) => Self::Bool(*value),
            Self::Unit => Self::Unit,
            _ => unreachable!("checked IR copies only scalar variables"),
        }
    }

    fn scalar_equal(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Int(left), Self::Int(right)) => left == right,
            (Self::Float(left), Self::Float(right)) => left == right,
            (Self::Bool(left), Self::Bool(right)) => left == right,
            (Self::Unit, Self::Unit) => true,
            _ => unreachable!("checked IR compares scalars of the same type"),
        }
    }

    fn array_len(&self) -> usize {
        match self {
            Self::I64Array(values) => values.len(),
            Self::F64Array(values) => values.len(),
            _ => unreachable!("checked IR requires an available array owner"),
        }
    }

    fn array_element(&self, index: usize) -> Self {
        match self {
            Self::I64Array(values) => Self::Int(values[index]),
            Self::F64Array(values) => Self::Float(values[index]),
            _ => unreachable!("checked IR requires an available array owner"),
        }
    }

    fn set_array_element(&mut self, index: usize, value: Self) {
        match (self, value) {
            (Self::I64Array(values), Self::Int(value)) => values[index] = value,
            (Self::F64Array(values), Self::Float(value)) => values[index] = value,
            _ => unreachable!("checked IR writes the owner's element type"),
        }
    }
}

/// Classify raw encodings without performing a floating-point operation on sNaNs.
fn canonical_float_bits(bits: u64) -> u64 {
    if bits & 0x7fff_ffff_ffff_ffff > 0x7ff0_0000_0000_0000 {
        0x7ff8_0000_0000_0000
    } else {
        bits
    }
}

/// NaN payloads are unspecified internally but have one observable encoding.
fn observable_float_bits(value: f64) -> u64 {
    canonical_float_bits(value.to_bits())
}

enum Flow {
    Next,
    Return(Value),
    Break,
    Continue,
}

#[derive(Debug)]
enum Failure {
    Trap(Trap),
    Tensor { kind: TensorErrorKind, span: Span },
}

impl Failure {
    fn fatal(self) -> Trap {
        match self {
            Self::Trap(trap) => trap,
            Self::Tensor { .. } => unreachable!("checked tensor errors have a lexical handler"),
        }
    }
}

struct TensorContext {
    limit: usize,
    used: usize,
}

enum TensorInput {
    Local(ir::LocalId),
    Scalar(i64),
    List(Vec<i64>),
}

impl TensorInput {
    fn local(&self) -> ir::LocalId {
        let Self::Local(value) = self else {
            unreachable!()
        };
        *value
    }
    fn scalar(&self) -> i64 {
        let Self::Scalar(value) = self else {
            unreachable!()
        };
        *value
    }
    fn list(&self) -> &[i64] {
        let Self::List(value) = self else {
            unreachable!()
        };
        value
    }
}

struct Interpreter<'a> {
    program: &'a ir::Program,
    fuel: u64,
    call_depth: usize,
    stdout: String,
    tensor_context: Option<TensorContext>,
}

impl Interpreter<'_> {
    fn tensor_descriptor(locals: &[Value], base: ir::LocalId) -> (ir::LocalId, tensor::Descriptor) {
        match &locals[base] {
            Value::Tensor(owner) => (base, owner.descriptor),
            Value::TensorView(view) => (view.owner, view.descriptor),
            _ => unreachable!("checked tensor operand is an available owner or view"),
        }
    }

    fn tensor_view(locals: &[Value], base: ir::LocalId) -> tensor::View<'_> {
        let (owner, descriptor) = Self::tensor_descriptor(locals, base);
        let Value::Tensor(storage) = &locals[owner] else {
            unreachable!()
        };
        tensor::View {
            data: &storage.data,
            descriptor,
        }
    }

    fn tensor_admit(&self, elements: usize, span: Span) -> Result<(), Failure> {
        let context = self
            .tensor_context
            .as_ref()
            .expect("checked tensor operation is scoped");
        let bytes = elements * 8;
        if bytes > context.limit - context.used {
            return Err(Failure::Tensor {
                kind: TensorErrorKind::BudgetLimit,
                span,
            });
        }
        Ok(())
    }

    fn tensor_charge(&mut self, work: u64, span: Span) -> Result<(), Failure> {
        if work > self.fuel {
            self.fuel = 0;
            return Err(Failure::Trap(Trap {
                kind: TrapKind::FuelExhausted,
                span,
            }));
        }
        self.fuel -= work;
        Ok(())
    }

    fn tensor_publish(&mut self, owner: tensor::Owned, span: Span) -> Result<Value, Failure> {
        let owner = tensor::Indirect::new(owner).map_err(|kind| Failure::Tensor { kind, span })?;
        let context = self
            .tensor_context
            .as_mut()
            .expect("owner created in active scope");
        context.used += owner.data.len() * 8;
        assert!(context.used <= context.limit);
        Ok(Value::Tensor(owner))
    }

    #[inline(never)]
    fn tensor_call(
        &mut self,
        op: TensorIntrinsic,
        args: &[TensorArg],
        span: Span,
        locals: &mut [Value],
    ) -> Result<Value, Failure> {
        let failure = |kind| Failure::Tensor { kind, span };
        let mut inputs = Vec::with_capacity(args.len());
        for arg in args {
            inputs.push(match arg {
                TensorArg::Local { local, .. } => TensorInput::Local(*local),
                TensorArg::Scalar(value) => {
                    TensorInput::Scalar(self.expression(value, locals)?.integer())
                }
                TensorArg::List(values) => {
                    let mut evaluated = Vec::with_capacity(values.len());
                    for value in values {
                        evaluated.push(self.expression(value, locals)?.integer());
                    }
                    TensorInput::List(evaluated)
                }
            });
        }
        use TensorIntrinsic as Op;
        match op {
            Op::Zeros | Op::FromArray => {
                let descriptor = tensor::Descriptor::new(inputs[0].list()).map_err(failure)?;
                let values = if op == Op::FromArray {
                    let (owner, start, len) = Self::storage_range(locals, inputs[1].local());
                    if len != descriptor.len {
                        return Err(failure(TensorErrorKind::ShapeMismatch));
                    }
                    let Value::F64Array(values) = &locals[owner] else {
                        unreachable!()
                    };
                    Some(&values[start..start + len])
                } else {
                    None
                };
                self.tensor_admit(descriptor.len, span)?;
                self.tensor_charge(descriptor.len as u64, span)?;
                let owner = if let Some(values) = values {
                    tensor::Owned::from_slice(descriptor, values)
                } else {
                    tensor::Owned::zeros(descriptor)
                }
                .map_err(failure)?;
                self.tensor_publish(owner, span)
            }
            Op::View | Op::Transpose | Op::Slice => {
                let (owner, descriptor) = Self::tensor_descriptor(locals, inputs[0].local());
                let descriptor = match op {
                    Op::View => descriptor,
                    Op::Transpose => descriptor.transpose(inputs[1].list()).map_err(failure)?,
                    _ => descriptor
                        .slice(
                            inputs[1].scalar(),
                            inputs[2].scalar(),
                            inputs[3].scalar(),
                            inputs[4].scalar(),
                        )
                        .map_err(failure)?,
                };
                tensor::Indirect::new(TensorView { owner, descriptor })
                    .map(Value::TensorView)
                    .map_err(failure)
            }
            Op::Get | Op::Dim | Op::Rank | Op::Len => {
                let view = Self::tensor_view(locals, inputs[0].local());
                match op {
                    Op::Get => {
                        let index = inputs[1].scalar();
                        let physical = view.descriptor.linear_index(index).map_err(failure)?;
                        Ok(Value::Float(view.data[physical]))
                    }
                    Op::Dim => {
                        let axis = inputs[1].scalar();
                        if axis < 0 || axis as usize >= view.descriptor.rank {
                            return Err(failure(TensorErrorKind::Bounds));
                        }
                        Ok(Value::Int(view.descriptor.dims[axis as usize] as i64))
                    }
                    Op::Rank => Ok(Value::Int(view.descriptor.rank as i64)),
                    _ => Ok(Value::Int(view.descriptor.len as i64)),
                }
            }
            Op::ErrorCode | Op::ErrorStart | Op::ErrorEnd => {
                let Value::TensorError { kind, span } = &locals[inputs[0].local()] else {
                    unreachable!()
                };
                Ok(Value::Int(match op {
                    Op::ErrorCode => *kind as i64,
                    Op::ErrorStart => span.start as i64,
                    _ => span.end as i64,
                }))
            }
            Op::Add
            | Op::Mul
            | Op::Matmul
            | Op::Sigmoid
            | Op::Softmax
            | Op::Sum
            | Op::Logsumexp => {
                let left = Self::tensor_view(locals, inputs[0].local());
                let right = if inputs.len() == 2 {
                    Some(Self::tensor_view(locals, inputs[1].local()))
                } else {
                    None
                };
                let plan = tensor::prepare(op, left, right).map_err(failure)?;
                if let Some(output) = plan.output {
                    self.tensor_admit(output.len, span)?;
                }
                self.tensor_charge(plan.work, span)?;
                if plan.output.is_some() {
                    let owner = tensor::tensor_kernel(op, left, right, plan).map_err(failure)?;
                    self.tensor_publish(owner, span)
                } else {
                    tensor::scalar_kernel(op, left)
                        .map(Value::Float)
                        .map_err(failure)
                }
            }
            Op::Set => unreachable!("setter lowers to a target-first statement"),
        }
    }

    fn tick(&mut self, span: Span) -> Result<(), Failure> {
        if self.fuel == 0 {
            return Err(Failure::Trap(Trap {
                kind: TrapKind::FuelExhausted,
                span,
            }));
        }
        self.fuel -= 1;
        Ok(())
    }

    fn call(&mut self, id: ir::FunctionId, args: Vec<Value>) -> Result<Value, Failure> {
        let function = &self.program.functions[id];
        if self.call_depth >= MAX_CALL_DEPTH {
            return Err(Failure::Trap(Trap {
                kind: TrapKind::CallDepth,
                span: function.span,
            }));
        }
        self.tick(function.span)?;
        self.call_depth += 1;
        let mut locals: Vec<Value> = function
            .locals
            .iter()
            .map(|local| match local.ty {
                Type::I64 => Value::Int(0),
                Type::F64 => Value::Float(0.0),
                Type::Bool => Value::Bool(false),
                Type::Unit => Value::Unit,
                Type::Array { .. }
                | Type::Record(_)
                | Type::Union(_)
                | Type::Slice { .. }
                | Type::Tensor
                | Type::TensorView { .. }
                | Type::TensorError => Value::Unavailable,
            })
            .collect();
        debug_assert_eq!(function.params.len(), args.len());
        for (&local, value) in function.params.iter().zip(args) {
            locals[local] = value;
        }
        let result = self.block(&function.body, &mut locals);
        for local in (0..locals.len()).rev() {
            self.release(&mut locals[local]);
        }
        self.call_depth -= 1;
        match result? {
            Flow::Return(value) => Ok(value),
            _ => unreachable!("checked IR requires every terminating function path to return"),
        }
    }

    fn block(&mut self, block: &ir::Block, locals: &mut [Value]) -> Result<Flow, Failure> {
        let result = (|| {
            for stmt in &block.stmts {
                match self.statement(stmt, locals)? {
                    Flow::Next => {}
                    flow => return Ok(flow),
                }
            }
            Ok(Flow::Next)
        })();
        for &local in block.cleanup.iter().rev() {
            self.release(&mut locals[local]);
        }
        result
    }

    fn release(&mut self, local: &mut Value) {
        let value = std::mem::replace(local, Value::Unavailable);
        if let Value::Tensor(owner) = value {
            let context = self
                .tensor_context
                .as_mut()
                .expect("tensor owner has active context");
            context.used = context
                .used
                .checked_sub(owner.data.len() * 8)
                .expect("tensor charge released once");
        }
    }

    fn statement(&mut self, stmt: &ir::Stmt, locals: &mut [Value]) -> Result<Flow, Failure> {
        self.tick(stmt.span)?;
        match &stmt.kind {
            StmtKind::Match { owner, arms } => {
                let value = std::mem::replace(&mut locals[*owner], Value::Unavailable);
                let Value::Union { tag, mut fields } = value else {
                    unreachable!("checked union owner")
                };
                let Some(arm) = arms.iter().find(|arm| arm.variant == tag) else {
                    return Err(Failure::Trap(Trap {
                        kind: TrapKind::InvalidUnionTag,
                        span: stmt.span,
                    }));
                };
                for (field, local) in &arm.bindings {
                    locals[*local] = std::mem::replace(&mut fields[*field], Value::Unavailable);
                }
                return self.block(&arm.body, locals);
            }
            StmtKind::Let { local, value } | StmtKind::Assign { local, value } => {
                let value = self.expression(value, locals)?;
                self.release(&mut locals[*local]);
                locals[*local] = value;
            }
            StmtKind::IndexAssign { base, index, value } => {
                let index = self.expression(index, locals)?.integer();
                // Resolve and validate the destination before any RHS effects.
                let (owner, offset) = Self::indexed_location(locals, *base, index, stmt.span)?;
                let value = self.expression(value, locals)?;
                locals[owner].set_array_element(offset, value);
            }
            StmtKind::FieldAssign { base, field, value } => {
                let value = self.expression(value, locals)?;
                *locals[*base].field_mut(field) = value;
            }
            StmtKind::If {
                condition,
                then_block,
                else_block,
            } => {
                if self.expression(condition, locals)?.boolean() {
                    return self.block(then_block, locals);
                }
                if let Some(block) = else_block {
                    return self.block(block, locals);
                }
            }
            StmtKind::While { condition, body } => {
                while self.expression(condition, locals)?.boolean() {
                    match self.block(body, locals)? {
                        Flow::Next | Flow::Continue => {}
                        Flow::Break => break,
                        flow @ Flow::Return(_) => return Ok(flow),
                    }
                }
            }
            StmtKind::Return(value) => {
                return self.expression(value, locals).map(Flow::Return);
            }
            StmtKind::Print(value) => {
                let line = match self.expression(value, locals)? {
                    Value::Int(value) => format!("{value}\n"),
                    Value::Float(value) => format!("f64:0x{:016x}\n", observable_float_bits(value)),
                    Value::Bool(value) => format!("{value}\n"),
                    _ => unreachable!("checked IR prints only scalars"),
                };
                if line.len() > MAX_OUTPUT_BYTES - self.stdout.len() {
                    return Err(Failure::Trap(Trap {
                        kind: TrapKind::OutputLimit,
                        span: stmt.span,
                    }));
                }
                self.stdout.push_str(&line);
            }
            StmtKind::Expr(value) => {
                self.expression(value, locals)?;
            }
            StmtKind::Block(block) => return self.block(block, locals),
            StmtKind::TensorScope { .. } => return self.tensor_scope(stmt, locals),
            StmtKind::TensorSet { .. } => return self.tensor_set(stmt, locals),
            StmtKind::Break => return Ok(Flow::Break),
            StmtKind::Continue => return Ok(Flow::Continue),
        }
        Ok(Flow::Next)
    }

    // These branches must not inflate every scalar recursive statement frame.
    #[inline(never)]
    fn tensor_scope(&mut self, stmt: &ir::Stmt, locals: &mut [Value]) -> Result<Flow, Failure> {
        let StmtKind::TensorScope {
            budget,
            body,
            error_local,
            handler,
        } = &stmt.kind
        else {
            unreachable!()
        };
        let requested = self.expression(budget, locals)?.integer();
        let entry_error = if self.tensor_context.is_some() {
            Some(TensorErrorKind::NestedScope)
        } else if requested < 0 || requested as u64 > tensor::MAX_BUDGET as u64 {
            Some(TensorErrorKind::BudgetLimit)
        } else {
            None
        };
        let result = if let Some(kind) = entry_error {
            Err(Failure::Tensor {
                kind,
                span: stmt.span,
            })
        } else {
            self.tensor_context = Some(TensorContext {
                limit: requested as usize,
                used: 0,
            });
            let result = self.block(body, locals);
            let context = self
                .tensor_context
                .take()
                .expect("scope context remains active until cleanup");
            assert_eq!(context.used, 0, "tensor scope leaked payload charges");
            result
        };
        match result {
            Err(Failure::Tensor { kind, span }) => {
                locals[*error_local] = Value::TensorError { kind, span };
                let result = self.block(handler, locals);
                self.release(&mut locals[*error_local]);
                result
            }
            other => other,
        }
    }

    #[inline(never)]
    fn tensor_set(&mut self, stmt: &ir::Stmt, locals: &mut [Value]) -> Result<Flow, Failure> {
        let StmtKind::TensorSet { base, index, value } = &stmt.kind else {
            unreachable!()
        };
        let index = self.expression(index, locals)?.integer();
        let (owner, descriptor) = Self::tensor_descriptor(locals, *base);
        let physical = descriptor
            .linear_index(index)
            .map_err(|kind| Failure::Tensor {
                kind,
                span: stmt.span,
            })?;
        let value = self.expression(value, locals)?.float();
        let Value::Tensor(owner) = &mut locals[owner] else {
            unreachable!()
        };
        owner.data[physical] = tensor::canonical(value);
        Ok(Flow::Next)
    }

    /// The checker ensures a slice directly names a live array in this frame.
    fn storage_range(locals: &[Value], base: ir::LocalId) -> (ir::LocalId, usize, usize) {
        match &locals[base] {
            value @ (Value::I64Array(_) | Value::F64Array(_)) => (base, 0, value.array_len()),
            Value::Slice { owner, start, len } => (*owner, *start, *len),
            _ => unreachable!("checked IR indexes only an available array or slice"),
        }
    }

    fn indexed_location(
        locals: &[Value],
        base: ir::LocalId,
        index: i64,
        span: Span,
    ) -> Result<(ir::LocalId, usize), Failure> {
        let (owner, start, len) = Self::storage_range(locals, base);
        if index < 0 || index >= len as i64 {
            return Err(Failure::Trap(Trap {
                kind: TrapKind::Bounds,
                span,
            }));
        }
        Ok((owner, start + index as usize))
    }

    fn expression(&mut self, expr: &ir::Expr, locals: &mut [Value]) -> Result<Value, Failure> {
        self.tick(expr.span)?;
        let overflow = || {
            Failure::Trap(Trap {
                kind: TrapKind::Overflow,
                span: expr.span,
            })
        };
        match &expr.kind {
            ExprKind::Int(value) => Ok(Value::Int(*value)),
            ExprKind::Float(bits) => Ok(Value::Float(f64::from_bits(*bits))),
            ExprKind::Bool(value) => Ok(Value::Bool(*value)),
            ExprKind::Unit => Ok(Value::Unit),
            ExprKind::Var(local) => Ok(locals[*local].scalar_copy()),
            ExprKind::Field { base, field } => Ok(locals[*base].field(field).scalar_copy()),
            ExprKind::MoveField { base, field } => Ok(std::mem::replace(
                locals[*base].field_mut(field),
                Value::Unavailable,
            )),
            ExprKind::Record(initializers) => {
                // Declaration order determines slots, written order determines effects.
                let mut fields: Vec<_> = (0..initializers.len())
                    .map(|_| Value::Unavailable)
                    .collect();
                for (index, value) in initializers {
                    fields[*index] = self.expression(value, locals)?;
                }
                Ok(Value::Record(fields))
            }
            ExprKind::Union {
                layout_variant,
                variant,
                fields: initializers,
            } => {
                let Type::Union(id) = expr.ty else {
                    unreachable!("checked union constructor")
                };
                let count = self
                    .program
                    .unions
                    .iter()
                    .find(|union| union.id == id)
                    .expect("resolved union type")
                    .variants[*layout_variant]
                    .fields
                    .len();
                let mut fields: Vec<_> = (0..count).map(|_| Value::Int(0)).collect();
                for (index, value) in initializers {
                    fields[*index] = self.expression(value, locals)?;
                }
                Ok(Value::Union {
                    tag: *variant,
                    fields,
                })
            }
            ExprKind::Array(elements) => match expr.ty {
                Type::Array {
                    element: ElementType::I64,
                    ..
                } => {
                    let mut values = Vec::with_capacity(elements.len());
                    for element in elements {
                        values.push(self.expression(element, locals)?.integer());
                    }
                    Ok(Value::I64Array(values))
                }
                Type::Array {
                    element: ElementType::F64,
                    ..
                } => {
                    let mut values = Vec::with_capacity(elements.len());
                    for element in elements {
                        values.push(self.expression(element, locals)?.float());
                    }
                    Ok(Value::F64Array(values))
                }
                _ => unreachable!("checked IR array literals have array type"),
            },
            ExprKind::Move(local) => {
                let value = std::mem::replace(&mut locals[*local], Value::Unavailable);
                debug_assert!(matches!(
                    value,
                    Value::I64Array(_)
                        | Value::F64Array(_)
                        | Value::Tensor(_)
                        | Value::Record(_)
                        | Value::Union { .. }
                ));
                Ok(value)
            }
            ExprKind::Borrow {
                owner,
                mutable: _,
                start,
                end,
            } => {
                // Both bounds evaluate once, left to right, before checking the range.
                let start = self.expression(start, locals)?.integer();
                let end = self.expression(end, locals)?.integer();
                let owner_len = locals[*owner].array_len();
                if start < 0 || start > end || end > owner_len as i64 {
                    return Err(Failure::Trap(Trap {
                        kind: TrapKind::Bounds,
                        span: expr.span,
                    }));
                }
                Ok(Value::Slice {
                    owner: *owner,
                    start: start as usize,
                    len: (end - start) as usize,
                })
            }
            ExprKind::Index { base, index } => {
                let index = self.expression(index, locals)?.integer();
                let (owner, offset) = Self::indexed_location(locals, *base, index, expr.span)?;
                Ok(locals[owner].array_element(offset))
            }
            ExprKind::Len(local) => {
                let (_, _, len) = Self::storage_range(locals, *local);
                Ok(Value::Int(len as i64))
            }
            ExprKind::Unary { op, value } => {
                let value = self.expression(value, locals)?;
                match op {
                    UnaryOp::Neg if matches!(value, Value::Float(_)) => {
                        Ok(Value::Float(-value.float()))
                    }
                    UnaryOp::Neg => value
                        .integer()
                        .checked_neg()
                        .map(Value::Int)
                        .ok_or_else(overflow),
                    UnaryOp::Not => Ok(Value::Bool(!value.boolean())),
                    UnaryOp::BitNot => Ok(Value::Int(!value.integer())),
                }
            }
            ExprKind::Binary { op, left, right } => {
                let left = self.expression(left, locals)?;
                // Evaluate the right operand only when the short-circuit operator needs it.
                if *op == BinaryOp::And && !left.boolean() {
                    return Ok(Value::Bool(false));
                }
                if *op == BinaryOp::Or && left.boolean() {
                    return Ok(Value::Bool(true));
                }
                let right = self.expression(right, locals)?;
                if let (Value::Float(left), Value::Float(right)) = (&left, &right) {
                    // Each source operation rounds separately. No fused operation,
                    // reassociation, integer overflow, or zero-divisor guard applies.
                    let (left, right) = (*left, *right);
                    return Ok(match op {
                        BinaryOp::Add => Value::Float(left + right),
                        BinaryOp::Sub => Value::Float(left - right),
                        BinaryOp::Mul => Value::Float(left * right),
                        BinaryOp::Div => Value::Float(left / right),
                        BinaryOp::Eq => Value::Bool(left == right),
                        BinaryOp::Ne => Value::Bool(left != right),
                        BinaryOp::Lt => Value::Bool(left < right),
                        BinaryOp::Le => Value::Bool(left <= right),
                        BinaryOp::Gt => Value::Bool(left > right),
                        BinaryOp::Ge => Value::Bool(left >= right),
                        _ => unreachable!("checked IR restricts f64 binary operators"),
                    });
                }
                match op {
                    BinaryOp::And | BinaryOp::Or => Ok(Value::Bool(right.boolean())),
                    BinaryOp::Eq => Ok(Value::Bool(left.scalar_equal(&right))),
                    BinaryOp::Ne => Ok(Value::Bool(!left.scalar_equal(&right))),
                    _ => {
                        let left = left.integer();
                        let right = right.integer();
                        let value = match op {
                            BinaryOp::Add => left.checked_add(right).ok_or_else(overflow)?,
                            BinaryOp::Sub => left.checked_sub(right).ok_or_else(overflow)?,
                            BinaryOp::Mul => left.checked_mul(right).ok_or_else(overflow)?,
                            BinaryOp::Div | BinaryOp::Rem => {
                                if right == 0 {
                                    return Err(Failure::Trap(Trap {
                                        kind: TrapKind::DivisionByZero,
                                        span: expr.span,
                                    }));
                                }
                                if *op == BinaryOp::Div {
                                    left.checked_div(right).ok_or_else(overflow)?
                                } else {
                                    left.checked_rem(right).ok_or_else(overflow)?
                                }
                            }
                            BinaryOp::Shl | BinaryOp::Shr => {
                                if !(0..=63).contains(&right) {
                                    return Err(Failure::Trap(Trap {
                                        kind: TrapKind::ShiftCount,
                                        span: expr.span,
                                    }));
                                }
                                if *op == BinaryOp::Shl {
                                    i64::try_from((left as i128) << right)
                                        .map_err(|_| overflow())?
                                } else {
                                    left >> right
                                }
                            }
                            BinaryOp::BitAnd => left & right,
                            BinaryOp::BitOr => left | right,
                            BinaryOp::BitXor => left ^ right,
                            BinaryOp::Lt => return Ok(Value::Bool(left < right)),
                            BinaryOp::Le => return Ok(Value::Bool(left <= right)),
                            BinaryOp::Gt => return Ok(Value::Bool(left > right)),
                            BinaryOp::Ge => return Ok(Value::Bool(left >= right)),
                            _ => unreachable!("non-integer operators handled above"),
                        };
                        Ok(Value::Int(value))
                    }
                }
            }
            ExprKind::Call { function, args } => {
                let mut values = Vec::with_capacity(args.len());
                for arg in args {
                    values.push(self.expression(arg, locals)?);
                }
                self.call(*function, values)
            }
            ExprKind::Intrinsic { op, value } => {
                let value = self.expression(value, locals)?;
                Ok(match op {
                    FloatIntrinsic::FromI64 => Value::Float(value.integer() as f64),
                    FloatIntrinsic::ToBits => {
                        Value::Int(observable_float_bits(value.float()) as i64)
                    }
                    FloatIntrinsic::FromBits => {
                        let bits = canonical_float_bits(value.integer() as u64);
                        Value::Float(f64::from_bits(bits))
                    }
                })
            }
            ExprKind::TensorCall { op, args } => self.tensor_call(*op, args, expr.span, locals),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SPAN: Span = Span {
        source: crate::diagnostic::SourceId(0),
        start: 0,
        end: 1,
    };
    const OP_SPAN: Span = Span {
        source: crate::diagnostic::SourceId(0),
        start: 8,
        end: 13,
    };

    fn int(value: i64) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Int(value),
            ty: Type::I64,
            span: SPAN,
        }
    }

    fn boolean(value: bool) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Bool(value),
            ty: Type::Bool,
            span: SPAN,
        }
    }

    fn var(local: usize) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Var(local),
            ty: Type::I64,
            span: SPAN,
        }
    }

    fn unary(op: UnaryOp, value: ir::Expr) -> ir::Expr {
        let ty = if op == UnaryOp::Not {
            Type::Bool
        } else {
            Type::I64
        };
        ir::Expr {
            kind: ExprKind::Unary {
                op,
                value: Box::new(value),
            },
            ty,
            span: OP_SPAN,
        }
    }

    fn binary(op: BinaryOp, left: ir::Expr, right: ir::Expr) -> ir::Expr {
        let ty = match op {
            BinaryOp::Eq
            | BinaryOp::Ne
            | BinaryOp::Lt
            | BinaryOp::Le
            | BinaryOp::Gt
            | BinaryOp::Ge
            | BinaryOp::And
            | BinaryOp::Or => Type::Bool,
            _ => Type::I64,
        };
        ir::Expr {
            kind: ExprKind::Binary {
                op,
                left: Box::new(left),
                right: Box::new(right),
            },
            ty,
            span: OP_SPAN,
        }
    }

    fn call(function: usize, args: Vec<ir::Expr>, ty: Type) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Call { function, args },
            ty,
            span: SPAN,
        }
    }

    fn stmt(kind: StmtKind) -> ir::Stmt {
        ir::Stmt { kind, span: SPAN }
    }

    fn block(stmts: Vec<ir::Stmt>) -> ir::Block {
        ir::Block {
            stmts,
            span: SPAN,
            cleanup: vec![],
        }
    }

    fn local() -> ir::Local {
        ir::Local {
            name: "x".into(),
            ty: Type::I64,
            mutable: true,
            span: SPAN,
        }
    }

    fn function(body: Vec<ir::Stmt>, params: usize, locals: usize) -> ir::Function {
        ir::Function {
            name: "f".into(),
            params: (0..params).collect(),
            return_type: Type::I64,
            locals: (0..locals).map(|_| local()).collect(),
            body: block(body),
            span: SPAN,
        }
    }

    fn program(body: Vec<ir::Stmt>) -> ir::Program {
        ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![function(body, 0, 0)],
            main: 0,
        }
    }

    fn evaluate(expr: ir::Expr) -> Execution {
        run(&program(vec![stmt(StmtKind::Return(expr))]), DEFAULT_FUEL)
    }

    fn assert_trap(expr: ir::Expr, kind: TrapKind) {
        let span = expr.span;
        assert_eq!(evaluate(expr).result, Err(Trap { kind, span }));
    }

    #[test]
    fn checked_integer_arithmetic() {
        for (op, left, right, expected) in [
            (BinaryOp::Add, 17, 9, 26),
            (BinaryOp::Sub, 17, 9, 8),
            (BinaryOp::Mul, -17, 9, -153),
            (BinaryOp::Div, -17, 9, -1),
            (BinaryOp::Rem, -17, 9, -8),
            (BinaryOp::Div, 17, -9, -1),
            (BinaryOp::Rem, 17, -9, 8),
            (BinaryOp::Add, i64::MIN, i64::MAX, -1),
            (BinaryOp::Mul, i64::MIN, 1, i64::MIN),
        ] {
            assert_eq!(
                evaluate(binary(op, int(left), int(right))).result,
                Ok(expected)
            );
        }
        assert_eq!(
            evaluate(unary(UnaryOp::Neg, int(i64::MAX))).result,
            Ok(-i64::MAX)
        );
    }

    #[test]
    fn all_integer_overflow_paths_trap_at_operator() {
        for (op, left, right) in [
            (BinaryOp::Add, i64::MAX, 1),
            (BinaryOp::Add, i64::MIN, -1),
            (BinaryOp::Sub, i64::MIN, 1),
            (BinaryOp::Sub, i64::MAX, -1),
            (BinaryOp::Mul, i64::MAX, 2),
            (BinaryOp::Mul, i64::MIN, -1),
            (BinaryOp::Div, i64::MIN, -1),
            (BinaryOp::Rem, i64::MIN, -1),
        ] {
            assert_trap(binary(op, int(left), int(right)), TrapKind::Overflow);
        }
        assert_trap(unary(UnaryOp::Neg, int(i64::MIN)), TrapKind::Overflow);
    }

    #[test]
    fn division_and_remainder_by_zero_trap() {
        for op in [BinaryOp::Div, BinaryOp::Rem] {
            for numerator in [i64::MIN, -1, 0, 1, i64::MAX] {
                assert_trap(binary(op, int(numerator), int(0)), TrapKind::DivisionByZero);
            }
        }
    }

    #[test]
    fn shifts_preserve_signed_mathematical_value() {
        for (op, left, right, expected) in [
            (BinaryOp::Shl, -1, 63, i64::MIN),
            (BinaryOp::Shl, 1, 62, 1 << 62),
            (BinaryOp::Shl, i64::MIN, 0, i64::MIN),
            (BinaryOp::Shl, 0, 63, 0),
            (BinaryOp::Shr, -1, 63, -1),
            (BinaryOp::Shr, i64::MIN, 63, -1),
            (BinaryOp::Shr, i64::MAX, 63, 0),
            (BinaryOp::Shr, -3, 1, -2),
        ] {
            assert_eq!(
                evaluate(binary(op, int(left), int(right))).result,
                Ok(expected)
            );
        }
        for (left, right) in [(1, 63), (-2, 63), (i64::MIN, 1), (i64::MAX, 1)] {
            assert_trap(
                binary(BinaryOp::Shl, int(left), int(right)),
                TrapKind::Overflow,
            );
        }
        for op in [BinaryOp::Shl, BinaryOp::Shr] {
            for count in [-1, 64, i64::MIN, i64::MAX] {
                assert_trap(binary(op, int(0), int(count)), TrapKind::ShiftCount);
            }
        }
    }

    #[test]
    fn bitwise_operators() {
        for (op, left, right, expected) in [
            (BinaryOp::BitAnd, 0b1100, 0b1010, 0b1000),
            (BinaryOp::BitOr, 0b1100, 0b1010, 0b1110),
            (BinaryOp::BitXor, 0b1100, 0b1010, 0b0110),
            (BinaryOp::BitAnd, -1, i64::MIN, i64::MIN),
        ] {
            assert_eq!(
                evaluate(binary(op, int(left), int(right))).result,
                Ok(expected)
            );
        }
        assert_eq!(
            evaluate(unary(UnaryOp::BitNot, int(i64::MAX))).result,
            Ok(i64::MIN)
        );
    }

    #[test]
    fn bool_and_comparison_operators_print_exactly() {
        let mut expressions = vec![
            boolean(true),
            boolean(false),
            unary(UnaryOp::Not, boolean(false)),
            binary(BinaryOp::Eq, boolean(true), boolean(false)),
            binary(BinaryOp::Ne, boolean(true), boolean(false)),
        ];
        for op in [
            BinaryOp::Eq,
            BinaryOp::Ne,
            BinaryOp::Lt,
            BinaryOp::Le,
            BinaryOp::Gt,
            BinaryOp::Ge,
        ] {
            expressions.push(binary(op, int(-1), int(1)));
        }
        let mut body: Vec<_> = expressions
            .into_iter()
            .map(|expr| stmt(StmtKind::Print(expr)))
            .collect();
        body.push(stmt(StmtKind::Return(int(0))));
        let execution = run(&program(body), DEFAULT_FUEL);
        assert_eq!(execution.result, Ok(0));
        assert_eq!(
            execution.stdout,
            "true\nfalse\ntrue\nfalse\ntrue\nfalse\ntrue\ntrue\ntrue\nfalse\nfalse\n"
        );
    }

    #[test]
    fn short_circuit_skips_traps_and_fuel() {
        for (op, left, expected) in [
            (BinaryOp::And, false, "false\n"),
            (BinaryOp::Or, true, "true\n"),
        ] {
            let bad = binary(BinaryOp::Eq, binary(BinaryOp::Div, int(1), int(0)), int(0));
            let p = program(vec![
                stmt(StmtKind::Print(binary(op, boolean(left), bad))),
                stmt(StmtKind::Return(int(0))),
            ]);
            // Entry + print + binary + left operand + return + return value.
            assert_eq!(
                run(&p, 6),
                Execution {
                    stdout: expected.into(),
                    result: Ok(0)
                }
            );
        }
    }

    #[test]
    fn non_short_circuited_operands_are_evaluated() {
        for (op, left) in [(BinaryOp::And, true), (BinaryOp::Or, false)] {
            let bad = binary(BinaryOp::Eq, binary(BinaryOp::Div, int(1), int(0)), int(0));
            let p = program(vec![stmt(StmtKind::Print(binary(op, boolean(left), bad)))]);
            assert_eq!(
                run(&p, DEFAULT_FUEL).result,
                Err(Trap {
                    kind: TrapKind::DivisionByZero,
                    span: OP_SPAN
                })
            );
        }
    }

    #[test]
    fn binary_operands_and_call_arguments_evaluate_left_to_right() {
        let marker = |value| {
            function(
                vec![
                    stmt(StmtKind::Print(int(value))),
                    stmt(StmtKind::Return(int(value))),
                ],
                0,
                0,
            )
        };
        let p = ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![
                function(
                    vec![
                        stmt(StmtKind::Print(binary(
                            BinaryOp::Add,
                            call(1, vec![], Type::I64),
                            call(2, vec![], Type::I64),
                        ))),
                        stmt(StmtKind::Return(call(
                            3,
                            vec![call(1, vec![], Type::I64), call(2, vec![], Type::I64)],
                            Type::I64,
                        ))),
                    ],
                    0,
                    0,
                ),
                marker(1),
                marker(2),
                function(
                    vec![stmt(StmtKind::Return(binary(
                        BinaryOp::Sub,
                        var(0),
                        var(1),
                    )))],
                    2,
                    2,
                ),
            ],
            main: 0,
        };
        assert_eq!(
            run(&p, DEFAULT_FUEL),
            Execution {
                stdout: "1\n2\n3\n1\n2\n".into(),
                result: Ok(-1)
            }
        );
    }

    #[test]
    fn first_operand_trap_prevents_later_effects() {
        let p = ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![
                function(
                    vec![stmt(StmtKind::Return(binary(
                        BinaryOp::Add,
                        binary(BinaryOp::Div, int(1), int(0)),
                        call(1, vec![], Type::I64),
                    )))],
                    0,
                    0,
                ),
                function(
                    vec![
                        stmt(StmtKind::Print(int(999))),
                        stmt(StmtKind::Return(int(0))),
                    ],
                    0,
                    0,
                ),
            ],
            main: 0,
        };
        assert_eq!(
            run(&p, DEFAULT_FUEL),
            Execution {
                stdout: String::new(),
                result: Err(Trap {
                    kind: TrapKind::DivisionByZero,
                    span: OP_SPAN
                })
            }
        );
    }

    #[test]
    fn loop_break_continue_and_scoped_return() {
        let p = ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![function(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: int(0),
                    }),
                    stmt(StmtKind::Let {
                        local: 1,
                        value: int(0),
                    }),
                    stmt(StmtKind::While {
                        condition: boolean(true),
                        body: block(vec![
                            stmt(StmtKind::Assign {
                                local: 0,
                                value: binary(BinaryOp::Add, var(0), int(1)),
                            }),
                            stmt(StmtKind::If {
                                condition: binary(BinaryOp::Eq, var(0), int(2)),
                                then_block: block(vec![stmt(StmtKind::Continue)]),
                                else_block: None,
                            }),
                            stmt(StmtKind::If {
                                condition: binary(BinaryOp::Eq, var(0), int(5)),
                                then_block: block(vec![stmt(StmtKind::Break)]),
                                else_block: None,
                            }),
                            stmt(StmtKind::Assign {
                                local: 1,
                                value: binary(BinaryOp::Add, var(1), var(0)),
                            }),
                        ]),
                    }),
                    stmt(StmtKind::While {
                        condition: boolean(false),
                        body: block(vec![stmt(StmtKind::Print(int(999)))]),
                    }),
                    stmt(StmtKind::If {
                        condition: boolean(false),
                        then_block: block(vec![stmt(StmtKind::Return(int(999)))]),
                        else_block: Some(block(vec![stmt(StmtKind::Block(block(vec![stmt(
                            StmtKind::Return(var(1)),
                        )])))])),
                    }),
                ],
                0,
                2,
            )],
            main: 0,
        };
        assert_eq!(
            run(&p, DEFAULT_FUEL),
            Execution {
                stdout: String::new(),
                result: Ok(8)
            }
        );
    }

    #[test]
    fn return_propagates_through_nested_loops() {
        let p = program(vec![stmt(StmtKind::While {
            condition: boolean(true),
            body: block(vec![stmt(StmtKind::While {
                condition: boolean(true),
                body: block(vec![stmt(StmtKind::Return(int(42)))]),
            })]),
        })]);
        assert_eq!(run(&p, DEFAULT_FUEL).result, Ok(42));
    }

    #[test]
    fn recursion_has_independent_local_frames_and_exact_depth_limit() {
        let recurse_span = Span::new(30, 40);
        let mut recurse = function(
            vec![
                stmt(StmtKind::If {
                    condition: binary(BinaryOp::Eq, var(0), int(0)),
                    then_block: block(vec![stmt(StmtKind::Return(int(0)))]),
                    else_block: None,
                }),
                stmt(StmtKind::Expr(call(
                    1,
                    vec![binary(BinaryOp::Sub, var(0), int(1))],
                    Type::I64,
                ))),
                stmt(StmtKind::Return(var(0))),
            ],
            1,
            1,
        );
        recurse.span = recurse_span;
        let make = |count| ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![
                function(
                    vec![stmt(StmtKind::Return(call(1, vec![int(count)], Type::I64)))],
                    0,
                    0,
                ),
                recurse.clone(),
            ],
            main: 0,
        };
        // Main plus recurse(62), recurse(61), ..., recurse(0) = 64 frames.
        assert_eq!(run(&make(62), DEFAULT_FUEL).result, Ok(62));
        assert_eq!(
            run(&make(63), DEFAULT_FUEL).result,
            Err(Trap {
                kind: TrapKind::CallDepth,
                span: recurse_span
            })
        );
    }

    #[test]
    fn returned_calls_release_depth_budget() {
        let mut body = Vec::new();
        for _ in 0..MAX_CALL_DEPTH * 2 {
            body.push(stmt(StmtKind::Expr(call(1, vec![], Type::I64))));
        }
        body.push(stmt(StmtKind::Return(int(42))));
        let p = ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![
                function(body, 0, 0),
                function(vec![stmt(StmtKind::Return(int(0)))], 0, 0),
            ],
            main: 0,
        };
        assert_eq!(run(&p, DEFAULT_FUEL).result, Ok(42));
    }

    #[test]
    fn fuel_is_charged_at_entry_statements_and_expressions() {
        let mut p = program(vec![stmt(StmtKind::Return(int(42)))]);
        p.functions[0].span = Span::new(1, 2);
        p.functions[0].body.stmts[0].span = Span::new(3, 4);
        for (fuel, span) in [(0, Span::new(1, 2)), (1, Span::new(3, 4)), (2, SPAN)] {
            assert_eq!(
                run(&p, fuel).result,
                Err(Trap {
                    kind: TrapKind::FuelExhausted,
                    span
                })
            );
        }
        assert_eq!(run(&p, 3).result, Ok(42));
    }

    #[test]
    fn empty_infinite_loop_exhausts_fuel() {
        let p = program(vec![stmt(StmtKind::While {
            condition: boolean(true),
            body: block(vec![]),
        })]);
        assert_eq!(
            run(&p, 100).result,
            Err(Trap {
                kind: TrapKind::FuelExhausted,
                span: SPAN
            })
        );
    }

    #[test]
    fn output_before_arithmetic_or_fuel_traps_is_preserved() {
        let p = program(vec![
            stmt(StmtKind::Print(int(i64::MIN))),
            stmt(StmtKind::Print(boolean(true))),
            stmt(StmtKind::Return(binary(
                BinaryOp::Add,
                int(i64::MAX),
                int(1),
            ))),
        ]);
        let output = format!("{}\ntrue\n", i64::MIN);
        assert_eq!(
            run(&p, DEFAULT_FUEL),
            Execution {
                stdout: output.clone(),
                result: Err(Trap {
                    kind: TrapKind::Overflow,
                    span: OP_SPAN
                })
            }
        );
        assert_eq!(
            run(&p, 5),
            Execution {
                stdout: output,
                result: Err(Trap {
                    kind: TrapKind::FuelExhausted,
                    span: SPAN
                })
            }
        );
    }

    #[test]
    fn output_limit_accepts_exact_boundary_then_rejects_entire_line() {
        let print_span = Span::new(50, 60);
        let mut print = stmt(StmtKind::Print(int(0)));
        print.span = print_span;
        let p = ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            functions: vec![function(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: int(0),
                    }),
                    stmt(StmtKind::While {
                        condition: binary(BinaryOp::Lt, var(0), int((MAX_OUTPUT_BYTES / 2) as i64)),
                        body: block(vec![
                            print.clone(),
                            stmt(StmtKind::Assign {
                                local: 0,
                                value: binary(BinaryOp::Add, var(0), int(1)),
                            }),
                        ]),
                    }),
                    stmt(StmtKind::Return(int(0))),
                ],
                0,
                1,
            )],
            main: 0,
        };
        let execution = run(&p, 10_000_000);
        assert_eq!(execution.result, Ok(0));
        assert_eq!(execution.stdout.len(), MAX_OUTPUT_BYTES);
        assert!(execution
            .stdout
            .as_bytes()
            .chunks_exact(2)
            .all(|line| line == b"0\n"));
        let mut over = p.clone();
        over.functions[0].body.stmts.insert(2, print);
        let exceeded = run(&over, 10_000_000);
        assert_eq!(
            exceeded.result,
            Err(Trap {
                kind: TrapKind::OutputLimit,
                span: print_span
            })
        );
        assert_eq!(exceeded.stdout, execution.stdout);
    }

    #[test]
    fn output_limit_never_partially_writes_a_line() {
        let mut interpreter = Interpreter {
            program: &program(vec![]),
            fuel: 2,
            call_depth: 0,
            tensor_context: None,
            stdout: "x".repeat(MAX_OUTPUT_BYTES - 1),
        };
        assert_eq!(
            interpreter
                .statement(&stmt(StmtKind::Print(int(0))), &mut [])
                .err()
                .map(Failure::fatal),
            Some(Trap {
                kind: TrapKind::OutputLimit,
                span: SPAN
            })
        );
        assert_eq!(interpreter.stdout.len(), MAX_OUTPUT_BYTES - 1);
    }

    #[test]
    fn traps_have_stable_codes_names_and_unicode_columns() {
        for (kind, code, name) in [
            (TrapKind::Overflow, 1, "overflow"),
            (TrapKind::DivisionByZero, 2, "division_by_zero"),
            (TrapKind::ShiftCount, 3, "shift_count"),
            (TrapKind::CallDepth, 4, "call_depth"),
            (TrapKind::FuelExhausted, 5, "fuel_exhausted"),
            (TrapKind::OutputLimit, 6, "output_limit"),
            (TrapKind::Bounds, 7, "bounds"),
        ] {
            assert_eq!(kind.code(), code);
            assert_eq!(kind.name(), name);
            assert_eq!(
                Trap {
                    kind,
                    span: Span::new(5, 6)
                }
                .render("a\né x"),
                format!("trap[{name}] at 2:3\n")
            );
        }
    }

    mod floats {
        use super::*;

        const CANONICAL_NAN: u64 = 0x7ff8_0000_0000_0000;

        fn bits(bits: u64) -> ir::Expr {
            ir::Expr {
                kind: ExprKind::Float(bits),
                ty: Type::F64,
                span: SPAN,
            }
        }

        fn float(value: f64) -> ir::Expr {
            bits(value.to_bits())
        }

        fn operation(op: BinaryOp, left: ir::Expr, right: ir::Expr) -> ir::Expr {
            let mut result = binary(op, left, right);
            if matches!(
                op,
                BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Div
            ) {
                result.ty = Type::F64;
            }
            result
        }

        fn intrinsic(op: FloatIntrinsic, value: ir::Expr) -> ir::Expr {
            ir::Expr {
                kind: ExprKind::Intrinsic {
                    op,
                    value: Box::new(value),
                },
                ty: if op == FloatIntrinsic::ToBits {
                    Type::I64
                } else {
                    Type::F64
                },
                span: SPAN,
            }
        }

        fn observed(value: ir::Expr) -> u64 {
            evaluate(intrinsic(FloatIntrinsic::ToBits, value))
                .result
                .unwrap() as u64
        }

        #[test]
        fn arithmetic_uses_binary64_rounding_and_ieee_exception_values() {
            for (op, left, right, expected) in [
                (BinaryOp::Add, 0.1, 0.2, 0x3fd3_3333_3333_3334),
                (BinaryOp::Sub, 1.0, 0.1, 0x3fec_cccc_cccc_cccd),
                (BinaryOp::Mul, 1.5, 2.0, 0x4008_0000_0000_0000),
                (BinaryOp::Div, 1.0, 10.0, 0x3fb9_9999_9999_999a),
                (BinaryOp::Mul, f64::MAX, 2.0, 0x7ff0_0000_0000_0000),
                (BinaryOp::Mul, -f64::MAX, 2.0, 0xfff0_0000_0000_0000),
                (BinaryOp::Div, 1.0, 0.0, 0x7ff0_0000_0000_0000),
                (BinaryOp::Div, 1.0, -0.0, 0xfff0_0000_0000_0000),
                (BinaryOp::Div, -1.0, -0.0, 0x7ff0_0000_0000_0000),
                (BinaryOp::Div, 0.0, 0.0, CANONICAL_NAN),
                (BinaryOp::Sub, f64::INFINITY, f64::INFINITY, CANONICAL_NAN),
                (BinaryOp::Mul, f64::INFINITY, 0.0, CANONICAL_NAN),
            ] {
                assert_eq!(
                    observed(operation(op, float(left), float(right))),
                    expected,
                    "{left} {op:?} {right}"
                );
            }
        }

        #[test]
        fn subnormals_are_preserved_and_underflow_rounds_to_even() {
            for (op, left, right, expected) in [
                (
                    BinaryOp::Div,
                    0x0010_0000_0000_0000,
                    2.0,
                    0x0008_0000_0000_0000,
                ),
                (BinaryOp::Mul, 1, 1.0, 1),
                (BinaryOp::Div, 1, 2.0, 0),
                (BinaryOp::Div, 3, 2.0, 2),
                (
                    BinaryOp::Div,
                    0x8000_0000_0000_0001,
                    2.0,
                    0x8000_0000_0000_0000,
                ),
            ] {
                assert_eq!(observed(operation(op, bits(left), float(right))), expected);
            }
        }

        #[test]
        fn signed_zero_is_preserved_by_negation_and_arithmetic() {
            for (input, expected) in [(0, 0x8000_0000_0000_0000), (0x8000_0000_0000_0000, 0)] {
                let mut expr = unary(UnaryOp::Neg, bits(input));
                expr.ty = Type::F64;
                assert_eq!(observed(expr), expected);
            }
            for (op, left, right, expected) in [
                (BinaryOp::Add, -0.0, -0.0, 0x8000_0000_0000_0000),
                (BinaryOp::Add, 0.0, -0.0, 0),
                (BinaryOp::Mul, -0.0, 2.0, 0x8000_0000_0000_0000),
                (BinaryOp::Div, -0.0, -2.0, 0),
            ] {
                assert_eq!(observed(operation(op, float(left), float(right))), expected);
            }
        }

        #[test]
        fn comparisons_use_ieee_equality_and_unordered_nan_rules() {
            let mut expressions = vec![
                operation(BinaryOp::Eq, float(0.0), float(-0.0)),
                operation(BinaryOp::Ne, float(0.0), float(-0.0)),
                operation(BinaryOp::Lt, float(f64::NEG_INFINITY), float(-f64::MAX)),
                operation(BinaryOp::Gt, float(f64::INFINITY), float(f64::MAX)),
            ];
            for op in [
                BinaryOp::Eq,
                BinaryOp::Ne,
                BinaryOp::Lt,
                BinaryOp::Le,
                BinaryOp::Gt,
                BinaryOp::Ge,
            ] {
                expressions.push(operation(op, bits(CANONICAL_NAN), bits(CANONICAL_NAN)));
                expressions.push(operation(op, float(1.0), bits(CANONICAL_NAN)));
            }
            let mut body: Vec<_> = expressions
                .into_iter()
                .map(|expr| stmt(StmtKind::Print(expr)))
                .collect();
            body.push(stmt(StmtKind::Return(int(0))));
            assert_eq!(run(&program(body), DEFAULT_FUEL), Execution {
                stdout: "true\nfalse\ntrue\ntrue\nfalse\nfalse\ntrue\ntrue\nfalse\nfalse\nfalse\nfalse\nfalse\nfalse\nfalse\nfalse\n".into(),
                result: Ok(0),
            });
        }

        #[test]
        fn integer_conversion_rounds_halfway_cases_to_even() {
            for (input, expected) in [
                (0, 0),
                (-1, 0xbff0_0000_0000_0000),
                (9_007_199_254_740_992, 0x4340_0000_0000_0000),
                (9_007_199_254_740_993, 0x4340_0000_0000_0000),
                (9_007_199_254_740_995, 0x4340_0000_0000_0002),
                (-9_007_199_254_740_993, 0xc340_0000_0000_0000),
                (-9_007_199_254_740_995, 0xc340_0000_0000_0002),
                (i64::MAX, 0x43e0_0000_0000_0000),
                (i64::MIN, 0xc3e0_0000_0000_0000),
            ] {
                assert_eq!(
                    observed(intrinsic(FloatIntrinsic::FromI64, int(input))),
                    expected
                );
            }
        }

        #[test]
        fn bit_intrinsics_preserve_nonnans_and_canonicalize_all_nan_encodings() {
            for raw in [
                0,
                0x8000_0000_0000_0000,
                1,
                0x8000_0000_0000_0001,
                0x0010_0000_0000_0000,
                0x7fef_ffff_ffff_ffff,
                0x7ff0_0000_0000_0000,
                0xfff0_0000_0000_0000,
            ] {
                assert_eq!(canonical_float_bits(raw), raw);
                assert_eq!(
                    observed(intrinsic(FloatIntrinsic::FromBits, int(raw as i64))),
                    raw
                );
            }
            for raw in [
                CANONICAL_NAN,
                0x7ff0_0000_0000_0001,
                0xfff0_0000_0000_0001,
                0x7ff7_ffff_ffff_ffff,
                0xfff7_ffff_ffff_ffff,
                0x7fff_ffff_ffff_ffff,
                0xffff_ffff_ffff_ffff,
            ] {
                assert_eq!(canonical_float_bits(raw), CANONICAL_NAN);
                assert_eq!(observed(bits(raw)), CANONICAL_NAN);
                let p = program(vec![]);
                let mut interpreter = Interpreter {
                    program: &p,
                    fuel: 2,
                    call_depth: 0,
                    tensor_context: None,
                    stdout: String::new(),
                };
                let value = interpreter
                    .expression(
                        &intrinsic(FloatIntrinsic::FromBits, int(raw as i64)),
                        &mut [],
                    )
                    .unwrap();
                assert_eq!(value.float().to_bits(), CANONICAL_NAN, "NaN ingress");
            }
        }

        #[test]
        fn multiplication_addition_and_association_are_not_fused_or_reordered() {
            let product = operation(
                BinaryOp::Mul,
                bits(0x3ff0_0000_0200_0000),
                bits(0x3fef_ffff_fc00_0000),
            );
            assert_eq!(observed(operation(BinaryOp::Sub, product, float(1.0))), 0);
            let left_grouped = operation(
                BinaryOp::Add,
                operation(BinaryOp::Add, float(1e16), float(-1e16)),
                float(1.0),
            );
            let right_grouped = operation(
                BinaryOp::Add,
                float(1e16),
                operation(BinaryOp::Add, float(-1e16), float(1.0)),
            );
            assert_eq!(observed(left_grouped), 1.0_f64.to_bits());
            assert_eq!(observed(right_grouped), 0);
        }

        #[test]
        fn float_print_is_exact_hex_with_canonical_nan_and_prior_output_survives_traps() {
            let p = program(vec![
                stmt(StmtKind::Print(float(1.5))),
                stmt(StmtKind::Print(float(-0.0))),
                stmt(StmtKind::Print(bits(1))),
                stmt(StmtKind::Print(float(f64::INFINITY))),
                stmt(StmtKind::Print(float(f64::NEG_INFINITY))),
                stmt(StmtKind::Print(bits(0xffff_ffff_ffff_ffff))),
                stmt(StmtKind::Return(binary(BinaryOp::Div, int(1), int(0)))),
            ]);
            assert_eq!(run(&p, DEFAULT_FUEL), Execution {
                stdout: "f64:0x3ff8000000000000\nf64:0x8000000000000000\nf64:0x0000000000000001\nf64:0x7ff0000000000000\nf64:0xfff0000000000000\nf64:0x7ff8000000000000\n".into(),
                result: Err(Trap { kind: TrapKind::DivisionByZero, span: OP_SPAN }),
            });
        }

        #[test]
        fn float_print_obeys_full_line_output_boundary() {
            let p = program(vec![]);
            for available in [22, 23] {
                let mut interpreter = Interpreter {
                    program: &p,
                    fuel: 2,
                    call_depth: 0,
                    tensor_context: None,
                    stdout: "x".repeat(MAX_OUTPUT_BYTES - available),
                };
                let result = interpreter.statement(&stmt(StmtKind::Print(float(-0.0))), &mut []);
                if available == 23 {
                    assert!(result.is_ok());
                    assert_eq!(interpreter.stdout.len(), MAX_OUTPUT_BYTES);
                    assert!(interpreter.stdout.ends_with("f64:0x8000000000000000\n"));
                } else {
                    assert_eq!(
                        result.err().map(Failure::fatal),
                        Some(Trap {
                            kind: TrapKind::OutputLimit,
                            span: SPAN
                        })
                    );
                    assert_eq!(interpreter.stdout.len(), MAX_OUTPUT_BYTES - available);
                }
            }
        }

        #[test]
        fn float_intrinsics_use_one_unit_plus_their_operand_fuel() {
            let p = program(vec![stmt(StmtKind::Return(intrinsic(
                FloatIntrinsic::ToBits,
                intrinsic(FloatIntrinsic::FromI64, int(1)),
            )))]);
            assert_eq!(run(&p, 5).result, Ok(1.0_f64.to_bits() as i64));
            assert_eq!(run(&p, 4).result.unwrap_err().kind, TrapKind::FuelExhausted);
        }

        #[test]
        fn checked_source_float_calls_arrays_slices_moves_and_scalar_copies() {
            let source = r#"
                fn bump(x: f64) -> f64 { return x + 0.5; }
                fn main() -> i64 {
                    let mut a: [f64; 3] = [bump(1.0), -0.0, f64_from_i64(2)];
                    let copied: f64 = a[0];
                    {
                        let s: &mut [f64] = &mut a[1..3];
                        s[0] = copied / s[1];
                        print(s[0]);
                    }
                    let b: [f64; 3] = a;
                    print(copied);
                    print(b[1]);
                    return f64_bits(b[2]);
                }
            "#;
            let p =
                crate::compile(source).expect("checked homogeneous f64 arrays and scalar calls");
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout:
                        "f64:0x3fe8000000000000\nf64:0x3ff8000000000000\nf64:0x3fe8000000000000\n"
                            .into(),
                    result: Ok(0x4000_0000_0000_0000),
                }
            );
        }

        #[test]
        fn checked_source_float_index_target_traps_before_rhs_effects() {
            let source = r#"
                fn index() -> i64 { print(1); return -1; }
                fn value() -> f64 { print(2); return 0.5; }
                fn main() -> i64 {
                    let mut a: [f64; 1] = [0.0];
                    a[index()] = value();
                    return 0;
                }
            "#;
            let p = crate::compile(source).expect("dynamically invalid f64 array index");
            let execution = run(&p, DEFAULT_FUEL);
            assert_eq!(execution.stdout, "1\n");
            let trap = execution.result.unwrap_err();
            assert_eq!(trap.kind, TrapKind::Bounds);
            assert_eq!(trap.span.start, source.find("a[index()]").unwrap());
        }
    }

    mod aggregates {
        use super::*;

        #[test]
        fn checked_source_preserves_borrows_moves_and_shadow_initializers() {
            let source = r#"
                fn mark(value: i64) -> i64 { print(value); return value; }
                fn main() -> i64 {
                    let mut values: [i64; 3] = [mark(2), mark(4), mark(6)];
                    {
                        let window: &mut [i64] = &mut values[1..3];
                        window[0] = window[1] + 10;
                        print(len(window));
                    }
                    {
                        let values: [i64; 2] = [values[0], values[1]];
                        print(values[1]);
                    }
                    let transferred: [i64; 3] = values;
                    return transferred[0] + transferred[1] + transferred[2];
                }
            "#;
            let p = crate::compile(source).expect("valid array and lexical borrowing program");
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "2\n4\n6\n2\n16\n".into(),
                    result: Ok(24)
                }
            );
        }

        #[test]
        fn checked_source_uses_expected_bounds_guard_order_and_locations() {
            let prefix = "fn mark(x: i64) -> i64 { print(x); return x; } fn main() -> i64 { let mut a: [i64; 1] = [0]; ";
            for (body, marker, expected_output) in [
                ("a[mark(-1)] = mark(99); return 0; }", "a[mark(-1)]", "-1\n"),
                (
                    "let s: &[i64] = &a[mark(-1)..mark(1)]; return 0; }",
                    "&a[mark(-1)",
                    "-1\n1\n",
                ),
                (
                    "let s: &[i64] = &a[len(a)..len(a)]; return s[0]; }",
                    "s[0]",
                    "",
                ),
            ] {
                let source = format!("{prefix}{body}");
                let p = crate::compile(&source).expect("bounds are dynamically checked");
                let result = run(&p, DEFAULT_FUEL);
                assert_eq!(result.stdout, expected_output);
                let trap = result.result.expect_err("invalid bounds trap");
                assert_eq!(trap.kind, TrapKind::Bounds);
                assert_eq!(trap.span.start, source.find(marker).unwrap());
            }
        }

        fn array(elements: Vec<ir::Expr>) -> ir::Expr {
            ir::Expr {
                ty: Type::Array {
                    element: ElementType::I64,
                    length: elements.len(),
                },
                kind: ExprKind::Array(elements),
                span: SPAN,
            }
        }

        fn moved(local: usize, len: usize) -> ir::Expr {
            ir::Expr {
                ty: Type::Array {
                    element: ElementType::I64,
                    length: len,
                },
                kind: ExprKind::Move(local),
                span: SPAN,
            }
        }

        fn borrow(owner: usize, mutable: bool, start: ir::Expr, end: ir::Expr) -> ir::Expr {
            ir::Expr {
                ty: Type::Slice {
                    element: ElementType::I64,
                    mutable,
                },
                kind: ExprKind::Borrow {
                    owner,
                    mutable,
                    start: Box::new(start),
                    end: Box::new(end),
                },
                span: OP_SPAN,
            }
        }

        fn index(base: usize, value: ir::Expr) -> ir::Expr {
            ir::Expr {
                ty: Type::I64,
                kind: ExprKind::Index {
                    base,
                    index: Box::new(value),
                },
                span: OP_SPAN,
            }
        }

        fn len(local: usize) -> ir::Expr {
            ir::Expr {
                ty: Type::I64,
                kind: ExprKind::Len(local),
                span: SPAN,
            }
        }

        fn typed(body: Vec<ir::Stmt>, types: &[Type]) -> ir::Program {
            let mut f = function(body, 0, types.len());
            for (local, ty) in f.locals.iter_mut().zip(types) {
                local.ty = *ty;
            }
            ir::Program {
                records: Vec::new(),
                unions: Vec::new(),
                functions: vec![f],
                main: 0,
            }
        }

        fn marker(value: i64) -> ir::Function {
            function(
                vec![
                    stmt(StmtKind::Print(int(value))),
                    stmt(StmtKind::Return(int(value))),
                ],
                0,
                0,
            )
        }

        #[test]
        fn literal_elements_evaluate_left_to_right_and_indexing_copies_scalars() {
            let mut p = typed(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: array(vec![call(1, vec![], Type::I64), call(2, vec![], Type::I64)]),
                    }),
                    stmt(StmtKind::Let {
                        local: 1,
                        value: index(0, int(0)),
                    }),
                    stmt(StmtKind::IndexAssign {
                        base: 0,
                        index: int(0),
                        value: int(99),
                    }),
                    stmt(StmtKind::Print(index(0, int(0)))),
                    stmt(StmtKind::Print(index(0, int(1)))),
                    stmt(StmtKind::Print(len(0))),
                    stmt(StmtKind::Return(var(1))),
                ],
                &[
                    Type::Array {
                        element: ElementType::I64,
                        length: 2,
                    },
                    Type::I64,
                ],
            );
            p.functions.extend([marker(10), marker(20)]);
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "10\n20\n99\n20\n2\n".into(),
                    result: Ok(10)
                }
            );
        }

        #[test]
        fn array_moves_transfer_elements_and_support_new_owner_mutation() {
            let p = typed(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: array(vec![int(11), int(22)]),
                    }),
                    stmt(StmtKind::Let {
                        local: 1,
                        value: moved(0, 2),
                    }),
                    stmt(StmtKind::IndexAssign {
                        base: 1,
                        index: int(1),
                        value: int(33),
                    }),
                    stmt(StmtKind::Let {
                        local: 2,
                        value: moved(1, 2),
                    }),
                    stmt(StmtKind::Print(len(2))),
                    stmt(StmtKind::Return(binary(
                        BinaryOp::Add,
                        index(2, int(0)),
                        index(2, int(1)),
                    ))),
                ],
                &[Type::Array {
                    element: ElementType::I64,
                    length: 2,
                }; 3],
            );
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "2\n".into(),
                    result: Ok(44)
                }
            );
        }

        #[test]
        fn slice_offsets_and_mutation_refer_to_owner_storage() {
            let p = typed(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: array(vec![int(10), int(20), int(30), int(40)]),
                    }),
                    stmt(StmtKind::Block(block(vec![
                        stmt(StmtKind::Let {
                            local: 1,
                            value: borrow(0, false, int(1), int(3)),
                        }),
                        stmt(StmtKind::Let {
                            local: 2,
                            value: borrow(0, false, int(2), int(4)),
                        }),
                        stmt(StmtKind::Print(index(1, int(0)))),
                        stmt(StmtKind::Print(index(2, int(1)))),
                        stmt(StmtKind::Print(len(1))),
                    ]))),
                    stmt(StmtKind::Block(block(vec![
                        stmt(StmtKind::Let {
                            local: 3,
                            value: borrow(0, true, int(1), int(3)),
                        }),
                        stmt(StmtKind::IndexAssign {
                            base: 3,
                            index: int(1),
                            value: int(99),
                        }),
                        stmt(StmtKind::Print(index(3, int(1)))),
                    ]))),
                    stmt(StmtKind::Return(index(0, int(2)))),
                ],
                &[
                    Type::Array {
                        element: ElementType::I64,
                        length: 4,
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: false,
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: false,
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: true,
                    },
                ],
            );
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "20\n40\n2\n99\n".into(),
                    result: Ok(99)
                }
            );
        }

        #[test]
        fn empty_slices_at_both_boundaries_have_zero_length_and_cannot_be_indexed() {
            for endpoint in [0, 2] {
                let mut p = typed(
                    vec![
                        stmt(StmtKind::Let {
                            local: 0,
                            value: array(vec![int(10), int(20)]),
                        }),
                        stmt(StmtKind::Let {
                            local: 1,
                            value: borrow(0, false, int(endpoint), int(endpoint)),
                        }),
                        stmt(StmtKind::Return(len(1))),
                    ],
                    &[
                        Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        },
                        Type::Slice {
                            element: ElementType::I64,
                            mutable: false,
                        },
                    ],
                );
                assert_eq!(run(&p, DEFAULT_FUEL).result, Ok(0));
                p.functions[0].body.stmts[2] = stmt(StmtKind::Return(index(1, int(0))));
                assert_eq!(
                    run(&p, DEFAULT_FUEL).result,
                    Err(Trap {
                        kind: TrapKind::Bounds,
                        span: OP_SPAN
                    })
                );
            }
        }

        #[test]
        fn borrow_ranges_reject_all_invalid_boundaries() {
            for (start, end) in [
                (-1, 1),
                (0, -1),
                (2, 1),
                (0, 3),
                (3, 3),
                (i64::MIN, i64::MAX),
                (0, i64::MAX),
            ] {
                let p = typed(
                    vec![
                        stmt(StmtKind::Let {
                            local: 0,
                            value: array(vec![int(10), int(20)]),
                        }),
                        stmt(StmtKind::Let {
                            local: 1,
                            value: borrow(0, false, int(start), int(end)),
                        }),
                        stmt(StmtKind::Return(int(0))),
                    ],
                    &[
                        Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        },
                        Type::Slice {
                            element: ElementType::I64,
                            mutable: false,
                        },
                    ],
                );
                assert_eq!(
                    run(&p, DEFAULT_FUEL).result,
                    Err(Trap {
                        kind: TrapKind::Bounds,
                        span: OP_SPAN
                    }),
                    "range {start}..{end}"
                );
            }
        }

        #[test]
        fn borrow_evaluates_both_endpoints_once_before_bounds_checking() {
            let mut p = typed(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: array(vec![int(10), int(20)]),
                    }),
                    stmt(StmtKind::Let {
                        local: 1,
                        value: borrow(
                            0,
                            false,
                            call(1, vec![], Type::I64),
                            call(2, vec![], Type::I64),
                        ),
                    }),
                    stmt(StmtKind::Return(int(0))),
                ],
                &[
                    Type::Array {
                        element: ElementType::I64,
                        length: 2,
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: false,
                    },
                ],
            );
            p.functions.extend([marker(-1), marker(1)]);
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "-1\n1\n".into(),
                    result: Err(Trap {
                        kind: TrapKind::Bounds,
                        span: OP_SPAN
                    })
                }
            );
            p.functions[2] = function(
                vec![stmt(StmtKind::Return(binary(
                    BinaryOp::Div,
                    int(1),
                    int(0),
                )))],
                0,
                0,
            );
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "-1\n".into(),
                    result: Err(Trap {
                        kind: TrapKind::DivisionByZero,
                        span: OP_SPAN
                    })
                }
            );
        }

        #[test]
        fn array_and_slice_indices_reject_negative_or_out_of_range_values() {
            for index_value in [i64::MIN, -1, 2, i64::MAX] {
                for base in [0, 1] {
                    let p = typed(
                        vec![
                            stmt(StmtKind::Let {
                                local: 0,
                                value: array(vec![int(10), int(20)]),
                            }),
                            stmt(StmtKind::Let {
                                local: 1,
                                value: borrow(0, false, int(0), int(2)),
                            }),
                            stmt(StmtKind::Return(index(base, int(index_value)))),
                        ],
                        &[
                            Type::Array {
                                element: ElementType::I64,
                                length: 2,
                            },
                            Type::Slice {
                                element: ElementType::I64,
                                mutable: false,
                            },
                        ],
                    );
                    assert_eq!(
                        run(&p, DEFAULT_FUEL).result,
                        Err(Trap {
                            kind: TrapKind::Bounds,
                            span: OP_SPAN
                        }),
                        "base {base}, index {index_value}"
                    );
                }
            }
        }

        #[test]
        fn indexed_assignment_checks_target_before_evaluating_rhs() {
            let assignment_span = Span::new(40, 60);
            for base in [0, 1] {
                for index_value in [-1, 0, 2] {
                    let mut assignment = stmt(StmtKind::IndexAssign {
                        base,
                        index: call(1, vec![], Type::I64),
                        value: call(2, vec![], Type::I64),
                    });
                    assignment.span = assignment_span;
                    let mut body = vec![stmt(StmtKind::Let {
                        local: 0,
                        value: array(vec![int(10), int(20)]),
                    })];
                    if base == 1 {
                        body.push(stmt(StmtKind::Let {
                            local: 1,
                            value: borrow(0, true, int(0), int(2)),
                        }));
                    }
                    body.extend([assignment, stmt(StmtKind::Return(index(base, int(0))))]);
                    let mut p = typed(
                        body,
                        &[
                            Type::Array {
                                element: ElementType::I64,
                                length: 2,
                            },
                            Type::Slice {
                                element: ElementType::I64,
                                mutable: true,
                            },
                        ],
                    );
                    p.functions.extend([marker(index_value), marker(99)]);
                    let expected = if index_value == 0 {
                        Execution {
                            stdout: "0\n99\n".into(),
                            result: Ok(99),
                        }
                    } else {
                        Execution {
                            stdout: format!("{index_value}\n"),
                            result: Err(Trap {
                                kind: TrapKind::Bounds,
                                span: assignment_span,
                            }),
                        }
                    };
                    assert_eq!(run(&p, DEFAULT_FUEL), expected);
                }
            }
        }

        #[test]
        fn aggregate_operations_charge_only_their_normal_expression_fuel() {
            let make = |tail, types: &[Type]| {
                let mut body = vec![stmt(StmtKind::Let {
                    local: 0,
                    value: array(vec![int(10), int(20)]),
                })];
                body.extend(tail);
                typed(body, types)
            };
            for (p, fuel, result) in [
                (
                    make(
                        vec![stmt(StmtKind::Return(len(0)))],
                        &[Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        }],
                    ),
                    7,
                    2,
                ),
                (
                    make(
                        vec![stmt(StmtKind::Return(index(0, int(1))))],
                        &[Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        }],
                    ),
                    8,
                    20,
                ),
                (
                    make(
                        vec![
                            stmt(StmtKind::Let {
                                local: 1,
                                value: moved(0, 2),
                            }),
                            stmt(StmtKind::Return(len(1))),
                        ],
                        &[Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        }; 2],
                    ),
                    9,
                    2,
                ),
                (
                    make(
                        vec![
                            stmt(StmtKind::Let {
                                local: 1,
                                value: borrow(0, false, int(0), int(1)),
                            }),
                            stmt(StmtKind::Return(len(1))),
                        ],
                        &[
                            Type::Array {
                                element: ElementType::I64,
                                length: 2,
                            },
                            Type::Slice {
                                element: ElementType::I64,
                                mutable: false,
                            },
                        ],
                    ),
                    11,
                    1,
                ),
                (
                    make(
                        vec![
                            stmt(StmtKind::IndexAssign {
                                base: 0,
                                index: int(0),
                                value: int(30),
                            }),
                            stmt(StmtKind::Return(index(0, int(0)))),
                        ],
                        &[Type::Array {
                            element: ElementType::I64,
                            length: 2,
                        }],
                    ),
                    11,
                    30,
                ),
            ] {
                assert_eq!(run(&p, fuel).result, Ok(result));
                assert_eq!(
                    run(&p, fuel - 1).result.unwrap_err().kind,
                    TrapKind::FuelExhausted
                );
            }
        }

        #[test]
        fn loop_iterations_replace_array_and_slice_slots() {
            let p = typed(
                vec![
                    stmt(StmtKind::Let {
                        local: 0,
                        value: int(0),
                    }),
                    stmt(StmtKind::While {
                        condition: binary(BinaryOp::Lt, var(0), int(3)),
                        body: block(vec![
                            stmt(StmtKind::Let {
                                local: 1,
                                value: array(vec![var(0)]),
                            }),
                            stmt(StmtKind::Let {
                                local: 2,
                                value: borrow(1, true, int(0), int(1)),
                            }),
                            stmt(StmtKind::IndexAssign {
                                base: 2,
                                index: int(0),
                                value: binary(BinaryOp::Add, index(2, int(0)), int(10)),
                            }),
                            stmt(StmtKind::Print(index(2, int(0)))),
                            stmt(StmtKind::Assign {
                                local: 0,
                                value: binary(BinaryOp::Add, var(0), int(1)),
                            }),
                        ]),
                    }),
                    stmt(StmtKind::Return(var(0))),
                ],
                &[
                    Type::I64,
                    Type::Array {
                        element: ElementType::I64,
                        length: 1,
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: true,
                    },
                ],
            );
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "10\n11\n12\n".into(),
                    result: Ok(3)
                }
            );
        }

        #[test]
        fn recursive_calls_keep_local_slice_owners_in_their_own_frame() {
            let mut f = function(
                vec![
                    stmt(StmtKind::Let {
                        local: 1,
                        value: array(vec![var(0)]),
                    }),
                    stmt(StmtKind::Let {
                        local: 2,
                        value: borrow(1, false, int(0), int(1)),
                    }),
                    stmt(StmtKind::If {
                        condition: binary(BinaryOp::Gt, var(0), int(0)),
                        then_block: block(vec![stmt(StmtKind::Expr(call(
                            1,
                            vec![binary(BinaryOp::Sub, var(0), int(1))],
                            Type::I64,
                        )))]),
                        else_block: None,
                    }),
                    stmt(StmtKind::Print(index(2, int(0)))),
                    stmt(StmtKind::Return(index(2, int(0)))),
                ],
                1,
                3,
            );
            f.locals[1].ty = Type::Array {
                element: ElementType::I64,
                length: 1,
            };
            f.locals[2].ty = Type::Slice {
                element: ElementType::I64,
                mutable: false,
            };
            let mut p = program(vec![stmt(StmtKind::Return(call(
                1,
                vec![int(3)],
                Type::I64,
            )))]);
            p.functions.push(f);
            assert_eq!(
                run(&p, DEFAULT_FUEL),
                Execution {
                    stdout: "0\n1\n2\n3\n".into(),
                    result: Ok(3)
                }
            );
        }
    }
}
