//! Deterministic textual LLVM IR for the checked hosted language subset.
//!
//! The generated module uses the LLVM 19 opaque-pointer dialect. Language
//! functions are internal: the small `ax_rt_*` C ABI is a provisional runtime
//! boundary, not yet a stable ABI for exporting language functions.

use crate::diagnostic::{SourceMap, SourceTable, Span};
use crate::ir::{
    self, BinaryOp, ElementType, ExprKind, FloatIntrinsic, StmtKind, TensorArg, TensorIntrinsic,
    Type, UnaryOp,
};
use std::fmt::Write;

/// Native targets with the current 64-bit pointer and inline-storage contract.
/// Target selection does not imply that the host toolchain or runtime is installed.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Target {
    LinuxX86_64,
    WindowsX86_64,
}

impl Target {
    pub fn triple(self) -> &'static str {
        match self {
            Self::LinuxX86_64 => "x86_64-pc-linux-gnu",
            Self::WindowsX86_64 => "x86_64-pc-windows-msvc",
        }
    }
}

/// Emit a module for an already type-checked, name-resolved program.
///
/// Only source contents (for trap locations) affect the result; no host paths,
/// timestamps, or process-global counters enter the module.
/// The historical entry point retains the Linux target on every host.
pub fn emit(program: &ir::Program, source: &str) -> String {
    emit_for_target(program, source, Target::LinuxX86_64)
}

/// Emit the same checked semantics for an explicitly selected native target.
pub fn emit_for_target(program: &ir::Program, source: &str, target: Target) -> String {
    let source_map = SourceMap::new(source);
    emit_with_locations(program, Locations::Single(&source_map), target)
}

/// Emit file-aware traps using only normalized logical source identities.
/// Scalar arithmetic, evaluation order, and the tensor ABI are unchanged.
pub fn emit_modules(program: &ir::Program, sources: &SourceTable) -> String {
    emit_modules_for_target(program, sources, Target::LinuxX86_64)
}

/// Emit file-aware traps and checked semantics for the selected native target.
pub fn emit_modules_for_target(
    program: &ir::Program,
    sources: &SourceTable,
    target: Target,
) -> String {
    emit_with_locations(program, Locations::Modules(sources), target)
}

#[derive(Clone, Copy)]
enum Locations<'a> {
    Single(&'a SourceMap),
    Modules(&'a SourceTable),
}

impl Locations<'_> {
    fn call(self, name: &str, arguments: &str, span: Span) -> String {
        let (suffix, location) = match self {
            Self::Single(map) => {
                let (line, column) = map.line_col(span.start);
                ("", format!("i64 {line}, i64 {column}"))
            }
            Self::Modules(sources) => {
                let (line, column) = sources.line_col(span);
                (
                    "_v2",
                    format!("ptr @ax_source_{}, i64 {line}, i64 {column}", span.source.0),
                )
            }
        };
        format!("call void @ax_rt_{name}{suffix}({arguments}{location})")
    }
}

fn emit_with_locations(program: &ir::Program, locations: Locations<'_>, target: Target) -> String {
    let layouts = crate::ast::InlineLayouts::build(&program.records, &program.unions)
        .expect("checked finite layouts");
    let mut out = String::from(
        "; Axeon checked integer LLVM IR\n\
         source_filename = \"axeon\"\n\
         target triple = \"",
    );
    out.push_str(target.triple());
    out.push_str(
        "\"\n\n\
         declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)\n\
         declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64, i64)\n\
         declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64)\n",
    );
    out.push_str(match locations {
        Locations::Single(_) => {
            "\
         declare void @ax_rt_enter(i64, i64)\n\
         declare void @ax_rt_leave()\n\
         declare void @ax_rt_print_i64(i64, i64, i64)\n\
         declare void @ax_rt_print_f64(double, i64, i64)\n\
         declare void @ax_rt_print_bool(i32, i64, i64)\n\
         declare void @ax_rt_result(i64)\n\
         declare void @ax_rt_trap(i32, i64, i64) noreturn\n\n"
        }
        Locations::Modules(_) => {
            "\
         declare void @ax_rt_enter_v2(ptr, i64, i64)\n\
         declare void @ax_rt_leave()\n\
         declare void @ax_rt_print_i64_v2(i64, ptr, i64, i64)\n\
         declare void @ax_rt_print_f64_v2(double, ptr, i64, i64)\n\
         declare void @ax_rt_print_bool_v2(i32, ptr, i64, i64)\n\
         declare void @ax_rt_result(i64)\n\
         declare void @ax_rt_trap_v2(i32, ptr, i64, i64) noreturn\n\n"
        }
    });
    if let Locations::Modules(sources) = locations {
        for (id, file) in sources.files().iter().enumerate() {
            assert!(
                !file.logical_path.contains('\0'),
                "NUL in logical source path"
            );
            write!(
                out,
                "@ax_source_{id} = private unnamed_addr constant [{} x i8] c\"",
                file.logical_path.len() + 1
            )
            .unwrap();
            // Escape UTF-8 bytes, including quotes, backslashes and controls.
            // Byte lengths, not Unicode scalar counts, define LLVM arrays.
            for byte in file.logical_path.bytes().chain(std::iter::once(0)) {
                write!(out, "\\{byte:02X}").unwrap();
            }
            out.push_str("\", align 1\n");
        }
        out.push('\n');
    }
    if program.uses_tensors() {
        out.push_str(TENSOR_DECLARATIONS);
    }
    for record in &program.records {
        writeln!(
            out,
            "{} = type [{} x i64]",
            record_type(record.id),
            layouts.slots(Type::Record(record.id))
        )
        .unwrap();
    }
    for (id, function) in program.functions.iter().enumerate() {
        out.push_str(&FunctionEmitter::new(program, function, locations, &layouts).emit(id));
    }
    writeln!(out, "define i32 @main() {{\nentry:").unwrap();
    if program.uses_tensors() {
        out.push_str(
            "  %tensor.abi = call i32 @ax_tensor_runtime_abi_version()\n\
             \x20 %tensor.version = call i64 @ax_tensor_runtime_version()\n\
             \x20 %tensor.abi.ok = icmp eq i32 %tensor.abi, 1\n\
             \x20 %tensor.version.ok = icmp eq i64 %tensor.version, 256\n\
             \x20 %tensor.compatible = and i1 %tensor.abi.ok, %tensor.version.ok\n\
             \x20 br i1 %tensor.compatible, label %tensor.ready, label %tensor.mismatch\n\
             tensor.mismatch:\n\
             \x20 call void @ax_rt_tensor_runtime_mismatch()\n\
             \x20 unreachable\n\
             tensor.ready:\n",
        );
    }
    writeln!(out, "  %result = call i64 @ax_fn_{}()", program.main).unwrap();
    out.push_str("  call void @ax_rt_result(i64 %result)\n  ret i32 0\n}\n");
    out
}

const TENSOR_DECLARATIONS: &str = "\
declare i32 @ax_tensor_runtime_abi_version()\n\
declare i64 @ax_tensor_runtime_version()\n\
declare void @ax_rt_tensor_runtime_mismatch() noreturn\n\
declare i32 @ax_tensor_v1_scope(i64, ptr, ptr, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_zeros(ptr, ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_from_array(ptr, ptr, i64, ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_view(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_transpose(ptr, i64, i64, ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_slice(ptr, i64, i64, i64, i64, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_add(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_mul(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_matmul(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_sigmoid(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_softmax(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_sum(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_logsumexp(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_get(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_check_index(ptr, i64, i64, i64, i64)\n\
declare i32 @ax_tensor_v1_set(ptr, i64, i64, double, i64, i64)\n\
declare i32 @ax_tensor_v1_rank(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_dim(ptr, i64, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_len(ptr, i64, i64, i64, ptr)\n\
declare i32 @ax_tensor_v1_release(ptr, i64, i64, i64)\n\n";

fn llvm_type(ty: Type) -> &'static str {
    match ty {
        Type::I64 | Type::Tensor | Type::TensorView { .. } => "i64",
        Type::F64 => "double",
        Type::Bool => "i1",
        Type::Unit => "i8",
        Type::Array { .. }
        | Type::Slice { .. }
        | Type::TensorError
        | Type::Record(_)
        | Type::Union(_) => {
            unreachable!("checked aggregates have no scalar LLVM type")
        }
    }
}

fn record_type(id: ir::NominalId) -> String {
    match id {
        ir::NominalId::Declared(id) => format!("%ax_record_{}_{}", id.source.0, id.offset),
        ir::NominalId::Instance(index) => format!("%ax_record_instance_{index}"),
    }
}

