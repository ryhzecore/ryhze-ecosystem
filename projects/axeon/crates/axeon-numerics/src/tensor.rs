use crate::{canonicalize, Error, Result, Shape, MAX_RANK};
use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};

pub const MAX_BUDGET_BYTES: usize = 64 * 1024 * 1024;
static NEXT_ALLOCATION_ID: AtomicU64 = AtomicU64::new(1);

fn next_allocation_id(counter: &AtomicU64) -> Result<u64> {
    counter
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |id| id.checked_add(1))
        .map_err(|_| Error::AllocationFailure)
}

fn reserve_elements(data: &mut Vec<f64>, count: usize) -> Result<()> {
    data.try_reserve_exact(count)
        .map_err(|_| Error::AllocationFailure)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DType {
    F64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Device {
    Cpu,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Layout {
    Dense,
    Strided,
}

/// Serial accounting of live requested f64 payload bytes, excluding allocator
/// overhead, excess capacity, and caller-owned input storage. Leaking a tensor
/// also leaks its reservation; a leaked allocation never refunds its budget.
#[derive(Debug)]
pub struct Budget {
    limit: usize,
    used: Cell<usize>,
}

impl Budget {
    pub fn new(limit_bytes: usize) -> Result<Self> {
        if limit_bytes > MAX_BUDGET_BYTES {
            return Err(Error::BudgetLimit);
        }
        Ok(Self {
            limit: limit_bytes,
            used: Cell::new(0),
        })
    }

    pub fn limit_bytes(&self) -> usize {
        self.limit
    }

    pub fn used_bytes(&self) -> usize {
        self.used.get()
    }

    fn reserve(&self, bytes: usize) -> Result<Permit<'_>> {
        let new_used = self
            .used
            .get()
            .checked_add(bytes)
            .ok_or(Error::BudgetLimit)?;
        if new_used > self.limit {
            return Err(Error::BudgetLimit);
        }
        self.used.set(new_used);
        Ok(Permit {
            budget: self,
            bytes,
        })
    }
}

#[derive(Debug)]
struct Permit<'budget> {
    budget: &'budget Budget,
    bytes: usize,
}

impl Drop for Permit<'_> {
    fn drop(&mut self) {
        self.budget.used.set(self.budget.used.get() - self.bytes);
    }
}

/// Owned, dense row-major storage. Moving preserves allocation identity and
/// accounting. There is intentionally no implicit Clone or raw mutable slice.
#[derive(Debug)]
pub struct Tensor<'budget> {
    data: Vec<f64>,
    shape: Shape,
    _permit: Permit<'budget>,
    allocation_id: u64,
}

impl<'budget> Tensor<'budget> {
    pub fn zeros(budget: &'budget Budget, dimensions: &[usize]) -> Result<Self> {
        let shape = Shape::new(dimensions)?;
        Self::allocate(budget, shape, |_| 0.0)
    }

    pub fn from_slice(
        budget: &'budget Budget,
        dimensions: &[usize],
        values: &[f64],
    ) -> Result<Self> {
        let shape = Shape::new(dimensions)?;
        if shape.len() != values.len() {
            return Err(Error::ShapeMismatch);
        }
        Self::allocate(budget, shape, |index| values[index])
    }

    fn allocate(
        budget: &'budget Budget,
        shape: Shape,
        value: impl FnMut(usize) -> f64,
    ) -> Result<Self> {
        Self::allocate_using(budget, shape, value, reserve_elements, || {
            next_allocation_id(&NEXT_ALLOCATION_ID)
        })
    }

    // Private fallible boundaries also let unit tests exercise failure cleanup
    // without host OOM, a public allocator API, or a shared test-only switch.
    fn allocate_using(
        budget: &'budget Budget,
        shape: Shape,
        mut value: impl FnMut(usize) -> f64,
        reserve: impl FnOnce(&mut Vec<f64>, usize) -> Result<()>,
        identify: impl FnOnce() -> Result<u64>,
    ) -> Result<Self> {
        let bytes = shape.len().checked_mul(8).ok_or(Error::ElementLimit)?;
        let permit = budget.reserve(bytes)?;
        let mut data = Vec::new();
        reserve(&mut data, shape.len())?;
        for index in 0..shape.len() {
            data.push(canonicalize(value(index)));
        }
        let allocation_id = identify()?;
        Ok(Self {
            data,
            shape,
            _permit: permit,
            allocation_id,
        })
    }

