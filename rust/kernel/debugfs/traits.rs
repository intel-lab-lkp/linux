// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2025 Google LLC.

//! Traits for rendering or updating values exported to DebugFS.

use crate::sync::Mutex;
use core::fmt::{self, Debug, Formatter};

/// A trait for types that can be rendered into a string.
///
/// This works very similarly to `Debug`, and is automatically implemented if `Debug` is
/// implemented for a type. It is also implemented for any renderable type inside a `Mutex`.
pub trait Render {
    /// Formats the value using the given formatter.
    fn render(&self, f: &mut Formatter<'_>) -> fmt::Result;
}

impl<T: Render> Render for Mutex<T> {
    fn render(&self, f: &mut Formatter<'_>) -> fmt::Result {
        self.lock().render(f)
    }
}

impl<T: Debug> Render for T {
    fn render(&self, f: &mut Formatter<'_>) -> fmt::Result {
        writeln!(f, "{self:?}")
    }
}
