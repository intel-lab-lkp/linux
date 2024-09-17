// SPDX-License-Identifier: GPL-2.0

use super::{pin_init, tbox::BoxTimerHandle, Timer, TimerCallback, TimerPointer, TimerRestart};
use crate::{
    alloc::{flags, Flags},
    impl_has_timer, new_mutex,
    prelude::*,
    sync::Mutex,
    time::Ktime,
};
use macros::pin_data;

#[pin_data]
pub struct ClosureTimer<T> {
    #[pin]
    timer: Timer<ClosureTimer<T>>,
    #[pin]
    callback: Mutex<Option<T>>,
}

impl_has_timer! {
    impl{T} HasTimer<Self> for ClosureTimer<T> { self.timer }
}

impl<T> TimerCallback for ClosureTimer<T>
where
    T: FnOnce() + 'static,
{
    type CallbackTarget<'a> = Pin<Box<ClosureTimer<T>>>;
    type CallbackPointer<'a> = &'a ClosureTimer<T>;

    fn run(this: Self::CallbackPointer<'_>) -> TimerRestart
    where
        Self: Sized,
    {
        if let Some(callback) = this.callback.lock().take() {
            callback();
        }
        TimerRestart::NoRestart
    }
}

impl<T> ClosureTimer<T>
where
    T: FnOnce() + 'static,
    T: Send,
    T: Sync,
{
    fn new(f: T, flags: Flags) -> Result<Pin<Box<Self>>> {
        Box::pin_init(
            pin_init!(
                Self {
                    timer <- Timer::new(),
                    callback <- new_mutex!(Some(f)),
                }
            ),
            flags,
        )
    }
}

/// Schedule `f` for execution after `expires` time.
pub fn schedule_function<T>(expires: Ktime, f: T) -> Result<BoxTimerHandle<ClosureTimer<T>>>
where
    T: FnOnce() + 'static,
    T: Send,
    T: Sync,
{
    let timer = ClosureTimer::<T>::new(f, flags::GFP_KERNEL)?;
    let handle = <Pin<Box<ClosureTimer<T>>> as TimerPointer>::schedule(timer, expires);
    Ok(handle)
}