fn alignment(ty: Type) -> usize {
    match ty {
        Type::I64
        | Type::F64
        | Type::Array { .. }
        | Type::Record(_)
        | Type::Union(_)
        | Type::Slice { .. }
        | Type::Tensor
        | Type::TensorView { .. }
        | Type::TensorError => 8,
        Type::Bool | Type::Unit => 1,
    }
}

fn aggregate_element(ty: Type) -> ElementType {
    match ty {
        Type::Array { element, .. } | Type::Slice { element, .. } => element,
        _ => unreachable!("checked aggregate element type"),
    }
}

struct LoopTargets {
    condition: String,
    end: String,
    cleanup_depth: usize,
}

enum TensorValue {
    Local(ir::LocalId),
    Scalar(String),
    List(usize),
}

impl TensorValue {
    fn local(&self) -> ir::LocalId {
        match self {
            Self::Local(local) => *local,
            _ => unreachable!("checked tensor local argument"),
        }
    }
    fn scalar(&self) -> &str {
        match self {
            Self::Scalar(value) => value,
            _ => unreachable!("checked tensor scalar argument"),
        }
    }
    fn list_len(&self) -> usize {
        match self {
            Self::List(len) => *len,
            _ => unreachable!("checked tensor list argument"),
        }
    }
}

struct FunctionEmitter<'a> {
    program: &'a ir::Program,
    function: &'a ir::Function,
    locations: Locations<'a>,
    layouts: &'a crate::ast::InlineLayouts,
    out: String,
    next_value: usize,
    next_label: usize,
    current_block: String,
    terminated: bool,
    loops: Vec<LoopTargets>,
    cleanup_stack: Vec<Vec<ir::LocalId>>,
    function_id: usize,
    next_scope: usize,
    helpers: String,
    tensor_callback: bool,
}

impl<'a> FunctionEmitter<'a> {
    fn new(
        program: &'a ir::Program,
        function: &'a ir::Function,
        locations: Locations<'a>,
        layouts: &'a crate::ast::InlineLayouts,
    ) -> Self {
        Self {
            program,
            function,
            locations,
            layouts,
            out: String::new(),
            next_value: 0,
            next_label: 0,
            current_block: String::new(),
            terminated: false,
            loops: Vec::new(),
            cleanup_stack: Vec::new(),
            function_id: 0,
            next_scope: 0,
            helpers: String::new(),
            tensor_callback: false,
        }
    }

    fn emit(mut self, id: usize) -> String {
        self.function_id = id;
        let mut params = self
            .function
            .params
            .iter()
            .enumerate()
            .map(|(index, &local)| {
                let ty = self.function.locals[local].ty;
                format!(
                    "{} %arg{index}",
                    if ty.inline_owned() {
                        "ptr"
                    } else {
                        llvm_type(ty)
                    }
                )
            })
            .collect::<Vec<_>>()
            .join(", ");
        let result_type = if self.function.return_type.inline_owned() {
            params = format!(
                "ptr %union.result{}",
                if params.is_empty() {
                    String::new()
                } else {
                    format!(", {params}")
                }
            );
            "void"
        } else {
            llvm_type(self.function.return_type)
        };
        writeln!(
            self.out,
            "define internal {} @ax_fn_{id}({params}) {{",
            result_type
        )
        .unwrap();
        self.label("entry");
        // Every slot is in entry, even if its declaration occurs in a loop.
        for (id, local) in self.function.locals.iter().enumerate() {
            match local.ty {
                Type::Union(_) => {
                    self.instruction(format!(
                        "%local{id} = alloca {}, align 8",
                        self.union_storage_type(local.ty)
                    ));
                }
                Type::Record(record) => {
                    self.instruction(format!(
                        "%local{id} = alloca {}, align 8",
                        record_type(record)
                    ));
                }
                Type::Array { element, length } => {
                    let element = llvm_type(element.scalar_type());
                    self.instruction(format!(
                        "%local{id} = alloca [{length} x {element}], align 8"
                    ))
                }
                Type::Slice { .. } => {
                    self.instruction(format!("%local{id}.ptr = alloca ptr, align 8"));
                    self.instruction(format!("%local{id}.len = alloca i64, align 8"));
                }
                Type::TensorError => {
                    self.instruction(format!("%local{id} = alloca [3 x i64], align 8"));
                }
                ty => self.instruction(format!(
                    "%local{id} = alloca {}, align {}",
                    llvm_type(ty),
                    alignment(ty)
                )),
            }
        }
        if self
            .function
            .body
            .stmts
            .iter()
            .any(|statement| matches!(statement.kind, StmtKind::TensorScope { .. }))
        {
            let slots = self.capture_slots();
            self.instruction(format!(
                "%tensor.captures = alloca [{} x ptr], align 8",
                slots.len()
            ));
            for (index, slot) in slots.iter().enumerate() {
                let address = self.value(format!(
                    "getelementptr ptr, ptr %tensor.captures, i64 {index}"
                ));
                self.instruction(format!("store ptr {slot}, ptr {address}, align 8"));
            }
        }
        for (id, local) in self.function.locals.iter().enumerate() {
            if matches!(local.ty, Type::Tensor | Type::TensorView { .. }) {
                self.instruction(format!("store i64 0, ptr %local{id}, align 8"));
            }
        }
        for (index, &local) in self.function.params.iter().enumerate() {
            let ty = self.function.locals[local].ty;
            if ty.inline_owned() {
                self.copy_owned(ty, &format!("%arg{index}"), &format!("%local{local}"));
            } else {
                self.store(local, &format!("%arg{index}"));
            }
        }
        self.instruction(self.locations.call("enter", "", self.function.span));
        self.block(&self.function.body);
        if !self.terminated {
            // Semantic checking guarantees no reachable fallthrough. Keeping
            // this terminator also supports syntactically infinite loops.
            self.terminate("unreachable".to_owned());
        }
        self.out.push_str("}\n\n");
        self.out.push_str(&self.helpers);
        self.out
    }

    fn capture_slots(&self) -> Vec<String> {
        self.function
            .locals
            .iter()
            .enumerate()
            .flat_map(|(id, local)| {
                if matches!(local.ty, Type::Slice { .. }) {
                    vec![format!("%local{id}.ptr"), format!("%local{id}.len")]
                } else {
                    vec![format!("%local{id}")]
                }
            })
            .collect()
    }

    fn emit_tensor_callback(mut self, name: &str, body: &ir::Block) -> String {
        self.tensor_callback = true;
        writeln!(
            self.out,
            "define internal i32 @{name}(ptr %tensor.context, ptr %tensor.captures) {{"
        )
        .unwrap();
        self.label("entry");
        self.instruction("%tensor.list = alloca [8 x i64], align 8".into());
        self.instruction("%tensor.result = alloca i64, align 8".into());
        for (index, slot) in self.capture_slots().iter().enumerate() {
            let address = self.value(format!(
                "getelementptr ptr, ptr %tensor.captures, i64 {index}"
            ));
            self.instruction(format!("{slot} = load ptr, ptr {address}, align 8"));
        }
        // A previous recovered error may leave stale integer slot contents.
        // No tensor can escape a scope, so each callback starts with no handles.
        for (id, local) in self.function.locals.iter().enumerate() {
            if matches!(local.ty, Type::Tensor | Type::TensorView { .. }) {
                self.instruction(format!("store i64 0, ptr %local{id}, align 8"));
            }
        }
        self.block(body);
        if !self.terminated {
            self.terminate("ret i32 0".into());
        }
        self.out.push_str("}\n\n");
        self.out
    }

    fn instruction(&mut self, instruction: String) {
        debug_assert!(!self.terminated);
        writeln!(self.out, "  {instruction}").unwrap();
    }

    fn value(&mut self, instruction: String) -> String {
        let name = format!("%v{}", self.next_value);
        self.next_value += 1;
        debug_assert!(!self.terminated);
        writeln!(self.out, "  {name} = {instruction}").unwrap();
        name
    }

    fn fresh_label(&mut self, prefix: &str) -> String {
        let label = format!("{prefix}.{}", self.next_label);
        self.next_label += 1;
        label
    }

    fn label(&mut self, label: &str) {
        writeln!(self.out, "{label}:").unwrap();
        self.current_block = label.to_owned();
        self.terminated = false;
    }

    fn terminate(&mut self, instruction: String) {
        self.instruction(instruction);
        self.terminated = true;
    }

    fn branch(&mut self, label: &str) {
        self.terminate(format!("br label %{label}"));
    }

    fn conditional_branch(&mut self, condition: &str, yes: &str, no: &str) {
        self.terminate(format!("br i1 {condition}, label %{yes}, label %{no}"));
    }

    fn store(&mut self, local: ir::LocalId, value: &str) {
        let ty = self.function.locals[local].ty;
        self.instruction(format!(
            "store {} {value}, ptr %local{local}, align {}",
            llvm_type(ty),
            alignment(ty)
        ));
    }

