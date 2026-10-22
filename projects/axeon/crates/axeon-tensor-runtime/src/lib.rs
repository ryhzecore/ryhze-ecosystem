//! Internal hosted tensor ABI for compiler-generated, single-threaded programs.
//!
//! Storage and all numerical kernels come from `axeon-numerics`. This adapter
//! owns only a bounded handle table and validated, inline view descriptions.
//! A scope owns a stack-local Budget; every Tensor is dropped before that Budget.
//!
//! # FFI safety contract
//! These symbols are an internal compiler ABI, not a general foreign interface.
//! Non-null pointers must designate live, aligned storage of the stated size.
//! Output storage must be writable and must not alias the context or any input.
//! Callbacks and input storage must remain live for the duration of the call.
//! The compiler never retains context pointers or exposes tensor handles to user
//! code. Nulls, alignment, counts, active-context identity and handles are checked;
//! arbitrary pointer provenance cannot be established by a C ABI.
//!
//! Unexpected Rust panics terminate the process after being caught at the ABI
//! boundary. They are never translated into an ordinary tensor error. No Rust
//! unwind crosses C. Scalar traps invoke the registered cleanup only after any
//! tensor wrapper has returned and released its temporary context borrow.

#![deny(unsafe_op_in_unsafe_fn)]

use axeon_numerics::{
    self as numeric, Budget, Error, Shape, Tensor, View, ViewMut, MAX_ELEMENTS, MAX_RANK,
};
use std::cell::Cell;
use std::ffi::c_void;
use std::mem::{align_of, size_of};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicU64, Ordering};

pub const ABI_VERSION: u32 = 1;
pub const RUNTIME_VERSION: u64 = 0x000100;
pub const MAX_HANDLES: usize = 256;
const NESTED_SCOPE: i32 = 15;
static NEXT_HANDLE: AtomicU64 = AtomicU64::new(1);

#[no_mangle]
pub extern "C" fn ax_tensor_runtime_abi_version() -> u32 {
    ABI_VERSION
}
#[no_mangle]
pub extern "C" fn ax_tensor_runtime_version() -> u64 {
    RUNTIME_VERSION
}

type Cleanup = unsafe extern "C" fn(*mut c_void);
pub type ScopeCallback = unsafe extern "C" fn(*mut c_void, *mut c_void) -> i32;

#[cfg(not(test))]
extern "C" {
    fn ax_rt_register_cleanup(cleanup: Option<Cleanup>, context: *mut c_void) -> i32;
    fn ax_rt_unregister_cleanup(context: *mut c_void) -> i32;
    fn ax_rt_cleanup_active() -> i32;
}

thread_local! {
    // This registry holds an address, never ownership. The pointee exists only
    // within ax_tensor_v1_scope's synchronous callback and cleanup interval.
    static ACTIVE: Cell<*mut c_void> = const { Cell::new(std::ptr::null_mut()) };
    static BORROWED: Cell<bool> = const { Cell::new(false) };
}

fn error_code(error: Error) -> i32 {
    match error {
        Error::RankLimit => 1,
        Error::ExtentLimit => 2,
        Error::ElementLimit => 3,
        Error::ShapeMismatch => 4,
        Error::RankMismatch => 5,
        Error::Bounds => 6,
        Error::InvalidPermutation => 7,
        Error::InvalidStep => 8,
        Error::BudgetLimit => 9,
        Error::AllocationFailure => 10,
        Error::WorkLimit => 11,
        Error::NonFiniteInput => 12,
        Error::EmptyInput => 13,
        Error::NumericalFailure => 14,
    }
}

