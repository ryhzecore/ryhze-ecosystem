use std::fmt;

/// Checked failures do not terminate the process or mutate input storage.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    RankLimit,
    ExtentLimit,
    ElementLimit,
    ShapeMismatch,
    RankMismatch,
    Bounds,
    InvalidPermutation,
    InvalidStep,
    BudgetLimit,
    AllocationFailure,
    WorkLimit,
    NonFiniteInput,
    EmptyInput,
    NumericalFailure,
}

pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::RankLimit => "tensor rank exceeds its limit",
            Self::ExtentLimit => "tensor extent exceeds its limit",
            Self::ElementLimit => "tensor element count exceeds its limit",
            Self::ShapeMismatch => "tensor shapes or element counts do not match",
            Self::RankMismatch => "tensor rank does not match the operation",
            Self::Bounds => "tensor index or slice is out of bounds",
            Self::InvalidPermutation => "transpose requires a permutation of every axis",
            Self::InvalidStep => "axis slice step must be positive and representable",
            Self::BudgetLimit => "tensor payload budget exceeded",
            Self::AllocationFailure => "tensor allocation failed",
            Self::WorkLimit => "numerical operation exceeds its work limit",
            Self::NonFiniteInput => "numerical operation requires finite inputs",
            Self::EmptyInput => "numerical operation requires nonempty input",
            Self::NumericalFailure => "numerical operation produced an invalid result",
        })
    }
}

impl std::error::Error for Error {}
