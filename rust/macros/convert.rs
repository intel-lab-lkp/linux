// SPDX-License-Identifier: GPL-2.0

use proc_macro2::{
    Span,
    TokenStream, //
};

use std::fmt;

use syn::{
    parse::{
        Parse,
        ParseStream, //
    },
    parse_quote,
    parse_str,
    punctuated::Punctuated,
    spanned::Spanned,
    Attribute,
    Data,
    DeriveInput,
    Expr,
    Fields,
    Ident,
    LitInt,
    Token, //
};

pub(crate) fn derive_into(input: DeriveInput) -> syn::Result<TokenStream> {
    derive(DeriveTarget::Into, input)
}

pub(crate) fn derive_try_from(input: DeriveInput) -> syn::Result<TokenStream> {
    derive(DeriveTarget::TryFrom, input)
}

fn derive(target: DeriveTarget, input: DeriveInput) -> syn::Result<TokenStream> {
    let mut errors: Option<syn::Error> = None;
    let mut combine_error = |err| match errors.as_mut() {
        Some(errors) => errors.combine(err),
        None => errors = Some(err),
    };

    let (helper_tys, repr_ty) = parse_attrs(target, &input.attrs)?;
    for ty in &helper_tys {
        if let Err(err) = ty.validate() {
            combine_error(err);
        }
    }

    let data_enum = match input.data {
        Data::Enum(data) => data,
        Data::Struct(data) => {
            let msg = format!(
                "expected `enum`, found `struct`; \
                 `#[derive({})]` can only be applied to a unit-only enum",
                target.get_trait_name()
            );
            return Err(syn::Error::new(data.struct_token.span(), msg));
        }
        Data::Union(data) => {
            let msg = format!(
                "expected `enum`, found `union`; \
                 `#[derive({})]` can only be applied to a unit-only enum",
                target.get_trait_name()
            );
            return Err(syn::Error::new(data.union_token.span(), msg));
        }
    };

    for variant in &data_enum.variants {
        match &variant.fields {
            Fields::Named(fields) => {
                let msg = format!(
                    "expected unit-like variant, found struct-like variant; \
                    `#[derive({})]` can only be applied to a unit-only enum",
                    target.get_trait_name()
                );
                combine_error(syn::Error::new_spanned(fields, msg));
            }
            Fields::Unnamed(fields) => {
                let msg = format!(
                    "expected unit-like variant, found tuple-like variant; \
                    `#[derive({})]` can only be applied to a unit-only enum",
                    target.get_trait_name()
                );
                combine_error(syn::Error::new_spanned(fields, msg));
            }
            _ => (),
        }
    }

    if let Some(errors) = errors {
        return Err(errors);
    }

    let variants: Vec<_> = data_enum
        .variants
        .into_iter()
        .map(|variant| variant.ident)
        .collect();

    Ok(derive_for_enum(
        target,
        &input.ident,
        &variants,
        &helper_tys,
        &repr_ty,
    ))
}

#[derive(Clone, Copy, Debug)]
enum DeriveTarget {
    Into,
    TryFrom,
}

impl DeriveTarget {
    fn get_trait_name(&self) -> &'static str {
        match self {
            Self::Into => "Into",
            Self::TryFrom => "TryFrom",
        }
    }

    fn get_helper_name(&self) -> &'static str {
        match self {
            Self::Into => "into",
            Self::TryFrom => "try_from",
        }
    }
}