#[derive(Clone, Copy)]
enum Transform {
    Identity,
    Transpose {
        axes: [usize; MAX_RANK],
        rank: usize,
    },
    Slice {
        axis: usize,
        start: usize,
        end: usize,
        step: usize,
    },
}
impl Transform {
    fn shared(self, view: View<'_>) -> numeric::Result<View<'_>> {
        match self {
            Self::Identity => Ok(view),
            Self::Transpose { axes, rank } => view.transpose(&axes[..rank]),
            Self::Slice {
                axis,
                start,
                end,
                step,
            } => view.axis_slice(axis, start, end, step),
        }
    }
    fn exclusive(self, view: ViewMut<'_>) -> numeric::Result<ViewMut<'_>> {
        match self {
            Self::Identity => Ok(view),
            Self::Transpose { axes, rank } => view.transpose(&axes[..rank]),
            Self::Slice {
                axis,
                start,
                end,
                step,
            } => view.axis_slice(axis, start, end, step),
        }
    }
}

struct Owner<'budget> {
    tensor: Tensor<'budget>,
    shared_loans: u16,
    exclusive_loan: bool,
}
#[derive(Clone, Copy)]
struct ViewEntry {
    owner: u64,
    mutable: bool,
    transform: Transform,
}
enum Entry<'budget> {
    Owner(Owner<'budget>),
    View(ViewEntry),
}
struct Slot<'budget> {
    handle: u64,
    entry: Option<Entry<'budget>>,
}
struct Context<'budget> {
    budget: &'budget Budget,
    slots: [Slot<'budget>; MAX_HANDLES],
    first_error: [i64; 3],
    closed: bool,
}
impl<'budget> Context<'budget> {
    fn new(budget: &'budget Budget) -> Self {
        Self {
            budget,
            slots: std::array::from_fn(|_| Slot {
                handle: 0,
                entry: None,
            }),
            first_error: [0; 3],
            closed: false,
        }
    }
    fn fail(&mut self, error: Error, start: i64, end: i64) -> i32 {
        let code = error_code(error);
        if self.first_error[0] == 0 {
            self.first_error = [i64::from(code), start, end];
        }
        code
    }
    fn slot(&self, handle: u64) -> numeric::Result<usize> {
        if self.closed || handle == 0 {
            return Err(Error::Bounds);
        }
        self.slots
            .iter()
            .position(|slot| slot.handle == handle && slot.entry.is_some())
            .ok_or(Error::Bounds)
    }
    fn space(&self) -> numeric::Result<usize> {
        if self.closed {
            return Err(Error::Bounds);
        }
        self.slots
            .iter()
            .position(|slot| slot.entry.is_none())
            .ok_or(Error::AllocationFailure)
    }
    fn insert(&mut self, entry: Entry<'budget>) -> numeric::Result<u64> {
        let index = self.space()?;
        let handle = NEXT_HANDLE
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |value| {
                value.checked_add(1)
            })
            .map_err(|_| Error::AllocationFailure)?;
        self.slots[index] = Slot {
            handle,
            entry: Some(entry),
        };
        Ok(handle)
    }
    fn own(&mut self, tensor: Tensor<'budget>) -> numeric::Result<u64> {
        self.insert(Entry::Owner(Owner {
            tensor,
            shared_loans: 0,
            exclusive_loan: false,
        }))
    }
    fn owner(&self, handle: u64) -> numeric::Result<&Owner<'budget>> {
        match self.slots[self.slot(handle)?].entry.as_ref() {
            Some(Entry::Owner(owner)) => Ok(owner),
            _ => Err(Error::Bounds),
        }
    }
    fn owner_mut(&mut self, handle: u64) -> numeric::Result<&mut Owner<'budget>> {
        let index = self.slot(handle)?;
        match self.slots[index].entry.as_mut() {
            Some(Entry::Owner(owner)) => Ok(owner),
            _ => Err(Error::Bounds),
        }
    }
    fn view(&self, handle: u64) -> numeric::Result<View<'_>> {
        match self.slots[self.slot(handle)?].entry.as_ref() {
            Some(Entry::Owner(owner)) if !owner.exclusive_loan => Ok(owner.tensor.view()),
            Some(Entry::View(view)) => {
                let owner = self.owner(view.owner)?;
                if (view.mutable && !owner.exclusive_loan)
                    || (!view.mutable && (owner.exclusive_loan || owner.shared_loans == 0))
                {
                    return Err(Error::Bounds);
                }
                view.transform.shared(owner.tensor.view())
            }
            _ => Err(Error::Bounds),
        }
    }
    fn make_view(
        &mut self,
        owner_handle: u64,
        mutable: bool,
        transform: Transform,
    ) -> numeric::Result<u64> {
        let owner = self.owner(owner_handle)?;
        if owner.exclusive_loan || (mutable && owner.shared_loans != 0) {
            return Err(Error::Bounds);
        }
        // Validate with the real crate before installing either metadata or loan.
        transform.shared(owner.tensor.view())?;
        let handle = self.insert(Entry::View(ViewEntry {
            owner: owner_handle,
            mutable,
            transform,
        }))?;
        let owner = self.owner_mut(owner_handle)?;
        if mutable {
            owner.exclusive_loan = true;
        } else {
            owner.shared_loans += 1;
        }
        Ok(handle)
    }
    fn writable(&self, handle: u64) -> numeric::Result<(u64, Transform)> {
        match self.slots[self.slot(handle)?].entry.as_ref() {
            Some(Entry::Owner(owner)) if owner.shared_loans == 0 && !owner.exclusive_loan => {
                Ok((handle, Transform::Identity))
            }
            Some(Entry::View(view)) if view.mutable => {
                let owner = self.owner(view.owner)?;
                if !owner.exclusive_loan || owner.shared_loans != 0 {
                    return Err(Error::Bounds);
                }
                Ok((view.owner, view.transform))
            }
            _ => Err(Error::Bounds),
        }
    }
    fn check_index(&self, handle: u64, index: usize) -> numeric::Result<()> {
        let (owner, transform) = self.writable(handle)?;
        transform
            .shared(self.owner(owner)?.tensor.view())?
            .get_linear(index)
            .map(|_| ())
    }
    fn set(&mut self, handle: u64, index: usize, value: f64) -> numeric::Result<()> {
        let (owner, transform) = self.writable(handle)?;
        transform
            .exclusive(self.owner_mut(owner)?.tensor.view_mut())?
            .set_linear(index, value)
    }
    fn release(&mut self, handle: u64) -> numeric::Result<()> {
        if handle == 0 {
            return Ok(());
        }
        let index = self.slot(handle)?;
        match self.slots[index].entry.as_ref() {
            Some(Entry::Owner(owner)) if owner.exclusive_loan || owner.shared_loans != 0 => {
                return Err(Error::Bounds)
            }
            Some(Entry::View(view)) => {
                let view = *view;
                let owner = self.owner_mut(view.owner)?;
                if view.mutable {
                    owner.exclusive_loan = false;
                } else {
                    owner.shared_loans = owner.shared_loans.checked_sub(1).ok_or(Error::Bounds)?;
                }
            }
            _ => {}
        }
        self.slots[index].entry = None;
        self.slots[index].handle = 0;
        Ok(())
    }
    fn shutdown(&mut self) {
        // Descriptors disappear first; owners then refund their live Budget.
        for slot in &mut self.slots {
            if matches!(slot.entry, Some(Entry::View(_))) {
                slot.entry = None;
                slot.handle = 0;
            }
        }
        for slot in &mut self.slots {
            slot.entry = None;
            slot.handle = 0;
        }
        self.closed = true;
    }
}

