//! The only unsafe module in this developer-only process. Every allocation is
//! forwarded unchanged to its allocator; instrumentation never touches payloads.
//! Hooks use only atomics and checked integer arithmetic: no allocation, IO,
//! locks that can allocate, assertions, panics, or unwinding.
use std::alloc::{GlobalAlloc, Layout};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering::Relaxed};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Events {
    pub alloc_attempts: u64,
    pub alloc_successes: u64,
    pub alloc_failures: u64,
    pub zeroed_attempts: u64,
    pub zeroed_successes: u64,
    pub zeroed_failures: u64,
    pub realloc_attempts: u64,
    pub realloc_successes: u64,
    pub realloc_failures: u64,
    pub dealloc_calls: u64,
    pub allocation_requested_bytes: u64,
    pub allocation_successful_bytes: u64,
    pub realloc_requested_bytes: u64,
    pub realloc_successful_old_bytes: u64,
    pub realloc_successful_new_bytes: u64,
    pub deallocated_bytes: u64,
}

macro_rules! event_fields {
    ($macro:ident) => {
        $macro! {alloc_attempts, alloc_successes, alloc_failures,
        zeroed_attempts, zeroed_successes, zeroed_failures,
        realloc_attempts, realloc_successes, realloc_failures, dealloc_calls,
        allocation_requested_bytes, allocation_successful_bytes,
        realloc_requested_bytes, realloc_successful_old_bytes,
        realloc_successful_new_bytes, deallocated_bytes}
    };
}

macro_rules! define_counters {
    ($($field:ident),*) => {
        struct Counters { $( $field: AtomicU64, )* }
        impl Counters {
            const fn new() -> Self { Self { $( $field: AtomicU64::new(0), )* } }
            fn snapshot(&self) -> Events { Events { $( $field: self.$field.load(Relaxed), )* } }
        }
    };
}
event_fields!(define_counters);

#[derive(Clone, Copy)]
pub struct Start {
    events: Events,
    live_bytes: u64,
    live_blocks: u64,
}

#[derive(Clone, Copy, Debug)]
pub struct Sample {
    pub events: Events,
    pub live_bytes_begin: u64,
    pub live_bytes_end: u64,
    pub live_blocks_begin: u64,
    pub live_blocks_end: u64,
    pub peak_live_requested_bytes: u64,
    pub valid: bool,
}

impl Sample {
    pub fn balanced(&self) -> bool {
        let e = self.events;
        let bytes_in = self.live_bytes_begin as u128
            + e.allocation_successful_bytes as u128
            + e.realloc_successful_new_bytes as u128;
        let bytes_out = e.deallocated_bytes as u128 + e.realloc_successful_old_bytes as u128;
        let blocks_in =
            self.live_blocks_begin as u128 + e.alloc_successes as u128 + e.zeroed_successes as u128;
        self.valid
            && e.alloc_attempts as u128 == e.alloc_successes as u128 + e.alloc_failures as u128
            && e.zeroed_attempts as u128 == e.zeroed_successes as u128 + e.zeroed_failures as u128
            && e.realloc_attempts as u128
                == e.realloc_successes as u128 + e.realloc_failures as u128
            && bytes_in.checked_sub(bytes_out) == Some(self.live_bytes_end as u128)
            && blocks_in.checked_sub(e.dealloc_calls as u128) == Some(self.live_blocks_end as u128)
            && self.peak_live_requested_bytes >= self.live_bytes_begin.max(self.live_bytes_end)
            && self.peak_live_requested_bytes as u128 <= bytes_in
            && e.allocation_requested_bytes >= e.allocation_successful_bytes
            && e.realloc_requested_bytes >= e.realloc_successful_new_bytes
    }
}

pub struct Counting<A> {
    inner: A,
    counters: Counters,
    live_bytes: AtomicU64,
    live_blocks: AtomicU64,
    phase_peak: AtomicU64,
    phase_active: AtomicBool,
    invalid: AtomicBool,
}

impl<A> Counting<A> {
    pub const fn new(inner: A) -> Self {
        Self {
            inner,
            counters: Counters::new(),
            live_bytes: AtomicU64::new(0),
            live_blocks: AtomicU64::new(0),
            phase_peak: AtomicU64::new(0),
            phase_active: AtomicBool::new(false),
            invalid: AtomicBool::new(false),
        }
    }