fn parse_attrs(target: DeriveTarget, attrs: &[Attribute]) -> syn::Result<(Vec<ValidTy>, Ident)> {
    let helper = target.get_helper_name();

    let mut repr_ty = None;
    let mut helper_tys = Vec::new();
    for attr in attrs {
        if attr.path().is_ident("repr") {
            attr.parse_nested_meta(|meta| {
                let ident = meta.path.get_ident();
                if ident.is_some_and(is_valid_primitive) {
                    repr_ty = ident.cloned();
                }
                // Delegate `repr` attribute validation to rustc.
                Ok(())
            })?;
        } else if attr.path().is_ident(helper) && helper_tys.is_empty() {
            let args = attr.parse_args_with(Punctuated::<ValidTy, Token![,]>::parse_terminated)?;
            helper_tys.extend(args);
        }
    }

    // Note on field-less `repr(C)` enums (quote from [1]):
    //
    //   In C, enums with discriminants that do not all fit into an `int` or all fit into an
    //   `unsigned int` are a portability hazard: such enums are only permitted since C23, and not
    //   supported e.g. by MSVC.
    //
    //   Furthermore, Rust interprets the discriminant values of `repr(C)` enums as expressions of
    //   type `isize`. This makes it impossible to implement the C23 behavior of enums where the
    //   enum discriminants have no predefined type and instead the enum uses a type large enough
    //   to hold all discriminants.
    //
    //   Therefore, `repr(C)` enums in Rust require that either all discriminants to fit into a C
    //   `int` or they all fit into an `unsigned int`.
    //
    // As such, `isize` is a reasonable representation for `repr(C)` enums, as it covers the range
    //  of both `int` and `unsigned int`.
    //
    // For more information, see:
    // - https://github.com/rust-lang/rust/issues/124403
    // - https://github.com/rust-lang/rust/pull/147017
    // - https://github.com/rust-lang/rust/blob/2ca7bcd03b87b52f7055a59b817443b0ac4a530d/compiler/rustc_lint_defs/src/builtin.rs#L5251-L5263 [1]

    // Extract the representation passed by `#[repr(...)]` if present. If nothing is
    // specified, the default is `Rust` representation, which uses `isize` for its
    // discriminant type.
    // See: https://doc.rust-lang.org/reference/items/enumerations.html#r-items.enum.discriminant.repr-rust
    let repr_ty = repr_ty.unwrap_or_else(|| Ident::new("isize", Span::call_site()));
    Ok((helper_tys, repr_ty))
}