fn firewall<T>(body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => std::process::abort(),
    }
}
struct BorrowGuard;
impl Drop for BorrowGuard {
    fn drop(&mut self) {
        BORROWED.with(|flag| flag.set(false));
    }
}

fn with_context(
    context: *mut c_void,
    start: i64,
    end: i64,
    body: impl FnOnce(&mut Context<'_>) -> numeric::Result<()>,
) -> i32 {
    firewall(|| {
        if context.is_null()
            || ACTIVE.with(|active| active.get() != context)
            || BORROWED.with(|flag| flag.replace(true))
        {
            return error_code(Error::Bounds);
        }
        let _guard = BorrowGuard;
        // SAFETY: identity is exactly the live scope-local Context registered on
        // this thread. The guard excludes a second mutable borrow until return.
        let context = unsafe { &mut *context.cast::<Context<'_>>() };
        if context.closed {
            return context.fail(Error::Bounds, start, end);
        }
        match body(context) {
            Ok(()) => 0,
            Err(error) => context.fail(error, start, end),
        }
    })
}

fn aligned<T>(pointer: *const T) -> bool {
    !pointer.is_null() && (pointer as usize) % align_of::<T>() == 0
}
unsafe fn input<'a, T>(pointer: *const T, length: usize) -> numeric::Result<&'a [T]> {
    if length == 0 {
        return Ok(&[]);
    }
    if !aligned(pointer) || length > isize::MAX as usize / size_of::<T>() {
        return Err(Error::Bounds);
    }
    // SAFETY: alignment and representable byte size checked; provenance and
    // liveness are the generated-code ABI caller's contract.
    Ok(unsafe { std::slice::from_raw_parts(pointer, length) })
}
unsafe fn output<T: Copy>(pointer: *mut T, value: T) -> numeric::Result<()> {
    if !aligned(pointer) {
        return Err(Error::Bounds);
    }
    // SAFETY: caller supplies a distinct writable result object.
    unsafe {
        pointer.write(value);
    }
    Ok(())
}
unsafe fn list(
    pointer: *const i64,
    count: i64,
    permutation: bool,
) -> numeric::Result<([usize; MAX_RANK], usize)> {
    let rank = usize::try_from(count).map_err(|_| Error::RankLimit)?;
    if rank > MAX_RANK {
        return Err(Error::RankLimit);
    }
    let values = unsafe { input(pointer, rank)? };
    let mut result = [0; MAX_RANK];
    for (slot, value) in result.iter_mut().zip(values) {
        *slot = usize::try_from(*value).map_err(|_| {
            if permutation {
                Error::InvalidPermutation
            } else {
                Error::ExtentLimit
            }
        })?;
    }
    Ok((result, rank))
}
fn index(value: i64) -> numeric::Result<usize> {
    usize::try_from(value).map_err(|_| Error::Bounds)
}
fn mutability(value: i64) -> numeric::Result<bool> {
    match value {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err(Error::Bounds),
    }
}

unsafe extern "C" fn cleanup(context: *mut c_void) {
    firewall(|| {
        if context.is_null()
            || ACTIVE.with(|active| active.get() != context)
            || BORROWED.with(Cell::get)
        {
            std::process::abort();
        }
        // SAFETY: the C hook can only run during this scope's registered interval,
        // outside a tensor wrapper. Budget and Context are still stack-live.
        unsafe {
            (*context.cast::<Context<'_>>()).shutdown();
        }
    });
}

/// Enter a synchronous tensor scope; `error_out` receives [code, start, end].
/// # Safety
/// The module FFI contract applies; error_out must hold three writable i64s.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_scope(
    budget_bytes: i64,
    callback: Option<ScopeCallback>,
    captures: *mut c_void,
    start: i64,
    end: i64,
    error_out: *mut i64,
) -> i32 {
    firewall(|| {
        if !aligned(error_out) {
            return error_code(Error::Bounds);
        }
        let write_error = |error: [i64; 3]| {
            // SAFETY: the scope ABI reserves three aligned writable i64s.
            unsafe {
                std::ptr::copy_nonoverlapping(error.as_ptr(), error_out, 3);
            }
        };
        // An existing registration wins even over malformed budget/callback.
        if unsafe { ax_rt_cleanup_active() } != 0 || ACTIVE.with(|active| !active.get().is_null()) {
            write_error([i64::from(NESTED_SCOPE), start, end]);
            return NESTED_SCOPE;
        }
        let Some(callback) = callback else {
            write_error([6, start, end]);
            return 6;
        };
        let budget = match usize::try_from(budget_bytes)
            .map_err(|_| Error::BudgetLimit)
            .and_then(Budget::new)
        {
            Ok(budget) => budget,
            Err(error) => {
                let code = error_code(error);
                write_error([i64::from(code), start, end]);
                return code;
            }
        };
        // The large arena frame is entered only after the nested-scope guard.
        // In particular, repeated rejected nested calls never accumulate arenas.
        run_scope(budget, callback, captures, start, end, error_out)
    })
}

#[inline(never)]
fn run_scope(
    budget: Budget,
    callback: ScopeCallback,
    captures: *mut c_void,
    start: i64,
    end: i64,
    error_out: *mut i64,
) -> i32 {
    let mut context = Context::new(&budget);
    let pointer = (&mut context as *mut Context<'_>).cast::<c_void>();
    ACTIVE.with(|active| active.set(pointer));
    if unsafe { ax_rt_register_cleanup(Some(cleanup), pointer) } != 0 {
        ACTIVE.with(|active| active.set(std::ptr::null_mut()));
        let error = [i64::from(NESTED_SCOPE), start, end];
        // SAFETY: the scope caller supplied three writable i64s.
        unsafe {
            std::ptr::copy_nonoverlapping(error.as_ptr(), error_out, 3);
        }
        return NESTED_SCOPE;
    }
    // No Rust reference to context or its entries is held during callback.
    let status = unsafe { callback(pointer, captures) };
    if unsafe { ax_rt_unregister_cleanup(pointer) } != 0 {
        std::process::abort();
    }
    ACTIVE.with(|active| active.set(std::ptr::null_mut()));
    context.shutdown();
    let error = if context.first_error[0] != 0 {
        context.first_error
    } else if status == 0 {
        [0; 3]
    } else {
        [
            i64::from(if (1..=NESTED_SCOPE).contains(&status) {
                status
            } else {
                6
            }),
            start,
            end,
        ]
    };
    // SAFETY: the scope caller supplied three writable i64s.
    unsafe {
        std::ptr::copy_nonoverlapping(error.as_ptr(), error_out, 3);
    }
    error[0] as i32
}

/// Allocate an owned tensor initialized to zero.
/// # Safety
/// The module FFI contract applies to context, dimensions and result storage.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_zeros(
    context: *mut c_void,
    dimensions: *const i64,
    count: i64,
    start: i64,
    end: i64,
    out: *mut u64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        let (dims, rank) = unsafe { list(dimensions, count, false)? };
        Shape::new(&dims[..rank])?;
        context.space()?;
        let tensor = Tensor::zeros(context.budget, &dims[..rank])?;
        unsafe { output(out, context.own(tensor)?) }
    })
}

