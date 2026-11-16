//! The system layer: a second cache directory, read only, that one trusted writer fills for every
//! process of the machine (the consumer's render server primes the OS's Metal libraries into it).
//!
//! - **Laid out as a device's own directory** (`store.rs`): the same `v1/<build>/<xx>/<name>`
//!   entries and content records, so what the writer's device wrote there under its own rules is
//!   what a reader's device looks up. A lookup asks the device's own directory first and this one
//!   only for what that lacks: a record the process added to (a making's driver data) is its own
//!   from then on.
//! - **Trusted by who may write it, at every level opened.** The directory, each directory
//!   under it on the way to an entry, and the entry are opened without following a link
//!   (`O_NOFOLLOW`, relative to the level above) and judged by `fstat`: a directory or a regular
//!   file, owned by one of the owners the consumer named, that neither its group nor others may
//!   write. The directories above the layer are judged by `lstat` when it is opened: no link,
//!   owned by the superuser or one of the owners, and written by others only where the sticky bit
//!   keeps them from replacing what is not theirs. An entry's envelope carries a checksum, not a
//!   signature, and the driver's pipeline data is parsed by the driver as it is: a process must
//!   therefore never read an entry that anyone but those owners could have put there, and a
//!   level that fails the test is refused and counted, never read.
//! - **Never written, touched or removed from here.** A hit refreshes no access time and
//!   contents a decoder refuses stay (counted): the directory is its writer's.
//! - **Process-wide.** [`set_system_layer`] names the layer for the devices made after it;
//!   [`set_cache_readable_by_all`] makes the directories and entries a device writes readable by
//!   everyone (`0755`, `0644`) instead of by their owner alone, for the process that fills the
//!   layer through its own directory.

use std::ffi::CString;
use std::io::Read;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::ffi::OsStrExt;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, RwLock};

/// What a system layer did for this process ([`SystemLayer::counts`]).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SystemLayerCounts {
    /// Entries and records read from the layer and accepted by their decoder: work not done
    /// in this process.
    pub accepted: u64,
    /// Entries read from the layer whose contents the decoder refused (left in place).
    pub rejected: u64,
    /// Levels (a directory or an entry) refused for their kind, owner or mode: never read.
    pub refused: u64,
    /// Bytes of the accepted entries.
    pub accepted_bytes: u64,
}

/// An opened system layer.
#[derive(Debug)]
pub struct SystemLayer {
    root: OwnedFd,
    path: PathBuf,
    owners: Vec<u32>,
    accepted: AtomicU64,
    rejected: AtomicU64,
    refused: AtomicU64,
    accepted_bytes: AtomicU64,
}

static LAYER: RwLock<Option<Arc<SystemLayer>>> = RwLock::new(None);
static READABLE_BY_ALL: AtomicBool = AtomicBool::new(false);

/// Name the system layer the devices made from now on consult, or none.
pub fn set_system_layer(layer: Option<Arc<SystemLayer>>) {
    *LAYER.write().unwrap_or_else(|e| e.into_inner()) = layer;
}

/// The system layer named last.
pub fn system_layer() -> Option<Arc<SystemLayer>> {
    LAYER.read().unwrap_or_else(|e| e.into_inner()).clone()
}

/// Whether the cache directories opened from now on make their directories and entries readable
/// by everyone.
pub fn set_cache_readable_by_all(on: bool) {
    READABLE_BY_ALL.store(on, Ordering::Relaxed);
}

pub(super) fn cache_readable_by_all() -> bool {
    READABLE_BY_ALL.load(Ordering::Relaxed)
}

fn c_path(bytes: &[u8]) -> Option<CString> {
    CString::new(bytes).ok()
}

fn fstat(fd: &OwnedFd) -> Option<libc::stat> {
    // SAFETY: a zeroed stat is a valid out value and the descriptor is open.
    unsafe {
        let mut st: libc::stat = std::mem::zeroed();
        (libc::fstat(fd.as_raw_fd(), &mut st) == 0).then_some(st)
    }
}

/// Open `name` under `parent` (or by its path where `parent` is `None`), never through a link.
fn open_level(parent: Option<&OwnedFd>, name: &[u8], directory: bool) -> Result<OwnedFd, i32> {
    let name = c_path(name).ok_or(libc::EINVAL)?;
    let kind = if directory { libc::O_DIRECTORY } else { libc::O_NONBLOCK };
    let flags = libc::O_RDONLY | libc::O_NOFOLLOW | libc::O_CLOEXEC | kind;
    // SAFETY: the name is a C string and the parent descriptor is open.
    let fd = unsafe {
        match parent {
            Some(parent) => libc::openat(parent.as_raw_fd(), name.as_ptr(), flags),
            None => libc::open(name.as_ptr(), flags),
        }
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error().raw_os_error().unwrap_or(libc::EIO));
    }
    // SAFETY: a descriptor this call opened and nothing else owns.
    Ok(unsafe { OwnedFd::from_raw_fd(fd) })
}

