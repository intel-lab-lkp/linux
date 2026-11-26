// SPDX-License-Identifier: GPL-2.0

//! A fixed-capacity FIFO ring buffer.
//!
//! This module provides [`RingBuffer`], a circular buffer implementation that
//! supports efficient push and pop operations at opposite ends of the buffer.
//!
//! # Examples
//!
//! ```
//! use kernel::alloc::KRingBuffer;
//!
//! let mut buffer = KRingBuffer::new(3, GFP_KERNEL)?;
//!
//! // Values are pushed to the head of the buffer until it is full.
//! buffer.push_head(1)?;
//! buffer.push_head(2)?;
//! buffer.push_head(3)?;
//! assert!(buffer.full());
//! assert_eq!(buffer.push_head(4), Err(ENOSPC));
//!
//! // Values are popped from the tail of the buffer in FIFO order.
//! assert_eq!(buffer.pop_tail(), Some(1));
//! assert_eq!(buffer.pop_tail(), Some(2));
//! assert_eq!(buffer.free_count(), 2);
//!
//! // The freed slots are reused, the buffer wraps around.
//! buffer.push_head(4)?;
//! buffer.push_head(5)?;
//! assert_eq!(buffer.pop_tail(), Some(3));
//! assert_eq!(buffer.pop_tail(), Some(4));
//! assert_eq!(buffer.pop_tail(), Some(5));
//! assert_eq!(buffer.pop_tail(), None);
//! # Ok::<(), Error>(())
//! ```

use super::{
    allocator::{
        KVmalloc,
        Kmalloc,
        Vmalloc, //
    },
    Allocator,
    Flags,
    Vec, //
};
use kernel::prelude::*;

/// A fixed-capacity FIFO ring buffer.
///
/// `RingBuffer` is a circular buffer that allows pushing elements to the head
/// and popping elements from the tail in constant time. The buffer has a fixed
/// capacity specified at construction time and will return an error if a push
/// is attempted when full.
///
/// The backing storage is provided by the allocator `A`. Type aliases are
/// provided for the common choices: [`KRingBuffer`], [`VRingBuffer`] and
/// [`KVRingBuffer`].
///
/// # Invariants
///
/// - `self.head` points at the next vacant slot.
/// - `self.tail` points at the last occupied slot, except if the buffer is
///   empty.
/// - The buffer is empty when `self.head == self.tail`.
/// - The buffer will always have at least one vacant slot, even when full.
pub struct RingBuffer<T, A: Allocator> {
    nodes: Vec<Option<T>, A>,
    head: usize,
    tail: usize,
}

/// A [`RingBuffer`] backed by [`Kmalloc`].
pub type KRingBuffer<T> = RingBuffer<T, Kmalloc>;

/// A [`RingBuffer`] backed by [`Vmalloc`].
pub type VRingBuffer<T> = RingBuffer<T, Vmalloc>;

/// A [`RingBuffer`] backed by [`KVmalloc`].
pub type KVRingBuffer<T> = RingBuffer<T, KVmalloc>;

impl<T, A: Allocator> RingBuffer<T, A> {
    /// Creates a new `RingBuffer` with the specified capacity.
    ///
    /// The buffer will be able to hold exactly `capacity` elements. Memory is
    /// allocated during construction using `flags`.
    ///
    /// # Errors
    ///
    /// Returns an error if memory allocation fails.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::KRingBuffer;
    ///
    /// let buffer = KRingBuffer::<u32>::new(8, GFP_KERNEL)?;
    /// assert_eq!(buffer.free_count(), 8);
    ///
    /// // The memory for a buffer of this capacity cannot be allocated.
    /// assert!(KRingBuffer::<u32>::new(usize::MAX, GFP_KERNEL).is_err());
    /// # Ok::<(), Error>(())
    /// ```
    pub fn new(capacity: usize, flags: Flags) -> Result<Self> {
        let slots = capacity.checked_add(1).ok_or(ENOMEM)?;
        let mut nodes = Vec::with_capacity(slots, flags)?;

        for _ in 0..nodes.capacity() {
            nodes.push_within_capacity(None)?;
        }

        // INVARIANT: `nodes` holds `nodes.capacity() >= slots >= 1` slots, all
        // of which are vacant. Thus:
        // - `head` points at a vacant slot.
        // - `tail` need not point at an occupied slot, since the buffer is
        //   empty.
        // - The buffer is empty and `head == tail`.
        // - The buffer has at least one vacant slot.
        Ok(Self {
            nodes,
            head: 0,
            tail: 0,
        })
    }

