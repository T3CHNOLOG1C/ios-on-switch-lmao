//! The seed beneath a cache directory ([`super::store::SEED_VARIABLE`]), in either of two layouts:
//!
//! - **Packed**: `<seed>/index` and `<seed>/packs/<NNN>`. The index is read through one shared
//!   read-only mapping: a header, then one record per entry sorted by name, so a lookup is a binary
//!   search and a hit is one positioned read of a pack. A seed of 100k entries is a few hundred
//!   files rather than 100k, which is what a card written over USB mass storage can take.
//! - **Directory**: laid out as a cache directory (`<seed>/v1/...`), one file per entry.
//!
//! ```text
//! index:  magic "M2VSEED1" | version u32 = 1 | packs u32 | entries u64 | build [u8; 32]
//!         then per entry, sorted by (name, space):
//!         name [u8; 32] | space u8 (0: the build's, 1: the content directory's) | 0 u8 | pack u16
//!         | length u32 | offset u64                                          (all little endian)
//! ```

use super::store::{content, entry_path_in, LAYOUT};
use metal2vulkan::cache::Hash256;
use std::fs;
use std::os::unix::fs::FileExt;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};
use std::sync::OnceLock;

const MAGIC: &[u8; 8] = b"M2VSEED1";
const VERSION: u32 = 1;
const HEADER: usize = 56;
const RECORD: usize = 48;
const INDEX: &str = "index";
const PACKS: &str = "packs";

/// Where an entry of a seed lives: a build's entry or a content record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Space {
    Build = 0,
    Content = 1,
}

/// What a read of a seed's entry found.
pub(crate) enum Read {
    Absent,
    Failed,
    Bytes(Vec<u8>),
}

/// An opened seed.
#[derive(Debug)]
pub(crate) struct Seed {
    root: PathBuf,
    packed: Option<Packed>,
}

#[derive(Debug)]
struct Packed {
    map: Mapping,
    entries: usize,
    directory: PathBuf,
    packs: Vec<OnceLock<Option<fs::File>>>,
}

impl Seed {
    /// Open the seed at `root` for `build`: packed when it has an index of this layout, else a
    /// directory when it has `v1/`, else none. A packed seed of another build is none.
    pub(crate) fn open(root: PathBuf, build: &Hash256) -> Option<Seed> {
        if let Some(packed) = Packed::open(&root, build) {
            return Some(Seed { root, packed: Some(packed) });
        }
        root.join(LAYOUT).is_dir().then_some(Seed { root, packed: None })
    }

    pub(crate) fn root(&self) -> &Path {
        &self.root
    }

    /// The entry named `name` (hex) of `space` (`build` names a build's directory), of at most
    /// `limit` bytes; a larger one is absent.
    pub(crate) fn read(&self, build: &Hash256, space: Space, name: &str, limit: u64) -> Read {
        match &self.packed {
            Some(packed) => packed.read(space, name, limit),
            None => {
                let directory = match space {
                    Space::Build => *build,
                    Space::Content => content(),
                };
                read_file(&entry_path_in(&self.root, &directory, name), limit)
            }
        }
    }
}

impl Packed {
    fn open(root: &Path, build: &Hash256) -> Option<Packed> {
        let file = fs::File::open(root.join(INDEX)).ok()?;
        let length = usize::try_from(file.metadata().ok()?.len()).ok()?;
        if length < HEADER {
            return None;
        }
        let map = Mapping::new(&file, length)?;
        let bytes = map.bytes();
        let entries = usize::try_from(u64::from_le_bytes(bytes[16..24].try_into().ok()?)).ok()?;
        let packs = u32::from_le_bytes(bytes[12..16].try_into().ok()?) as usize;
        let fits = entries.checked_mul(RECORD).and_then(|n| n.checked_add(HEADER)) == Some(length);
        if &bytes[..8] != MAGIC
            || u32::from_le_bytes(bytes[8..12].try_into().ok()?) != VERSION
            || !fits
            || bytes[24..56] != build[..]
        {
            return None;
        }
        Some(Packed {
            map,
            entries,
            directory: root.join(PACKS),
            packs: (0..packs).map(|_| OnceLock::new()).collect(),
        })
    }

