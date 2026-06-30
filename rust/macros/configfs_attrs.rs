// SPDX-License-Identifier: GPL-2.0

use std::collections::HashSet;

use quote::{
    format_ident,
    quote, //
};

use syn::{
    bracketed,
    ext::IdentExt,
    parse::{
        Parse,
        ParseStream, //
    },
    punctuated::Punctuated,
    spanned::Spanned,
    Error,
    Ident,
    LitInt,
    Token,
    Type, //
};

use crate::helpers::parse_ordered_fields;

pub(crate) struct ConfigfsAttrs {
    container: Type,
    data: Type,
    child: Option<Type>,
    attributes: Vec<(Ident, LitInt)>,
}

fn parse_attribute_field(stream: ParseStream<'_>) -> syn::Result<(Ident, LitInt)> {
    let id = stream.parse::<syn::Ident>()?;
    let _colon = stream.parse::<Token![:]>()?;
    let v = stream.parse::<LitInt>()?;
    Ok((id, v))
}

fn parse_attributes(stream: ParseStream<'_>) -> syn::Result<Vec<(Ident, LitInt)>> {
    let attr_stream;
    let _bracket = bracketed!(attr_stream in stream);
    let attributes = Punctuated::<(Ident, LitInt), Token![,]>::parse_terminated_with(
        &attr_stream,
        parse_attribute_field,
    )?;
    let mut attr_set = HashSet::new();
    for attr in attributes.iter() {
        if !attr_set.insert(attr.0.clone()) {
            return Err(syn::Error::new(attr.0.span(), "duplicate attribute"));
        }
    }

    Ok(attributes.into_iter().collect::<Vec<_>>())
}

impl Parse for ConfigfsAttrs {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        parse_ordered_fields!(
            from input;
            container [required] => (input.parse::<Type>())?,
            data [required] => (input.parse::<Type>())?,
            child => (input.parse::<Type>())?,
            attributes [required] => parse_attributes(input)?,
        );

        Ok(ConfigfsAttrs {
            container,
            data,
            child,
            attributes,
        })
    }
}

pub(crate) fn configfs_attrs(cfs_attrs: ConfigfsAttrs) -> proc_macro2::TokenStream {
    let (container_ty, data_ty) = (&cfs_attrs.container, &cfs_attrs.data);

    let data_tp_ident = Ident::new("DATA_TPE", cfs_attrs.data.span());
    let data_attr_ident = Ident::new("DATA_ATTR_LIST", cfs_attrs.data.span());

    let n = cfs_attrs.attributes.len() + 1;

    let attr_list = quote! {
        static #data_attr_ident: kernel::configfs::AttributeList<#n, #data_ty> =
            // SAFETY: We are expanding `configfs_attrs`.
            unsafe { kernel::configfs::AttributeList::new() };
    };

    let mut attrs = Vec::new();
    for (attr_idx, (name, id)) in cfs_attrs.attributes.iter().enumerate() {
        let name_with_attr = format_ident!(
            "{}_ATTR_{}",
            name.unraw().to_string().to_uppercase(),
            attr_idx
        );

        let id: u64 = match id.base10_parse::<u64>() {
            Ok(v) => v,
            Err(_) => {
                return syn::Error::new(id.span(), "Could not parse attribute ID as a u64")
                    .to_compile_error();
            }
        };

        attrs.push(quote! {
        static #name_with_attr: kernel::configfs::Attribute<#id, #data_ty, #data_ty> =
            // SAFETY: We are expanding `configfs_attrs`.
            unsafe {
              kernel::configfs::Attribute::new(kernel::c_str!(::core::stringify!(#name)))
            };

          // SAFETY: By design of this macro, the name of the variable we
          // invoke the `add` method on below, is not visible outside of
          // the macro expansion. The macro does not operate concurrently
          // on this variable, and thus we have exclusive access to the
          // variable. Calls to `make_group` are serialized on the C side
          // by per subsystem mutex.
          unsafe { #data_attr_ident.add::<#attr_idx, #id, _>(&#name_with_attr) }
        });
    }

    let has_child_code = if let Some(child) = cfs_attrs.child {
        quote! { new_with_child_ctor::<#n, #child>}
    } else {
        quote! { new::<#n> }
    };

    let data_type = quote! {
        {
            static #data_tp_ident:
            kernel::configfs::ItemType<#container_ty, #data_ty> =
                kernel::configfs::ItemType::<#container_ty, #data_ty>::#has_child_code(
                    &THIS_MODULE, &#data_attr_ident
                );
            &#data_tp_ident
        }
    };

    quote! {
        {
            #attr_list
            #(#attrs)*
            #data_type
        }
    }
}
