//! Skipping a two-plane YCbCr texture's conversion (`ycbcr.rs`) when its planes' bytes are the
//! ones the image already holds the conversion of.
//!
//! The copy in of a converted texture's shadow is a conversion at every encoder that uses the
//! texture (the module docs of `ycbcr.rs`): the surface's bytes are the CPU's to write unseen,
//! so the runtime cannot tell cheaply that they are unchanged. On a device where one conversion
//! of a large texture costs tens of milliseconds (EVB10, Mali-G610: 74 ms for a 3416 x 3416
//! lock-screen wallpaper whose few pixels in a scissor rectangle the frame samples), a screen
//! that samples such a texture in every frame spends its frames on converting it.
//!
//! **What this skips, and when.** Only when `M2V_YCBCR_MEMO=N` names a sample count above 0
//! (the program's choice: the skip trusts a sample of the bytes). The conversion keeps, from the
//! last one it recorded, a hash of `N` sampled 64-byte chunks of the planes' bytes read through
//! the buffer's host address, and the writes' count of the buffer's identity. It records no
//! conversion again while both are as they were. A buffer with no host address (`Private`)
//! always converts.
//!
//! - **GPU writes** are counted where every command's uses are recorded (`EncoderUses::record`,
//!   through [`note_write`]): any use that writes the buffer's key, or its alias's (the host
//!   memory it joined), at any range, tracked or not, by any encoder, bumps the count. A write
//!   after the conversion in the same encoder, or in an encoder of another command buffer
//!   encoded before the next conversion, is seen as a changed count. A command that wrote the
//!   planes' bytes and has not run yet is such a use already; so is one on another thread that
//!   encodes while this one hashes (the hash is of what the host sees, the count of what was
//!   encoded: either changing converts again).
//! - **CPU writes and other processes' writes** are seen through the host address, as far as the
//!   sample reaches: a change of a small region between two samples' chunks is missed. A
//!   program that updates a few rows of a large YCbCr surface and samples it needs a full hash
//!   (`N` at least the span's chunks); a decoder's frames change every chunk.
//! - **What it does not see**: a recording that is never submitted (an encoder that failed, an
//!   encoder encoded again at a rebind, `command_buffer/reencode.rs`) leaves the memo saying the
//!   image holds a conversion it does not; the texture converts again as soon as its bytes or
//!   the count change.

use crate::buffer::Buffer;
use crate::key_hash::KeyHasher;
use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hash, Hasher};
use std::ops::Range;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

/// The size of a sampled chunk.
const CHUNK: usize = 64;

/// How many watched keys the table holds: [`note_write`] returns at once while 0.
static WATCHED: AtomicUsize = AtomicUsize::new(0);

/// A watched key's writes so far, and how many conversions watch it.
type Table = HashMap<u64, (usize, Arc<AtomicU64>), BuildHasherDefault<KeyHasher>>;

fn table() -> &'static Mutex<Table> {
    static TABLE: OnceLock<Mutex<Table>> = OnceLock::new();
    TABLE.get_or_init(|| Mutex::new(Table::default()))
}

