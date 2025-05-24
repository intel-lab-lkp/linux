// SPDX-License-Identifier: GPL-2.0

use proc_macro::{Delimiter, Group, Ident, Punct, Spacing, Span, TokenStream, TokenTree};
use std::collections::BTreeSet;

/// Please see [`crate::fmt`] for documentation.
pub(crate) fn fmt(input: TokenStream) -> TokenStream {
    let mut input = input.into_iter();

    let first_opt = input.next();
    let first_owned_str;
    let mut names = BTreeSet::new();
    let first_lit = {
        let Some((mut first_str, first_lit)) = (match first_opt.as_ref() {
            Some(TokenTree::Literal(first_lit)) => {
                first_owned_str = first_lit.to_string();
                Some(first_owned_str.as_str()).and_then(|first| {
                    let first = first.strip_prefix('"')?;
                    let first = first.strip_suffix('"')?;
                    Some((first, first_lit))
                })
            }
            _ => None,
        }) else {
            return first_opt.into_iter().chain(input).collect();
        };
        while let Some((_, rest)) = first_str.split_once('{') {
            first_str = rest;
            if let Some(rest) = first_str.strip_prefix('{') {
                first_str = rest;
                continue;
            }
            while let Some((name, rest)) = first_str.split_once('}') {
                first_str = rest;
                if let Some(rest) = first_str.strip_prefix('}') {
                    first_str = rest;
                    continue;
                }
                let name = name.split_once(':').map_or(name, |(name, _)| name);
                if !name.is_empty() && !name.chars().all(|c| c.is_ascii_digit()) {
                    names.insert(name);
                }
                break;
            }
        }
        first_lit
    };

    let first_span = first_lit.span();
    let adapt = |expr| {
        let mut borrow =
            TokenStream::from_iter([TokenTree::Punct(Punct::new('&', Spacing::Alone))]);
        borrow.extend(expr);
        make_ident(first_span, ["kernel", "fmt", "Adapter"])
            .chain([TokenTree::Group(Group::new(Delimiter::Parenthesis, borrow))])
    };

    let flush = |args: &mut TokenStream, current: &mut TokenStream| {
        let current = std::mem::take(current);
        if !current.is_empty() {
            args.extend(adapt(current));
        }
    };

    let mut args = TokenStream::from_iter(first_opt);
    {
        let mut current = TokenStream::new();
        for tt in input {
            match &tt {
                TokenTree::Punct(p) => match p.as_char() {
                    ',' => {
                        flush(&mut args, &mut current);
                        &mut args
                    }
                    '=' => {
                        names.remove(current.to_string().as_str());
                        args.extend(std::mem::take(&mut current));
                        &mut args
                    }
                    _ => &mut current,
                },
                _ => &mut current,
            }
            .extend([tt]);
        }
        flush(&mut args, &mut current);
    }

    for name in names {
        args.extend(
            [
                TokenTree::Punct(Punct::new(',', Spacing::Alone)),
                TokenTree::Ident(Ident::new(name, first_span)),
                TokenTree::Punct(Punct::new('=', Spacing::Alone)),
            ]
            .into_iter()
            .chain(adapt(TokenTree::Ident(Ident::new(name, first_span)).into())),
        );
    }

    TokenStream::from_iter(make_ident(first_span, ["core", "format_args"]).chain([
        TokenTree::Punct(Punct::new('!', Spacing::Alone)),
        TokenTree::Group(Group::new(Delimiter::Parenthesis, args)),
    ]))
}

fn make_ident<'a, T: IntoIterator<Item = &'a str>>(
    span: Span,
    names: T,
) -> impl Iterator<Item = TokenTree> + use<'a, T> {
    names.into_iter().flat_map(move |name| {
        [
            TokenTree::Punct(Punct::new(':', Spacing::Joint)),
            TokenTree::Punct(Punct::new(':', Spacing::Alone)),
            TokenTree::Ident(Ident::new(name, span)),
        ]
    })
}
