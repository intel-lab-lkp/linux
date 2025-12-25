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

mod derive_into_tests {
    /// ```
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(u8)]
    /// enum Foo {
    ///     // Works with const expressions.
    ///     A = add(0, 0),
    ///     B = 2_isize.pow(1) - 1,
    /// }
    ///
    /// const fn add(a: isize, b: isize) -> isize {
    ///     a + b
    /// }
    ///
    /// assert_eq!(0_u8, Foo::A.into());
    /// assert_eq!(1_u8, Foo::B.into());
    /// ```
    mod works_with_const_expr {}

    /// ```
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(bool)]
    /// enum Foo {
    ///     A,
    ///     B,
    /// }
    ///
    /// assert_eq!(false, Foo::A.into());
    /// assert_eq!(true, Foo::B.into());
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(bool)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `bool`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(bool)]
    /// enum Foo {
    ///     // `2` cannot be represented with `bool`.
    ///     A = 2,
    /// }
    /// ```
    mod overflow_assert_works_on_bool {}

    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 7>)]
    /// enum Foo {
    ///     A = -1 << 6,      // The minimum value of `Bounded<i8, 7>`.
    ///     B = (1 << 6) - 1, // The maximum value of `Bounded<i8, 7>`.
    /// }
    ///
    /// let foo_a: Bounded<i8, 7> = Foo::A.into();
    /// let foo_b: Bounded<i8, 7> = Foo::B.into();
    /// assert_eq!(Bounded::<i8, 7>::new::<{ -1_i8 << 6 }>(), foo_a);
    /// assert_eq!(Bounded::<i8, 7>::new::<{ (1_i8 << 6) - 1 }>(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 7>)]
    /// enum Foo {
    ///     // `1 << 6` cannot be represented with `Bounded<i8, 7>`.
    ///     A = 1 << 6,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 7>)]
    /// enum Foo {
    ///     // `(-1 << 6) - 1` cannot be represented with `Bounded<i8, 7>`.
    ///     A = (-1 << 6) - 1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 1>)]
    /// enum Foo {
    ///     A = -1, // The minimum value of `Bounded<i8, 1>`.
    ///     B,      // The maximum value of `Bounded<i8, 1>`.
    /// }
    ///
    /// let foo_a: Bounded<i8, 1> = Foo::A.into();
    /// let foo_b: Bounded<i8, 1> = Foo::B.into();
    /// assert_eq!(Bounded::<i8, 1>::new::<{ -1_i8 }>(), foo_a);
    /// assert_eq!(Bounded::<i8, 1>::new::<{ 0_i8 } >(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 1>)]
    /// enum Foo {
    ///     // `1` cannot be represented with `Bounded<i8, 1>`.
    ///     A = 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 1>)]
    /// enum Foo {
    ///     // `-2` cannot be represented with `Bounded<i8, 1>`.
    ///     A = -2,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = i32::MIN as i64,
    ///     B = i32::MAX as i64,
    /// }
    ///
    /// let foo_a: Bounded<i32, 32> = Foo::A.into();
    /// let foo_b: Bounded<i32, 32> = Foo::B.into();
    /// assert_eq!(Bounded::<i32, 32>::new::<{ i32::MIN }>(), foo_a);
    /// assert_eq!(Bounded::<i32, 32>::new::<{ i32::MAX }>(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     // `1 << 31` cannot be represented with `Bounded<i32, 32>`.
    ///     A = 1 << 31,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     // `(-1 << 31) - 1` cannot be represented with `Bounded<i32, 32>`.
    ///     A = (-1 << 31) - 1,
    /// }
    /// ```
    mod overflow_assert_works_on_signed_bounded {}

    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 7>)]
    /// enum Foo {
    ///     A,                // The minimum value of `Bounded<u8, 7>`.
    ///     B = (1 << 7) - 1, // The maximum value of `Bounded<u8, 7>`.
    /// }
    ///
    /// let foo_a: Bounded<u8, 7> = Foo::A.into();
    /// let foo_b: Bounded<u8, 7> = Foo::B.into();
    /// assert_eq!(Bounded::<u8, 7>::new::<{ 0 }>(), foo_a);
    /// assert_eq!(Bounded::<u8, 7>::new::<{ (1_u8 << 7) - 1 }>(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 7>)]
    /// enum Foo {
    ///     // `1 << 7` cannot be represented with `Bounded<u8, 7>`.
    ///     A = 1 << 7,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 7>)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u8, 7>`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 1>)]
    /// enum Foo {
    ///     A, // The minimum value of `Bounded<u8, 1>`.
    ///     B, // The maximum value of `Bounded<u8, 1>`.
    /// }
    ///
    /// let foo_a: Bounded<u8, 1> = Foo::A.into();
    /// let foo_b: Bounded<u8, 1> = Foo::B.into();
    /// assert_eq!(Bounded::<u8, 1>::new::<{ 0 }>(), foo_a);
    /// assert_eq!(Bounded::<u8, 1>::new::<{ 1 }>(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 1>)]
    /// enum Foo {
    ///     // `2` cannot be represented with `Bounded<u8, 1>`.
    ///     A = 2,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u8, 1>)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u8, 1>`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::Into,
    ///     num::Bounded, //
    /// };
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     A = u32::MIN as u64,
    ///     B = u32::MAX as u64,
    /// }
    ///
    /// let foo_a: Bounded<u32, 32> = Foo::A.into();
    /// let foo_b: Bounded<u32, 32> = Foo::B.into();
    /// assert_eq!(Bounded::<u32, 32>::new::<{ u32::MIN }>(), foo_a);
    /// assert_eq!(Bounded::<u32, 32>::new::<{ u32::MAX }>(), foo_b);
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     // `1 << 32` cannot be represented with `Bounded<u32, 32>`.
    ///     A = 1 << 32,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u32, 32>`.
    ///     A = -1,
    /// }
    /// ```
    mod overflow_assert_works_on_unsigned_bounded {}

    /// ```
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(isize)]
    /// #[repr(isize)]
    /// enum Foo {
    ///     A = isize::MIN,
    ///     B = isize::MAX,
    /// }
    ///
    /// assert_eq!(isize::MIN, Foo::A.into());
    /// assert_eq!(isize::MAX, Foo::B.into());
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(isize)]
    /// #[repr(usize)]
    /// enum Foo {
    ///     A = (isize::MAX as usize) + 1
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(i32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (i32::MIN as i64) - 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(i32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (i32::MAX as i64) + 1,
    /// }
    /// ```
    mod overflow_assert_works_on_signed_int {}

    /// ```
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(usize)]
    /// #[repr(usize)]
    /// enum Foo {
    ///     A = usize::MIN,
    ///     B = usize::MAX,
    /// }
    ///
    /// assert_eq!(usize::MIN, Foo::A.into());
    /// assert_eq!(usize::MAX, Foo::B.into());
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(usize)]
    /// #[repr(isize)]
    /// enum Foo {
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(u32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (u32::MIN as i64) - 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(u32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (u32::MAX as i64) + 1,
    /// }
    /// ```
    mod overflow_assert_works_on_unsigned_int {}

    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(Bounded<i8, 7>, i8, i16, i32, i64)]
    /// #[repr(i8)]
    /// enum Foo {
    ///     // `i8::MAX` cannot be represented with `Bounded<i8, 7>`.
    ///     A = i8::MAX,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::Into;
    ///
    /// #[derive(Into)]
    /// #[into(i8, i16, i32, i64, Bounded<i8, 7>)]
    /// #[repr(i8)]
    /// enum Foo {
    ///     // `i8::MAX` cannot be represented with `Bounded<i8, 7>`.
    ///     A = i8::MAX,
    /// }
    /// ```
    mod any_into_target_overflow_is_rejected {}
}

