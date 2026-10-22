//! Independent reference storage and kernels; no native numerical crate calls.
use crate::ir::{TensorErrorKind as Error, TensorIntrinsic as Op};

pub(super) const MAX_BUDGET: usize = 64 * 1024 * 1024;
const MAX_ELEMENTS: usize = 65_536;
const MAX_WORK: usize = 1_048_576;
type Result<T> = std::result::Result<T, Error>;

/// Keep the interpreter's frequently copied enum small without infallible Box
/// allocation. Metadata is separate from the requested tensor payload budget.
#[derive(Debug)]
pub(super) struct Indirect<T>(Vec<T>);

impl<T> Indirect<T> {
    pub fn new(value: T) -> Result<Self> {
        let mut storage = Vec::new();
        storage
            .try_reserve_exact(1)
            .map_err(|_| Error::AllocationFailure)?;
        storage.push(value);
        Ok(Self(storage))
    }
}

impl<T> std::ops::Deref for Indirect<T> {
    type Target = T;

    fn deref(&self) -> &T {
        &self.0[0]
    }
}

impl<T> std::ops::DerefMut for Indirect<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0[0]
    }
}

#[derive(Debug, Clone, Copy)]
pub(super) struct Descriptor {
    pub rank: usize,
    pub dims: [usize; 8],
    pub strides: [usize; 8],
    pub offset: usize,
    pub len: usize,
}

impl Descriptor {
    pub fn new(dimensions: &[i64]) -> Result<Self> {
        if dimensions.len() > 8 {
            return Err(Error::RankLimit);
        }
        let mut value = Self {
            rank: dimensions.len(),
            dims: [0; 8],
            strides: [0; 8],
            offset: 0,
            len: 1,
        };
        for (index, &dimension) in dimensions.iter().enumerate() {
            if !(0..=65_536).contains(&dimension) {
                return Err(Error::ExtentLimit);
            }
            value.dims[index] = dimension as usize;
        }
        if dimensions.contains(&0) {
            value.len = 0;
            return Ok(value);
        }
        for axis in (0..value.rank).rev() {
            value.strides[axis] = value.len;
            value.len = value
                .len
                .checked_mul(value.dims[axis])
                .ok_or(Error::ElementLimit)?;
            if value.len > MAX_ELEMENTS {
                return Err(Error::ElementLimit);
            }
        }
        Ok(value)
    }

    pub fn shape(&self) -> &[usize] {
        &self.dims[..self.rank]
    }

    pub fn transpose(mut self, axes: &[i64]) -> Result<Self> {
        if axes.len() != self.rank {
            return Err(Error::InvalidPermutation);
        }
        let old = self;
        let mut seen = [false; 8];
        for (index, &axis) in axes.iter().enumerate() {
            if axis < 0 || axis as usize >= self.rank || seen[axis as usize] {
                return Err(Error::InvalidPermutation);
            }
            let axis = axis as usize;
            seen[axis] = true;
            self.dims[index] = old.dims[axis];
            self.strides[index] = old.strides[axis];
        }
        Ok(self)
    }

    pub fn slice(mut self, axis: i64, start: i64, end: i64, step: i64) -> Result<Self> {
        if axis < 0 || axis as usize >= self.rank {
            return Err(Error::Bounds);
        }
        let axis = axis as usize;
        if start < 0 || end < start || end as usize > self.dims[axis] {
            return Err(Error::Bounds);
        }
        if step <= 0 {
            return Err(Error::InvalidStep);
        }
        let (start, end, step) = (start as usize, end as usize, step as usize);
        let extent = (end - start) / step + usize::from((end - start) % step != 0);
        self.dims[axis] = extent;
        if self.shape().contains(&0) {
            self.len = 0;
            self.offset = 0;
            self.strides = [0; 8];
            return Ok(self);
        }
        self.offset = self
            .offset
            .checked_add(start.checked_mul(self.strides[axis]).ok_or(Error::Bounds)?)
            .ok_or(Error::Bounds)?;
        if extent > 1 {
            self.strides[axis] = self.strides[axis]
                .checked_mul(step)
                .ok_or(Error::InvalidStep)?;
        }
        self.len = self.shape().iter().product();
        Ok(self)
    }

