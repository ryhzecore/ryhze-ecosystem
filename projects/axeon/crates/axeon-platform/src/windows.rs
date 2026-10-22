use std::ffi::c_void;
use std::fs::File;
use std::io;
use std::os::windows::io::AsRawHandle;
use std::ptr;

/// Physical NTFS identity, independent of hard-link or path spelling.
///
/// IDs may be reused after deletion. Keep the relevant handles open while
/// comparing identities; this value is not a durable source-content hash.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct FileIdentity {
    volume_serial: u32,
    file_index: u64,
}

// These definitions mirror FILETIME and BY_HANDLE_FILE_INFORMATION in
// fileapi.h. Win32 DWORD is u32 and BOOL is i32 on every Windows architecture.
#[repr(C)]
#[derive(Default)]
struct FileTime {
    low: u32,
    high: u32,
}

#[repr(C)]
#[derive(Default)]
struct ByHandleFileInformation {
    attributes: u32,
    creation: FileTime,
    access: FileTime,
    write: FileTime,
    volume_serial: u32,
    size_high: u32,
    size_low: u32,
    links: u32,
    index_high: u32,
    index_low: u32,
}

#[link(name = "kernel32")]
extern "system" {
    fn GetFileInformationByHandle(
        file: *mut c_void,
        information: *mut ByHandleFileInformation,
    ) -> i32;
    fn GetVolumeInformationByHandleW(
        file: *mut c_void,
        volume_name: *mut u16,
        volume_name_size: u32,
        volume_serial: *mut u32,
        maximum_component_length: *mut u32,
        filesystem_flags: *mut u32,
        filesystem_name: *mut u16,
        filesystem_name_size: u32,
    ) -> i32;
}

/// Query a borrowed, open file without closing it, transferring ownership,
/// changing its cursor, or performing any path lookup.
///
/// The initial Windows adapter supports NTFS only. Other filesystems and
/// unavailable volume information return an error: no identity is guessed.
pub fn identity(file: &File) -> io::Result<FileIdentity> {
    // Validate the volume on the same borrowed handle, not a racy path query.
    // MAX_PATH + 1 is the documented maximum filesystem-name buffer length.
    let mut filesystem = [0u16; 261];
    // SAFETY: File owns a live handle throughout this synchronous call. The
    // writable UTF-16 buffer has exactly the element count supplied below;
    // all omitted outputs are documented optional pointers. Win32 retains
    // neither the handle nor the buffer after returning.
    let volume_ok = unsafe {
        GetVolumeInformationByHandleW(
            file.as_raw_handle(),
            ptr::null_mut(),
            0,
            ptr::null_mut(),
            ptr::null_mut(),
            ptr::null_mut(),
            filesystem.as_mut_ptr(),
            filesystem.len() as u32,
        )
    };
    if volume_ok == 0 {
        let cause = io::Error::last_os_error();
        return Err(io::Error::new(
            cause.kind(),
            format!("cannot verify NTFS file identity; use a local NTFS volume: {cause}"),
        ));
    }
    let ntfs = [b'N' as u16, b'T' as u16, b'F' as u16, b'S' as u16, 0];
    if filesystem[..ntfs.len()] != ntfs {
        return Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "Windows file identity currently requires NTFS; move the project and outputs to a local NTFS volume",
        ));
    }
    let mut information = ByHandleFileInformation::default();
    // SAFETY: the borrowed File keeps the handle valid. information is an
    // initialized, correctly aligned repr(C) instance matching the Win32
    // output structure; its exclusive pointer remains valid for the call.
    // The API writes at most that structure and does not retain the pointer.
    let identity_ok = unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut information) };
    if identity_ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(FileIdentity {
        volume_serial: information.volume_serial,
        file_index: (u64::from(information.index_high) << 32) | u64::from(information.index_low),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::io::{Read, Seek, SeekFrom};
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Fixture(PathBuf);
    impl Fixture {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "axeon-platform-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn win32_information_layout_matches_the_documented_abi() {
        assert_eq!(std::mem::size_of::<FileTime>(), 8);
        assert_eq!(std::mem::size_of::<ByHandleFileInformation>(), 52);
        assert_eq!(std::mem::align_of::<ByHandleFileInformation>(), 4);
    }

    #[test]
    fn hard_links_and_reopens_share_identity_but_distinct_files_do_not() {
        let fixture = Fixture::new();
        let original = fixture.0.join("original.ax");
        let alias = fixture.0.join("alias.ax");
        let separate = fixture.0.join("separate.ax");
        fs::write(&original, b"same bytes").unwrap();
        fs::hard_link(&original, &alias).unwrap();
        fs::write(&separate, b"same bytes").unwrap();
        let first = File::open(&original).unwrap();
        let second = File::open(&alias).unwrap();
        let reopened = File::open(&original).unwrap();
        let different = File::open(&separate).unwrap();
        assert_eq!(identity(&first).unwrap(), identity(&second).unwrap());
        assert_eq!(identity(&first).unwrap(), identity(&reopened).unwrap());
        assert_ne!(identity(&first).unwrap(), identity(&different).unwrap());
    }

    #[test]
    fn querying_identity_borrows_the_handle_and_preserves_its_cursor() {
        let fixture = Fixture::new();
        let path = fixture.0.join("cursor.ax");
        fs::write(&path, b"012345").unwrap();
        let mut file = File::open(&path).unwrap();
        file.seek(SeekFrom::Start(2)).unwrap();
        let before = identity(&file).unwrap();
        let mut suffix = String::new();
        file.read_to_string(&mut suffix).unwrap();
        assert_eq!(suffix, "2345");
        assert_eq!(identity(&file).unwrap(), before);
    }
}