    pub fn shape(&self) -> &[usize] {
        self.shape.dims()
    }

    pub fn rank(&self) -> usize {
        self.shape.rank()
    }

    pub fn len(&self) -> usize {
        self.shape.len()
    }

    pub fn is_empty(&self) -> bool {
        self.shape.is_empty()
    }

    pub fn dtype(&self) -> DType {
        DType::F64
    }

    pub fn device(&self) -> Device {
        Device::Cpu
    }

    pub fn layout(&self) -> Layout {
        Layout::Dense
    }

    pub fn allocation_id(&self) -> u64 {
        self.allocation_id
    }

    pub fn as_slice(&self) -> &[f64] {
        &self.data
    }

    pub fn view(&self) -> View<'_> {
        View {
            data: &self.data,
            descriptor: Descriptor::dense(self.shape),
            allocation_id: self.allocation_id,
        }
    }

    pub fn view_mut(&mut self) -> ViewMut<'_> {
        ViewMut {
            data: &mut self.data,
            descriptor: Descriptor::dense(self.shape),
            allocation_id: self.allocation_id,
        }
    }

    pub fn get(&self, indices: &[usize]) -> Result<f64> {
        self.view().get(indices)
    }

    pub fn set(&mut self, indices: &[usize], value: f64) -> Result<()> {
        self.view_mut().set(indices, value)
    }

    pub fn get_linear(&self, index: usize) -> Result<f64> {
        self.data.get(index).copied().ok_or(Error::Bounds)
    }

    pub fn set_linear(&mut self, index: usize, value: f64) -> Result<()> {
        *self.data.get_mut(index).ok_or(Error::Bounds)? = canonicalize(value);
        Ok(())
    }
}

#[derive(Clone, Copy, Debug)]
struct Descriptor {
    shape: Shape,
    strides: [usize; MAX_RANK],
    offset: usize,
}

impl Descriptor {
    fn dense(shape: Shape) -> Self {
        Self {
            strides: shape.stride_array(),
            shape,
            offset: 0,
        }
    }

    fn layout(&self) -> Layout {
        if self.shape.is_empty()
            || self.shape.dims().iter().enumerate().all(|(axis, &extent)| {
                extent <= 1 || self.strides[axis] == self.shape.strides()[axis]
            })
        {
            Layout::Dense
        } else {
            Layout::Strided
        }
    }

    fn index(&self, indices: &[usize]) -> Result<usize> {
        if indices.len() != self.shape.rank() {
            return Err(Error::RankMismatch);
        }
        let mut offset = self.offset;
        for (axis, &index) in indices.iter().enumerate() {
            if index >= self.shape.dims()[axis] {
                return Err(Error::Bounds);
            }
            offset = offset
                .checked_add(index.checked_mul(self.strides[axis]).ok_or(Error::Bounds)?)
                .ok_or(Error::Bounds)?;
        }
        Ok(offset)
    }

    fn linear_index(&self, mut index: usize) -> Result<usize> {
        if index >= self.shape.len() {
            return Err(Error::Bounds);
        }
        let mut offset = self.offset;
        for axis in (0..self.shape.rank()).rev() {
            let extent = self.shape.dims()[axis];
            let coordinate = index % extent;
            index /= extent;
            offset = offset
                .checked_add(
                    coordinate
                        .checked_mul(self.strides[axis])
                        .ok_or(Error::Bounds)?,
                )
                .ok_or(Error::Bounds)?;
        }
        Ok(offset)
    }

    fn validate(mut self, storage_len: usize) -> Result<Self> {
        if self.shape.is_empty() {
            self.offset = 0;
            self.strides = [0; MAX_RANK];
            return Ok(self);
        }
        let mut last = self.offset;
        for (axis, &extent) in self.shape.dims().iter().enumerate() {
            last = last
                .checked_add(
                    (extent - 1)
                        .checked_mul(self.strides[axis])
                        .ok_or(Error::Bounds)?,
                )
                .ok_or(Error::Bounds)?;
        }
        if last >= storage_len {
            return Err(Error::Bounds);
        }
        Ok(self)
    }