    fn initialize(&mut self, local: ir::LocalId, expression: &ir::Expr) {
        if expression.ty.inline_owned() {
            self.initialize_owned(&format!("%local{local}"), expression);
            return;
        }
        match self.function.locals[local].ty {
            Type::Array { .. } | Type::Record(_) | Type::Union(_) => {
                unreachable!("owned initializer handled above")
            }
            Type::I64 | Type::F64 | Type::Bool | Type::Unit => {
                let value = self.expression(expression);
                self.store(local, &value);
            }
            Type::Slice { .. } => {
                let ExprKind::Borrow {
                    owner, start, end, ..
                } = &expression.kind
                else {
                    unreachable!("checked slice initializer");
                };
                let Type::Array { element, length } = self.function.locals[*owner].ty else {
                    unreachable!("checked direct array owner");
                };
                // Both endpoints are evaluated before any range check, even
                // if the first endpoint already makes the range invalid.
                let start = self.expression(start);
                let end = self.expression(end);
                let negative = self.value(format!("icmp slt i64 {start}, 0"));
                let reversed = self.value(format!("icmp slt i64 {end}, {start}"));
                let excessive = self.value(format!("icmp sgt i64 {end}, {length}"));
                let invalid_order = self.value(format!("or i1 {negative}, {reversed}"));
                let invalid = self.value(format!("or i1 {invalid_order}, {excessive}"));
                self.trap_if(&invalid, 7, expression.span);
                // The guard proves this subtraction fits (0..=32). An empty
                // range may point one past the owner, but cannot be indexed.
                let count = self.value(format!("sub i64 {end}, {start}"));
                let pointer = self.element_pointer(&format!("%local{owner}"), &start, element);
                self.instruction(format!(
                    "store ptr {pointer}, ptr %local{local}.ptr, align 8"
                ));
                self.instruction(format!("store i64 {count}, ptr %local{local}.len, align 8"));
            }
            Type::Tensor | Type::TensorView { .. } => {
                if let ExprKind::Move(owner) = expression.kind {
                    let value = self.tensor_handle(owner);
                    self.store(local, &value);
                    self.store(owner, "0");
                } else {
                    let value = self.expression(expression);
                    self.store(local, &value);
                }
            }
            Type::TensorError => unreachable!("tensor errors are initialized by their scope"),
        }
    }

    fn union_storage_type(&self, ty: Type) -> String {
        format!("[{} x i64]", self.layouts.slots(ty))
    }

    fn owned_storage_type(&self, ty: Type) -> String {
        match ty {
            Type::Array { element, length } => {
                format!("[{length} x {}]", llvm_type(element.scalar_type()))
            }
            Type::Record(id) => record_type(id),
            Type::Union(_) => self.union_storage_type(ty),
            _ => unreachable!("checked inline owner storage"),
        }
    }

    fn copy_owned(&mut self, ty: Type, source: &str, destination: &str) {
        let storage = self.owned_storage_type(ty);
        let value = self.value(format!("load {storage}, ptr {source}, align 8"));
        self.instruction(format!(
            "store {storage} {value}, ptr {destination}, align 8"
        ));
    }

    fn call_argument(&mut self, expression: &ir::Expr) -> String {
        if expression.ty.inline_owned() {
            let ExprKind::Move(owner) = expression.kind else {
                unreachable!("checked aggregate arguments are bare owner moves");
            };
            format!("ptr %local{owner}")
        } else {
            let value = self.expression(expression);
            format!("{} {value}", llvm_type(expression.ty))
        }
    }

    fn initialize_owned(&mut self, destination: &str, expression: &ir::Expr) {
        match &expression.kind {
            ExprKind::Move(owner) => {
                self.copy_owned(expression.ty, &format!("%local{owner}"), destination)
            }
            ExprKind::MoveField { base, field } => {
                let source = self.record_pointer(*base, field);
                self.copy_owned(expression.ty, &source, destination);
            }
            ExprKind::Call { function, args } => {
                let mut arguments = vec![format!("ptr {destination}")];
                for arg in args {
                    arguments.push(self.call_argument(arg));
                }
                self.instruction(format!(
                    "call void @ax_fn_{function}({})",
                    arguments.join(", ")
                ));
            }
            ExprKind::Array(elements) => {
                let Type::Array { element, length } = expression.ty else {
                    unreachable!("checked array literal");
                };
                debug_assert_eq!(length, elements.len());
                for (index, initializer) in elements.iter().enumerate() {
                    let value = self.expression(initializer);
                    let address = self.element_pointer(destination, &index.to_string(), element);
                    self.instruction(format!(
                        "store {} {value}, ptr {address}, align 8",
                        llvm_type(element.scalar_type())
                    ));
                }
            }
            ExprKind::Record(fields) => {
                let Type::Record(id) = expression.ty else {
                    unreachable!("checked record literal");
                };
                self.instruction(format!(
                    "store {} zeroinitializer, ptr {destination}, align 8",
                    record_type(id)
                ));
                for (field, initializer) in fields {
                    let address = self.value(format!(
                        "getelementptr i64, ptr {destination}, i64 {}",
                        self.layouts.record_offset(id, *field)
                    ));
                    self.initialize_field(&address, initializer);
                }
            }
            ExprKind::Union { .. } => self.initialize_union(destination, expression),
            _ => unreachable!("checked owned initializer"),
        }
    }

    fn initialize_union(&mut self, destination: &str, expression: &ir::Expr) {
        let storage = self.union_storage_type(expression.ty);
        let Type::Union(id) = expression.ty else {
            unreachable!("checked union")
        };
        match &expression.kind {
            ExprKind::Union {
                layout_variant,
                variant,
                fields,
            } => {
                // Every inactive slot is defined; the tag commits only after all
                // field effects have succeeded. Destinations are not yet visible.
                self.instruction(format!(
                    "store {storage} zeroinitializer, ptr {destination}, align 8"
                ));
                for (field, initializer) in fields {
                    let address = self.value(format!(
                        "getelementptr i64, ptr {destination}, i64 {}",
                        self.layouts.union_offset(id, *layout_variant, *field)
                    ));
                    self.initialize_field(&address, initializer);
                }
                self.instruction(format!("store i64 {variant}, ptr {destination}, align 8"));
            }
            ExprKind::Move(owner) => {
                let value = self.value(format!("load {storage}, ptr %local{owner}, align 8"));
                self.instruction(format!(
                    "store {storage} {value}, ptr {destination}, align 8"
                ));
            }
            ExprKind::Call { function, args } => {
                let mut arguments = vec![format!("ptr {destination}")];
                for arg in args {
                    arguments.push(self.call_argument(arg));
                }
                self.instruction(format!(
                    "call void @ax_fn_{function}({})",
                    arguments.join(", ")
                ));
            }
            _ => unreachable!("checked union initializer"),
        }
    }

    fn initialize_field(&mut self, destination: &str, expression: &ir::Expr) {
        if expression.ty.inline_owned() {
            self.initialize_owned(destination, expression);
        } else {
            let value = self.expression(expression);
            self.store_field(destination, expression.ty, value);
        }
    }

    fn store_field(&mut self, destination: &str, ty: Type, value: String) {
        let bits = match ty {
            Type::I64 => value,
            Type::F64 => self.value(format!("bitcast double {value} to i64")),
            Type::Bool => self.value(format!("zext i1 {value} to i64")),
            Type::Unit => "0".into(),
            _ => unreachable!("checked scalar field"),
        };
        self.instruction(format!("store i64 {bits}, ptr {destination}, align 8"));
    }

    fn load_field(&mut self, address: &str, ty: Type) -> String {
        let bits = self.value(format!("load i64, ptr {address}, align 8"));
        match ty {
            Type::I64 => bits,
            Type::F64 => self.value(format!("bitcast i64 {bits} to double")),
            Type::Bool => self.value(format!("trunc i64 {bits} to i1")),
            Type::Unit => "0".into(),
            _ => unreachable!("checked scalar field"),
        }
    }

    fn record_pointer(&mut self, local: ir::LocalId, field: &[usize]) -> String {
        let mut ty = self.function.locals[local].ty;
        let mut offset = 0;
        for &index in field {
            let Type::Record(id) = ty else {
                unreachable!("checked record path")
            };
            offset += self.layouts.record_offset(id, index);
            ty = self
                .program
                .records
                .iter()
                .find(|r| r.id == id)
                .expect("checked record")
                .fields[index]
                .ty;
        }
        self.value(format!(
            "getelementptr i64, ptr %local{local}, i64 {offset}"
        ))
    }

    fn length(&mut self, local: ir::LocalId) -> String {
        match self.function.locals[local].ty {
            Type::Array { length, .. } => length.to_string(),
            Type::Slice { .. } => self.value(format!("load i64, ptr %local{local}.len, align 8")),
            _ => unreachable!("checked aggregate length"),
        }
    }