    pub fn linear_index(&self, index: i64) -> Result<usize> {
        if index < 0 || index as usize >= self.len {
            return Err(Error::Bounds);
        }
        let mut remaining = index as usize;
        let mut physical = self.offset;
        for axis in (0..self.rank).rev() {
            let component = remaining % self.dims[axis];
            remaining /= self.dims[axis];
            physical = physical
                .checked_add(
                    component
                        .checked_mul(self.strides[axis])
                        .ok_or(Error::Bounds)?,
                )
                .ok_or(Error::Bounds)?;
        }
        Ok(physical)
    }
}

#[derive(Debug)]
pub(super) struct Owned {
    pub data: Vec<f64>,
    pub descriptor: Descriptor,
}

pub(super) fn canonical(value: f64) -> f64 {
    f64::from_bits(super::canonical_float_bits(value.to_bits()))
}

impl Owned {
    pub fn zeros(descriptor: Descriptor) -> Result<Self> {
        let mut data = Vec::new();
        data.try_reserve_exact(descriptor.len)
            .map_err(|_| Error::AllocationFailure)?;
        data.resize(descriptor.len, 0.0);
        Ok(Self { data, descriptor })
    }

    pub fn from_slice(descriptor: Descriptor, values: &[f64]) -> Result<Self> {
        if descriptor.len != values.len() {
            return Err(Error::ShapeMismatch);
        }
        let mut output = Self::zeros(descriptor)?;
        for (destination, &value) in output.data.iter_mut().zip(values) {
            *destination = canonical(value);
        }
        Ok(output)
    }
}

#[derive(Clone, Copy)]
pub(super) struct View<'a> {
    pub data: &'a [f64],
    pub descriptor: Descriptor,
}

impl View<'_> {
    pub fn get(self, index: usize) -> Result<f64> {
        self.data
            .get(self.descriptor.linear_index(index as i64)?)
            .copied()
            .ok_or(Error::Bounds)
    }
}

#[derive(Clone, Copy)]
pub(super) struct Plan {
    pub output: Option<Descriptor>,
    pub work: u64,
}

pub(super) fn prepare(op: Op, left: View<'_>, right: Option<View<'_>>) -> Result<Plan> {
    let descriptor = left.descriptor;
    let dense = || {
        let mut dims = [0i64; 8];
        for (slot, &value) in dims.iter_mut().zip(descriptor.shape()) {
            *slot = value as i64;
        }
        Descriptor::new(&dims[..descriptor.rank])
    };
    Ok(match op {
        Op::Add | Op::Mul => {
            if descriptor.shape() != right.unwrap().descriptor.shape() {
                return Err(Error::ShapeMismatch);
            }
            Plan {
                output: Some(dense()?),
                work: descriptor.len as u64,
            }
        }
        Op::Matmul => {
            let right = right.unwrap().descriptor;
            if descriptor.rank != 2 || right.rank != 2 {
                return Err(Error::RankMismatch);
            }
            let (m, k, n) = (descriptor.dims[0], descriptor.dims[1], right.dims[1]);
            if k != right.dims[0] {
                return Err(Error::ShapeMismatch);
            }
            let output = Descriptor::new(&[m as i64, n as i64])?;
            let work = output
                .len
                .checked_mul(k)
                .and_then(|v| v.checked_add(output.len))
                .ok_or(Error::WorkLimit)?;
            if work > MAX_WORK {
                return Err(Error::WorkLimit);
            }
            Plan {
                output: Some(output),
                work: work as u64,
            }
        }
        Op::Sum => Plan {
            output: None,
            work: descriptor.len as u64,
        },
        Op::Sigmoid | Op::Softmax | Op::Logsumexp => {
            if op != Op::Sigmoid {
                if descriptor.rank != 1 {
                    return Err(Error::RankMismatch);
                }
                if descriptor.len == 0 {
                    return Err(Error::EmptyInput);
                }
            }
            for index in 0..descriptor.len {
                if !left.get(index)?.is_finite() {
                    return Err(Error::NonFiniteInput);
                }
            }
            Plan {
                output: if op == Op::Logsumexp {
                    None
                } else {
                    Some(dense()?)
                },
                work: descriptor.len as u64
                    * match op {
                        Op::Softmax => 3,
                        Op::Logsumexp => 2,
                        _ => 1,
                    },
            }
        }
        _ => unreachable!("non-kernel tensor intrinsic"),
    })
}