/// Copy an existing f64 array/slice into checked, canonical owned storage.
/// # Safety
/// The module FFI contract applies to all input and output storage.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_from_array(
    context: *mut c_void,
    dimensions: *const i64,
    count: i64,
    values: *const f64,
    length: i64,
    start: i64,
    end: i64,
    out: *mut u64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        let (dims, rank) = unsafe { list(dimensions, count, false)? };
        let shape = Shape::new(&dims[..rank])?;
        let length = index(length)?;
        if length > MAX_ELEMENTS {
            return Err(Error::ElementLimit);
        }
        if shape.len() != length {
            return Err(Error::ShapeMismatch);
        }
        let values = unsafe { input(values, length)? };
        context.space()?;
        let tensor = Tensor::from_slice(context.budget, &dims[..rank], values)?;
        unsafe { output(out, context.own(tensor)?) }
    })
}

/// Create a direct shared or exclusive view of an owner.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_view(
    context: *mut c_void,
    owner: u64,
    mutable: i64,
    start: i64,
    end: i64,
    out: *mut u64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        let handle = context.make_view(owner, mutability(mutable)?, Transform::Identity)?;
        unsafe { output(out, handle) }
    })
}

/// Create a directly borrowed transposed view.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_transpose(
    context: *mut c_void,
    owner: u64,
    mutable: i64,
    axes: *const i64,
    count: i64,
    start: i64,
    end: i64,
    out: *mut u64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        let (axes, rank) = unsafe { list(axes, count, true)? };
        let handle = context.make_view(
            owner,
            mutability(mutable)?,
            Transform::Transpose { axes, rank },
        )?;
        unsafe { output(out, handle) }
    })
}