    fn element_pointer(&mut self, base: &str, index: &str, element: ElementType) -> String {
        // Deliberately no inbounds or no-wrap flags. User-controlled indices
        // reach this operation only after an explicit bounds guard.
        let element = llvm_type(element.scalar_type());
        self.value(format!("getelementptr {element}, ptr {base}, i64 {index}"))
    }

    fn indexed_address(&mut self, base: ir::LocalId, index: &ir::Expr, span: Span) -> String {
        let index = self.expression(index);
        let length = self.length(base);
        let negative = self.value(format!("icmp slt i64 {index}, 0"));
        let excessive = self.value(format!("icmp sge i64 {index}, {length}"));
        let invalid = self.value(format!("or i1 {negative}, {excessive}"));
        self.trap_if(&invalid, 7, span);
        let pointer = match self.function.locals[base].ty {
            Type::Array { .. } => format!("%local{base}"),
            Type::Slice { .. } => self.value(format!("load ptr, ptr %local{base}.ptr, align 8")),
            _ => unreachable!("checked indexed aggregate"),
        };
        self.element_pointer(
            &pointer,
            &index,
            aggregate_element(self.function.locals[base].ty),
        )
    }

    fn block(&mut self, block: &ir::Block) {
        if self.tensor_callback {
            for local in &block.cleanup {
                self.store(*local, "0");
            }
        }
        self.cleanup_stack.push(block.cleanup.clone());
        for statement in &block.stmts {
            if self.terminated {
                break;
            }
            self.statement(statement);
        }
        if !self.terminated && self.tensor_callback {
            self.cleanup_from(self.cleanup_stack.len() - 1, block.span);
        }
        self.cleanup_stack.pop();
    }

    fn cleanup_from(&mut self, depth: usize, span: Span) {
        if !self.tensor_callback {
            return;
        }
        let locals: Vec<_> = self.cleanup_stack[depth..]
            .iter()
            .rev()
            .flat_map(|locals| locals.iter().rev().copied())
            .collect();
        for local in locals {
            let handle = self.tensor_handle(local);
            let status = self
                .value(format!(
                "call i32 @ax_tensor_v1_release(ptr %tensor.context, i64 {handle}, i64 {}, i64 {})",
                span.start, span.end));
            self.tensor_status(&status);
            self.store(local, "0");
        }
    }

    fn tensor_status(&mut self, status: &str) {
        debug_assert!(self.tensor_callback);
        let valid = self.value(format!("icmp eq i32 {status}, 0"));
        let success = self.fresh_label("tensor.ok");
        let failure = self.fresh_label("tensor.error");
        self.conditional_branch(&valid, &success, &failure);
        self.label(&failure);
        // Rust receives the error before dropping every remaining arena entry.
        self.terminate(format!("ret i32 {status}"));
        self.label(&success);
    }

    fn tensor_handle(&mut self, local: ir::LocalId) -> String {
        self.value(format!("load i64, ptr %local{local}, align 8"))
    }

    fn statement(&mut self, statement: &ir::Stmt) {
        match &statement.kind {
            StmtKind::Match { owner, arms } => {
                let Type::Union(id) = self.function.locals[*owner].ty else {
                    unreachable!("checked match owner")
                };
                let tag = self.value(format!("load i64, ptr %local{owner}, align 8"));
                let invalid = self.fresh_label("union.invalid");
                let end = self.fresh_label("union.end");
                let labels: Vec<_> = arms.iter().map(|_| self.fresh_label("union.arm")).collect();
                let cases = arms
                    .iter()
                    .zip(&labels)
                    .map(|(arm, label)| format!("i64 {}, label %{label}", arm.variant))
                    .collect::<Vec<_>>()
                    .join(" ");
                self.terminate(format!("switch i64 {tag}, label %{invalid} [ {cases} ]"));
                self.label(&invalid);
                self.instruction(self.locations.call("trap", "i32 8, ", statement.span));
                self.terminate("unreachable".to_owned());
                let mut continuing = false;
                for (arm, label) in arms.iter().zip(labels) {
                    self.label(&label);
                    for (field, local) in &arm.bindings {
                        let address = self.value(format!(
                            "getelementptr i64, ptr %local{owner}, i64 {}",
                            self.layouts.union_offset(id, arm.variant, *field)
                        ));
                        let ty = self.function.locals[*local].ty;
                        if ty.inline_owned() {
                            self.copy_owned(ty, &address, &format!("%local{local}"));
                        } else {
                            let value = self.load_field(&address, ty);
                            self.store(*local, &value);
                        }
                    }
                    self.block(&arm.body);
                    if !self.terminated {
                        continuing = true;
                        self.branch(&end);
                    }
                }
                if continuing {
                    self.label(&end);
                }
            }
            StmtKind::Let { local, value } => self.initialize(*local, value),
            StmtKind::Assign { local, value } => {
                let value = self.expression(value);
                self.store(*local, &value);
            }
            StmtKind::IndexAssign { base, index, value } => {
                // Target computation and its trap precede all RHS effects.
                let address = self.indexed_address(*base, index, statement.span);
                let value_type = llvm_type(value.ty);
                let value = self.expression(value);
                self.instruction(format!(
                    "store {value_type} {value}, ptr {address}, align 8"
                ));
            }
            StmtKind::FieldAssign { base, field, value } => {
                let address = self.record_pointer(*base, field);
                let ty = value.ty;
                let value = self.expression(value);
                self.store_field(&address, ty, value);
            }
            StmtKind::TensorSet { base, index, value } => {
                let handle = self.tensor_handle(*base);
                let index = self.expression(index);
                let span = statement.span;
                let status = self.value(format!(
                    "call i32 @ax_tensor_v1_check_index(ptr %tensor.context, i64 {handle}, i64 {index}, i64 {}, i64 {})",
                    span.start, span.end));
                self.tensor_status(&status);
                let value = self.expression(value);
                let status = self.value(format!(
                    "call i32 @ax_tensor_v1_set(ptr %tensor.context, i64 {handle}, i64 {index}, double {value}, i64 {}, i64 {})",
                    span.start, span.end));
                self.tensor_status(&status);
            }
            StmtKind::TensorScope {
                budget,
                body,
                error_local,
                handler,
            } => {
                debug_assert!(
                    !self.tensor_callback,
                    "nested lexical tensor scopes are rejected"
                );
                let budget = self.expression(budget);
                let helper = format!("ax_tensor_body_{}_{}", self.function_id, self.next_scope);
                self.next_scope += 1;
                let callback = Self::new(self.program, self.function, self.locations, self.layouts)
                    .emit_tensor_callback(&helper, body);
                self.helpers.push_str(&callback);
                let span = statement.span;
                let status = self.value(format!(
                    "call i32 @ax_tensor_v1_scope(i64 {budget}, ptr @{helper}, ptr %tensor.captures, i64 {}, i64 {}, ptr %local{error_local})",
                    span.start, span.end));
                let valid = self.value(format!("icmp eq i32 {status}, 0"));
                let caught = self.fresh_label("tensor.catch");
                let end = self.fresh_label("tensor.scope.end");
                self.conditional_branch(&valid, &end, &caught);
                self.label(&caught);
                self.block(handler);
                if !self.terminated {
                    self.branch(&end);
                }
                self.label(&end);
            }
            StmtKind::Expr(expression) => {
                self.expression(expression);
            }
            StmtKind::Block(block) => self.block(block),
            StmtKind::Return(expression) => {
                debug_assert!(!self.tensor_callback, "scope-crossing returns are rejected");
                if expression.ty.inline_owned() {
                    self.initialize_owned("%union.result", expression);
                    self.instruction("call void @ax_rt_leave()".to_owned());
                    self.terminate("ret void".to_owned());
                } else {
                    let result = self.expression(expression);
                    self.instruction("call void @ax_rt_leave()".to_owned());
                    self.terminate(format!("ret {} {result}", llvm_type(expression.ty)));
                }
            }
            StmtKind::Print(expression) => {
                let result = self.expression(expression);
                match expression.ty {
                    Type::I64 => self.instruction(self.locations.call(
                        "print_i64",
                        &format!("i64 {result}, "),
                        statement.span,
                    )),
                    Type::F64 => self.instruction(self.locations.call(
                        "print_f64",
                        &format!("double {result}, "),
                        statement.span,
                    )),
                    Type::Bool => {
                        let widened = self.value(format!("zext i1 {result} to i32"));
                        self.instruction(self.locations.call(
                            "print_bool",
                            &format!("i32 {widened}, "),
                            statement.span,
                        ));
                    }
                    Type::Unit
                    | Type::Array { .. }
                    | Type::Record(_)
                    | Type::Union(_)
                    | Type::Slice { .. }
                    | Type::Tensor
                    | Type::TensorView { .. }
                    | Type::TensorError => unreachable!("checked printable scalar"),
                }
            }
            StmtKind::If {
                condition,
                then_block,
                else_block,
            } => {
                let condition = self.expression(condition);
                let yes = self.fresh_label("if.then");
                let no = self.fresh_label("if.else");
                let end = self.fresh_label("if.end");
                self.conditional_branch(&condition, &yes, &no);
                self.label(&yes);
                self.block(then_block);
                let yes_terminated = self.terminated;
                if !yes_terminated {
                    self.branch(&end);
                }
                self.label(&no);
                if let Some(block) = else_block {
                    self.block(block);
                }
                let no_terminated = self.terminated;
                if !no_terminated {
                    self.branch(&end);
                }
                if !(yes_terminated && no_terminated) {
                    self.label(&end);
                }
            }
            StmtKind::While { condition, body } => {
                let test = self.fresh_label("while.condition");
                let body_label = self.fresh_label("while.body");
                let end = self.fresh_label("while.end");
                self.branch(&test);
                self.label(&test);
                let condition = self.expression(condition);
                self.conditional_branch(&condition, &body_label, &end);
                self.label(&body_label);
                self.loops.push(LoopTargets {
                    condition: test.clone(),
                    end: end.clone(),
                    cleanup_depth: self.cleanup_stack.len(),
                });
                self.block(body);
                self.loops.pop();
                if !self.terminated {
                    self.branch(&test);
                }
                self.label(&end);
            }
            StmtKind::Break => {
                let target = self.loops.last().expect("checked break target").end.clone();
                let depth = self.loops.last().unwrap().cleanup_depth;
                self.cleanup_from(depth, statement.span);
                self.branch(&target);
            }
            StmtKind::Continue => {
                let target = self
                    .loops
                    .last()
                    .expect("checked continue target")
                    .condition
                    .clone();
                let depth = self.loops.last().unwrap().cleanup_depth;
                self.cleanup_from(depth, statement.span);
                self.branch(&target);
            }
        }
    }