    fn transpose(self, axes: &[usize], storage_len: usize) -> Result<Self> {
        if axes.len() != self.shape.rank() {
            return Err(Error::InvalidPermutation);
        }
        let mut seen = [false; MAX_RANK];
        let mut dims = [0; MAX_RANK];
        let mut strides = [0; MAX_RANK];
        for (axis, &source) in axes.iter().enumerate() {
            if source >= self.shape.rank() || seen[source] {
                return Err(Error::InvalidPermutation);
            }
            seen[source] = true;
            dims[axis] = self.shape.dims()[source];
            strides[axis] = self.strides[source];
        }
        Self {
            shape: Shape::new(&dims[..axes.len()])?,
            strides,
            offset: self.offset,
        }
        .validate(storage_len)
    }

    fn axis_slice(
        self,
        axis: usize,
        start: usize,
        end: usize,
        step: usize,
        storage_len: usize,
    ) -> Result<Self> {
        if axis >= self.shape.rank() || start > end || end > self.shape.dims()[axis] {
            return Err(Error::Bounds);
        }
        if step == 0 {
            return Err(Error::InvalidStep);
        }
        let distance = end - start;
        let new_extent = distance / step + usize::from(distance % step != 0);
        let mut dims = [0; MAX_RANK];
        dims[..self.shape.rank()].copy_from_slice(self.shape.dims());
        dims[axis] = new_extent;
        let shape = Shape::new(&dims[..self.shape.rank()])?;
        if shape.is_empty() {
            return Ok(Self::dense(shape));
        }
        let mut strides = self.strides;
        // Singleton axes never advance, so retaining their old stride avoids
        // rejecting a valid huge positive step solely for unused arithmetic.
        if new_extent > 1 {
            strides[axis] = strides[axis].checked_mul(step).ok_or(Error::InvalidStep)?;
        }
        let offset = self
            .offset
            .checked_add(start.checked_mul(self.strides[axis]).ok_or(Error::Bounds)?)
            .ok_or(Error::Bounds)?;
        Self {
            shape,
            strides,
            offset,
        }
        .validate(storage_len)
    }
}

/// A shared view, indexed and iterated in its own logical row-major order.
#[derive(Clone, Copy, Debug)]
pub struct View<'data> {
    data: &'data [f64],
    descriptor: Descriptor,
    allocation_id: u64,
}

impl<'data> View<'data> {
    pub fn shape(&self) -> &[usize] {
        self.descriptor.shape.dims()
    }
    pub fn rank(&self) -> usize {
        self.descriptor.shape.rank()
    }
    pub fn len(&self) -> usize {
        self.descriptor.shape.len()
    }
    pub fn is_empty(&self) -> bool {
        self.descriptor.shape.is_empty()
    }
    pub fn dtype(&self) -> DType {
        DType::F64
    }
    pub fn device(&self) -> Device {
        Device::Cpu
    }
    pub fn layout(&self) -> Layout {
        self.descriptor.layout()
    }
    pub fn allocation_id(&self) -> u64 {
        self.allocation_id
    }
    pub fn strides(&self) -> &[usize] {
        &self.descriptor.strides[..self.rank()]
    }
    pub fn offset(&self) -> usize {
        self.descriptor.offset
    }

    pub fn get(&self, indices: &[usize]) -> Result<f64> {
        self.data
            .get(self.descriptor.index(indices)?)
            .copied()
            .ok_or(Error::Bounds)
    }

    pub fn get_linear(&self, index: usize) -> Result<f64> {
        self.data
            .get(self.descriptor.linear_index(index)?)
            .copied()
            .ok_or(Error::Bounds)
    }

    /// A contiguous logical view can expose immutable canonical storage.
    pub fn as_slice(&self) -> Option<&'data [f64]> {
        if self.layout() != Layout::Dense {
            return None;
        }
        self.data
            .get(self.offset()..self.offset().checked_add(self.len())?)
    }

    pub fn transpose(mut self, axes: &[usize]) -> Result<Self> {
        self.descriptor = self.descriptor.transpose(axes, self.data.len())?;
        Ok(self)
    }

    pub fn axis_slice(
        mut self,
        axis: usize,
        start: usize,
        end: usize,
        step: usize,
    ) -> Result<Self> {
        self.descriptor = self
            .descriptor
            .axis_slice(axis, start, end, step, self.data.len())?;
        Ok(self)
    }
}

/// An exclusive view. Transformations consume the old descriptor, and public
/// mutation writes values through checked setters rather than exposing &mut f64.
#[derive(Debug)]
pub struct ViewMut<'data> {
    data: &'data mut [f64],
    descriptor: Descriptor,
    allocation_id: u64,
}

