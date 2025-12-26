// SPDX-License-Identifier: GPL-2.0

use proc_macro2::TokenStream;
use syn::{parse_quote, DeriveInput, Fields, Ident, ItemConst, Path, WhereClause};

fn all_fields_impl(fields: &Fields, trait_: &Path) -> WhereClause {
    let tys = fields.iter().map(|field| &field.ty);
    // The `for<'a>` is a workaround for lacking `#![feature(trivial_bounds)]`
    // It allows us to add conditions which are trivially false to the where clause, which means
    // that we can write `impl`s that the compiler will determine to be irrelevant for us. For
    // example, it allows us to put a `*const c_void: FromBytes` restriction on an automatically
    // generated `FromBytes` implementation for a struct that has `*const c_void` as a member. The
    // implementation will not actually provide `FromBytes` (since its requirements are not met),
    // but it will not fail to compile.
    parse_quote! {
        where #(for<'a> #tys: #trait_),*
    }
}

fn struct_padding_check(fields: &Fields, name: &Ident) -> ItemConst {
    let tys = fields.iter().map(|field| &field.ty);
    parse_quote! {
        const _: () = {
            assert!(#(core::mem::size_of::<#tys>())+* == core::mem::size_of::<#name>());
        };
    }
}

pub(crate) fn as_bytes(input: DeriveInput) -> TokenStream {
    if !input.generics.params.is_empty() {
        return quote::quote! { compile_error!("#[derive(AsBytes)] does not support generics") };
    }
    let syn::Data::Struct(ref ds) = &input.data else {
        return quote::quote! { compile_error!("#[derive(AsBytes)] only supports structs") };
    };
    let name = input.ident;
    let trait_ = parse_quote! { ::kernel::transmute::AsBytes };
    let where_clause = all_fields_impl(&ds.fields, &trait_);
    let padding_check = struct_padding_check(&ds.fields, &name);
    quote::quote! {
        #padding_check
        // SAFETY: #name has no padding and all of its fields implement `AsBytes`
        unsafe impl #trait_ for #name #where_clause {}
    }
}

pub(crate) fn from_bytes(input: DeriveInput) -> TokenStream {
    let syn::Data::Struct(ref ds) = &input.data else {
        return quote::quote! { compile_error!("#[derive(FromBytes)] only supports structs") };
    };
    let (impl_generics, ty_generics, base_where_clause) = input.generics.split_for_impl();
    let name = input.ident;
    let trait_ = parse_quote! { ::kernel::transmute::FromBytes };
    let mut where_clause = all_fields_impl(&ds.fields, &trait_);
    if let Some(base_clause) = base_where_clause {
        where_clause
            .predicates
            .extend(base_clause.predicates.clone())
    };
    quote::quote! {
        // SAFETY: All fields of #name implement `FromBytes` and it is a struct, so there is no
        // implicit discriminator.
        unsafe impl #impl_generics #trait_ for #name #ty_generics #where_clause {}
    }
}
