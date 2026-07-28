// SPDX-License-Identifier: GPL-2.0

use std::collections::BTreeSet;

use proc_macro2::{
    Ident,
    Span,
    TokenStream,
    TokenTree, //
};
use quote::{
    quote_spanned,
    ToTokens, //
};
use syn::{
    Error,
    LitStr,
    Result, //
};

/// Please see [`crate::fmt`] for documentation.
pub(crate) fn fmt(input: TokenStream) -> Result<TokenStream> {
    let mut input = input.into_iter();

    let Some(fmt_tt) = input.next() else {
        return Err(Error::new(
            Span::call_site(),
            "requires at least a format string argument",
        ));
    };

    let fmt: LitStr = syn::parse2(fmt_tt.into())?;
    let fmt_str = fmt.value();
    let fmt_span = fmt.span();

    // Parse `identifier`s from the format string.
    //
    // See https://doc.rust-lang.org/std/fmt/index.html#syntax.
    let mut names = BTreeSet::new();
    let mut fmt_str_rest = fmt_str.as_str();
    while let Some((_, rest)) = fmt_str_rest.split_once('{') {
        fmt_str_rest = rest;
        if let Some(rest) = fmt_str_rest.strip_prefix('{') {
            fmt_str_rest = rest;
            continue;
        }
        if let Some((name, rest)) = fmt_str_rest.split_once('}') {
            fmt_str_rest = rest;
            let name = name.split_once(':').map_or(name, |(name, _)| name);
            if !name.is_empty() && !name.chars().all(|c| c.is_ascii_digit()) {
                names.insert(name);
            }
        }
    }

    let adapter = quote_spanned!(fmt_span => ::kernel::fmt::Adapter);

    let mut args = fmt.to_token_stream();
    {
        let mut flush = |args: &mut TokenStream, current: &mut TokenStream| {
            let current = std::mem::take(current);
            if !current.is_empty() {
                let (lhs, rhs) = (|| {
                    let mut current = current.into_iter();
                    let mut acc = TokenStream::new();
                    while let Some(tt) = current.next() {
                        // Split on `=` only once to handle cases like `a = b = c`.
                        if matches!(&tt, TokenTree::Punct(p) if p.as_char() == '=') {
                            names.remove(acc.to_string().as_str());
                            // Include the `=` itself to keep the handling below uniform.
                            acc.extend([tt]);
                            return (Some(acc), current.collect::<TokenStream>());
                        }
                        acc.extend([tt]);
                    }
                    (None, acc)
                })();
                args.extend(quote_spanned!(fmt_span => #lhs #adapter(&(#rhs))));
            }
        };

        let mut current = TokenStream::new();
        for tt in input {
            match &tt {
                TokenTree::Punct(p) if p.as_char() == ',' => {
                    flush(&mut args, &mut current);
                    &mut args
                }
                _ => &mut current,
            }
            .extend([tt]);
        }
        flush(&mut args, &mut current);
    }

    for name in names {
        let name = Ident::new(name, fmt_span);
        args.extend(quote_spanned!(fmt_span => , #name = #adapter(&#name)));
    }

    Ok(quote_spanned!(fmt_span => ::core::format_args!(#args)))
}
