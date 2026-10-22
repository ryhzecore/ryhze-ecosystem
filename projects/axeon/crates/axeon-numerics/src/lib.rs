//! Bounded, owned binary64 storage for an optional Rust CPU library.
//!
//! This crate does not extend the Axeon language or its hosted C ABI. Budgets
//! count requested element payload bytes, not allocator capacity or process RSS.
//! A budget is deliberately not thread-shared. Views borrow their owner; only
//! checked transformations can construct descriptors, and mutation cannot expose
//! a raw reference that bypasses canonical NaN storage.
//!
//! Owned tensors move rather than implicitly cloning their allocation:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! let tensor = Tensor::zeros(&budget, &[2]).unwrap();
//! let moved = tensor;
//! let _ = tensor.len();
//! ```
//! Copying an allocation must be explicit and budgeted; Tensor has no Clone:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! let tensor = Tensor::zeros(&budget, &[2]).unwrap();
//! let _copy: Tensor<'_> = tensor.clone();
//! ```
//! A shared view prevents mutation of its owner while that view remains live:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! let mut tensor = Tensor::zeros(&budget, &[2]).unwrap();
//! let view = tensor.view();
//! tensor.set_linear(0, 1.0).unwrap();
//! let _ = view.get_linear(0);
//! ```
//! Mutable views are exclusive:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! let mut tensor = Tensor::zeros(&budget, &[2]).unwrap();
//! let mut first = tensor.view_mut();
//! let mut second = tensor.view_mut();
//! first.set_linear(0, 1.0).unwrap();
//! second.set_linear(1, 2.0).unwrap();
//! ```
//! Neither an owner nor its accounting reservation can outlive its budget:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let tensor;
//! {
//!     let budget = Budget::new(64).unwrap();
//!     tensor = Tensor::zeros(&budget, &[2]).unwrap();
//! }
//! let _ = tensor.len();
//! ```
//! Views cannot escape the owned allocation:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! let view;
//! {
//!     let tensor = Tensor::zeros(&budget, &[2]).unwrap();
//!     view = tensor.view();
//! }
//! let _ = view.len();
//! ```
//! Cell-based budget accounting cannot be shared between threads:
//! ```compile_fail
//! use axeon_numerics::{Budget, Tensor};
//! let budget = Budget::new(64).unwrap();
//! std::thread::scope(|scope| {
//!     scope.spawn(|| { let _ = Tensor::zeros(&budget, &[2]); });
//! });
//! ```

#![forbid(unsafe_code)]

mod error;
pub mod ops;
mod shape;
mod tensor;

pub use error::{Error, Result};
pub use ops::{add, logsumexp, matmul, mul, sigmoid, softmax, sum, MAX_MATMUL_WORK};
pub use shape::{Shape, MAX_ELEMENTS, MAX_EXTENT, MAX_RANK};
pub use tensor::{Budget, DType, Device, Layout, Tensor, View, ViewMut, MAX_BUDGET_BYTES};

pub const LIBRARY_VERSION: &str = "0.1.0";
pub const BASIC_PRECISION_PROFILE: &str = "cpu-f64-canonical-storage-ordered-v1";
pub const HOSTED_MATH_PROFILE: &str = "cpu-f64-exp-log1p-corpus-v1";

/// Every NaN written to owned storage uses this positive quiet representation.
pub const CANONICAL_NAN_BITS: u64 = 0x7ff8_0000_0000_0000;

/// Preserve every non-NaN bit and normalize either sign/payload of NaN.
pub fn canonicalize(value: f64) -> f64 {
    if value.to_bits() & 0x7fff_ffff_ffff_ffff > 0x7ff0_0000_0000_0000 {
        f64::from_bits(CANONICAL_NAN_BITS)
    } else {
        value
    }
}
