// SPDX-License-Identifier: GPL-2.0

use proc_macro2::{
    Span,
    TokenStream, //
};
use quote::{
    format_ident,
    quote,
    ToTokens, //
};
use syn::{
    parse_quote,
    ExprMethodCall,
    ItemFn, //
};

pub(crate) fn const_eval_only(mut input: ItemFn) -> TokenStream {
    // Prevent code generation as the function is for const evaluation only.
    input.attrs.push(parse_quote!(
        #[inline(always)]
    ));

    input.block.stmts.insert(
        0,
        parse_quote!(
            ::kernel::const_eval::assert_in_const_eval();
        ),
    );

    input.into_token_stream()
}

pub(crate) fn const_call(mut input: ExprMethodCall) -> TokenStream {
    let expr = input.receiver;

    let expr_ident = format_ident!("expr", span = Span::mixed_site());
    input.receiver = parse_quote!(#expr_ident);

    let would_call = quote!(#input);
    input.receiver = parse_quote!(::kernel::const_eval::Const(#expr_ident));

    quote!({
        let #expr_ident = #expr;
        if false {
            ::kernel::const_eval::would_call(#expr_ident, |#expr_ident| #would_call)
        } else {
            #input
        }
    })
}
