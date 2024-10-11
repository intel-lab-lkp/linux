// SPDX-License-Identifier: GPL-2.0

use proc_macro::{token_stream, Delimiter, Literal, TokenStream, TokenTree};

fn expect_punct(input: &mut impl Iterator<Item = TokenTree>, expected: char, reason: &str) {
    let Some(TokenTree::Punct(punct)) = input.next() else {
        panic!("expected '{expected}' {reason}");
    };

    if punct.as_char() != expected {
        panic!("expected '{expected}' {reason}");
    }
}

fn expect_ident(input: &mut impl Iterator<Item = TokenTree>, expected: &str, reason: &str) {
    let Some(TokenTree::Ident(ident)) = input.next() else {
        panic!("expected '{expected}' {reason}");
    };

    if ident.to_string() != expected {
        panic!("expected '{expected}' {reason}");
    }
}

fn expect_group(
    input: &mut impl Iterator<Item = TokenTree>,
    expected: Delimiter,
    reason: &str,
) -> token_stream::IntoIter {
    let Some(TokenTree::Group(group)) = input.next() else {
        panic!("expected group {reason}");
    };

    if group.delimiter() != expected {
        panic!("expected group {reason}");
    }

    group.stream().into_iter()
}

fn parse_attribute(input: &mut impl Iterator<Item = TokenTree>) -> (u8, u8) {
    expect_punct(input, '#', "to start attribute");

    let mut stream = expect_group(input, Delimiter::Bracket, "as attribute body");

    expect_ident(&mut stream, "ioctl", "as attribute name");

    let mut inner_stream = expect_group(
        &mut stream,
        Delimiter::Parenthesis,
        "as attribute arguments",
    );

    expect_ident(&mut inner_stream, "code", "as ioctl attribute field");
    expect_punct(&mut inner_stream, '=', "in ioctl attribute field");

    let Some(TokenTree::Literal(lit)) = inner_stream.next() else {
        panic!("expected ioctl attribute code value");
    };

    let lit_str = lit.to_string();
    let code = if lit_str.starts_with("b'") {
        lit_str
            .chars()
            .nth(2)
            .expect("expected ioctl attribute code value") as u8
    } else if let Some(hex) = lit_str.strip_prefix("0x") {
        u8::from_str_radix(hex, 16).expect("expected ioctl attribute code value")
    } else {
        lit_str
            .parse()
            .expect("expected ioctl attribute code value")
    };

    let start_num = if let Some(tree) = inner_stream.next() {
        if !matches!(tree, TokenTree::Punct(punct) if punct.as_char() == ',') {
            panic!("expected ioctl attribute comma");
        }

        expect_ident(&mut inner_stream, "start_num", "as ioctl attribute field");
        expect_punct(&mut inner_stream, '=', "in ioctl attribute field");

        let Some(TokenTree::Literal(lit)) = inner_stream.next() else {
            panic!("expected ioctl attribute start number value");
        };

        lit.to_string()
            .parse()
            .expect("expected ioctl attribute start number value")
    } else {
        0
    };

    assert!(
        inner_stream.next().is_none(),
        "unexpected token in ioctl attribute"
    );
    assert!(
        stream.next().is_none(),
        "unexpected token in ioctl attribute"
    );

    (code, start_num)
}

fn parse_enum_def(input: &mut impl Iterator<Item = TokenTree>) -> TokenTree {
    expect_ident(input, "enum", "to start enum definition");

    let Some(ident @ TokenTree::Ident(_)) = input.next() else {
        panic!("expected enum name");
    };

    ident
}

fn parse_enum_body(
    input: &mut impl Iterator<Item = TokenTree>,
) -> Vec<(TokenTree, Option<TokenTree>)> {
    let mut stream = expect_group(input, Delimiter::Brace, "as enum body").peekable();

    let mut variants = Vec::new();

    while let Some(variant) = stream.next_if(|t| matches!(t, TokenTree::Ident(_))) {
        let arg_type = if let Some(TokenTree::Group(group)) =
            stream.next_if(|t| matches!(t, TokenTree::Group(_)))
        {
            if group.delimiter() != Delimiter::Parenthesis {
                panic!("expected group");
            }

            let mut inner_stream = group.stream().into_iter();

            let arg_type = if let Some(ident @ TokenTree::Ident(_)) = inner_stream.next() {
                ident
            } else {
                panic!("expected argument type")
            };

            assert!(
                inner_stream.next().is_none(),
                "unexpected token in enum variant"
            );

            Some(arg_type)
        } else {
            None
        };

        variants.push((variant, arg_type));

        if stream
            .next_if(|t| matches!(t, TokenTree::Punct(punct) if punct.as_char() == ','))
            .is_none()
        {
            break;
        }
    }

    assert!(stream.next().is_none(), "unexpected token in enum body");

    variants
}

pub(crate) fn derive(input: TokenStream) -> TokenStream {
    let mut input = input.into_iter();

    let (code, start_num) = parse_attribute(&mut input);
    let enum_name = parse_enum_def(&mut input);
    let variants = parse_enum_body(&mut input);

    assert!(input.next().is_none(), "unexpected token in ioctl_cmd");

    let code = TokenTree::from(Literal::u8_suffixed(code));

    let variants = variants
        .into_iter()
        .enumerate()
        .map(|(i, (variant, arg_type))| {
            let i = i as u8 + start_num;

            let i = TokenTree::from(Literal::u8_suffixed(i));

            if let Some(arg_type) = arg_type {
                quote! {
                    @variant(#i, #variant, #arg_type),
                }
            } else {
                quote! {
                    @variant(#i, #variant, None),
                }
            }
        });

    quote! {
        ::kernel::__derive_ioctl_cmd!(
            parse_input:
                @enum_name(#enum_name),
                @code(#code),
                @variants(#(#variants)*)
        );
    }
}