    fn expression(&mut self, expression: &ir::Expr) -> String {
        match &expression.kind {
            ExprKind::Int(value) => value.to_string(),
            ExprKind::Float(bits) => format!("0x{bits:016X}"),
            ExprKind::Bool(value) => value.to_string(),
            ExprKind::Unit => "0".into(),
            ExprKind::Var(local) => {
                let ty = self.function.locals[*local].ty;
                self.value(format!(
                    "load {}, ptr %local{local}, align {}",
                    llvm_type(ty),
                    alignment(ty)
                ))
            }
            ExprKind::Index { base, index } => {
                let address = self.indexed_address(*base, index, expression.span);
                let value_type = llvm_type(expression.ty);
                self.value(format!("load {value_type}, ptr {address}, align 8"))
            }
            ExprKind::Field { base, field } => {
                let address = self.record_pointer(*base, field);
                self.load_field(&address, expression.ty)
            }
            ExprKind::Len(local) => self.length(*local),
            ExprKind::Array(_)
            | ExprKind::Record(_)
            | ExprKind::Union { .. }
            | ExprKind::Move(_)
            | ExprKind::MoveField { .. }
            | ExprKind::Borrow { .. } => {
                unreachable!("checked aggregates only initialize local bindings")
            }
            ExprKind::Call { function, args } => {
                // Construct argument values in source order before the call.
                let args = args
                    .iter()
                    .map(|argument| self.call_argument(argument))
                    .collect::<Vec<_>>()
                    .join(", ");
                self.value(format!(
                    "call {} @ax_fn_{function}({args})",
                    llvm_type(self.program.functions[*function].return_type)
                ))
            }
            ExprKind::Intrinsic { op, value } => {
                let value = self.expression(value);
                match op {
                    FloatIntrinsic::FromI64 => self.value(format!("sitofp i64 {value} to double")),
                    FloatIntrinsic::ToBits => {
                        let bits = self.value(format!("bitcast double {value} to i64"));
                        self.canonical_float_bits(&bits)
                    }
                    FloatIntrinsic::FromBits => {
                        let bits = self.canonical_float_bits(&value);
                        self.value(format!("bitcast i64 {bits} to double"))
                    }
                }
            }
            ExprKind::TensorCall { op, args } => {
                self.tensor_call(*op, args, expression.ty, expression.span)
            }
            ExprKind::Unary { op, value } => {
                let value_type = value.ty;
                let value = self.expression(value);
                match op {
                    UnaryOp::Neg if value_type == Type::F64 => {
                        self.value(format!("fneg double {value}"))
                    }
                    UnaryOp::Neg => self.checked_arithmetic("ssub", "0", &value, expression.span),
                    UnaryOp::Not => self.value(format!("xor i1 {value}, true")),
                    UnaryOp::BitNot => self.value(format!("xor i64 {value}, -1")),
                }
            }
            ExprKind::Binary { op, left, right } => {
                if matches!(op, BinaryOp::And | BinaryOp::Or) {
                    return self.short_circuit(*op, left, right);
                }
                let left_type = llvm_type(left.ty);
                let floating = left.ty == Type::F64;
                let left = self.expression(left);
                let right = self.expression(right);
                if floating {
                    return self.float_binary(*op, &left, &right);
                }
                match op {
                    BinaryOp::Add => {
                        self.checked_arithmetic("sadd", &left, &right, expression.span)
                    }
                    BinaryOp::Sub => {
                        self.checked_arithmetic("ssub", &left, &right, expression.span)
                    }
                    BinaryOp::Mul => {
                        self.checked_arithmetic("smul", &left, &right, expression.span)
                    }
                    BinaryOp::Div | BinaryOp::Rem => {
                        let zero = self.value(format!("icmp eq i64 {right}, 0"));
                        self.trap_if(&zero, 2, expression.span);
                        let minimum =
                            self.value(format!("icmp eq i64 {left}, -9223372036854775808"));
                        let minus_one = self.value(format!("icmp eq i64 {right}, -1"));
                        let overflow = self.value(format!("and i1 {minimum}, {minus_one}"));
                        self.trap_if(&overflow, 1, expression.span);
                        let instruction = if *op == BinaryOp::Div { "sdiv" } else { "srem" };
                        self.value(format!("{instruction} i64 {left}, {right}"))
                    }
                    BinaryOp::Shl | BinaryOp::Shr => {
                        let negative = self.value(format!("icmp slt i64 {right}, 0"));
                        let large = self.value(format!("icmp sgt i64 {right}, 63"));
                        let invalid = self.value(format!("or i1 {negative}, {large}"));
                        self.trap_if(&invalid, 3, expression.span);
                        if *op == BinaryOp::Shr {
                            self.value(format!("ashr i64 {left}, {right}"))
                        } else {
                            // With a valid count, plain shl wraps without poison.
                            // A signed arithmetic backshift detects precisely
                            // whether mathematical multiplication by 2^n fits.
                            let shifted = self.value(format!("shl i64 {left}, {right}"));
                            let restored = self.value(format!("ashr i64 {shifted}, {right}"));
                            let overflow = self.value(format!("icmp ne i64 {restored}, {left}"));
                            self.trap_if(&overflow, 1, expression.span);
                            shifted
                        }
                    }
                    BinaryOp::BitAnd => self.value(format!("and i64 {left}, {right}")),
                    BinaryOp::BitOr => self.value(format!("or i64 {left}, {right}")),
                    BinaryOp::BitXor => self.value(format!("xor i64 {left}, {right}")),
                    BinaryOp::Eq => self.value(format!("icmp eq {left_type} {left}, {right}")),
                    BinaryOp::Ne => self.value(format!("icmp ne {left_type} {left}, {right}")),
                    BinaryOp::Lt => self.value(format!("icmp slt i64 {left}, {right}")),
                    BinaryOp::Le => self.value(format!("icmp sle i64 {left}, {right}")),
                    BinaryOp::Gt => self.value(format!("icmp sgt i64 {left}, {right}")),
                    BinaryOp::Ge => self.value(format!("icmp sge i64 {left}, {right}")),
                    BinaryOp::And | BinaryOp::Or => unreachable!(),
                }
            }
        }
    }

