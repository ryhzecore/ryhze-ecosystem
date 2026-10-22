//! Audited operating-system boundaries required by the safe compiler driver.
//!
//! Windows identities deliberately require NTFS. The 64-bit file identifier
//! returned by GetFileInformationByHandle is not guaranteed unique on ReFS;
//! unsupported volume queries and filesystems fail instead of using path names
//! as an identity fallback. This is an implementation boundary, not evidence
//! of platform qualification.
#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::{identity, FileIdentity};