fn derive_for_enum(
    target: DeriveTarget,
    enum_ident: &Ident,
    variants: &[Ident],
    helper_tys: &[ValidTy],
    repr_ty: &Ident,
) -> TokenStream {
    let impl_fn = match target {
        DeriveTarget::Into => impl_into,
        DeriveTarget::TryFrom => impl_try_from,
    };

    let qualified_repr_ty: syn::Path = parse_quote! { ::core::primitive::#repr_ty };

    return if helper_tys.is_empty() {
        let ty = ValidTy::Primitive(repr_ty.clone());
        let impls =
            std::iter::once(ty).map(|ty| impl_fn(enum_ident, variants, &qualified_repr_ty, &ty));
        ::quote::quote! { #(#impls)* }
    } else {
        let impls = helper_tys
            .iter()
            .map(|ty| impl_fn(enum_ident, variants, &qualified_repr_ty, ty));
        ::quote::quote! { #(#impls)* }
    };

    fn impl_into(
        enum_ident: &Ident,
        variants: &[Ident],
        repr_ty: &syn::Path,
        input_ty: &ValidTy,
    ) -> TokenStream {
        let param = Ident::new("value", Span::call_site());

        let overflow_assertion = emit_overflow_assert(enum_ident, variants, repr_ty, input_ty);
        let cast = match input_ty {
            ValidTy::Bounded(inner) => {
                let base_ty = inner.emit_qualified_base_ty();
                let expr = parse_quote! { #param as #base_ty };
                // Since the discriminant of `#param`, an enum variant, is determined
                // at compile-time, we can rely on `Bounded::from_expr()`. It requires
                // the provided expression to be verifiable at compile-time to avoid
                // triggering a build error.
                inner.emit_from_expr(&expr)
            }
            ValidTy::Primitive(ident) if ident == "bool" => {
                ::quote::quote! { (#param as #repr_ty) == 1 }
            }
            qualified @ ValidTy::Primitive(_) => ::quote::quote! { #param as #qualified },
        };

        ::quote::quote! {
            #[automatically_derived]
            impl ::core::convert::From<#enum_ident> for #input_ty {
                fn from(#param: #enum_ident) -> #input_ty {
                    #overflow_assertion

                    #cast
                }
            }
        }
    }

    fn impl_try_from(
        enum_ident: &Ident,
        variants: &[Ident],
        repr_ty: &syn::Path,
        input_ty: &ValidTy,
    ) -> TokenStream {
        let param = Ident::new("value", Span::call_site());

        let overflow_assertion = emit_overflow_assert(enum_ident, variants, repr_ty, input_ty);
        let emit_cast = |variant| {
            let variant = ::quote::quote! { #enum_ident::#variant };
            match input_ty {
                ValidTy::Bounded(inner) => {
                    let base_ty = inner.emit_qualified_base_ty();
                    let expr = parse_quote! { #variant as #base_ty };
                    inner.emit_new(&expr)
                }
                ValidTy::Primitive(ident) if ident == "bool" => {
                    ::quote::quote! { ((#variant as #repr_ty) == 1) }
                }
                qualified @ ValidTy::Primitive(_) => ::quote::quote! { #variant as #qualified },
            }
        };

        let clauses = variants.iter().map(|variant| {
            let cast = emit_cast(variant);
            ::quote::quote! {
                if #param == #cast {
                    ::core::result::Result::Ok(#enum_ident::#variant)
                } else
            }
        });

        ::quote::quote! {
            #[automatically_derived]
            impl ::core::convert::TryFrom<#input_ty> for #enum_ident {
                type Error = ::kernel::prelude::Error;
                fn try_from(#param: #input_ty) -> Result<#enum_ident, Self::Error> {
                    #overflow_assertion

                    #(#clauses)* {
                        ::core::result::Result::Err(::kernel::prelude::EINVAL)
                    }
                }
            }
        }
    }

    fn emit_overflow_assert(
        enum_ident: &Ident,
        variants: &[Ident],
        repr_ty: &syn::Path,
        input_ty: &ValidTy,
    ) -> TokenStream {
        let qualified_i128: syn::Path = parse_quote! { ::core::primitive::i128 };
        let qualified_u128: syn::Path = parse_quote! { ::core::primitive::u128 };

        let input_min = input_ty.emit_min();
        let input_max = input_ty.emit_max();

        let variant_fits = variants.iter().map(|variant| {
            let msg = format!(
                "enum discriminant overflow: \
                `{enum_ident}::{variant}` does not fit in `{input_ty}`",
            );
            ::quote::quote! {
                ::core::assert!(fits(#enum_ident::#variant as #repr_ty), #msg);
            }
        });

        ::quote::quote! {
            const _: () = {
                const fn fits(d: #repr_ty) -> ::core::primitive::bool {
                    // For every integer type, its minimum value always fits in `i128`.
                    let dst_min = #input_min;
                    // For every integer type, its maximum value always fits in `u128`.
                    let dst_max = #input_max;

                    #[allow(unused_comparisons)]
                    let is_src_signed = #repr_ty::MIN < 0;
                    #[allow(unused_comparisons)]
                    let is_dst_signed = dst_min < 0;

                    if is_src_signed && is_dst_signed {
                        // Casting from a signed value to `i128` does not overflow since
                        // `i128` is the largest signed primitive integer type.
                        (d as #qualified_i128) >= (dst_min as #qualified_i128)
                            && (d as #qualified_i128) <= (dst_max as #qualified_i128)
                    } else if is_src_signed && !is_dst_signed {
                        // Casting from a signed value greater than 0 to `u128` does not
                        // overflow since `u128::MAX` is greater than `i128::MAX`.
                        d >= 0 && (d as #qualified_u128) <= (dst_max as #qualified_u128)
                    } else {
                        // Casting from an unsigned value to `u128` does not overflow since
                        // `u128` is the largest unsigned primitive integer type.
                        (d as #qualified_u128) <= (dst_max as #qualified_u128)
                    }
                }

                #(#variant_fits)*
            };
        }
    }
}

enum ValidTy {
    Bounded(Bounded),
    Primitive(Ident),
}

impl ValidTy {
    fn validate(&self) -> syn::Result<()> {
        match self {
            Self::Bounded(inner) => inner.validate(),
            Self::Primitive(ident) => validate_primitive(ident),
        }
    }

    fn emit_min(&self) -> TokenStream {
        match self {
            Self::Bounded(inner) => inner.emit_min(),
            Self::Primitive(ident) if ident == "bool" => {
                ::quote::quote! { 0 }
            }
            qualified @ Self::Primitive(_) => ::quote::quote! { #qualified::MIN },
        }
    }

    fn emit_max(&self) -> TokenStream {
        match self {
            Self::Bounded(inner) => inner.emit_max(),
            Self::Primitive(ident) if ident == "bool" => {
                ::quote::quote! { 1 }
            }
            qualified @ Self::Primitive(_) => ::quote::quote! { #qualified::MAX },
        }
    }
}

impl Parse for ValidTy {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        if input.peek(Ident) && input.peek2(Token![<]) {
            return Ok(ValidTy::Bounded(input.parse()?));
        }
        Ok(ValidTy::Primitive(input.parse()?))
    }
}

impl ::quote::ToTokens for ValidTy {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        match self {
            Self::Bounded(inner) => inner.to_tokens(tokens),
            Self::Primitive(ident) => {
                let qualified_name: syn::Path = parse_quote! { ::core::primitive::#ident };
                qualified_name.to_tokens(tokens)
            }
        }
    }
}

impl fmt::Display for ValidTy {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Bounded(inner) => inner.fmt(f),
            Self::Primitive(ident) => ident.fmt(f),
        }
    }
}

struct Bounded {
    name: Ident,
    open_angle: Token![<],
    base_ty: Ident,
    comma: Token![,],
    bits: LitInt,
    close_angle: Token![>],
}

impl Bounded {
    const NAME: &'static str = "Bounded";
    const QUALIFIED_NAME: &'static str = "::kernel::num::Bounded";

    fn validate(&self) -> syn::Result<()> {
        let name = &self.name;
        if name != Self::NAME {
            let msg = format!("expected `{}`, found {}", Self::NAME, name);
            return Err(syn::Error::new(name.span(), msg));
        }
        validate_primitive(&self.base_ty)?;
        Ok(())
    }

    fn emit_from_expr(&self, expr: &Expr) -> TokenStream {
        let Self { base_ty, bits, .. } = self;
        let qualified_name: syn::Path = parse_str(Self::QUALIFIED_NAME).expect("valid path");
        ::quote::quote! {
            #qualified_name::<#base_ty, #bits>::from_expr(#expr)
        }
    }

    fn emit_new(&self, expr: &Expr) -> TokenStream {
        let Self { base_ty, bits, .. } = self;
        let qualified_name: syn::Path = parse_str(Self::QUALIFIED_NAME).expect("valid path");
        ::quote::quote! {
            #qualified_name::<#base_ty, #bits>::new::<{ #expr }>()
        }
    }

    fn emit_qualified_base_ty(&self) -> TokenStream {
        let base_ty = &self.base_ty;
        ::quote::quote! { ::core::primitive::#base_ty }
    }

    fn emit_min(&self) -> TokenStream {
        let bits = &self.bits;
        let base_ty = self.emit_qualified_base_ty();
        ::quote::quote! { #base_ty::MIN >> (#base_ty::BITS - #bits) }
    }

    fn emit_max(&self) -> TokenStream {
        let bits = &self.bits;
        let base_ty = self.emit_qualified_base_ty();
        ::quote::quote! { #base_ty::MAX >> (#base_ty::BITS - #bits) }
    }
}

impl Parse for Bounded {
    fn parse(input: ParseStream<'_>) -> syn::Result<Self> {
        Ok(Self {
            name: input.parse()?,
            open_angle: input.parse()?,
            base_ty: input.parse()?,
            comma: input.parse()?,
            bits: input.parse()?,
            close_angle: input.parse()?,
        })
    }
}

impl ::quote::ToTokens for Bounded {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let qualified_name: syn::Path = parse_str(Self::QUALIFIED_NAME).expect("valid path");
        qualified_name.to_tokens(tokens);
        self.open_angle.to_tokens(tokens);
        self.base_ty.to_tokens(tokens);
        self.comma.to_tokens(tokens);
        self.bits.to_tokens(tokens);
        self.close_angle.to_tokens(tokens);
    }
}

impl fmt::Display for Bounded {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}<{}, {}>", Self::NAME, self.base_ty, self.bits)
    }
}

fn validate_primitive(ident: &Ident) -> syn::Result<()> {
    if is_valid_primitive(ident) {
        return Ok(());
    }
    let msg =
        format!("expected `bool` or primitive integer type (e.g., `u8`, `i8`), found {ident}");
    Err(syn::Error::new(ident.span(), msg))
}

fn is_valid_primitive(ident: &Ident) -> bool {
    matches!(
        ident.to_string().as_str(),
        "bool"
            | "u8"
            | "u16"
            | "u32"
            | "u64"
            | "u128"
            | "usize"
            | "i8"
            | "i16"
            | "i32"
            | "i64"
            | "i128"
            | "isize"
    )
}
