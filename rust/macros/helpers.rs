// SPDX-License-Identifier: GPL-2.0

use proc_macro2::TokenStream;
use quote::ToTokens;
use syn::{
    parse::{
        Parse,
        ParseStream, //
    },
    Attribute,
    Error,
    LitStr,
    Result, //
};

/// A string literal that is required to have ASCII value only.
pub(crate) struct AsciiLitStr(LitStr);

impl Parse for AsciiLitStr {
    fn parse(input: ParseStream<'_>) -> Result<Self> {
        let s: LitStr = input.parse()?;
        if !s.value().is_ascii() {
            return Err(Error::new_spanned(s, "expected ASCII-only string literal"));
        }
        Ok(Self(s))
    }
}

impl ToTokens for AsciiLitStr {
    fn to_tokens(&self, ts: &mut TokenStream) {
        self.0.to_tokens(ts);
    }
}

impl AsciiLitStr {
    pub(crate) fn value(&self) -> String {
        self.0.value()
    }
}

pub(crate) fn file() -> String {
    #[cfg(not(CONFIG_RUSTC_HAS_SPAN_FILE))]
    {
        proc_macro::Span::call_site()
            .source_file()
            .path()
            .to_string_lossy()
            .into_owned()
    }

    #[cfg(CONFIG_RUSTC_HAS_SPAN_FILE)]
    {
        proc_macro::Span::call_site().file()
    }
}

/// Obtain all `#[cfg]` attributes.
pub(crate) fn gather_cfg_attrs(attr: &[Attribute]) -> impl Iterator<Item = &Attribute> + '_ {
    attr.iter().filter(|a| a.path().is_ident("cfg"))
}

/// Parse fields that are required to use a specific order.
///
/// As fields must follow a specific order, we *could* just parse fields one by one by peeking.
/// However the error message generated when implementing that way is not very friendly.
///
/// So instead we parse fields in an arbitrary order, but only enforce the ordering after parsing,
/// and if the wrong order is used, the proper order is communicated to the user with error message.
///
/// Usage looks like this:
/// ```ignore
/// parse_ordered_fields! {
///     from input;
///
///     // This will extract `foo: <field>` into a variable named `foo`.
///     // The variable will have type `Option<_>`.
///     foo => <expression that parses the field>,
///
///     // If you need the variable name to be different than the key name.
///     // This extracts `baz: <field>` into a variable named `bar`.
///     // You might want this if `baz` is a keyword.
///     baz as bar => <expression that parse the field>,
///
///     // You can mark a key as required, and the variable will no longer be `Option`.
///     // foobar will be of type `Expr` instead of `Option<Expr>`.
///     foobar [required] => input.parse::<Expr>()?,
/// }
/// ```
macro_rules! parse_ordered_fields {
    (@gen
        [$input:expr]
        [$([$name:ident; $key:ident; $parser:expr])*]
        [$([$req_name:ident; $req_key:ident])*]
    ) => {
        $(let mut $name = None;)*

        const EXPECTED_KEYS: &[&str] = &[$(stringify!($key),)*];
        const REQUIRED_KEYS: &[&str] = &[$(stringify!($req_key),)*];

        let span = $input.span();
        let mut seen_keys = Vec::new();

        while !$input.is_empty() {
            let key = $input.call(Ident::parse_any)?;

            if seen_keys.contains(&key) {
                Err(Error::new_spanned(
                    &key,
                    format!(r#"duplicated key "{key}". Keys can only be specified once."#),
                ))?
            }

            $input.parse::<Token![:]>()?;

            match &*key.to_string() {
                $(
                    stringify!($key) => $name = Some($parser),
                )*
                _ => {
                    Err(Error::new_spanned(
                        &key,
                        format!(r#"unknown key "{key}". Valid keys are: {EXPECTED_KEYS:?}."#),
                    ))?
                }
            }

            $input.parse::<Token![,]>()?;
            seen_keys.push(key);
        }

        for key in REQUIRED_KEYS {
            if !seen_keys.iter().any(|e| e == key) {
                Err(Error::new(span, format!(r#"missing required key "{key}""#)))?
            }
        }

        let mut ordered_keys: Vec<&str> = Vec::new();
        for key in EXPECTED_KEYS {
            if seen_keys.iter().any(|e| e == key) {
                ordered_keys.push(key);
            }
        }

        if seen_keys != ordered_keys {
            Err(Error::new(
                span,
                format!(r#"keys are not ordered as expected. Order them like: {ordered_keys:?}."#),
            ))?
        }

        $(let $req_name = $req_name.expect("required field");)*
    };

    // Handle required fields.
    (@gen
        [$input:expr] [$($tok:tt)*] [$($req:tt)*]
        $key:ident as $name:ident [required] => $parser:expr,
        $($rest:tt)*
    ) => {
        parse_ordered_fields!(
            @gen [$input] [$($tok)* [$name; $key; $parser]] [$($req)* [$name; $key]] $($rest)*
        )
    };
    (@gen
        [$input:expr] [$($tok:tt)*] [$($req:tt)*]
        $name:ident [required] => $parser:expr,
        $($rest:tt)*
    ) => {
        parse_ordered_fields!(
            @gen [$input] [$($tok)* [$name; $name; $parser]] [$($req)* [$name; $name]] $($rest)*
        )
    };

    // Handle optional fields.
    (@gen
        [$input:expr] [$($tok:tt)*] [$($req:tt)*]
        $key:ident as $name:ident => $parser:expr,
        $($rest:tt)*
    ) => {
        parse_ordered_fields!(
            @gen [$input] [$($tok)* [$name; $key; $parser]] [$($req)*] $($rest)*
        )
    };
    (@gen
        [$input:expr] [$($tok:tt)*] [$($req:tt)*]
        $name:ident => $parser:expr,
        $($rest:tt)*
    ) => {
        parse_ordered_fields!(
            @gen [$input] [$($tok)* [$name; $name; $parser]] [$($req)*] $($rest)*
        )
    };

    (from $input:expr; $($tok:tt)*) => {
        parse_ordered_fields!(@gen [$input] [] [] $($tok)*)
    }
}

pub(crate) use parse_ordered_fields;