    fn tensor_call(
        &mut self,
        op: TensorIntrinsic,
        args: &[TensorArg],
        ty: Type,
        span: Span,
    ) -> String {
        // Finish every source argument left-to-right before performing a
        // runtime check or acquiring a view loan. List storage is one fixed
        // callback-entry scratch array; the source grammar admits one list.
        let arguments: Vec<_> = args
            .iter()
            .map(|argument| match argument {
                TensorArg::Local { local, .. } => TensorValue::Local(*local),
                TensorArg::Scalar(expression) => TensorValue::Scalar(self.expression(expression)),
                TensorArg::List(expressions) => {
                    for (index, expression) in expressions.iter().enumerate() {
                        let value = self.expression(expression);
                        let slot =
                            self.value(format!("getelementptr i64, ptr %tensor.list, i64 {index}"));
                        self.instruction(format!("store i64 {value}, ptr {slot}, align 8"));
                    }
                    TensorValue::List(expressions.len())
                }
            })
            .collect();
        if matches!(
            op,
            TensorIntrinsic::ErrorCode | TensorIntrinsic::ErrorStart | TensorIntrinsic::ErrorEnd
        ) {
            let local = arguments[0].local();
            let index = match op {
                TensorIntrinsic::ErrorCode => 0,
                TensorIntrinsic::ErrorStart => 1,
                TensorIntrinsic::ErrorEnd => 2,
                _ => unreachable!(),
            };
            let address = self.value(format!("getelementptr i64, ptr %local{local}, i64 {index}"));
            return self.value(format!("load i64, ptr {address}, align 8"));
        }
        debug_assert!(self.tensor_callback);
        let mutable = i64::from(matches!(ty, Type::TensorView { mutable: true }));
        let parameters = match op {
            TensorIntrinsic::Zeros => format!("ptr %tensor.list, i64 {}", arguments[0].list_len()),
            TensorIntrinsic::FromArray => {
                let local = arguments[1].local();
                let pointer = match self.function.locals[local].ty {
                    Type::Array { .. } => format!("%local{local}"),
                    Type::Slice { .. } => {
                        self.value(format!("load ptr, ptr %local{local}.ptr, align 8"))
                    }
                    _ => unreachable!("checked f64 array source"),
                };
                let length = self.length(local);
                format!(
                    "ptr %tensor.list, i64 {}, ptr {pointer}, i64 {length}",
                    arguments[0].list_len()
                )
            }
            TensorIntrinsic::View => {
                let handle = self.tensor_handle(arguments[0].local());
                format!("i64 {handle}, i64 {mutable}")
            }
            TensorIntrinsic::Transpose => {
                let handle = self.tensor_handle(arguments[0].local());
                format!(
                    "i64 {handle}, i64 {mutable}, ptr %tensor.list, i64 {}",
                    arguments[1].list_len()
                )
            }
            TensorIntrinsic::Slice => {
                let handle = self.tensor_handle(arguments[0].local());
                format!(
                    "i64 {handle}, i64 {mutable}, i64 {}, i64 {}, i64 {}, i64 {}",
                    arguments[1].scalar(),
                    arguments[2].scalar(),
                    arguments[3].scalar(),
                    arguments[4].scalar()
                )
            }
            TensorIntrinsic::Add | TensorIntrinsic::Mul | TensorIntrinsic::Matmul => {
                let left = self.tensor_handle(arguments[0].local());
                let right = self.tensor_handle(arguments[1].local());
                format!("i64 {left}, i64 {right}")
            }
            TensorIntrinsic::Sigmoid
            | TensorIntrinsic::Softmax
            | TensorIntrinsic::Sum
            | TensorIntrinsic::Logsumexp
            | TensorIntrinsic::Rank
            | TensorIntrinsic::Len => {
                let handle = self.tensor_handle(arguments[0].local());
                format!("i64 {handle}")
            }
            TensorIntrinsic::Get | TensorIntrinsic::Dim => {
                let handle = self.tensor_handle(arguments[0].local());
                format!("i64 {handle}, i64 {}", arguments[1].scalar())
            }
            TensorIntrinsic::Set => unreachable!("tensor_set has a dedicated statement"),
            TensorIntrinsic::ErrorCode
            | TensorIntrinsic::ErrorStart
            | TensorIntrinsic::ErrorEnd => unreachable!(),
        };
        let name = op.name().strip_prefix("tensor_").unwrap();
        let status = self.value(format!(
            "call i32 @ax_tensor_v1_{name}(ptr %tensor.context, {parameters}, i64 {}, i64 {}, ptr %tensor.result)",
            span.start, span.end));
        self.tensor_status(&status);
        // The runtime must write a successful output; failed outputs are never
        // loaded. Handles, integers and double values share aligned scratch.
        self.value(format!(
            "load {}, ptr %tensor.result, align 8",
            llvm_type(ty)
        ))
    }

    fn float_binary(&mut self, op: BinaryOp, left: &str, right: &str) -> String {
        // Ordinary IEEE binary64 operations in the default process environment:
        // no fast-math, contraction, reassociation, or exception observation.
        let instruction = match op {
            BinaryOp::Add => "fadd",
            BinaryOp::Sub => "fsub",
            BinaryOp::Mul => "fmul",
            BinaryOp::Div => "fdiv",
            BinaryOp::Eq => "fcmp oeq",
            BinaryOp::Ne => "fcmp une",
            BinaryOp::Lt => "fcmp olt",
            BinaryOp::Le => "fcmp ole",
            BinaryOp::Gt => "fcmp ogt",
            BinaryOp::Ge => "fcmp oge",
            _ => unreachable!("checked floating-point operator"),
        };
        self.value(format!("{instruction} double {left}, {right}"))
    }

    fn canonical_float_bits(&mut self, bits: &str) -> String {
        // Sign/payload are intentionally unspecified during arithmetic. Only
        // observations and from-bits normalize NaNs, preserving every other bit.
        let magnitude = self.value(format!("and i64 {bits}, 9223372036854775807"));
        let nan = self.value(format!("icmp ugt i64 {magnitude}, 9218868437227405312"));
        self.value(format!(
            "select i1 {nan}, i64 9221120237041090560, i64 {bits}"
        ))
    }

    fn short_circuit(&mut self, op: BinaryOp, left: &ir::Expr, right: &ir::Expr) -> String {
        let left = self.expression(left);
        let left_block = self.current_block.clone();
        let rhs = self.fresh_label("logic.rhs");
        let end = self.fresh_label("logic.end");
        let short_value = if op == BinaryOp::And {
            self.conditional_branch(&left, &rhs, &end);
            "false"
        } else {
            self.conditional_branch(&left, &end, &rhs);
            "true"
        };
        self.label(&rhs);
        let right = self.expression(right);
        let right_block = self.current_block.clone();
        self.branch(&end);
        self.label(&end);
        self.value(format!(
            "phi i1 [ {short_value}, %{left_block} ], [ {right}, %{right_block} ]"
        ))
    }

    fn checked_arithmetic(
        &mut self,
        operation: &str,
        left: &str,
        right: &str,
        span: Span,
    ) -> String {
        let pair = self.value(format!(
            "call {{ i64, i1 }} @llvm.{operation}.with.overflow.i64(i64 {left}, i64 {right})"
        ));
        let value = self.value(format!("extractvalue {{ i64, i1 }} {pair}, 0"));
        let overflow = self.value(format!("extractvalue {{ i64, i1 }} {pair}, 1"));
        self.trap_if(&overflow, 1, span);
        value
    }

