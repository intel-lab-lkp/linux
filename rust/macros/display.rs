// SPDX-License-Identifier: GPL-2.0

//! Derive macro for `Display` on enums.
//!
//! This module provides a derive macro that implements `kernel::fmt::Display`
//! for enums, outputting the exact variant name as written.

use proc_macro::TokenStream;

pub(crate) fn derive_display(input: TokenStream) -> TokenStream {
    let input: syn::DeriveInput = syn::parse(input).expect("failed to parse input");

    let data = match &input.data {
        syn::Data::Enum(data) => data,
        syn::Data::Struct(_) => {
            panic!("derive(Display) only supports enums, not structs");
        }
        syn::Data::Union(_) => {
            panic!("derive(Display) only supports enums, not unions");
        }
    };

    // Generate match arms for each variant.
    let match_arms = data.variants.iter().map(|variant| {
        let variant_ident = &variant.ident;
        let variant_name = variant_ident.to_string();

        // Handle different variant types: unit, tuple, and struct.
        let pattern = match &variant.fields {
            syn::Fields::Unit => quote::quote! { Self::#variant_ident },
            syn::Fields::Unnamed(_) => quote::quote! { Self::#variant_ident(..) },
            syn::Fields::Named(_) => quote::quote! { Self::#variant_ident { .. } },
        };

        quote::quote! {
            #pattern => f.write_str(#variant_name)
        }
    });

    let name = &input.ident;
    let expanded = quote::quote! {
        impl ::kernel::fmt::Display for #name {
            fn fmt(&self, f: &mut ::kernel::fmt::Formatter<'_>) -> ::kernel::fmt::Result {
                match self {
                    #(#match_arms),*
                }
            }
        }
    };

    expanded.into()
}
