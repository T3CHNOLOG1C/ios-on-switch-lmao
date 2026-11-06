//! The seed beneath a cache directory ([`super::store::SEED_VARIABLE`]), in either of two layouts:
//!
//! - **Packed**: `<seed>/index` and `<seed>/packs/<NNN>`, every one mapped read only when the seed
//!   is opened (the descriptors closed again), so a hit is a table look-up and a copy out of a
//!   mapping: no open, stat or read. Names are SHA-256 digests, so their first bytes are the hash:
//!   a table of 2^k runs, each the entries whose name starts with that k-bit prefix, sorted. A seed
//!   of 100k entries is a few hundred files rather than 100k, which is what a card written over
//!   USB mass storage can take.
//! - **Directory**: laid out as a cache directory (`<seed>/v1/...`), one file per entry.
//!
//! ```text
//! index:  magic "M2VSEED2" | version u32 = 2 | packs u32 | entries u64 | build [u8; 32] | k u32 | 0 u32
//!         | 2^k + 1 run starts u32 (entry numbers)
//!         | per entry, sorted by (name, space): name [u8; 32] | space u8 (0: the build's, 1: the
//!           content directory's) | 0 u8 | pack u16 | length u32 | offset u64      (little endian)
//! ```

use super::store::{content, entry_path_in, LAYOUT};
use metal2vulkan::cache::Hash256;
use std::fs;
use std::os::unix::io::AsRawFd;
use std::path::{Path, PathBuf};

const MAGIC: &[u8; 8] = b"M2VSEED2";
const VERSION: u32 = 2;
const HEADER: usize = 64;
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
    index: Mapping,
    entries: usize,
    prefix_bits: u32,
    records: usize,
    packs: Vec<Option<Mapping>>,
}

impl Seed {
    /// Open the seed at `root` for `build`: packed when it has an index of this layout, else a
    /// directory when it has `v1/`, else none. A packed seed of another build is none.
    pub(crate) fn open(root: PathBuf, build: &Hash256) -> Option<Seed> {
        if let Some(packed) = Packed::open(&root, build) {
            return Some(Seed {
                root,
                packed: Some(packed),
            });
        }
        root.join(LAYOUT)
            .is_dir()
            .then_some(Seed { root, packed: None })
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
        let index = Mapping::open(&root.join(INDEX))?;
        let bytes = index.bytes();
        if bytes.len() < HEADER
            || &bytes[..8] != MAGIC
            || u32_at(bytes, 8)? != VERSION
            || bytes[24..56] != build[..]
        {
            return None;
        }
        let packs = u32_at(bytes, 12)? as usize;
        let entries = usize::try_from(u64::from_le_bytes(bytes[16..24].try_into().ok()?)).ok()?;
        let prefix_bits = u32_at(bytes, 56)?;
        if prefix_bits > 24 {
            return None;
        }
        let records = HEADER + ((1usize << prefix_bits) + 1) * 4;
        if entries
            .checked_mul(RECORD)
            .and_then(|n| n.checked_add(records))
            != Some(bytes.len())
        {
            return None;
        }
        let directory = root.join(PACKS);
        let packs = (0..packs)
            .map(|pack| Mapping::open(&directory.join(format!("{pack:03}"))))
            .collect();
        Some(Packed {
            index,
            entries,
            prefix_bits,
            records,
            packs,
        })
    }

    fn record(&self, at: usize) -> &[u8] {
        &self.index.bytes()[self.records + at * RECORD..self.records + (at + 1) * RECORD]
    }

    fn find(&self, space: Space, name: &[u8; 32]) -> Option<(usize, usize, usize)> {
        let run = if self.prefix_bits == 0 {
            0
        } else {
            (u32::from_be_bytes([name[0], name[1], name[2], name[3]]) >> (32 - self.prefix_bits))
                as usize
        };
        let bytes = self.index.bytes();
        let first = u32_at(bytes, HEADER + run * 4)? as usize;
        let end = (u32_at(bytes, HEADER + (run + 1) * 4)? as usize).min(self.entries);
        (first..end).map(|at| self.record(at)).find_map(|record| {
            (record[..32] == name[..] && record[32] == space as u8).then(|| {
                let pack = u16::from_le_bytes([record[34], record[35]]) as usize;
                let length =
                    u32::from_le_bytes([record[36], record[37], record[38], record[39]]) as usize;
                let offset =
                    usize::try_from(u64::from_le_bytes(record[40..48].try_into().ok()?)).ok()?;
                Some((pack, length, offset))
            })?
        })
    }

    fn read(&self, space: Space, name: &str, limit: u64) -> Read {
        let Some(name) = unhex(name) else {
            return Read::Absent;
        };
        let Some((pack, length, offset)) = self.find(space, &name) else {
            return Read::Absent;
        };
        if length as u64 > limit {
            return Read::Absent;
        }
        let bytes = self
            .packs
            .get(pack)
            .and_then(Option::as_ref)
            .map(Mapping::bytes);
        match bytes.and_then(|bytes| bytes.get(offset..offset.checked_add(length)?)) {
            Some(entry) => Read::Bytes(entry.to_vec()),
            None => Read::Failed,
        }
    }
}

fn u32_at(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
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
    /// Map the whole file at `path`; the descriptor is closed again (the mapping keeps the file).
    /// An empty file maps to nothing.
    fn open(path: &Path) -> Option<Mapping> {
        let file = fs::File::open(path).ok()?;
        let length = usize::try_from(file.metadata().ok()?.len()).ok()?;
        if length == 0 {
            return Some(Mapping {
                address: std::ptr::null_mut(),
                length: 0,
            });
        }
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
        if self.length == 0 {
            return &[];
        }
        // SAFETY: the mapping covers `length` readable bytes for as long as `self` lives.
        unsafe { std::slice::from_raw_parts(self.address.cast::<u8>(), self.length) }
    }
}

impl Drop for Mapping {
    fn drop(&mut self) {
        if self.length != 0 {
            // SAFETY: unmaps exactly the region `open` mapped.
            unsafe {
                libc::munmap(self.address, self.length);
            }
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