    fn trap_if(&mut self, condition: &str, kind: i32, span: Span) {
        let trap = self.fresh_label("trap");
        let valid = self.fresh_label("checked");
        self.conditional_branch(condition, &trap, &valid);
        self.label(&trap);
        self.instruction(self.locations.call("trap", &format!("i32 {kind}, "), span));
        self.terminate("unreachable".to_owned());
        self.label(&valid);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tensor_runtime_is_optional_and_checked_before_language_main() {
        let plain = emit(
            &crate::compile("fn main() -> i64 { return 0; }").unwrap(),
            "",
        );
        assert!(!plain.contains("ax_tensor_"));
        let source = "fn main() -> i64 { tensor_scope(0) {} catch (e: tensor_error) {} return 0; }";
        let output = emit(&crate::compile(source).unwrap(), source);
        let hosted_main = output.split("define i32 @main()").nth(1).unwrap();
        assert!(
            hosted_main
                .find("@ax_tensor_runtime_abi_version()")
                .unwrap()
                < hosted_main.find("@ax_fn_0()").unwrap()
        );
        assert!(hosted_main.contains("icmp eq i32 %tensor.abi, 1"));
        assert!(hosted_main.contains("icmp eq i64 %tensor.version, 256"));
        assert!(hosted_main.contains("call void @ax_rt_tensor_runtime_mismatch()"));
        assert_eq!(output.matches("call void @ax_rt_enter").count(), 1);
        assert_eq!(output.matches("call void @ax_rt_leave").count(), 1);
    }

    #[test]
    fn tensor_callbacks_capture_slots_and_allocate_scratch_only_at_entry() {
        let source = "fn main() -> i64 { let values: [f64; 2] = [1.0, 2.0]; let mut i: i64 = 0; tensor_scope(32) { while i < 3 { let a: tensor = try tensor_from_array([2], values); i = i + 1; continue; } } catch (e: tensor_error) { print(tensor_error_code(e)); } return i; }";
        let output = emit(&crate::compile(source).unwrap(), source);
        assert!(output.contains("store ptr %local0, ptr"));
        let callback = output
            .split("define internal i32 @ax_tensor_body_0_0")
            .nth(1)
            .unwrap()
            .split("define i32 @main()")
            .next()
            .unwrap();
        assert!(callback.contains("%local0 = load ptr"));
        assert!(callback.contains("%tensor.list = alloca [8 x i64], align 8"));
        assert!(callback.contains("%tensor.result = alloca i64, align 8"));
        assert_eq!(callback.matches(" = alloca ").count(), 2);
        assert!(!callback.contains("call void @ax_rt_enter"));
        assert!(!callback.contains("call void @ax_rt_leave"));
        let release = callback.find("call i32 @ax_tensor_v1_release").unwrap();
        assert!(callback[release..].contains("br label %while.condition"));
        assert!(callback.contains("ret i32 0"));
        assert!(callback.contains("ret i32 %v"));
    }

    #[test]
    fn tensor_set_checks_target_before_rhs_and_loads_outputs_only_after_success() {
        let source = "fn value() -> f64 { print(99); return 2.0; } fn main() -> i64 { tensor_scope(8) { let mut a: tensor = try tensor_zeros([1]); try tensor_set(a, 1, value()); } catch (e: tensor_error) { print(tensor_error_code(e)); } return 0; }";
        let output = emit(&crate::compile(source).unwrap(), source);
        let callback = output
            .split("define internal i32 @ax_tensor_body_1_0")
            .nth(1)
            .unwrap();
        let check = callback.find("call i32 @ax_tensor_v1_check_index").unwrap();
        let rhs = callback.find("call double @ax_fn_0()").unwrap();
        let set = callback.find("call i32 @ax_tensor_v1_set").unwrap();
        assert!(check < rhs && rhs < set);
        let constructor = callback.find("call i32 @ax_tensor_v1_zeros").unwrap();
        let output_load = callback.find("load i64, ptr %tensor.result").unwrap();
        assert!(callback[constructor..output_load].contains("ret i32 %v"));
        assert!(callback[constructor..output_load].contains("tensor.ok"));
    }

    #[test]
    fn tensor_shape_arguments_finish_left_to_right_before_runtime_validation() {
        let source = "fn first() -> i64 { print(1); return -1; } fn second() -> i64 { print(2); return 4; } fn main() -> i64 { tensor_scope(0) { let a: tensor = try tensor_zeros([first(), second()]); } catch (e: tensor_error) { print(tensor_error_code(e)); } return 0; }";
        let output = emit(&crate::compile(source).unwrap(), source);
        let callback = output
            .split("define internal i32 @ax_tensor_body_2_0")
            .nth(1)
            .unwrap();
        let first = callback.find("call i64 @ax_fn_0()").unwrap();
        let second = callback.find("call i64 @ax_fn_1()").unwrap();
        let validate = callback.find("call i32 @ax_tensor_v1_zeros").unwrap();
        assert!(first < second && second < validate);
    }

    fn integer(value: i64) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Int(value),
            ty: Type::I64,
            span: Span::new(2, 3),
        }
    }

    fn binary(op: BinaryOp, left: ir::Expr, right: ir::Expr, ty: Type) -> ir::Expr {
        ir::Expr {
            kind: ExprKind::Binary {
                op,
                left: Box::new(left),
                right: Box::new(right),
            },
            ty,
            span: Span::new(2, 3),
        }
    }

    fn program(expression: ir::Expr) -> ir::Program {
        ir::Program {
            records: Vec::new(),
            unions: Vec::new(),
            main: 0,
            functions: vec![ir::Function {
                name: "main".to_owned(),
                params: vec![],
                return_type: Type::I64,
                locals: vec![],
                body: ir::Block {
                    stmts: vec![ir::Stmt {
                        kind: StmtKind::Return(expression),
                        span: Span::new(0, 3),
                    }],
                    span: Span::new(0, 3),
                    cleanup: vec![],
                },
                span: Span::new(0, 3),
            }],
        }
    }

    fn expression(kind: ExprKind, ty: Type) -> ir::Expr {
        ir::Expr {
            kind,
            ty,
            span: Span::new(2, 3),
        }
    }

    fn statement(kind: StmtKind) -> ir::Stmt {
        ir::Stmt {
            kind,
            span: Span::new(0, 1),
        }
    }

    fn local(ty: Type) -> ir::Local {
        ir::Local {
            name: "test".into(),
            ty,
            mutable: true,
            span: Span::default(),
        }
    }

    fn array_initializer(local: usize) -> ir::Stmt {
        statement(StmtKind::Let {
            local,
            value: expression(
                ExprKind::Array(vec![integer(3), integer(4)]),
                Type::Array {
                    element: ElementType::I64,
                    length: 2,
                },
            ),
        })
    }

    fn floating(value: f64) -> ir::Expr {
        expression(ExprKind::Float(value.to_bits()), Type::F64)
    }

    fn float_intrinsic(op: FloatIntrinsic, value: ir::Expr) -> ir::Expr {
        let ty = if matches!(op, FloatIntrinsic::ToBits) {
            Type::I64
        } else {
            Type::F64
        };
        expression(
            ExprKind::Intrinsic {
                op,
                value: Box::new(value),
            },
            ty,
        )
    }

    #[test]
    fn floating_arithmetic_has_no_unsafe_flags_or_per_operation_nan_normalization() {
        let mut fixture = program(integer(0));
        for op in [BinaryOp::Add, BinaryOp::Sub, BinaryOp::Mul, BinaryOp::Div] {
            fixture.functions[0].body.stmts.insert(
                0,
                statement(StmtKind::Print(binary(
                    op,
                    floating(-0.0),
                    floating(0.0),
                    Type::F64,
                ))),
            );
        }
        for op in [
            BinaryOp::Eq,
            BinaryOp::Ne,
            BinaryOp::Lt,
            BinaryOp::Le,
            BinaryOp::Gt,
            BinaryOp::Ge,
        ] {
            fixture.functions[0].body.stmts.insert(
                0,
                statement(StmtKind::Print(binary(
                    op,
                    floating(f64::NAN),
                    floating(1.0),
                    Type::Bool,
                ))),
            );
        }
        fixture.functions[0].body.stmts.insert(
            0,
            statement(StmtKind::Print(expression(
                ExprKind::Unary {
                    op: UnaryOp::Neg,
                    value: Box::new(floating(0.0)),
                },
                Type::F64,
            ))),
        );
        fixture.functions[0].body.stmts.insert(
            0,
            statement(StmtKind::Print(float_intrinsic(
                FloatIntrinsic::FromI64,
                integer(i64::MAX),
            ))),
        );
        let output = emit(&fixture, "a\nb");
        for instruction in [
            "fadd", "fsub", "fmul", "fdiv", "fneg", "fcmp oeq", "fcmp une", "fcmp olt", "fcmp ole",
            "fcmp ogt", "fcmp oge",
        ] {
            assert!(output.contains(&format!(" = {instruction} double ")));
        }
        let forbidden = [
            "fast", "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc",
        ];
        for instruction in output.lines().filter(|line| line.contains(" = f")) {
            assert!(!instruction
                .split_whitespace()
                .any(|word| forbidden.contains(&word)));
        }
        assert!(!output.contains("llvm.fma"));
        assert!(!output.contains("llvm.fmuladd"));
        assert!(!output.contains("select i1"));
        assert!(!output.contains("call void @ax_rt_trap"));
        assert!(output.contains("sitofp i64 9223372036854775807 to double"));
        assert!(output.contains("call void @ax_rt_print_f64(double"));
        assert!(output.contains("0x8000000000000000"));
    }

    #[test]
    fn floating_bit_boundaries_canonicalize_nan_without_float_comparisons() {
        let signaling_nan = integer(0xfff0000000000001u64 as i64);
        let converted = float_intrinsic(FloatIntrinsic::FromBits, signaling_nan);
        let observed = float_intrinsic(FloatIntrinsic::ToBits, converted);
        let output = emit(&program(observed), "a\nb");
        assert_eq!(output.matches("icmp ugt i64").count(), 2);
        assert_eq!(output.matches(", i64 9221120237041090560, i64").count(), 2);
        assert!(output.contains("bitcast i64"));
        assert!(output.contains("bitcast double"));
        assert!(!output.contains("fcmp"));
    }

    #[test]
    fn floating_arrays_moves_and_slices_use_double_elements() {
        let array = Type::Array {
            element: ElementType::F64,
            length: 2,
        };
        let slice = Type::Slice {
            element: ElementType::F64,
            mutable: true,
        };
        let mut fixture = program(integer(0));
        fixture.functions[0].locals = vec![local(array), local(array), local(slice)];
        fixture.functions[0].body.stmts = vec![
            statement(StmtKind::Let {
                local: 0,
                value: expression(ExprKind::Array(vec![floating(1.5), floating(-0.0)]), array),
            }),
            statement(StmtKind::Let {
                local: 1,
                value: expression(ExprKind::Move(0), array),
            }),
            statement(StmtKind::Let {
                local: 2,
                value: expression(
                    ExprKind::Borrow {
                        owner: 1,
                        mutable: true,
                        start: Box::new(integer(0)),
                        end: Box::new(integer(2)),
                    },
                    slice,
                ),
            }),
            statement(StmtKind::IndexAssign {
                base: 2,
                index: integer(0),
                value: floating(2.25),
            }),
            statement(StmtKind::Return(float_intrinsic(
                FloatIntrinsic::ToBits,
                expression(
                    ExprKind::Index {
                        base: 2,
                        index: Box::new(integer(0)),
                    },
                    Type::F64,
                ),
            ))),
        ];
        let output = emit(&fixture, "a\nb");
        assert_eq!(output.matches("alloca [2 x double], align 8").count(), 2);
        assert!(output.contains("load [2 x double], ptr %local0, align 8"));
        assert!(output.contains("store [2 x double]"));
        assert!(output.contains("store double 0x4002000000000000"));
        assert!(output.contains("getelementptr double, ptr"));
        assert!(output.contains("load double, ptr"));
        assert!(!output.contains("getelementptr inbounds"));
    }

    #[test]
    fn arrays_move_between_fixed_entry_slots_and_slices_keep_pointer_and_length() {
        let mut fixture = program(integer(0));
        fixture.functions[0].locals = vec![
            local(Type::Array {
                element: ElementType::I64,
                length: 2,
            }),
            local(Type::Array {
                element: ElementType::I64,
                length: 2,
            }),
            local(Type::Slice {
                element: ElementType::I64,
                mutable: true,
            }),
        ];
        fixture.functions[0].body.stmts = vec![
            array_initializer(0),
            statement(StmtKind::Let {
                local: 1,
                value: expression(
                    ExprKind::Move(0),
                    Type::Array {
                        element: ElementType::I64,
                        length: 2,
                    },
                ),
            }),
            statement(StmtKind::Let {
                local: 2,
                value: expression(
                    ExprKind::Borrow {
                        owner: 1,
                        mutable: true,
                        start: Box::new(integer(1)),
                        end: Box::new(integer(2)),
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: true,
                    },
                ),
            }),
            statement(StmtKind::IndexAssign {
                base: 2,
                index: integer(0),
                value: integer(9),
            }),
            statement(StmtKind::Return(expression(
                ExprKind::Index {
                    base: 2,
                    index: Box::new(integer(0)),
                },
                Type::I64,
            ))),
        ];
        let output = emit(&fixture, "a\nb");
        assert_eq!(output.matches("alloca [2 x i64], align 8").count(), 2);
        assert!(output.contains("%local2.ptr = alloca ptr, align 8"));
        assert!(output.contains("%local2.len = alloca i64, align 8"));
        assert!(output.contains("load [2 x i64], ptr %local0, align 8"));
        assert!(!output.contains("getelementptr inbounds"));
        assert!(
            output.rfind(" = alloca ").unwrap() < output.find("call void @ax_rt_enter").unwrap()
        );
        assert_eq!(output.matches("call void @ax_rt_trap(i32 7,").count(), 3);
    }

    #[test]
    fn indexed_write_traps_before_rhs_call() {
        let mut fixture = program(integer(0));
        fixture
            .functions
            .push(program(integer(9)).functions.remove(0));
        fixture.functions[0].locals = vec![local(Type::Array {
            element: ElementType::I64,
            length: 2,
        })];
        fixture.functions[0]
            .body
            .stmts
            .insert(0, array_initializer(0));
        fixture.functions[0].body.stmts.insert(
            1,
            statement(StmtKind::IndexAssign {
                base: 0,
                index: integer(2),
                value: expression(
                    ExprKind::Call {
                        function: 1,
                        args: vec![],
                    },
                    Type::I64,
                ),
            }),
        );
        let output = emit(&fixture, "a\nb");
        assert!(
            output
                .find("call void @ax_rt_trap(i32 7, i64 1, i64 1)")
                .unwrap()
                < output.find("call i64 @ax_fn_1()").unwrap()
        );
    }

    #[test]
    fn both_borrow_endpoints_precede_range_guard_and_pointer_creation() {
        let mut fixture = program(integer(0));
        fixture
            .functions
            .push(program(integer(-1)).functions.remove(0));
        fixture
            .functions
            .push(program(integer(2)).functions.remove(0));
        fixture.functions[0].locals = vec![
            local(Type::Array {
                element: ElementType::I64,
                length: 2,
            }),
            local(Type::Slice {
                element: ElementType::I64,
                mutable: false,
            }),
        ];
        fixture.functions[0]
            .body
            .stmts
            .insert(0, array_initializer(0));
        fixture.functions[0].body.stmts.insert(
            1,
            statement(StmtKind::Let {
                local: 1,
                value: expression(
                    ExprKind::Borrow {
                        owner: 0,
                        mutable: false,
                        start: Box::new(expression(
                            ExprKind::Call {
                                function: 1,
                                args: vec![],
                            },
                            Type::I64,
                        )),
                        end: Box::new(expression(
                            ExprKind::Call {
                                function: 2,
                                args: vec![],
                            },
                            Type::I64,
                        )),
                    },
                    Type::Slice {
                        element: ElementType::I64,
                        mutable: false,
                    },
                ),
            }),
        );
        let output = emit(&fixture, "a\nb");
        let start = output.find("call i64 @ax_fn_1()").unwrap();
        let end = output.find("call i64 @ax_fn_2()").unwrap();
        let guard = output
            .find("call void @ax_rt_trap(i32 7, i64 2, i64 1)")
            .unwrap();
        let address = output.rfind("getelementptr i64, ptr %local0").unwrap();
        assert!(start < end && end < guard && guard < address);
    }

    #[test]
    fn arithmetic_has_explicit_guards_without_overflow_assumptions() {
        for op in [BinaryOp::Add, BinaryOp::Sub, BinaryOp::Mul] {
            let output = emit(
                &program(binary(op, integer(7), integer(9), Type::I64)),
                "a\nb",
            );
            assert!(output.contains("extractvalue { i64, i1 }"));
            assert!(output.contains("call void @ax_rt_trap(i32 1, i64 2, i64 1)"));
            assert!(!output.contains(" nsw "));
            assert!(!output.contains(" nuw "));
        }
    }

    #[test]
    fn division_and_remainder_guard_both_undefined_cases_first() {
        for (op, instruction) in [(BinaryOp::Div, "sdiv"), (BinaryOp::Rem, "srem")] {
            let output = emit(
                &program(binary(op, integer(i64::MIN), integer(-1), Type::I64)),
                "a\nb",
            );
            let operation = output.find(&format!(" = {instruction} i64")).unwrap();
            assert!(output.find("call void @ax_rt_trap(i32 2,").unwrap() < operation);
            assert!(output.find("call void @ax_rt_trap(i32 1,").unwrap() < operation);
        }
    }

    #[test]
    fn shift_count_is_guarded_before_shift_and_left_shift_has_backcheck() {
        let output = emit(
            &program(binary(BinaryOp::Shl, integer(-1), integer(63), Type::I64)),
            "a\nb",
        );
        assert!(
            output.find("call void @ax_rt_trap(i32 3,").unwrap() < output.find(" = shl ").unwrap()
        );
        assert!(output.contains(" = ashr i64 "));
        assert!(output.contains(" = icmp ne i64 "));
    }

    #[test]
    fn logical_operators_use_branches_and_phi() {
        let rhs = binary(
            BinaryOp::Eq,
            binary(BinaryOp::Div, integer(1), integer(0), Type::I64),
            integer(0),
            Type::Bool,
        );
        let logical = binary(
            BinaryOp::And,
            ir::Expr {
                kind: ExprKind::Bool(false),
                ty: Type::Bool,
                span: Span::default(),
            },
            rhs,
            Type::Bool,
        );
        let mut fixture = program(integer(0));
        fixture.functions[0].body.stmts.insert(
            0,
            ir::Stmt {
                kind: StmtKind::Expr(logical),
                span: Span::default(),
            },
        );
        let output = emit(&fixture, "a\nb");
        assert!(output.contains("br i1 false, label %logic.rhs.0, label %logic.end.1"));
        assert!(output.contains("phi i1 [ false, %entry ],"));
        assert!(output.find("logic.rhs.0:").unwrap() < output.find(" = sdiv i64").unwrap());
        assert_eq!(output, emit(&fixture, "a\nb"));
    }
}