impl ViewMut<'_> {
    pub fn shape(&self) -> &[usize] {
        self.descriptor.shape.dims()
    }
    pub fn rank(&self) -> usize {
        self.descriptor.shape.rank()
    }
    pub fn len(&self) -> usize {
        self.descriptor.shape.len()
    }
    pub fn is_empty(&self) -> bool {
        self.descriptor.shape.is_empty()
    }
    pub fn dtype(&self) -> DType {
        DType::F64
    }
    pub fn device(&self) -> Device {
        Device::Cpu
    }
    pub fn layout(&self) -> Layout {
        self.descriptor.layout()
    }
    pub fn allocation_id(&self) -> u64 {
        self.allocation_id
    }
    pub fn strides(&self) -> &[usize] {
        &self.descriptor.strides[..self.rank()]
    }
    pub fn offset(&self) -> usize {
        self.descriptor.offset
    }

    pub fn view(&self) -> View<'_> {
        View {
            data: self.data,
            descriptor: self.descriptor,
            allocation_id: self.allocation_id,
        }
    }

    pub fn get(&self, indices: &[usize]) -> Result<f64> {
        self.view().get(indices)
    }
    pub fn get_linear(&self, index: usize) -> Result<f64> {
        self.view().get_linear(index)
    }

    pub fn set(&mut self, indices: &[usize], value: f64) -> Result<()> {
        let index = self.descriptor.index(indices)?;
        *self.data.get_mut(index).ok_or(Error::Bounds)? = canonicalize(value);
        Ok(())
    }

    pub fn set_linear(&mut self, index: usize, value: f64) -> Result<()> {
        let index = self.descriptor.linear_index(index)?;
        *self.data.get_mut(index).ok_or(Error::Bounds)? = canonicalize(value);
        Ok(())
    }

    pub fn transpose(mut self, axes: &[usize]) -> Result<Self> {
        self.descriptor = self.descriptor.transpose(axes, self.data.len())?;
        Ok(self)
    }

    pub fn axis_slice(
        mut self,
        axis: usize,
        start: usize,
        end: usize,
        step: usize,
    ) -> Result<Self> {
        self.descriptor = self
            .descriptor
            .axis_slice(axis, start, end, step, self.data.len())?;
        Ok(self)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reserve_failure_after_charge_rolls_back_without_initializing() {
        let budget = Budget::new(32).unwrap();
        let existing = Tensor::zeros(&budget, &[1]).unwrap();
        let shape = Shape::new(&[2]).unwrap();
        let result = Tensor::allocate_using(
            &budget,
            shape,
            |_| panic!("failed reservation must not initialize elements"),
            |storage, count| {
                assert_eq!(budget.used_bytes(), 24);
                assert_eq!(count, 2);
                assert!(storage.is_empty());
                Err(Error::AllocationFailure)
            },
            || panic!("failed reservation must not consume an identity"),
        );
        assert_eq!(result.unwrap_err(), Error::AllocationFailure);
        assert_eq!(budget.used_bytes(), 8);
        drop(existing);
        assert_eq!(budget.used_bytes(), 0);
    }

    #[test]
    fn identity_exhaustion_after_initialization_refunds_only_failed_owner() {
        let budget = Budget::new(32).unwrap();
        let existing = Tensor::zeros(&budget, &[1]).unwrap();
        let counter = AtomicU64::new(u64::MAX);
        let initialized = Cell::new(0);
        let result = Tensor::allocate_using(
            &budget,
            Shape::new(&[2]).unwrap(),
            |_| {
                initialized.set(initialized.get() + 1);
                1.0
            },
            reserve_elements,
            || {
                assert_eq!(budget.used_bytes(), 24);
                assert_eq!(initialized.get(), 2);
                next_allocation_id(&counter)
            },
        );
        assert_eq!(result.unwrap_err(), Error::AllocationFailure);
        assert_eq!(budget.used_bytes(), 8);
        assert_eq!(counter.load(Ordering::Relaxed), u64::MAX);
        drop(existing);
        assert_eq!(budget.used_bytes(), 0);
    }

    #[test]
    fn identities_never_wrap_or_repeat_at_the_counter_boundary() {
        let counter = AtomicU64::new(u64::MAX - 1);
        assert_eq!(next_allocation_id(&counter), Ok(u64::MAX - 1));
        assert_eq!(next_allocation_id(&counter), Err(Error::AllocationFailure));
        assert_eq!(next_allocation_id(&counter), Err(Error::AllocationFailure));
        assert_eq!(counter.load(Ordering::Relaxed), u64::MAX);
    }
}