    /// Returns [`true`] if the buffer is full.
    ///
    /// When the buffer is full, any call to [`push_head`] will return an error.
    ///
    /// [`push_head`]: Self::push_head
    #[inline]
    pub fn full(&self) -> bool {
        (self.head + 1) % self.nodes.len() == self.tail
    }

    #[inline]
    fn empty(&self) -> bool {
        self.head == self.tail
    }

    /// Returns the number of available slots in the buffer.
    ///
    /// This is the number of elements that can be pushed before the buffer
    /// becomes full.
    #[inline]
    pub fn free_count(&self) -> usize {
        (if self.head >= self.tail {
            self.nodes.len() - (self.head - self.tail)
        } else {
            self.tail - self.head
        } - 1)
    }

    /// Pushes a value to the head of the buffer.
    ///
    /// # Errors
    ///
    /// Returns [`ENOSPC`] if the buffer is full.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::KRingBuffer;
    ///
    /// let mut buffer = KRingBuffer::new(1, GFP_KERNEL)?;
    ///
    /// assert_eq!(buffer.push_head(1), Ok(()));
    ///
    /// // The buffer is full, so the value is rejected.
    /// assert_eq!(buffer.push_head(2), Err(ENOSPC));
    /// # Ok::<(), Error>(())
    /// ```
    #[inline]
    pub fn push_head(&mut self, value: T) -> Result {
        if self.full() {
            return Err(ENOSPC);
        }

        self.nodes[self.head] = Some(value);

        // INVARIANT: The occupied slots are now the ones from `tail` up to and
        // including the slot at `head`, which now holds `value`. The buffer was
        // not full, so the slot after `head` is not `tail` and is thus vacant.
        // After advancing `head` to that slot:
        // - `head` points at the next vacant slot.
        // - `tail` points at the last occupied slot. If the buffer was empty,
        //   that is the slot that now holds `value`.
        // - The buffer is not empty and `head != tail`.
        // - The buffer has at least one vacant slot, the one at `head`.
        self.head = (self.head + 1) % self.nodes.len();

        Ok(())
    }

    /// Pops and returns the value at the tail of the buffer.
    ///
    /// Returns [`None`] if the buffer is empty. Values are returned in FIFO
    /// order, i.e., the oldest value is returned first.
    ///
    /// # Examples
    ///
    /// ```
    /// use kernel::alloc::KRingBuffer;
    ///
    /// let mut buffer = KRingBuffer::new(2, GFP_KERNEL)?;
    /// buffer.push_head(1)?;
    /// buffer.push_head(2)?;
    ///
    /// assert_eq!(buffer.pop_tail(), Some(1));
    /// assert_eq!(buffer.pop_tail(), Some(2));
    ///
    /// // The buffer is empty, so there is no value to return.
    /// assert_eq!(buffer.pop_tail(), None);
    /// # Ok::<(), Error>(())
    /// ```
    #[inline]
    pub fn pop_tail(&mut self) -> Option<T> {
        if self.empty() {
            return None;
        }

        let value = self.nodes[self.tail].take();

        // INVARIANT: The buffer was not empty, so the slot at `tail` was
        // occupied. It is now vacant, and the remaining occupied slots are the
        // ones from the slot after `tail` up to, but not including, the slot at
        // `head`. After advancing `tail` to the slot after it:
        // - `head` points at the next vacant slot, since `head` is unchanged
        //   and no slot became occupied.
        // - `tail` points at the last occupied slot, unless no occupied slots
        //   remain.
        // - No occupied slots remain exactly when `tail` reached `head`, so the
        //   buffer is empty when `head == tail`.
        // - The buffer has at least one vacant slot, the one at `head`.
        self.tail = (self.tail + 1) % self.nodes.len();
        value
    }
}

#[kunit_tests(rust_kernel_ringbuffer)]
mod tests {
    use super::*;

    #[test]
    fn test_new_buffer_is_empty() {
        let buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");
        assert!(!buffer.full());
        assert!(buffer.empty());
    }

    #[test]
    fn test_new_buffer_has_correct_capacity() {
        let capacity = 10;
        let buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");
        assert_eq!(buffer.free_count(), capacity);
    }