mod derive_try_from_tests {
    /// ```
    /// use kernel::{
    ///     macros::{
    ///         Into,
    ///         TryFrom, //
    ///     },
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, Into, PartialEq, TryFrom)]
    /// #[into(bool, Bounded<i8, 7>, Bounded<u8, 7>, i8, i16, i32, i64, i128, isize, u8, u16, u32, u64, u128, usize)]
    /// #[try_from(bool, Bounded<i8, 7>, Bounded<u8, 7>, i8, i16, i32, i64, i128, isize, u8, u16, u32, u64, u128, usize)]
    /// enum Foo {
    ///     A,
    ///     B,
    /// }
    ///
    /// assert_eq!(false, Foo::A.into());
    /// assert_eq!(true, Foo::B.into());
    /// assert_eq!(Ok(Foo::A), Foo::try_from(false));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(true));
    ///
    /// let foo_a: Bounded<i8, 7> = Foo::A.into();
    /// let foo_b: Bounded<i8, 7> = Foo::B.into();
    /// assert_eq!(Bounded::<i8, 7>::new::<0>(), foo_a);
    /// assert_eq!(Bounded::<i8, 7>::new::<1>(), foo_b);
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<i8, 7>::new::<0>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<i8, 7>::new::<1>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i8, 7>::new::<-1>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i8, 7>::new::<2>()));
    ///
    /// let foo_a: Bounded<u8, 7> = Foo::A.into();
    /// let foo_b: Bounded<u8, 7> = Foo::B.into();
    /// assert_eq!(Bounded::<u8, 7>::new::<0>(), foo_a);
    /// assert_eq!(Bounded::<u8, 7>::new::<1>(), foo_b);
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<u8, 7>::new::<0>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<u8, 7>::new::<1>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<u8, 7>::new::<2>()));
    ///
    /// macro_rules! gen_signed_tests {
    ///     ($($type:ty),*) => {
    ///         $(
    ///             assert_eq!(0 as $type, Foo::A.into());
    ///             assert_eq!(1 as $type, Foo::B.into());
    ///             assert_eq!(Ok(Foo::A), Foo::try_from(0 as $type));
    ///             assert_eq!(Ok(Foo::B), Foo::try_from(1 as $type));
    ///             assert_eq!(Err(EINVAL), Foo::try_from((0 as $type) - 1));
    ///             assert_eq!(Err(EINVAL), Foo::try_from((1 as $type) + 1));
    ///         )*
    ///     };
    /// }
    /// macro_rules! gen_unsigned_tests {
    ///     ($($type:ty),*) => {
    ///         $(
    ///             assert_eq!(0 as $type, Foo::A.into());
    ///             assert_eq!(1 as $type, Foo::B.into());
    ///             assert_eq!(Ok(Foo::A), Foo::try_from(0 as $type));
    ///             assert_eq!(Ok(Foo::B), Foo::try_from(1 as $type));
    ///             assert_eq!(Err(EINVAL), Foo::try_from((1 as $type) + 1));
    ///         )*
    ///     };
    /// }
    /// gen_signed_tests!(i8, i16, i32, i64, i128, isize);
    /// gen_unsigned_tests!(u8, u16, u32, u64, u128, usize);
    /// ```
    mod works_with_derive_into {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(u8)]
    /// enum Foo {
    ///     // Works with const expressions.
    ///     A = add(0, 0),
    ///     B = 2_isize.pow(1) - 1,
    /// }
    ///
    /// const fn add(a: isize, b: isize) -> isize {
    ///     a + b
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(0_u8));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(1_u8));
    /// assert_eq!(Err(EINVAL), Foo::try_from(2_u8));
    /// ```
    mod works_with_const_expr {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(bool)]
    /// enum Foo {
    ///     A,
    ///     B,
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(false));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(true));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(bool)]
    /// enum Bar {
    ///     A,
    /// }
    ///
    /// assert_eq!(Ok(Bar::A), Bar::try_from(false));
    /// assert_eq!(Err(EINVAL), Bar::try_from(true));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(bool)]
    /// enum Baz {
    ///     A = 1,
    /// }
    ///
    /// assert_eq!(Err(EINVAL), Baz::try_from(false));
    /// assert_eq!(Ok(Baz::A), Baz::try_from(true));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(bool)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `bool`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(bool)]
    /// enum Foo {
    ///     // `2` cannot be represented with `bool`.
    ///     A = 2,
    /// }
    /// ```
    mod overflow_assert_works_on_bool {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<i8, 7>)]
    /// enum Foo {
    ///     A = -1 << 6,      // The minimum value of `Bounded<i8, 7>`.
    ///     B = (1 << 6) - 1, // The maximum value of `Bounded<i8, 7>`.
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<i8, 7>::new::<{ -1_i8 << 6 }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<i8, 7>::new::<{ (1_i8 << 6) - 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i8, 7>::new::<{ (-1_i8 << 6) + 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i8, 7>::new::<{ (1_i8 << 6) - 2 }>()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i8, 7>)]
    /// enum Foo {
    ///     // `1 << 6` cannot be represented with `Bounded<i8, 7>`.
    ///     A = 1 << 6,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i8, 7>)]
    /// enum Foo {
    ///     // `(-1 << 6) - 1` cannot be represented with `Bounded<i8, 7>`.
    ///     A = (-1 << 6) - 1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<i8, 1>)]
    /// enum Foo {
    ///     A = -1, // The minimum value of `Bounded<i8, 1>`.
    ///     B,      // The maximum value of `Bounded<i8, 1>`.
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<i8, 1>::new::<{ -1_i8 }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<i8, 1>::new::<{ 0_i8 } >()));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<i8, 1>)]
    /// enum Bar {
    ///     A = -1, // The minimum value of `Bounded<i8, 1>`.
    /// }
    ///
    /// assert_eq!(Ok(Bar::A), Bar::try_from(Bounded::<i8, 1>::new::<{ -1_i8 }>()));
    /// assert_eq!(Err(EINVAL), Bar::try_from(Bounded::<i8, 1>::new::<{ 0_i8 } >()));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<i8, 1>)]
    /// enum Baz {
    ///     A, // The maximum value of `Bounded<i8, 1>`.
    /// }
    ///
    /// assert_eq!(Err(EINVAL), Baz::try_from(Bounded::<i8, 1>::new::<{ -1_i8 }>()));
    /// assert_eq!(Ok(Baz::A), Baz::try_from(Bounded::<i8, 1>::new::<{ 0_i8 } >()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i8, 1>)]
    /// enum Foo {
    ///     // `1` cannot be represented with `Bounded<i8, 1>`.
    ///     A = 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i8, 1>)]
    /// enum Foo {
    ///     // `-2` cannot be represented with `Bounded<i8, 1>`.
    ///     A = -2,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = i32::MIN as i64,
    ///     B = i32::MAX as i64,
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<i32, 32>::new::<{ i32::MIN }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<i32, 32>::new::<{ i32::MAX }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i32, 32>::new::<{ i32::MIN + 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<i32, 32>::new::<{ i32::MAX - 1 }>()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     // `1 << 31` cannot be represented with `Bounded<i32, 32>`.
    ///     A = 1 << 31,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i32, 32>)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     // `(-1 << 31) - 1` cannot be represented with `Bounded<i32, 32>`.
    ///     A = (-1 << 31) - 1,
    /// }
    /// ```
    mod overflow_assert_works_on_signed_bounded {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<u8, 7>)]
    /// enum Foo {
    ///     A,                // The minimum value of `Bounded<u8, 7>`.
    ///     B = (1 << 7) - 1, // The maximum value of `Bounded<u8, 7>`.
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<u8, 7>::new::<{ 0 }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<u8, 7>::new::<{ (1_u8 << 7) - 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<u8, 7>::new::<{ 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<u8, 7>::new::<{ (1_u8 << 7) - 2 }>()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u8, 7>)]
    /// enum Foo {
    ///     // `1 << 7` cannot be represented with `Bounded<u8, 7>`.
    ///     A = 1 << 7,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u8, 7>)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u8, 7>`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<u8, 1>)]
    /// enum Foo {
    ///     A, // The minimum value of `Bounded<u8, 1>`.
    ///     B, // The maximum value of `Bounded<u8, 1>`.
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<u8, 1>::new::<{ 0 }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<u8, 1>::new::<{ 1 }>()));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<u8, 1>)]
    /// enum Bar {
    ///     A, // The minimum value of `Bounded<u8, 1>`.
    /// }
    ///
    /// assert_eq!(Ok(Bar::A), Bar::try_from(Bounded::<u8, 1>::new::<{ 0 }>()));
    /// assert_eq!(Err(EINVAL), Bar::try_from(Bounded::<u8, 1>::new::<{ 1 }>()));
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<u8, 1>)]
    /// enum Baz {
    ///     A = 1, // The maximum value of `Bounded<u8, 1>`.
    /// }
    ///
    /// assert_eq!(Err(EINVAL), Baz::try_from(Bounded::<u8, 1>::new::<{ 0 }>()));
    /// assert_eq!(Ok(Baz::A), Baz::try_from(Bounded::<u8, 1>::new::<{ 1 }>()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u8, 1>)]
    /// enum Foo {
    ///     // `2` cannot be represented with `Bounded<u8, 1>`.
    ///     A = 2,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u8, 1>)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u8, 1>`.
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     A = u32::MIN as u64,
    ///     B = u32::MAX as u64,
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(Bounded::<u32, 32>::new::<{ u32::MIN }>()));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(Bounded::<u32, 32>::new::<{ u32::MAX }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<u32, 32>::new::<{ u32::MIN + 1 }>()));
    /// assert_eq!(Err(EINVAL), Foo::try_from(Bounded::<u32, 32>::new::<{ u32::MAX - 1 }>()));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     // `1 << 32` cannot be represented with `Bounded<u32, 32>`.
    ///     A = 1 << 32,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<u32, 32>)]
    /// #[repr(u64)]
    /// enum Foo {
    ///     // `-1` cannot be represented with `Bounded<u32, 32>`.
    ///     A = -1,
    /// }
    /// ```
    mod overflow_assert_works_on_unsigned_bounded {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(isize)]
    /// #[repr(isize)]
    /// enum Foo {
    ///     A = isize::MIN,
    ///     B = isize::MAX,
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(isize::MIN));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(isize::MAX));
    /// assert_eq!(Err(EINVAL), Foo::try_from(isize::MIN + 1));
    /// assert_eq!(Err(EINVAL), Foo::try_from(isize::MAX - 1));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(isize)]
    /// #[repr(usize)]
    /// enum Foo {
    ///     A = (isize::MAX as usize) + 1
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(i32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (i32::MIN as i64) - 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(i32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (i32::MAX as i64) + 1,
    /// }
    /// ```
    mod overflow_assert_works_on_signed_int {}

    /// ```
    /// use kernel::{
    ///     macros::TryFrom,
    ///     num::Bounded,
    ///     prelude::*, //
    /// };
    ///
    /// #[derive(Debug, PartialEq, TryFrom)]
    /// #[try_from(usize)]
    /// #[repr(usize)]
    /// enum Foo {
    ///     A = usize::MIN,
    ///     B = usize::MAX,
    /// }
    ///
    /// assert_eq!(Ok(Foo::A), Foo::try_from(usize::MIN));
    /// assert_eq!(Ok(Foo::B), Foo::try_from(usize::MAX));
    /// assert_eq!(Err(EINVAL), Foo::try_from(usize::MIN + 1));
    /// assert_eq!(Err(EINVAL), Foo::try_from(usize::MAX - 1));
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(usize)]
    /// #[repr(isize)]
    /// enum Foo {
    ///     A = -1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(u32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (u32::MIN as i64) - 1,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(u32)]
    /// #[repr(i64)]
    /// enum Foo {
    ///     A = (u32::MAX as i64) + 1,
    /// }
    /// ```
    mod overflow_assert_works_on_unsigned_int {}

    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(Bounded<i8, 7>, i8, i16, i32, i64)]
    /// #[repr(i8)]
    /// enum Foo {
    ///     // `i8::MAX` cannot be represented with `Bounded<i8, 7>`.
    ///     A = i8::MAX,
    /// }
    /// ```
    ///
    /// ```compile_fail
    /// use kernel::macros::TryFrom;
    ///
    /// #[derive(TryFrom)]
    /// #[try_from(i8, i16, i32, i64, Bounded<i8, 7>)]
    /// #[repr(i8)]
    /// enum Foo {
    ///     // `i8::MAX` cannot be represented with `Bounded<i8, 7>`.
    ///     A = i8::MAX,
    /// }
    /// ```
    mod any_try_from_target_overflow_is_rejected {}
}