    fn record(&self, at: usize) -> &[u8] {
        &self.map.bytes()[HEADER + at * RECORD..HEADER + (at + 1) * RECORD]
    }

    fn find(&self, space: Space, name: &[u8; 32]) -> Option<(usize, u32, u64)> {
        let (mut low, mut high) = (0, self.entries);
        while low < high {
            let middle = low + (high - low) / 2;
            let record = self.record(middle);
            match (&record[..32], record[32]).cmp(&(&name[..], space as u8)) {
                std::cmp::Ordering::Less => low = middle + 1,
                std::cmp::Ordering::Greater => high = middle,
                std::cmp::Ordering::Equal => {
                    let pack = u16::from_le_bytes([record[34], record[35]]) as usize;
                    let length = u32::from_le_bytes(record[36..40].try_into().ok()?);
                    let offset = u64::from_le_bytes(record[40..48].try_into().ok()?);
                    return Some((pack, length, offset));
                }
            }
        }
        None
    }

    fn read(&self, space: Space, name: &str, limit: u64) -> Read {
        let Some(name) = unhex(name) else {
            return Read::Absent;
        };
        let Some((pack, length, offset)) = self.find(space, &name) else {
            return Read::Absent;
        };
        if u64::from(length) > limit {
            return Read::Absent;
        }
        let Some(file) = self.packs.get(pack).and_then(|slot| {
            slot.get_or_init(|| fs::File::open(self.directory.join(format!("{pack:03}"))).ok())
                .as_ref()
        }) else {
            return Read::Failed;
        };
        let mut bytes = vec![0; length as usize];
        match file.read_exact_at(&mut bytes, offset) {
            Ok(()) => Read::Bytes(bytes),
            Err(_) => Read::Failed,
        }
    }
}

/// A read-only shared mapping of a whole file, unmapped when dropped.
#[derive(Debug)]
struct Mapping {
    address: *mut libc::c_void,
    length: usize,
}

// SAFETY: the mapping is read only and lives as long as the value; nothing writes through it.
unsafe impl Send for Mapping {}
// SAFETY: as above: shared reads of immutable memory.
unsafe impl Sync for Mapping {}

impl Mapping {
    fn new(file: &fs::File, length: usize) -> Option<Mapping> {
        // SAFETY: a fresh read-only view of `length` bytes of an open file, shared with every other
        // process that maps the seed; the result is checked before use.
        let address = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                length,
                libc::PROT_READ,
                libc::MAP_SHARED,
                file.as_raw_fd(),
                0,
            )
        };
        (address != libc::MAP_FAILED).then_some(Mapping { address, length })
    }

    fn bytes(&self) -> &[u8] {
        // SAFETY: the mapping covers `length` readable bytes for as long as `self` lives.
        unsafe { std::slice::from_raw_parts(self.address.cast::<u8>(), self.length) }
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        // SAFETY: unmaps exactly the region `new` mapped.
        unsafe {
            libc::munmap(self.address, self.length);
        }
    }
}

fn unhex(name: &str) -> Option<[u8; 32]> {
    let digits = name.as_bytes();
    if digits.len() != 64 {
        return None;
    }
    let mut out = [0u8; 32];
    for (byte, pair) in out.iter_mut().zip(digits.chunks_exact(2)) {
        let high = (pair[0] as char).to_digit(16)?;
        let low = (pair[1] as char).to_digit(16)?;
        *byte = (high << 4 | low) as u8;
    }
    Some(out)
}

fn read_file(path: &Path, limit: u64) -> Read {
    match fs::metadata(path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Read::Absent,
        Err(_) => Read::Failed,
        Ok(metadata) if metadata.len() > limit => Read::Absent,
        Ok(_) => match fs::read(path) {
            Ok(bytes) if bytes.len() as u64 <= limit => Read::Bytes(bytes),
            Ok(_) => Read::Absent,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Read::Absent,
            Err(_) => Read::Failed,
        },
    }
}
