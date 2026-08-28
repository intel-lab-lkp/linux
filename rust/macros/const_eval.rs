// SPDX-License-Identifier: GPL-2.0

use proc_macro2::TokenStream;
use quote::ToTokens;
use syn::{
    parse_quote,
    ItemFn, //
};

pub(crate) fn const_eval(mut input: ItemFn) -> TokenStream {
    // Prevent code generation as the function is for const evaluation only.
    input.attrs.push(parse_quote!(
        #[inline(always)]
    ));

    input.block.stmts.insert(
        0,
        parse_quote!(
            ::kernel::build_assert::assert_in_const_eval();
        ),
    );

    input.into_token_stream()
}
