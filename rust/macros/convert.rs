// SPDX-License-Identifier: GPL-2.0

use proc_macro::{token_stream, Delimiter, Ident, Literal, Span, TokenStream, TokenTree};
use std::iter::Peekable;

pub(crate) fn derive(input: TokenStream) -> TokenStream {
    let mut tokens = input.into_iter().peekable();

    // Extract the representation passed by `#[repr(...)]` if present.
    // If nothing is specified, the default is `Rust` representation,
    // which uses `isize` for the discriminant type.
    // See: https://doc.rust-lang.org/reference/items/enumerations.html#r-items.enum.discriminant.repr-rust
    let repr_ty_ident =
        get_repr(&mut tokens).unwrap_or_else(|| Ident::new("isize", Span::mixed_site()));

    // Skip until the `enum` keyword, including the `enum` itself.
    for tt in tokens.by_ref() {
        if matches!(tt, TokenTree::Ident(ident) if ident.to_string() == "enum") {
            break;
        }
    }

    let Some(TokenTree::Ident(enum_ident)) = tokens.next() else {
        return "::core::compile_error!(\"`#[derive(FromPrimitive)]` can only \
                be applied to an enum\");"
            .parse::<TokenStream>()
            .unwrap();
    };

    let mut errs = TokenStream::new();

    if matches!(tokens.peek(), Some(TokenTree::Punct(p)) if p.as_char() == '<') {
        errs.extend(
            "::core::compile_error!(\"`#[derive(FromPrimitive)]` \
                    does not support enums with generic parameters\");"
                .parse::<TokenStream>()
                .unwrap(),
        );
    }

    let variants_group = tokens
        .find_map(|tt| match tt {
            TokenTree::Group(g) if g.delimiter() == Delimiter::Brace => Some(g),
            _ => None,
        })
        .expect("Missing main body of an enum");

    let zero = Literal::usize_unsuffixed(0);
    let one = Literal::usize_unsuffixed(1);
    let mut const_defs = vec![];
    let mut variant_idents = vec![];
    let mut variant_tokens = variants_group.stream().into_iter().peekable();

    if variant_tokens.peek().is_none() {
        return "::core::compile_error!(\"`#[derive(FromPrimitive)]` does not \
                support zero-variant enums \");"
            .parse::<TokenStream>()
            .unwrap();
    }

    while let Some(tt) = variant_tokens.next() {
        // Skip attributes like `#[...]` if present.
        if matches!(&tt, TokenTree::Punct(p) if p.as_char() == '#') {
            variant_tokens.next();
            continue;
        }

        let TokenTree::Ident(ident) = tt else {
            unreachable!("Missing enum variant identifier");
        };

        // Reject tuple-like or struct-like variants.
        if let Some(TokenTree::Group(g)) = variant_tokens.peek() {
            let variant_kind = match g.delimiter() {
                Delimiter::Brace => "struct-like",
                Delimiter::Parenthesis => "tuple-like",
                _ => unreachable!("Invalid enum variant syntax"),
            };
            errs.extend(
                format!(
                    "::core::compile_error!(\"`#[derive(FromPrimitive)]` does not \
                    support {variant_kind} variant `{enum_ident}::{ident}`; \
                    only unit variants are allowed\");"
                )
                .parse::<TokenStream>()
                .unwrap(),
            );
        }

        let const_expr: TokenStream = match variant_tokens.next() {
            Some(TokenTree::Punct(p)) if p.as_char() == '=' => {
                // Extract the explicit discriminant, which is a constant expression.
                // See: https://doc.rust-lang.org/reference/items/enumerations.html#r-items.enum.discriminant.explicit.intro
                variant_tokens
                    .by_ref()
                    .take_while(|tt| !matches!(&tt, TokenTree::Punct(p) if p.as_char() == ','))
                    .collect()
            }
            _ => {
                // In this case, we have an implicit discriminant.
                // Generate constant expression based on the previous identifier.
                match variant_idents.last() {
                    Some(prev) => quote! { #prev + #one },
                    None => quote! { #zero },
                }
            }
        };

        // These constants, named after each variant identifier, help detect overflows.
        const_defs.push(quote! {
            #[allow(non_upper_case_globals)]
            const #ident: #repr_ty_ident = #const_expr;
        });

        variant_idents.push(ident);
    }

    if !errs.is_empty() {
        return errs;
    }

    // Implement `from_*` methods for these types; other types use default implementations
    // that delegate to `from_i64` or `from_u64`. While `isize`, `i128`, `usize`, `u128`
    // also have default implementations, providing explicit ones avoids relying on
    // `u64::try_from`, which may silently fail (false negative) with `None` if the enum
    // is marked with a wide representation like `#[repr(i128)]`.
    let type_names = ["isize", "i64", "i128", "usize", "u64", "u128"];
    let methods = type_names.into_iter().map(|ty| {
        impl_method(
            &Ident::new(ty, Span::mixed_site()),
            &Ident::new(&format!("from_{ty}"), Span::mixed_site()),
            &variant_idents,
            &const_defs,
        )
    });

    quote! {
        #[automatically_derived]
        impl FromPrimitive for #enum_ident {
            #(#methods)*
        }
    }
}

fn get_repr(tokens: &mut Peekable<token_stream::IntoIter>) -> Option<Ident> {
    const PRIM_REPRS: [&str; 12] = [
        "u8", "u16", "u32", "u64", "u128", "usize", "i8", "i16", "i32", "i64", "i128", "isize",
    ];

    // Scan only the attributes. As soon as we see a token that is
    // not `#`, we know we have consumed all attributes.
    while let TokenTree::Punct(p) = tokens.peek()? {
        if p.as_char() != '#' {
            break;
        }
        tokens.next();

        // The next token should be a `Group` delimited by brackets.
        let TokenTree::Group(attr) = tokens.next()? else {
            break;
        };

        let mut inner = attr.stream().into_iter();

        // Skip attributes other than `repr`.
        if !matches!(inner.next()?, TokenTree::Ident(ident) if ident.to_string() == "repr") {
            continue;
        }

        // Extract arguments passed to `repr`.
        let TokenTree::Group(repr_args) = inner.next()? else {
            break;
        };

        // Look for any specified primitive representation in `#[repr(...)]` args.
        for arg in repr_args.stream() {
            if let TokenTree::Ident(ident) = arg {
                if PRIM_REPRS.contains(&ident.to_string().as_str()) {
                    return Some(ident);
                }
            }
        }
    }

    None
}

fn impl_method(
    ty: &Ident,
    method: &Ident,
    variants: &[Ident],
    const_defs: &[TokenStream],
) -> TokenStream {
    let param = Ident::new("n", Span::mixed_site());

    // Discriminants can only be cast to integers using `as`, which may silently
    // overflow. To avoid this, we use `try_from` on the defined constants instead.
    // A failed conversion indicates an overflow, which means the value doesn't
    // match the intended discriminant, so we fall through to the next clause.
    let clauses = variants.iter().map(|ident| {
        quote! {
            if Ok(#param) == #ty::try_from(#ident) {
                ::core::option::Option::Some(Self::#ident)
            } else
        }
    });

    quote! {
        #[inline]
        fn #method(#param: #ty) -> ::core::option::Option<Self> {
            #(#const_defs)*
            #(#clauses)* {
                ::core::option::Option::None
            }
        }
    }
}
