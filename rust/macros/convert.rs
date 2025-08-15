// SPDX-License-Identifier: GPL-2.0

use proc_macro::{token_stream, Delimiter, Ident, Span, TokenStream, TokenTree};
use std::iter::Peekable;

#[derive(Debug)]
struct TypeArgs {
    helper: Vec<Ident>,
    repr: Option<Ident>,
}

const VALID_TYPES: [&str; 12] = [
    "u8", "u16", "u32", "u64", "u128", "usize", "i8", "i16", "i32", "i64", "i128", "isize",
];

pub(crate) fn derive_try_from(input: TokenStream) -> TokenStream {
    derive(input)
}

fn derive(input: TokenStream) -> TokenStream {
    let derive_target = "TryFrom";
    let derive_helper = "try_from";

    let mut tokens = input.into_iter().peekable();

    let type_args = match parse_types(&mut tokens, derive_helper) {
        Ok(type_args) => type_args,
        Err(errs) => return errs,
    };

    // Skip until the `enum` keyword, including the `enum` itself.
    for tt in tokens.by_ref() {
        if matches!(tt, TokenTree::Ident(ident) if ident.to_string() == "enum") {
            break;
        }
    }

    let Some(TokenTree::Ident(enum_ident)) = tokens.next() else {
        return format!(
            "::core::compile_error!(\"`#[derive({derive_target})]` can only \
            be applied to an enum\");"
        )
        .parse::<TokenStream>()
        .unwrap();
    };

    let mut errs = TokenStream::new();

    if matches!(tokens.peek(), Some(TokenTree::Punct(p)) if p.as_char() == '<') {
        errs.extend(
            format!(
                "::core::compile_error!(\"`#[derive({derive_target})]` \
                does not support enums with generic parameters\");"
            )
            .parse::<TokenStream>()
            .unwrap(),
        );
    }

    let Some(variants_group) = tokens.find_map(|tt| match tt {
        TokenTree::Group(g) if g.delimiter() == Delimiter::Brace => Some(g),
        _ => None,
    }) else {
        unreachable!("Enums have its corresponding body")
    };

    let enum_body_tokens = variants_group.stream().into_iter().peekable();
    let variants = match parse_enum_variants(enum_body_tokens, &enum_ident, derive_target) {
        Ok(variants) => variants,
        Err(new_errs) => {
            errs.extend(new_errs);
            return errs;
        }
    };

    if !errs.is_empty() {
        return errs;
    }

    if type_args.helper.is_empty() {
        // Extract the representation passed by `#[repr(...)]` if present.
        // If nothing is specified, the default is `Rust` representation,
        // which uses `isize` for the discriminant type.
        // See: https://doc.rust-lang.org/reference/items/enumerations.html#r-items.enum.discriminant.repr-rust
        let ty = type_args
            .repr
            .unwrap_or_else(|| Ident::new("isize", Span::mixed_site()));
        impl_try_from(&ty, &enum_ident, &variants)
    } else {
        let impls = type_args
            .helper
            .iter()
            .map(|ty| impl_try_from(ty, &enum_ident, &variants));
        quote! { #(#impls)* }
    }
}

fn parse_types(
    attrs: &mut Peekable<token_stream::IntoIter>,
    helper: &str,
) -> Result<TypeArgs, TokenStream> {
    let mut helper_args = vec![];
    let mut repr_arg = None;

    // Scan only the attributes. As soon as we see a token that is
    // not `#`, we know we have consumed all attributes.
    while let Some(TokenTree::Punct(p)) = attrs.peek() {
        if p.as_char() != '#' {
            unreachable!("Outer attributes start with `#`");
        }
        attrs.next();

        // The next token should be a `Group` delimited by brackets.
        // (e.g., #[try_from(u8, u16)])
        //         ^^^^^^^^^^^^^^^^^^^
        let Some(TokenTree::Group(attr_group)) = attrs.next() else {
            unreachable!("Outer attributes are surrounded by `[` and `]`");
        };

        let mut inner = attr_group.stream().into_iter();

        // Extract the attribute identifier.
        // (e.g., #[try_from(u8, u16)])
        //          ^^^^^^^^
        let attr_name = match inner.next() {
            Some(TokenTree::Ident(ident)) => ident.to_string(),
            _ => unreachable!("Attributes have identifiers"),
        };

        if attr_name == helper {
            match parse_helper_args(inner, helper) {
                Ok(v) => helper_args.extend_from_slice(&v),
                Err(errs) => return Err(errs),
            }
        } else if attr_name == "repr" {
            repr_arg = parse_repr_args(inner);
        }
    }

    Ok(TypeArgs {
        helper: helper_args,
        repr: repr_arg,
    })
}

fn parse_repr_args(mut tt_group: impl Iterator<Item = TokenTree>) -> Option<Ident> {
    // The next token should be a `Group` delimited by parentheses.
    // (e.g., #[repr(C, u8)])
    //              ^^^^^^^
    let Some(TokenTree::Group(args_group)) = tt_group.next() else {
        unreachable!("`repr` attribute has at least one argument")
    };

    for arg in args_group.stream() {
        if let TokenTree::Ident(type_ident) = arg {
            if VALID_TYPES.contains(&type_ident.to_string().as_str()) {
                return Some(type_ident);
            }
        }
    }

    None
}

fn parse_helper_args(
    mut tt_group: impl Iterator<Item = TokenTree>,
    helper: &str,
) -> Result<Vec<Ident>, TokenStream> {
    let mut errs = TokenStream::new();
    let mut args = vec![];

    // The next token should be a `Group`.
    // (e.g., #[try_from(u8, u16)])
    //                  ^^^^^^^^^
    let Some(TokenTree::Group(args_group)) = tt_group.next() else {
        return Err(format!(
            "::core::compile_error!(\"`{helper}` attribute expects at \
            least one integer type argument (e.g., `#[{helper}(u8)]`)\");"
        )
        .parse::<TokenStream>()
        .unwrap());
    };

    let raw_args = args_group.stream();
    if raw_args.is_empty() {
        return Err(format!(
            "::core::compile_error!(\"`{helper}` attribute expects at \
            least one integer type argument (e.g., `#[{helper}(u8)]`)\");"
        )
        .parse::<TokenStream>()
        .unwrap());
    }

    // Iterate over the attribute argument tokens to collect valid integer
    // type identifiers.
    let mut raw_args = raw_args.into_iter();
    while let Some(tt) = raw_args.next() {
        let TokenTree::Ident(type_ident) = tt else {
            errs.extend(
                format!(
                    "::core::compile_error!(\"`{helper}` attribute expects \
                    comma-separated integer type arguments \
                    (e.g., `#[{helper}(u8, u16)]`)\");"
                )
                .parse::<TokenStream>()
                .unwrap(),
            );
            return Err(errs);
        };

        if VALID_TYPES.contains(&type_ident.to_string().as_str()) {
            args.push(type_ident);
        } else {
            errs.extend(
                format!(
                    "::core::compile_error!(\"`{type_ident}` in `{helper}` \
                    attribute is not an integer type\");"
                )
                .parse::<TokenStream>()
                .unwrap(),
            );
        }

        match raw_args.next() {
            Some(TokenTree::Punct(p)) if p.as_char() == ',' => continue,
            None => break,
            Some(_) => {
                errs.extend(
                    format!(
                        "::core::compile_error!(\"`{helper}` attribute expects \
                        comma-separated integer type arguments \
                        (e.g., `#[{helper}(u8, u16)]`)\");"
                    )
                    .parse::<TokenStream>()
                    .unwrap(),
                );
                return Err(errs);
            }
        }
    }

    if !errs.is_empty() {
        return Err(errs);
    }

    Ok(args)
}

fn parse_enum_variants(
    mut tokens: Peekable<token_stream::IntoIter>,
    enum_ident: &Ident,
    derive_target: &str,
) -> Result<Vec<Ident>, TokenStream> {
    let mut errs = TokenStream::new();

    let mut variants = vec![];

    if tokens.peek().is_none() {
        errs.extend(
            format!(
                "::core::compile_error!(\"`#[derive({derive_target})]` \
                does not support zero-variant enums\");"
            )
            .parse::<TokenStream>()
            .unwrap(),
        );
    }

    while let Some(tt) = tokens.next() {
        // Skip attributes like `#[...]` if present.
        if matches!(&tt, TokenTree::Punct(p) if p.as_char() == '#') {
            tokens.next();
            continue;
        }

        let TokenTree::Ident(ident) = tt else {
            unreachable!("Enum variants have its corresponding identifier");
        };

        // Reject tuple-like or struct-like variants.
        if let Some(TokenTree::Group(g)) = tokens.peek() {
            let variant_kind = match g.delimiter() {
                Delimiter::Brace => "struct-like",
                Delimiter::Parenthesis => "tuple-like",
                _ => unreachable!("Invalid enum variant syntax"),
            };
            errs.extend(
                format!(
                    "::core::compile_error!(\"`#[derive({derive_target})]` does not \
                    support {variant_kind} variant `{enum_ident}::{ident}`; \
                    only unit variants are allowed\");"
                )
                .parse::<TokenStream>()
                .unwrap(),
            );
        }

        // Skip through the comma.
        for tt in tokens.by_ref() {
            if matches!(tt, TokenTree::Punct(p) if p.as_char() == ',') {
                break;
            }
        }

        variants.push(ident);
    }

    if !errs.is_empty() {
        return Err(errs);
    }

    Ok(variants)
}

fn impl_try_from(ty: &Ident, enum_ident: &Ident, variants: &[Ident]) -> TokenStream {
    let param = Ident::new("value", Span::mixed_site());

    let clauses = variants.iter().map(|variant| {
        quote! {
            if #param == Self::#variant as #ty {
                ::core::result::Result::Ok(Self::#variant)
            } else
        }
    });

    quote! {
        #[automatically_derived]
        impl ::core::convert::TryFrom<#ty> for #enum_ident {
            type Error = ::kernel::prelude::Error;
            fn try_from(#param: #ty) -> Result<Self, Self::Error> {
                #(#clauses)* {
                    ::core::result::Result::Err(::kernel::prelude::EINVAL)
                }
            }
        }
    }
}