    #[test]
    fn test_push_single_element() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");
        assert!(buffer.push_head(42).is_ok());
        assert!(!buffer.empty());
        assert!(!buffer.full());
    }

    #[test]
    fn test_push_and_pop_single_element() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");
        buffer.push_head(42).expect("Failed to push");
        let value = buffer.pop_tail();
        assert_eq!(value, Some(42));
        assert!(buffer.empty());
    }

    #[test]
    fn test_push_and_pop_multiple_elements() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");

        for i in 0..5 {
            buffer.push_head(i).expect("Failed to push");
        }

        for i in 0..5 {
            assert_eq!(buffer.pop_tail(), Some(i));
        }

        assert!(buffer.empty());
    }

    #[test]
    fn test_pop_from_empty_buffer() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");
        assert_eq!(buffer.pop_tail(), None);
    }

    #[test]
    fn test_buffer_full() {
        let capacity = 3;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        for i in 0..capacity {
            assert!(buffer.push_head(i as i32).is_ok());
        }

        assert!(buffer.full());
        assert_eq!(buffer.free_count(), 0);
    }

    #[test]
    fn test_push_to_full_buffer_fails() {
        let capacity = 3;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        for i in 0..capacity {
            buffer.push_head(i as i32).expect("Failed to push");
        }

        assert!(buffer.full());
        assert!(buffer.push_head(999).is_err());
    }

    #[test]
    fn test_free_count_decreases_on_push() {
        let capacity = 5;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        assert_eq!(buffer.free_count(), capacity);

        for i in 0..capacity {
            buffer.push_head(i as i32).expect("Failed to push");
            assert_eq!(buffer.free_count(), capacity - i - 1);
        }
    }

    #[test]
    fn test_free_count_increases_on_pop() {
        let capacity = 5;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        for i in 0..capacity {
            buffer.push_head(i as i32).expect("Failed to push");
        }

        assert_eq!(buffer.free_count(), 0);

        for i in 1..=capacity {
            buffer.pop_tail();
            assert_eq!(buffer.free_count(), i);
        }
    }

    #[test]
    fn test_wrap_around_behavior() {
        let capacity = 3;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        // Fill the buffer.
        for i in 0..capacity {
            buffer.push_head(i as i32).expect("Failed to push");
        }

        // Pop two elements.
        assert_eq!(buffer.pop_tail(), Some(0));
        assert_eq!(buffer.pop_tail(), Some(1));

        // Push two more (should wrap around).
        buffer.push_head(10).expect("Failed to push");
        buffer.push_head(11).expect("Failed to push");

        // Verify order.
        assert_eq!(buffer.pop_tail(), Some(2));
        assert_eq!(buffer.pop_tail(), Some(10));
        assert_eq!(buffer.pop_tail(), Some(11));
        assert!(buffer.empty());
    }

    #[test]
    fn test_fifo_order() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(10, GFP_KERNEL).expect("Failed to create buffer");

        let values = [1, 2, 3, 4, 5];
        for &value in &values {
            buffer.push_head(value).expect("Failed to push");
        }

        for &expected in &values {
            assert_eq!(buffer.pop_tail(), Some(expected));
        }
    }

    #[test]
    fn test_interleaved_push_pop() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(5, GFP_KERNEL).expect("Failed to create buffer");

        buffer.push_head(1).expect("Failed to push");
        buffer.push_head(2).expect("Failed to push");
        assert_eq!(buffer.pop_tail(), Some(1));

        buffer.push_head(3).expect("Failed to push");
        assert_eq!(buffer.pop_tail(), Some(2));
        assert_eq!(buffer.pop_tail(), Some(3));

        assert!(buffer.empty());
    }

    #[test]
    fn test_buffer_state_after_fill_and_drain() {
        let capacity = 4;
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(capacity, GFP_KERNEL).expect("Failed to create buffer");

        // Fill and drain twice.
        for _ in 0..2 {
            for i in 0..capacity {
                buffer.push_head(i as i32).expect("Failed to push");
            }
            assert!(buffer.full());

            for _ in 0..capacity {
                buffer.pop_tail();
            }
            assert!(buffer.empty());
        }
    }

    #[test]
    fn test_single_capacity_buffer() {
        let mut buffer: KRingBuffer<i32> =
            KRingBuffer::new(1, GFP_KERNEL).expect("Failed to create buffer");

        assert_eq!(buffer.free_count(), 1);
        buffer.push_head(42).expect("Failed to push");
        assert!(buffer.full());
        assert_eq!(buffer.free_count(), 0);

        assert_eq!(buffer.pop_tail(), Some(42));
        assert!(buffer.empty());
    }
}