/// Create a directly borrowed positive-step axis slice.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_slice(
    context: *mut c_void,
    owner: u64,
    mutable: i64,
    axis: i64,
    begin: i64,
    end_index: i64,
    step: i64,
    start: i64,
    end: i64,
    out: *mut u64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        // Match the crate's ordering: axis/range errors precede step errors.
        // Represent negative steps as zero so the actual view validator selects
        // Bounds before InvalidStep when both parts are malformed.
        let step = usize::try_from(step).unwrap_or(0);
        let transform = Transform::Slice {
            axis: index(axis)?,
            start: index(begin)?,
            end: index(end_index)?,
            step,
        };
        let handle = context.make_view(owner, mutability(mutable)?, transform)?;
        unsafe { output(out, handle) }
    })
}

macro_rules! binary {
    ($name:ident, $operation:ident) => {
        /// Run a checked binary numerical kernel.
        /// # Safety
        /// The module FFI contract applies.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            context: *mut c_void,
            left: u64,
            right: u64,
            start: i64,
            end: i64,
            out: *mut u64,
        ) -> i32 {
            with_context(context, start, end, |context| {
                unsafe {
                    output(out, 0)?;
                }
                let left = context.view(left)?;
                let right = context.view(right)?;
                context.space()?;
                let tensor = numeric::$operation(context.budget, &left, &right)?;
                unsafe { output(out, context.own(tensor)?) }
            })
        }
    };
}
binary!(ax_tensor_v1_add, add);
binary!(ax_tensor_v1_mul, mul);
binary!(ax_tensor_v1_matmul, matmul);
macro_rules! unary {
    ($name:ident, $operation:ident) => {
        /// Run a checked unary numerical kernel.
        /// # Safety
        /// The module FFI contract applies.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            context: *mut c_void,
            input_handle: u64,
            start: i64,
            end: i64,
            out: *mut u64,
        ) -> i32 {
            with_context(context, start, end, |context| {
                unsafe {
                    output(out, 0)?;
                }
                let input = context.view(input_handle)?;
                context.space()?;
                let tensor = numeric::$operation(context.budget, &input)?;
                unsafe { output(out, context.own(tensor)?) }
            })
        }
    };
}
unary!(ax_tensor_v1_sigmoid, sigmoid);
unary!(ax_tensor_v1_softmax, softmax);
macro_rules! reduction {
    ($name:ident, $operation:ident) => {
        /// Run a checked scalar reduction.
        /// # Safety
        /// The module FFI contract applies.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            context: *mut c_void,
            input_handle: u64,
            start: i64,
            end: i64,
            out: *mut f64,
        ) -> i32 {
            with_context(context, start, end, |context| {
                unsafe {
                    output(out, 0.0)?;
                }
                let value = numeric::$operation(&context.view(input_handle)?)?;
                unsafe { output(out, value) }
            })
        }
    };
}
reduction!(ax_tensor_v1_sum, sum);
reduction!(ax_tensor_v1_logsumexp, logsumexp);

