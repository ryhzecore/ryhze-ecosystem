use crate::{Error, Result};

pub const MAX_RANK: usize = 8;
pub const MAX_EXTENT: usize = 65_536;
pub const MAX_ELEMENTS: usize = 65_536;

/// A validated shape with inline metadata and row-major logical strides.
/// Rank zero contains one scalar. Any zero extent gives zero elements, after
/// every extent has been validated, and canonicalizes every stride to zero.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Shape {
    rank: usize,
    dims: [usize; MAX_RANK],
    strides: [usize; MAX_RANK],
    len: usize,
}

impl Shape {
    pub fn new(dimensions: &[usize]) -> Result<Self> {
        if dimensions.len() > MAX_RANK {
            return Err(Error::RankLimit);
        }
        if dimensions.iter().any(|&extent| extent > MAX_EXTENT) {
            return Err(Error::ExtentLimit);
        }
        let mut dims = [0; MAX_RANK];
        dims[..dimensions.len()].copy_from_slice(dimensions);
        let mut strides = [0; MAX_RANK];
        let mut len = usize::from(!dimensions.contains(&0));
        if len != 0 {
            for axis in (0..dimensions.len()).rev() {
                strides[axis] = len;
                len = len.checked_mul(dims[axis]).ok_or(Error::ElementLimit)?;
                if len > MAX_ELEMENTS {
                    return Err(Error::ElementLimit);
                }
            }
        }
        Ok(Self {
            rank: dimensions.len(),
            dims,
            strides,
            len,
        })
    }

    pub fn dims(&self) -> &[usize] {
        &self.dims[..self.rank]
    }

    pub fn rank(&self) -> usize {
        self.rank
    }

    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn strides(&self) -> &[usize] {
        &self.strides[..self.rank]
    }

    pub(crate) fn stride_array(&self) -> [usize; MAX_RANK] {
        self.strides
    }
}
