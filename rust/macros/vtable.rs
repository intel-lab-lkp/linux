// SPDX-License-Identifier: GPL-2.0

use std::collections::HashSet;

use proc_macro2::{Ident, TokenStream};
use quote::ToTokens;
use syn::{parse_quote, Error, ImplItem, Item, ItemImpl, ItemTrait, Result, TraitItem};

fn handle_trait(mut item: ItemTrait) -> Result<ItemTrait> {
    let mut functions = Vec::new();
    let mut consts = HashSet::new();
    for item in &item.items {
        match item {
            TraitItem::Fn(fn_item) => {
                functions.push(fn_item.sig.ident.clone());
            }
            TraitItem::Const(const_item) => {
                consts.insert(const_item.ident.clone());
            }
            _ => {}
        }
    }

    item.items.push(parse_quote! {
         /// A marker to prevent implementors from forgetting to use [`#[vtable]`](vtable)
         /// attribute when implementing this trait.
         const USE_VTABLE_ATTR: ();
    });

    for name in functions {
        let gen_const_name = Ident::new(
            &format!("HAS_{}", name.to_string().to_uppercase()),
            name.span(),
        );
        // Skip if it's declared already -- this allows user override.
        if consts.contains(&gen_const_name) {
            continue;
        }
        // We don't know on the implementation-site whether a method is required or provided
        // so we have to generate a const for all methods.
        let comment = format!("Indicates if the `{name}` method is overridden by the implementor.");
        item.items.push(parse_quote! {
            #[doc = #comment]
            const #gen_const_name: bool = false;
        });
        consts.insert(gen_const_name);
    }

    Ok(item)
}

fn handle_impl(mut item: ItemImpl) -> Result<ItemImpl> {
    let mut functions = Vec::new();
    let mut consts = HashSet::new();
    for item in &item.items {
        match item {
            ImplItem::Fn(fn_item) => {
                functions.push(fn_item.sig.ident.clone());
            }
            ImplItem::Const(const_item) => {
                consts.insert(const_item.ident.clone());
            }
            _ => {}
        }
    }

    item.items.push(parse_quote! {
         const USE_VTABLE_ATTR: () = ();
    });

    for name in functions {
        let gen_const_name = Ident::new(
            &format!("HAS_{}", name.to_string().to_uppercase()),
            name.span(),
        );
        // Skip if it's declared already -- this allows user override.
        if consts.contains(&gen_const_name) {
            continue;
        }
        item.items.push(parse_quote! {
            const #gen_const_name: bool = true;
        });
        consts.insert(gen_const_name);
    }

    Ok(item)
}

pub(crate) fn vtable(input: Item) -> Result<TokenStream> {
    match input {
        Item::Trait(item) => Ok(handle_trait(item)?.into_token_stream()),
        Item::Impl(item) => Ok(handle_impl(item)?.into_token_stream()),
        _ => Err(Error::new_spanned(
            input,
            "`#[vtable]` attribute should only be applied to trait or impl block",
        ))?,
    }
}