    fn add(&self, counter: &AtomicU64, amount: u64) {
        if counter
            .fetch_update(Relaxed, Relaxed, |n| n.checked_add(amount))
            .is_err()
        {
            self.invalid.store(true, Relaxed);
        }
    }

    fn replace_size(&self, old: u64, new: u64) {
        match self
            .live_bytes
            .fetch_update(Relaxed, Relaxed, |n| n.checked_sub(old)?.checked_add(new))
        {
            Ok(previous) => {
                // The checked update above proves these operations cannot fail.
                // Use checked arithmetic nevertheless so a hook cannot unwind.
                if let Some(current) = previous.checked_sub(old).and_then(|n| n.checked_add(new)) {
                    self.phase_peak.fetch_max(current, Relaxed);
                } else {
                    self.invalid.store(true, Relaxed);
                }
            }
            Err(_) => self.invalid.store(true, Relaxed),
        }
    }

    fn remove_block(&self) {
        if self
            .live_blocks
            .fetch_update(Relaxed, Relaxed, |n| n.checked_sub(1))
            .is_err()
        {
            self.invalid.store(true, Relaxed);
        }
    }

    /// Phase boundaries are used only by the single-threaded driver. Counters
    /// remain active between phases, so freeing older blocks never underflows a
    /// reset counter. Overlapping/nested measurements are invalid, not merged.
    pub fn begin(&self) -> Start {
        if self.phase_active.swap(true, Relaxed) {
            self.invalid.store(true, Relaxed);
        }
        let live_bytes = self.live_bytes.load(Relaxed);
        self.phase_peak.store(live_bytes, Relaxed);
        Start {
            events: self.counters.snapshot(),
            live_bytes,
            live_blocks: self.live_blocks.load(Relaxed),
        }
    }

    pub fn end(&self, start: Start) -> Sample {
        let end = self.counters.snapshot();
        macro_rules! subtract {
            ($($field:ident),*) => {
                Events { $( $field: match end.$field.checked_sub(start.events.$field) {
                    Some(value) => value,
                    None => { self.invalid.store(true, Relaxed); 0 }
                }, )* }
            };
        }
        let events = event_fields!(subtract);
        let live_bytes_end = self.live_bytes.load(Relaxed);
        let live_blocks_end = self.live_blocks.load(Relaxed);
        let peak = self.phase_peak.load(Relaxed);
        if !self.phase_active.swap(false, Relaxed) {
            self.invalid.store(true, Relaxed);
        }
        Sample {
            events,
            live_bytes_begin: start.live_bytes,
            live_bytes_end,
            live_blocks_begin: start.live_blocks,
            live_blocks_end,
            peak_live_requested_bytes: peak,
            valid: !self.invalid.load(Relaxed),
        }
    }
}