fn lock<T>(mutex: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// The sample count `M2V_YCBCR_MEMO` names, 0 for none or not a number.
fn samples() -> usize {
    static SAMPLES: OnceLock<usize> = OnceLock::new();
    *SAMPLES.get_or_init(|| {
        std::env::var("M2V_YCBCR_MEMO")
            .ok()
            .and_then(|value| value.trim().parse().ok())
            .unwrap_or(0)
    })
}

fn hash_key<K: Hash>(key: &K) -> u64 {
    let mut hasher = KeyHasher::default();
    key.hash(&mut hasher);
    hasher.finish()
}

/// Whether any conversion watches a key: where it does not, [`note_write`] has nothing to do.
pub(super) fn watching() -> bool {
    WATCHED.load(Ordering::Relaxed) != 0
}

/// Note that a command uses the resource of `key` to write: the count of every watch of it goes
/// up. Where the runtime records the uses of every command; cheap while nothing is watched.
pub(super) fn note_write<K: Hash>(key: &K) {
    if WATCHED.load(Ordering::Relaxed) == 0 {
        return;
    }
    let hash = hash_key(key);
    if let Some((_, writes)) = lock(table()).get(&hash) {
        writes.fetch_add(1, Ordering::AcqRel);
    }
}

/// The writes' counts of the buffer's keys, watched while it lives.
struct Watch {
    slots: Vec<(u64, Arc<AtomicU64>)>,
}

impl Watch {
    fn new(hashes: impl IntoIterator<Item = u64>) -> Watch {
        let mut table = lock(table());
        let mut slots: Vec<(u64, Arc<AtomicU64>)> = Vec::new();
        for hash in hashes {
            if slots.iter().any(|(known, _)| *known == hash) {
                continue;
            }
            let entry = table
                .entry(hash)
                .or_insert_with(|| (0, Arc::new(AtomicU64::new(0))));
            entry.0 += 1;
            slots.push((hash, Arc::clone(&entry.1)));
        }
        WATCHED.store(table.len(), Ordering::Relaxed);
        Watch { slots }
    }

    /// The writes counted so far, over every key: only ever more.
    fn writes(&self) -> u64 {
        self.slots
            .iter()
            .map(|(_, writes)| writes.load(Ordering::Acquire))
            .fold(0, u64::wrapping_add)
    }
}

impl Drop for Watch {
    fn drop(&mut self) {
        let mut table = lock(table());
        for (hash, _) in &self.slots {
            if let Some(entry) = table.get_mut(hash) {
                entry.0 -= 1;
                if entry.0 == 0 {
                    table.remove(hash);
                }
            }
        }
        WATCHED.store(table.len(), Ordering::Relaxed);
    }
}

/// What the image holds the conversion of, by the module docs' hash and count.
pub(super) struct Memo {
    buffer: Buffer,
    /// The planes' bytes in the buffer.
    span: Range<u64>,
    watch: Watch,
    samples: usize,
    last: Mutex<Option<(u64, u64)>>,
}

impl Memo {
    /// The memo of the planes' bytes `span` of `buffer`, written through `keys` (its identity
    /// and its alias's); `None` where the program asked for none (`M2V_YCBCR_MEMO`) or the
    /// buffer has no host address.
    pub(super) fn of<K: Hash>(
        buffer: &Buffer,
        span: Range<u64>,
        keys: impl IntoIterator<Item = K>,
    ) -> Option<Memo> {
        let samples = samples();
        if samples == 0 || span.end <= span.start || span.end > buffer.length() {
            return None;
        }
        buffer.contents()?;
        let watch = Watch::new(keys.into_iter().map(|key| hash_key(&key)));
        Some(Memo {
            buffer: buffer.clone(),
            span,
            watch,
            samples,
            last: Mutex::new(None),
        })
    }

    /// Whether the image holds the conversion of the planes' bytes as they are: the sample's
    /// hash and the writes' count are what they were when it was last asked. Remembers them
    /// either way.
    pub(super) fn unchanged(&self) -> bool {
        let Some(contents) = self.buffer.contents() else {
            return false;
        };
        let len = (self.span.end - self.span.start) as usize;
        let base = contents.as_ptr().wrapping_add(self.span.start as usize);
        // The writes first: one that lands while the sample is read makes the next ask differ.
        let writes = self.watch.writes();
        let hash = sample(base, len, self.samples);
        let mut last = lock(&self.last);
        let same = *last == Some((hash, writes));
        *last = Some((hash, writes));
        same
    }
}

/// A hash of `samples` chunks of the `len` bytes at `base`, evenly spread and the last among
/// them; every chunk of a span of at most that many.
fn sample(base: *const u8, len: usize, samples: usize) -> u64 {
    let chunks = len / CHUNK;
    let mut hash = 0x9e37_79b9_7f4a_7c15_u64 ^ len as u64;
    let mut mix = |word: u64| {
        let spread = (hash ^ word).wrapping_mul(0x9e37_79b9_7f4a_7c15);
        hash = spread ^ (spread >> 29);
    };
    let mut chunk = |at: usize| {
        let from = base.wrapping_add(at * CHUNK) as *const u64;
        for word in 0..CHUNK / 8 {
            // SAFETY: the chunk lies in the span, which lies in the buffer's bytes (`Memo::of`),
            // mapped for the buffer's life; the words are read unaligned, and the bytes may be
            // written by others while they are read, which only makes the hash differ.
            mix(unsafe { std::ptr::read_unaligned(from.wrapping_add(word)) });
        }
    };
    if chunks == 0 {
        return hash;
    }
    if chunks <= samples {
        (0..chunks).for_each(&mut chunk);
    } else {
        for i in 0..samples {
            chunk((i as u128 * chunks as u128 / samples as u128) as usize);
        }
        chunk(chunks - 1);
    }
    hash
}
