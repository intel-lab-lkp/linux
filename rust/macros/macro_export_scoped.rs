// SPDX-License-Identifier: GPL-2.0

use std::hash::{
    DefaultHasher,
    Hash,
    Hasher, //
};

use proc_macro2::TokenStream;
use quote::{
    format_ident,
    quote, //
};
use syn::{
    Error,
    ItemMacro,
    Result, //
};

pub(crate) fn macro_export_scoped(mut input: ItemMacro) -> Result<TokenStream> {
    if !input.mac.path.is_ident("macro_rules") {
        return Err(Error::new_spanned(
            input,
            "#[macro_export_scoped] can only be used on `macro_rules!`",
        ));
    }

    let Some(name) = input.ident else {
        Err(Error::new_spanned(
            input,
            "`macro_rules!` definition missing an identifier",
        ))?
    };

    // Hash together file name and macro name to create a hash that is likely to be unique. Use
    // `DefaultHasher::new` which is free from RNG so the build is still reproducible.
    let mut hasher = DefaultHasher::new();
    crate::helpers::file().hash(&mut hasher);
    name.hash(&mut hasher);
    let hash = hasher.finish();

    let unique_name = format_ident!("macro_{name}_{hash:x}");
    input.ident = Some(unique_name.clone());

    Ok(quote!(
        #[doc(hidden)]
        #[macro_export]
        #input

        #[doc(inline)]
        pub use #unique_name as #name;
    ))
}