// SAFETY: Calls preserve the caller's exact allocator arguments and returned
// pointer. Counters never dereference, modify, retain, or free any payload. Each
// operation is forwarded exactly once. A failed realloc retains the old block.
// The underlying GlobalAlloc implementation carries alignment/lifetime duties.
unsafe impl<A: GlobalAlloc> GlobalAlloc for Counting<A> {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        self.add(&self.counters.alloc_attempts, 1);
        self.add(
            &self.counters.allocation_requested_bytes,
            layout.size() as u64,
        );
        // SAFETY: Forward the valid caller-supplied layout unchanged.
        let result = unsafe { self.inner.alloc(layout) };
        if result.is_null() {
            self.add(&self.counters.alloc_failures, 1);
        } else {
            self.add(&self.counters.alloc_successes, 1);
            self.add(
                &self.counters.allocation_successful_bytes,
                layout.size() as u64,
            );
            self.add(&self.live_blocks, 1);
            self.replace_size(0, layout.size() as u64);
        }
        result
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        self.add(&self.counters.zeroed_attempts, 1);
        self.add(
            &self.counters.allocation_requested_bytes,
            layout.size() as u64,
        );
        // SAFETY: Forward directly instead of calling our alloc and double-counting.
        let result = unsafe { self.inner.alloc_zeroed(layout) };
        if result.is_null() {
            self.add(&self.counters.zeroed_failures, 1);
        } else {
            self.add(&self.counters.zeroed_successes, 1);
            self.add(
                &self.counters.allocation_successful_bytes,
                layout.size() as u64,
            );
            self.add(&self.live_blocks, 1);
            self.replace_size(0, layout.size() as u64);
        }
        result
    }

    unsafe fn realloc(&self, pointer: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        self.add(&self.counters.realloc_attempts, 1);
        self.add(&self.counters.realloc_requested_bytes, new_size as u64);
        // SAFETY: The caller owns this exact layout and supplies a valid new size.
        let result = unsafe { self.inner.realloc(pointer, layout, new_size) };
        if result.is_null() {
            self.add(&self.counters.realloc_failures, 1);
        } else {
            self.add(&self.counters.realloc_successes, 1);
            self.add(
                &self.counters.realloc_successful_old_bytes,
                layout.size() as u64,
            );
            self.add(&self.counters.realloc_successful_new_bytes, new_size as u64);
            self.replace_size(layout.size() as u64, new_size as u64);
        }
        result
    }

    unsafe fn dealloc(&self, pointer: *mut u8, layout: Layout) {
        // SAFETY: Forward the live pointer and its exact layout exactly once.
        unsafe { self.inner.dealloc(pointer, layout) };
        self.add(&self.counters.dealloc_calls, 1);
        self.add(&self.counters.deallocated_bytes, layout.size() as u64);
        self.remove_block();
        self.replace_size(layout.size() as u64, 0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::alloc::System;

    // These tests use direct trait calls on a local allocator, not optimizable
    // Box/Vec allocation expressions or the test runner's global allocator.
    #[test]
    fn exact_success_counts_grow_shrink_and_completed_deallocation() {
        let allocator = Counting::new(System);
        let start = allocator.begin();
        let small = Layout::from_size_align(16, 8).unwrap();
        // SAFETY: Nonzero valid layouts; every returned block is checked and
        // subsequently passed only to this allocator with its current layout.
        unsafe {
            let p = allocator.alloc(small);
            assert!(!p.is_null());
            p.write(23);
            let grown = allocator.realloc(p, small, 64);
            assert!(!grown.is_null());
            assert_eq!(grown.read(), 23);
            let shrunk = allocator.realloc(grown, Layout::from_size_align(64, 8).unwrap(), 8);
            assert!(!shrunk.is_null());
            assert_eq!(shrunk.read(), 23);
            allocator.dealloc(shrunk, Layout::from_size_align(8, 8).unwrap());
        }
        let sample = allocator.end(start);
        assert!(sample.valid);
        assert_eq!(sample.events.alloc_attempts, 1);
        assert_eq!(sample.events.alloc_successes, 1);
        assert_eq!(sample.events.realloc_attempts, 2);
        assert_eq!(sample.events.realloc_successes, 2);
        assert_eq!(sample.events.allocation_requested_bytes, 16);
        assert_eq!(sample.events.allocation_successful_bytes, 16);
        assert_eq!(sample.events.realloc_requested_bytes, 72);
        assert_eq!(sample.events.realloc_successful_old_bytes, 80);
        assert_eq!(sample.events.realloc_successful_new_bytes, 72);
        assert_eq!(sample.events.deallocated_bytes, 8);
        assert_eq!(sample.events.dealloc_calls, 1);
        assert_eq!(sample.live_bytes_end, 0);
        assert_eq!(sample.live_blocks_end, 0);
        assert_eq!(sample.peak_live_requested_bytes, 64);
    }

    #[test]
    fn zeroed_and_overaligned_blocks_survive_phase_boundaries() {
        let allocator = Counting::new(System);
        let layout = Layout::from_size_align(4096, 4096).unwrap();
        let start = allocator.begin();
        // SAFETY: Valid nonzero layout; block is inspected within bounds and
        // deallocated exactly once below with the same allocator and layout.
        let p = unsafe { allocator.alloc_zeroed(layout) };
        assert!(!p.is_null());
        assert_eq!(p as usize % 4096, 0);
        for index in 0..4096 {
            // SAFETY: index is within the successful 4096-byte allocation.
            assert_eq!(unsafe { p.add(index).read() }, 0);
        }
        let first = allocator.end(start);
        assert_eq!(first.events.zeroed_successes, 1);
        assert_eq!(first.events.alloc_successes, 0);
        assert_eq!(first.live_bytes_end, 4096);
        let second = allocator.begin();
        // SAFETY: p still owns layout from the matching successful allocation.
        unsafe { allocator.dealloc(p, layout) };
        let second = allocator.end(second);
        assert!(second.valid);
        assert_eq!(second.events.allocation_successful_bytes, 0);
        assert_eq!(second.events.deallocated_bytes, 4096);
        assert_eq!(second.live_bytes_begin, 4096);
        assert_eq!(second.live_bytes_end, 0);
        assert_eq!(second.peak_live_requested_bytes, 4096);
    }

    struct Failable {
        fail: AtomicBool,
    }
    // SAFETY: On requested failure return null without touching existing memory;
    // otherwise forward exact arguments to System. Only tests use this backend.
    unsafe impl GlobalAlloc for Failable {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            if self.fail.load(Relaxed) {
                std::ptr::null_mut()
            } else {
                unsafe { System.alloc(layout) }
            }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            if self.fail.load(Relaxed) {
                std::ptr::null_mut()
            } else {
                unsafe { System.alloc_zeroed(layout) }
            }
        }
        unsafe fn realloc(&self, p: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            if self.fail.load(Relaxed) {
                std::ptr::null_mut()
            } else {
                unsafe { System.realloc(p, layout, size) }
            }
        }
        unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
            unsafe { System.dealloc(p, layout) }
        }
    }

    #[test]
    fn failed_requests_do_not_count_as_success_or_destroy_old_realloc_payload() {
        let allocator = Counting::new(Failable {
            fail: AtomicBool::new(false),
        });
        let layout = Layout::from_size_align(32, 8).unwrap();
        let start = allocator.begin();
        // SAFETY: The original block remains live when the controlled realloc
        // returns null; free it with its unchanged original layout exactly once.
        unsafe {
            let p = allocator.alloc(layout);
            assert!(!p.is_null());
            p.write(91);
            allocator.inner.fail.store(true, Relaxed);
            assert!(allocator.alloc(layout).is_null());
            assert!(allocator.alloc_zeroed(layout).is_null());
            assert!(allocator.realloc(p, layout, 128).is_null());
            assert_eq!(p.read(), 91);
            assert_eq!(allocator.live_bytes.load(Relaxed), 32);
            allocator.dealloc(p, layout);
        }
        let sample = allocator.end(start);
        assert!(sample.valid);
        assert_eq!(sample.events.alloc_successes, 1);
        assert_eq!(sample.events.alloc_failures, 1);
        assert_eq!(sample.events.zeroed_failures, 1);
        assert_eq!(sample.events.realloc_failures, 1);
        assert_eq!(sample.events.realloc_successes, 0);
        assert_eq!(sample.events.allocation_requested_bytes, 96);
        assert_eq!(sample.events.allocation_successful_bytes, 32);
        assert_eq!(sample.events.realloc_requested_bytes, 128);
        assert_eq!(sample.events.realloc_successful_old_bytes, 0);
        assert_eq!(sample.events.realloc_successful_new_bytes, 0);
        assert_eq!(sample.peak_live_requested_bytes, 32);
        assert_eq!(sample.live_bytes_end, 0);
    }

    #[test]
    fn overflow_underflow_and_nested_phases_invalidate_without_panicking() {
        let allocator = Counting::new(System);
        allocator.counters.alloc_attempts.store(u64::MAX, Relaxed);
        let start = allocator.begin();
        allocator.add(&allocator.counters.alloc_attempts, 1);
        assert!(!allocator.end(start).valid);
        let allocator = Counting::new(System);
        let start = allocator.begin();
        allocator.replace_size(1, 0);
        assert!(!allocator.end(start).valid);
        let allocator = Counting::new(System);
        let start = allocator.begin();
        let nested = allocator.begin();
        assert!(!allocator.end(nested).valid);
        assert!(!allocator.end(start).valid);
    }
}