/// Read a flattened logical element.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_get(
    context: *mut c_void,
    handle: u64,
    element: i64,
    start: i64,
    end: i64,
    out: *mut f64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0.0)?;
        }
        let value = context.view(handle)?.get_linear(index(element)?)?;
        unsafe { output(out, value) }
    })
}
/// Check write permission and index before evaluating a tensor_set RHS.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_check_index(
    context: *mut c_void,
    handle: u64,
    element: i64,
    start: i64,
    end: i64,
) -> i32 {
    with_context(context, start, end, |context| {
        context.check_index(handle, index(element)?)
    })
}
/// Store through an owner or exclusive view, canonicalizing NaNs in the crate.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_set(
    context: *mut c_void,
    handle: u64,
    element: i64,
    value: f64,
    start: i64,
    end: i64,
) -> i32 {
    with_context(context, start, end, |context| {
        context.set(handle, index(element)?, value)
    })
}
macro_rules! metadata {
    ($name:ident, $field:ident) => {
        /// Read checked tensor/view metadata.
        /// # Safety
        /// The module FFI contract applies.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            context: *mut c_void,
            handle: u64,
            start: i64,
            end: i64,
            out: *mut i64,
        ) -> i32 {
            with_context(context, start, end, |context| {
                unsafe {
                    output(out, 0)?;
                }
                let value = context.view(handle)?.$field() as i64;
                unsafe { output(out, value) }
            })
        }
    };
}
metadata!(ax_tensor_v1_rank, rank);
metadata!(ax_tensor_v1_len, len);
/// Read one checked dimension.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_dim(
    context: *mut c_void,
    handle: u64,
    axis: i64,
    start: i64,
    end: i64,
    out: *mut i64,
) -> i32 {
    with_context(context, start, end, |context| {
        unsafe {
            output(out, 0)?;
        }
        let view = context.view(handle)?;
        let value = *view.shape().get(index(axis)?).ok_or(Error::Bounds)? as i64;
        unsafe { output(out, value) }
    })
}
/// End a lexical owner/view lifetime; zero is a no-op for uninitialized locals.
/// # Safety
/// The module FFI contract applies.
#[no_mangle]
pub unsafe extern "C" fn ax_tensor_v1_release(
    context: *mut c_void,
    handle: u64,
    start: i64,
    end: i64,
) -> i32 {
    with_context(context, start, end, |context| context.release(handle))
}

#[cfg(test)]
mod tests;
#[cfg(test)]
use tests::{ax_rt_cleanup_active, ax_rt_register_cleanup, ax_rt_unregister_cleanup};
