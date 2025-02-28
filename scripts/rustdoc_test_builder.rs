// SPDX-License-Identifier: GPL-2.0

//! Test builder for `rustdoc`-generated tests.
//!
//! This script is a hack to extract the test from `rustdoc`'s output. Ideally, `rustdoc` would
//! have an option to generate this information instead, e.g. as JSON output.
//!
//! The `rustdoc`-generated test names look like `{file}_{line}_{number}`, e.g.
//! `...path_rust_kernel_sync_arc_rs_42_0`. `number` is the "test number", needed in cases like
//! a macro that expands into items with doctests is invoked several times within the same line.
//!
//! However, since these names are used for bisection in CI, the line number makes it not stable
//! at all. In the future, we would like `rustdoc` to give us the Rust item path associated with
//! the test, plus a "test number" (for cases with several examples per item) and generate a name
//! from that. For the moment, we generate ourselves a new name, `{file}_{number}` instead, in
//! the `gen` script (done there since we need to be aware of all the tests in a given file).

use std::fs::create_dir_all;
use std::io::Read;

use json::JsonValue;

mod json;

fn generate_doctest(file: &str, line: i32, doctest_code: &str) {
    let file = file
        .strip_suffix(".rs")
        .unwrap_or(file)
        .strip_prefix("../rust/kernel/")
        .unwrap_or(file)
        .replace('/', "_");
    let path = format!("rust/test/doctests/kernel/{file}-{line}.rs");

    // We replace the `Result` if needed.
    let doctest_code = doctest_code.replace(
        "fn main() { fn _inner() -> Result<",
        "fn main() { fn _inner() -> core::result::Result<",
    );
    // For tests that get generated with `Result`, like above, `rustdoc` generates an `unwrap()` on
    // the return value to check there were no returned errors. Instead, we use our assert macro
    // since we want to just fail the test, not panic the kernel.
    //
    // We save the result in a variable so that the failed assertion message looks nicer.
    let doctest_code = doctest_code.replace(
        "} _inner().unwrap() }",
        "} let test_return_value = _inner(); assert!(test_return_value.is_ok()); }",
    );
    std::fs::write(path, doctest_code.as_bytes()).unwrap();
}

fn main() {
    let mut stdin = std::io::stdin().lock();
    let mut body = String::new();
    stdin.read_to_string(&mut body).unwrap();

    let JsonValue::Object(rustdoc) = JsonValue::parse(&body).unwrap() else {
        panic!("Expected an object")
    };
    if let Some(JsonValue::Number(format_version)) = rustdoc.get("format_version") {
        if *format_version != 1 {
            panic!("unsupported rustdoc format version: {format_version}");
        }
    } else {
        panic!("missing `format_version` field");
    }
    let Some(JsonValue::Array(doctests)) = rustdoc.get("doctests") else {
        panic!("`doctests` field is missing or has the wrong type");
    };

    // We ignore the error since it will fail when generating doctests below if the folder doesn't
    // exist.
    let _ = create_dir_all("rust/test/doctests/kernel");

    let mut nb_generated = 0;
    for doctest in doctests {
        let JsonValue::Object(doctest) = doctest else {
            unreachable!()
        };

        // We check if we need to skip this test by checking it's a rust code and it's not ignored.
        if let Some(JsonValue::Object(attributes)) = doctest.get("doctest_attributes") {
            if attributes.get("rust") != Some(&JsonValue::Bool(true)) {
                continue;
            } else if let Some(JsonValue::String(ignore)) = attributes.get("ignore") {
                if ignore != "None" {
                    continue;
                }
            }
        }
        if let (
            Some(JsonValue::String(file)),
            Some(JsonValue::Number(line)),
            Some(JsonValue::String(doctest_code)),
        ) = (
            doctest.get("file"),
            doctest.get("line"),
            doctest.get("doctest_code"),
        ) {
            generate_doctest(file, *line, doctest_code);
            nb_generated += 1;
        }
    }

    if nb_generated == 0 {
        panic!("No test function found in `rustdoc`'s output.");
    }
}