impl SystemLayer {
    /// Open the layer at `root`, whose every level must be owned by one of `owners`. The error
    /// says which level failed and how.
    pub fn open(root: &Path, owners: &[u32]) -> Result<SystemLayer, String> {
        if !root.is_absolute() || owners.is_empty() {
            return Err("the layer needs an absolute path and an owner".into());
        }
        // The directories above the layer: no link among them, each the superuser's or an
        // owner's, written by others only under the sticky bit. A level the sandbox lets this
        // process see nothing of (EPERM, which no file mode gives) is one the OS made.
        let mut above = PathBuf::new();
        let mut parts = root.components().peekable();
        while let Some(part) = parts.next() {
            match part {
                Component::RootDir | Component::Normal(_) => above.push(part),
                _ => return Err(format!("{}: not a plain path", root.display())),
            }
            if parts.peek().is_none() {
                break;
            }
            let path = c_path(above.as_os_str().as_bytes()).ok_or("a path with a zero byte")?;
            // SAFETY: a zeroed stat is a valid out value and the path is a C string.
            let (result, st) = unsafe {
                let mut st: libc::stat = std::mem::zeroed();
                (libc::lstat(path.as_ptr(), &mut st), st)
            };
            if result != 0 {
                let error = std::io::Error::last_os_error();
                if error.raw_os_error() == Some(libc::EPERM) {
                    continue;
                }
                return Err(format!("{}: {error}", above.display()));
            }
            let directory = st.st_mode & libc::S_IFMT == libc::S_IFDIR;
            let owned = st.st_uid == 0 || owners.contains(&st.st_uid);
            let closed = st.st_mode & 0o022 == 0 || st.st_mode & libc::S_ISVTX != 0;
            if !directory || !owned || !closed {
                return Err(format!(
                    "{}: above the layer, uid {} mode {:o}",
                    above.display(),
                    st.st_uid,
                    st.st_mode
                ));
            }
        }
        let fd = open_level(None, root.as_os_str().as_bytes(), true)
            .map_err(|errno| format!("{}: errno {errno}", root.display()))?;
        let layer = SystemLayer {
            root: fd,
            path: root.to_path_buf(),
            owners: owners.to_vec(),
            accepted: AtomicU64::new(0),
            rejected: AtomicU64::new(0),
            refused: AtomicU64::new(0),
            accepted_bytes: AtomicU64::new(0),
        };
        match fstat(&layer.root) {
            Some(st) if layer.trusted(&st, true) => Ok(layer),
            Some(st) => Err(format!(
                "{}: uid {} mode {:o}",
                root.display(),
                st.st_uid,
                st.st_mode
            )),
            None => Err(format!("{}: no fstat", root.display())),
        }
    }

    /// The layer's directory.
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Whether a level of `st` is what the layer reads: a directory or a regular file, an
    /// owner's, that neither its group nor others may write.
    fn trusted(&self, st: &libc::stat, directory: bool) -> bool {
        let kind = if directory { libc::S_IFDIR } else { libc::S_IFREG };
        st.st_mode & libc::S_IFMT == kind && self.owners.contains(&st.st_uid) && st.st_mode & 0o022 == 0
    }

    /// The open file of `relative` under the layer and its length, every level on the way judged.
    /// `None` where a level is absent or refused (refusals are counted).
    fn entry(&self, relative: &Path) -> Option<(std::fs::File, u64)> {
        let mut parts = relative.components().peekable();
        let mut level: Option<OwnedFd> = None;
        while let Some(part) = parts.next() {
            let Component::Normal(name) = part else {
                return None;
            };
            let last = parts.peek().is_none();
            let parent = level.as_ref().unwrap_or(&self.root);
            let fd = match open_level(Some(parent), name.as_bytes(), !last) {
                Ok(fd) => fd,
                Err(errno) => {
                    // A link in an entry's place (ELOOP), or a file where a directory belongs.
                    if errno == libc::ELOOP || errno == libc::ENOTDIR {
                        self.refused.fetch_add(1, Ordering::Relaxed);
                    }
                    return None;
                }
            };
            let st = fstat(&fd)?;
            if !self.trusted(&st, !last) {
                self.refused.fetch_add(1, Ordering::Relaxed);
                return None;
            }
            if last {
                return Some((std::fs::File::from(fd), st.st_size as u64));
            }
            level = Some(fd);
        }
        None
    }

    /// Whether the layer holds `relative`, of at most `limit` bytes.
    pub fn holds(&self, relative: &Path, limit: u64) -> bool {
        self.entry(relative).is_some_and(|(_, length)| length <= limit)
    }

    /// The bytes of `relative` under the layer, at most `limit`; `None` where it is absent,
    /// refused, longer or unreadable. The caller's decoder judges the contents and reports the
    /// verdict ([`accepted`](Self::accepted), [`rejected`](Self::rejected)).
    pub fn read(&self, relative: &Path, limit: u64) -> Option<Vec<u8>> {
        let (file, length) = self.entry(relative)?;
        if length > limit {
            return None;
        }
        let mut bytes = Vec::with_capacity(usize::try_from(length).unwrap_or(0));
        match file.take(limit + 1).read_to_end(&mut bytes) {
            Ok(_) if bytes.len() as u64 <= limit => Some(bytes),
            _ => None,
        }
    }

    /// Count an entry of `bytes` bytes read from the layer that its decoder accepted.
    pub fn accepted(&self, bytes: u64) {
        self.accepted.fetch_add(1, Ordering::Relaxed);
        self.accepted_bytes.fetch_add(bytes, Ordering::Relaxed);
    }

    /// Count an entry read from the layer that its decoder refused.
    pub fn rejected(&self) {
        self.rejected.fetch_add(1, Ordering::Relaxed);
    }

    /// The counts so far.
    pub fn counts(&self) -> SystemLayerCounts {
        SystemLayerCounts {
            accepted: self.accepted.load(Ordering::Relaxed),
            rejected: self.rejected.load(Ordering::Relaxed),
            refused: self.refused.load(Ordering::Relaxed),
            accepted_bytes: self.accepted_bytes.load(Ordering::Relaxed),
        }
    }
}
