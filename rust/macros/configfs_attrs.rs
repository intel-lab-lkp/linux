// SPDX-License-Identifier: GPL-2.0

use quote::{
    quote, //
    ToTokens,
};

use proc_macro2::{
    Span, //
};

use syn::{
    bracketed,
    parse::{
        Parse,
        ParseStream, //
    },
    punctuated::Punctuated,
    spanned::Spanned,
    Ident,
    LitInt,
    Token,
    Type, //
};

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

fn parse_named_field(stream: ParseStream<'_>, name: &str) -> syn::Result<Type> {
    let try_stream = stream.fork();
    let name_id = try_stream.parse::<Ident>()?;
    if name_id != name {
        return Err(parse_field_error(name_id.span(), &name_id, name));
    }
    let _name_id = stream.parse::<Ident>()?;
    let _colon = stream.parse::<Token![:]>()?;
    let v = stream.parse::<Type>()?;
    stream.parse::<Token![,]>()?;
    Ok(v)
}

fn parse_attributes(stream: ParseStream<'_>) -> syn::Result<Vec<(Ident, LitInt)>> {
    let attribute_id = stream.parse::<Ident>()?;
    if attribute_id != "attributes" {
        return Err(syn::Error::new(
            attribute_id.span(),
            format!(
                "unexpected identifier: {}, expected: 'attributes'",
                attribute_id
            ),
        ));
    }
    stream.parse::<Token![:]>()?;
    let attr_stream;
    let _bracket = bracketed!(attr_stream in stream);
    let attributes = Punctuated::<(Ident, LitInt), Token![,]>::parse_terminated_with(
        &attr_stream,
        parse_attribute_field,
    )?;
    let attributes = attributes.into_iter().collect::<Vec<_>>();

    stream.parse::<Option<Token![,]>>()?;
    Ok(attributes)
}

fn parse_field_error(span: Span, name: &Ident, expected_name: &str) -> syn::Error {
    syn::Error::new(
        span,
        format!("Unexpected field name, got: {name} expected: {expected_name}"),
    )
}

impl Parse for ConfigfsAttrs {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        let container = parse_named_field(input, "container")?;
        let data = parse_named_field(input, "data")?;
        let child = parse_named_field(input, "child").ok();
        let attributes = parse_attributes(input)?;

        Ok(ConfigfsAttrs {
            container,
            data,
            child,
            attributes,
        })
    }
}

fn make_static_ident<T: ToTokens>(ty: &T, suffix: &str) -> syn::Ident {
    let raw_id = quote! { #ty }.to_string();

    // Sanitizing syn::Type::Path, this is safe since it is
    // only used as the identifier.
    let normalized = raw_id
        .split("::")
        .map(|s| String::from(s.trim()))
        .reduce(|a, b| format!("{a}_{b}"))
        .expect("Cannot be empty")
        .to_uppercase()
        .replace(|c: char| !c.is_alphanumeric(), "_");

    Ident::new(&format!("{}_{}", normalized, suffix), ty.span())
}

pub(crate) fn configfs_attrs(cfs_attrs: ConfigfsAttrs) -> proc_macro2::TokenStream {
    let (container_ty, data_ty) = (&cfs_attrs.container, &cfs_attrs.data);

    let data_tp_ident = make_static_ident(data_ty, "TPE");
    let data_attr_ident = make_static_ident(data_ty, "ATTR_LIST");

    let n = cfs_attrs.attributes.len() + 1;

    let attr_list = quote! {
        static #data_attr_ident: kernel::configfs::AttributeList<#n, #data_ty> =
            // SAFETY: We are expanding `configfs_attrs`.
            unsafe { kernel::configfs::AttributeList::new() };
    };

    let mut attrs = Vec::new();
    for (attr_idx, (name, id)) in cfs_attrs.attributes.iter().enumerate() {
        let name_with_attr = make_static_ident(name, "ATTR");

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
          // variable.
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