fn unit_interval(value: f64) -> Result<f64> {
    if value.is_finite() && (0.0..=1.0).contains(&value) {
        Ok(value)
    } else {
        Err(Error::NumericalFailure)
    }
}

fn maximum(input: View<'_>) -> Result<(usize, f64)> {
    let mut index = 0;
    let mut value = input.get(0)?;
    for current in 1..input.descriptor.len {
        let next = input.get(current)?;
        if next > value {
            index = current;
            value = next;
        }
    }
    Ok((index, value))
}

pub(super) fn tensor_kernel(
    op: Op,
    left: View<'_>,
    right: Option<View<'_>>,
    plan: Plan,
) -> Result<Owned> {
    let mut output = Owned::zeros(plan.output.unwrap())?;
    match op {
        Op::Add | Op::Mul => {
            let right = right.unwrap();
            for index in 0..output.data.len() {
                let (a, b) = (left.get(index)?, right.get(index)?);
                output.data[index] = canonical(if op == Op::Add { a + b } else { a * b });
            }
        }
        Op::Matmul => {
            let right = right.unwrap();
            let (m, k, n) = (
                left.descriptor.dims[0],
                left.descriptor.dims[1],
                right.descriptor.dims[1],
            );
            for row in 0..m {
                for column in 0..n {
                    let mut sum = 0.0;
                    for inner in 0..k {
                        let product = left.get(row * k + inner)? * right.get(inner * n + column)?;
                        sum += product;
                    }
                    output.data[row * n + column] = canonical(sum);
                }
            }
        }
        Op::Sigmoid => {
            for index in 0..output.data.len() {
                let x = left.get(index)?;
                let value = if x >= 0.0 {
                    1.0 / (1.0 + (-x).exp())
                } else {
                    let e = x.exp();
                    e / (1.0 + e)
                };
                output.data[index] = canonical(unit_interval(value)?);
            }
        }
        Op::Softmax => {
            let (_, max) = maximum(left)?;
            let mut total = 0.0;
            for index in 0..output.data.len() {
                let value = unit_interval((left.get(index)? - max).exp())?;
                output.data[index] = value;
                total += value;
            }
            if !total.is_finite() || total <= 0.0 {
                return Err(Error::NumericalFailure);
            }
            for value in &mut output.data {
                *value = canonical(unit_interval(*value / total)?);
            }
        }
        _ => unreachable!("scalar or non-kernel tensor operation"),
    }
    Ok(output)
}

pub(super) fn scalar_kernel(op: Op, input: View<'_>) -> Result<f64> {
    if op == Op::Sum {
        let mut sum = 0.0;
        for index in 0..input.descriptor.len {
            sum += input.get(index)?;
        }
        return Ok(canonical(sum));
    }
    let (maximum_index, max) = maximum(input)?;
    if input.descriptor.len == 1 {
        return Ok(canonical(max));
    }
    let mut tail = 0.0;
    for index in 0..input.descriptor.len {
        if index != maximum_index {
            tail += unit_interval((input.get(index)? - max).exp())?;
        }
    }
    if !tail.is_finite() || tail < 0.0 {
        return Err(Error::NumericalFailure);
    }
    let correction = tail.ln_1p();
    if !correction.is_finite() || correction < 0.0 {
        return Err(Error::NumericalFailure);
    }
    let result = max + correction;
    if !result.is_finite() {
        return Err(Error::NumericalFailure);
    }
    Ok(canonical(result))
}
