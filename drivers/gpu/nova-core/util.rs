// SPDX-License-Identifier: GPL-2.0

/// Converts a null-terminated byte array to a string slice.
///
/// Returns "invalid" if the bytes are not valid UTF-8 or not null-terminated.
pub(crate) fn str_from_null_terminated(bytes: &[u8]) -> &str {
    use kernel::str::CStr;

    // Find the first null byte, then create a slice that includes it.
    bytes
        .iter()
        .position(|&b| b == 0)
        .and_then(|null_pos| CStr::from_bytes_with_nul(&bytes[..=null_pos]).ok())
        .and_then(|cstr| cstr.to_str().ok())
        .unwrap_or("invalid")
}
