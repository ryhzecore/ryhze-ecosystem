//! Ordered CPU binary64 operations over validated logical tensor views.
//!
//! Output storage is always charged to the caller's budget. Shape, domain, and
//! work checks precede allocation; a later numerical failure drops the private
//! output and releases its charge. No kernel allocates temporary tensor storage.

use crate::{canonicalize, Budget, Error, Result, Shape, Tensor, View};

/// Maximum product count plus output-element count for one matrix product.
pub const MAX_MATMUL_WORK: usize = 1_048_576;

/// Add equally shaped tensors in logical row-major order.
pub fn add<'budget>(
    budget: &'budget Budget,
    left: &View<'_>,
    right: &View<'_>,
) -> Result<Tensor<'budget>> {
    elementwise(budget, left, right, |a, b| a + b)
}

/// Multiply equally shaped tensors in logical row-major order.
pub fn mul<'budget>(
    budget: &'budget Budget,
    left: &View<'_>,
    right: &View<'_>,
) -> Result<Tensor<'budget>> {
    elementwise(budget, left, right, |a, b| a * b)
}

fn elementwise<'budget>(
    budget: &'budget Budget,
    left: &View<'_>,
    right: &View<'_>,
    operation: impl Fn(f64, f64) -> f64,
) -> Result<Tensor<'budget>> {
    if left.shape() != right.shape() {
        return Err(Error::ShapeMismatch);
    }
    let mut output = Tensor::zeros(budget, left.shape())?;
    for index in 0..left.len() {
        output.set_linear(
            index,
            operation(left.get_linear(index)?, right.get_linear(index)?),
        )?;
    }
    Ok(output)
}

/// Sum all logical elements, starting at positive zero and adding in order.
pub fn sum(input: &View<'_>) -> Result<f64> {
    let mut total = 0.0;
    for index in 0..input.len() {
        total += input.get_linear(index)?;
    }
    Ok(canonicalize(total))
}

/// Rank-two matrix multiplication with increasing reduction indices.
///
/// Each product and addition is a separate binary64 operation. The work charge
/// is `rows * columns * inner + rows * columns`, including empty reductions.
pub fn matmul<'budget>(
    budget: &'budget Budget,
    left: &View<'_>,
    right: &View<'_>,
) -> Result<Tensor<'budget>> {
    if left.rank() != 2 || right.rank() != 2 {
        return Err(Error::RankMismatch);
    }
    let (rows, inner, columns) = (left.shape()[0], left.shape()[1], right.shape()[1]);
    if inner != right.shape()[0] {
        return Err(Error::ShapeMismatch);
    }
    let shape = Shape::new(&[rows, columns])?;
    let output_elements = shape.len();
    let work = output_elements
        .checked_mul(inner)
        .and_then(|products| products.checked_add(output_elements))
        .ok_or(Error::WorkLimit)?;
    if work > MAX_MATMUL_WORK {
        return Err(Error::WorkLimit);
    }
    let mut output = Tensor::zeros(budget, shape.dims())?;
    for row in 0..rows {
        for column in 0..columns {
            let mut total = 0.0;
            for k in 0..inner {
                let product =
                    left.get_linear(row * inner + k)? * right.get_linear(k * columns + column)?;
                total += product;
            }
            output.set_linear(row * columns + column, total)?;
        }
    }
    Ok(output)
}

/// Stable finite-input sigmoid, preserving the input shape.
pub fn sigmoid<'budget>(budget: &'budget Budget, input: &View<'_>) -> Result<Tensor<'budget>> {
    require_finite(input)?;
    let mut output = Tensor::zeros(budget, input.shape())?;
    for index in 0..input.len() {
        let value = input.get_linear(index)?;
        let exponential = if value >= 0.0 {
            (-value).exp()
        } else {
            value.exp()
        };
        require_unit_interval(exponential)?;
        let result = if value >= 0.0 {
            1.0 / (1.0 + exponential)
        } else {
            exponential / (1.0 + exponential)
        };
        require_unit_interval(result)?;
        output.set_linear(index, result)?;
    }
    Ok(output)
}

/// Stable softmax of a nonempty, finite, rank-one tensor.
pub fn softmax<'budget>(budget: &'budget Budget, input: &View<'_>) -> Result<Tensor<'budget>> {
    let (_, maximum) = finite_vector_maximum(input)?;
    let mut output = Tensor::zeros(budget, input.shape())?;
    for index in 0..input.len() {
        output.set_linear(index, shifted_exp(input.get_linear(index)?, maximum)?)?;
    }
    let mut denominator = 0.0;
    for index in 0..output.len() {
        denominator += output.get_linear(index)?;
    }
    if !denominator.is_finite() || denominator < 1.0 {
        return Err(Error::NumericalFailure);
    }
    for index in 0..output.len() {
        let probability = output.get_linear(index)? / denominator;
        require_unit_interval(probability)?;
        output.set_linear(index, probability)?;
    }
    Ok(output)
}

/// Stable log-sum-exp of a nonempty, finite, rank-one tensor.
///
/// The first maximum is excluded from the ordered exponential tail sum, then
/// `ln_1p` retains small tails. A singleton returns its input, including -0.
pub fn logsumexp(input: &View<'_>) -> Result<f64> {
    let (maximum_index, maximum) = finite_vector_maximum(input)?;
    if input.len() == 1 {
        return Ok(canonicalize(maximum));
    }
    let mut tail = 0.0;
    for index in 0..input.len() {
        if index != maximum_index {
            tail += shifted_exp(input.get_linear(index)?, maximum)?;
        }
    }
    if !tail.is_finite() || tail < 0.0 {
        return Err(Error::NumericalFailure);
    }
    let correction = tail.ln_1p();
    if !correction.is_finite() || correction < 0.0 {
        return Err(Error::NumericalFailure);
    }
    let result = maximum + correction;
    if !result.is_finite() {
        return Err(Error::NumericalFailure);
    }
    Ok(canonicalize(result))
}

fn require_finite(input: &View<'_>) -> Result<()> {
    for index in 0..input.len() {
        if !input.get_linear(index)?.is_finite() {
            return Err(Error::NonFiniteInput);
        }
    }
    Ok(())
}

fn finite_vector_maximum(input: &View<'_>) -> Result<(usize, f64)> {
    if input.rank() != 1 {
        return Err(Error::RankMismatch);
    }
    if input.is_empty() {
        return Err(Error::EmptyInput);
    }
    require_finite(input)?;
    let mut maximum_index = 0;
    let mut maximum = input.get_linear(0)?;
    for index in 1..input.len() {
        let value = input.get_linear(index)?;
        if value > maximum {
            maximum = value;
            maximum_index = index;
        }
    }
    Ok((maximum_index, maximum))
}

fn shifted_exp(value: f64, maximum: f64) -> Result<f64> {
    // A finite negative difference may overflow to -infinity. Its exponential
    // is correctly zero for these bounded stable-operation formulas.
    let result = (value - maximum).exp();
    require_unit_interval(result)?;
    Ok(result)
}

fn require_unit_interval(value: f64) -> Result<()> {
    if value.is_finite() && (0.0..=1.0).contains(&value) {
        Ok(())
    } else {
        Err(Error::NumericalFailure)
    }
}
