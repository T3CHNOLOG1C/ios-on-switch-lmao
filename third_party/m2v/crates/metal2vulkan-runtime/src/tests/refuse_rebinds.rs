//! A device made with `DeviceOptions::refuse_rebinds`: its encoders log no calls while it has a
//! buffer over host pages (`replay.rs`, `DeviceShared::logs_calls`), and it refuses a rebind
//! (`Invalid`, `RebindProblem::Refused`), dropping the deallocator unrun; a device made without it
//! logs while it has one, as before.
//!
//! A device test: compiled only under the validation crate's GPU safety cfgs (`m2v_gpu`,
//! `m2v_cpu_icd`), on the kind of device the cfg selects.

use crate::{
    Buffer, Deallocator, Device, DeviceKind, DeviceOptions, Error, HostMemory, Invalid, Problem,
    RebindProblem, ResourceOptions, ShaderCacheOptions,
};
use std::alloc::Layout;
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

/// The no-copy buffers' bytes and alignment.
const NO_COPY: usize = 64 << 10;
const NO_COPY_ALIGN: usize = 64 << 10;

/// The longest a released buffer's deallocator may take to run.
const WAIT: Duration = Duration::from_secs(10);

/// A scratch directory for the device's shader cache, removed when dropped.
struct Scratch(PathBuf);

impl Scratch {
    fn new(name: &str) -> Scratch {
        let path = std::env::temp_dir().join(format!(
            "m2v-refuse-rebinds-{name}-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&path).unwrap();
        Scratch(path)
    }
}

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

/// The device the cfg selects, refusing rebinds where `refuse`, its shader cache in `cache`.
fn device(cache: &Path, refuse: bool) -> Device {
    let options = DeviceOptions {
        kinds: match cfg!(m2v_cpu_icd) {
            true => vec![DeviceKind::Cpu],
            false => vec![
                DeviceKind::Discrete,
                DeviceKind::Integrated,
                DeviceKind::Virtual,
            ],
        },
        shader_cache: ShaderCacheOptions {
            directory: Some(cache.to_owned()),
            ..ShaderCacheOptions::default()
        },
        refuse_rebinds: refuse,
        ..DeviceOptions::default()
    };
    Device::system_default_with(&options).unwrap()
}

/// Memory of the test's, zeroed, and the deallocator that frees it, counting its runs in `runs`.
fn memory(runs: &Arc<AtomicUsize>) -> (NonNull<u8>, Deallocator) {
    let layout = Layout::from_size_align(NO_COPY, NO_COPY_ALIGN).unwrap();
    // SAFETY: the layout's size is not zero.
    let memory = NonNull::new(unsafe { std::alloc::alloc_zeroed(layout) }).unwrap();
    let runs = Arc::clone(runs);
    let deallocator: Deallocator = Box::new(move |pointer, _| {
        // SAFETY: the pointer is the memory allocated above with `layout`, freed once.
        unsafe { std::alloc::dealloc(pointer.as_ptr(), layout) };
        runs.fetch_add(1, Ordering::SeqCst);
    });
    (memory, deallocator)
}

/// A `Shared` buffer over memory of the test's, whose deallocator counts into `runs`, and the
/// memory.
fn no_copy(device: &Device, runs: &Arc<AtomicUsize>) -> (Buffer, NonNull<u8>) {
    let (memory, deallocator) = memory(runs);
    // SAFETY: the memory is the test's, mapped and writable, aligned to 64 KiB, until the
    // deallocator frees it once the buffer's last reference goes.
    let buffer = unsafe {
        device.new_buffer_with_bytes_no_copy(
            HostMemory::new(memory),
            NO_COPY as u64,
            ResourceOptions::shared(),
            Some(deallocator),
        )
    }
    .unwrap();
    (buffer, memory)
}

/// Wait for `runs` to reach one.
#[track_caller]
fn ran_once(runs: &AtomicUsize) {
    let start = Instant::now();
    while runs.load(Ordering::SeqCst) == 0 && start.elapsed() < WAIT {
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(runs.load(Ordering::SeqCst), 1, "the deallocator's runs");
}

/// Without the option, a device with a no-copy buffer logs its encoders' calls; with it, it logs
/// none, and refuses the buffer's rebind without running the new memory's deallocator (that
/// memory leaks), the buffer still over its own memory, which is freed when the buffer goes.
#[test]
fn a_device_that_refuses_rebinds_logs_no_calls() {
    let scratch = Scratch::new("default");
    let plain = device(&scratch.0, false);
    assert!(!plain.shared().logs_calls(), "no buffer over host pages yet");
    let plain_runs = Arc::new(AtomicUsize::new(0));
    let (kept, _) = no_copy(&plain, &plain_runs);
    assert!(plain.shared().logs_calls());
    drop(kept);
    ran_once(&plain_runs);

    let scratch = Scratch::new("refusing");
    let refusing = device(&scratch.0, true);
    let runs = Arc::new(AtomicUsize::new(0));
    let (buffer, own) = no_copy(&refusing, &runs);
    assert!(refusing.shared().rebinds.any());
    assert!(!refusing.shared().logs_calls());
    let new_runs = Arc::new(AtomicUsize::new(0));
    let (new, deallocator) = memory(&new_runs);
    // SAFETY: the new memory holds the buffer's length; the call is refused and keeps nothing.
    let rebound =
        unsafe { buffer.rebind_bytes_no_copy(HostMemory::new(new), Some(deallocator), None) };
    assert_eq!(
        rebound,
        Err(Error::Invalid(Invalid::new(
            "rebind_bytes_no_copy",
            Problem::Rebind(RebindProblem::Refused)
        )))
    );
    assert_eq!(new_runs.load(Ordering::SeqCst), 0);
    assert_eq!(buffer.contents(), Some(own));
    drop(buffer);
    ran_once(&runs);
    assert_eq!(new_runs.load(Ordering::SeqCst), 0);
}
