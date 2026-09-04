// SPDX-License-Identifier: GPL-2.0

use std::collections::HashMap;

use proc_macro2::{Ident, TokenStream};
use quote::quote_spanned;
use syn::{
    ext::IdentExt,
    parse::{Parse, ParseStream},
    parse_quote, Expr, LitStr, Result, Token,
};

pub(crate) struct FormatArgs {
    format_string: LitStr,
    positional_args: Vec<Expr>,
    named_args: HashMap<Ident, Expr>,
}

impl Parse for FormatArgs {
    fn parse(input: ParseStream<'_>) -> Result<Self> {
        let format_string: LitStr = input.parse()?;

        let mut args = FormatArgs {
            format_string,
            positional_args: Vec::new(),
            named_args: HashMap::new(),
        };

        if input.is_empty() {
            return Ok(args);
        }
        input.parse::<Token![,]>()?;

        while !input.is_empty() && !input.peek2(Token![=]) {
            args.positional_args.push(input.parse()?);
            if input.is_empty() {
                return Ok(args);
            }
            input.parse::<Token![,]>()?;
        }

        while !input.is_empty() {
            let name: Ident = input.call(Ident::parse_any)?;
            input.parse::<Token![=]>()?;
            let value: Expr = input.parse()?;
            args.named_args.insert(name, value);

            if input.is_empty() {
                return Ok(args);
            }
            input.parse::<Token![,]>()?;
        }

        return Ok(args);
    }
}

/// Please see [`crate::fmt`] for documentation.
pub(crate) fn fmt(args: FormatArgs) -> Result<TokenStream> {
    let FormatArgs {
        format_string,
        positional_args,
        mut named_args,
    } = args;

    let span = format_string.span();

    // Add inline parameters as named arguments, so they are adapted appropriately
    // Input: fmt!("{name}")
    // Output: fmt!("{name}", name = ::kernel::fmt::Adapter(&(name)))
    {
        let format_string = format_string.value();
        let mut format_string = format_string.as_str();
        while let Some((_, rest)) = format_string.split_once('{') {
            format_string = rest;

            if let Some(rest) = format_string.strip_prefix('{') {
                format_string = rest;
                continue;
            }

            if let Some((name, rest)) = format_string.split_once('}') {
                format_string = rest;
                let name = name.split_once(':').map_or(name, |(name, _)| name);
                if !name.is_empty() && !name.chars().all(|c| c.is_ascii_digit()) {
                    let ident = Ident::new(name, span);
                    let expr = parse_quote!(#ident);
                    named_args.entry(ident).or_insert(expr);
                }
            }
        }
    }

    // Wrap positional and named arguments with `kernel::fmt::Adapter`
    let adapter = quote_spanned!(span => ::kernel::fmt::Adapter);
    let positional_args = positional_args
        .into_iter()
        .map(|value| quote_spanned!(span => #adapter(&(#value))));
    let named_args = named_args
        .into_iter()
        .map(|(name, value)| quote_spanned!(span => #name = #adapter(&(#value))));

    let args = [quote_spanned!(span => #format_string)]
        .into_iter()
        .chain(positional_args)
        .chain(named_args);

    Ok(quote_spanned!(span => ::core::format_args!(#(#args),*)))
}
