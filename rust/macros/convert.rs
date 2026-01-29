// SPDX-License-Identifier: GPL-2.0

use proc_macro2::{
    Span,
    TokenStream, //
};

use std::fmt;

use syn::{
    parse_quote,
    parse_str,
    punctuated::Punctuated,
    spanned::Spanned,
    AngleBracketedGenericArguments,
    Attribute,
    Data,
    DeriveInput,
    Expr,
    ExprLit,
    Fields,
    GenericArgument,
    Ident,
    Lit,
    LitInt,
    PathArguments,
    PathSegment,
    Token,
    Type,
    TypePath, //
};

pub(crate) fn derive_into(input: DeriveInput) -> syn::Result<TokenStream> {
    derive(DeriveTarget::Into, input)
}

pub(crate) fn derive_try_from(input: DeriveInput) -> syn::Result<TokenStream> {
    derive(DeriveTarget::TryFrom, input)
}

fn derive(target: DeriveTarget, input: DeriveInput) -> syn::Result<TokenStream> {
    let data_enum = match input.data {
        Data::Enum(data) => data,
        Data::Struct(data) => {
            let msg = format!(
                "expected `enum`, found `struct`; \
                `#[derive({})]` can only be applied to a unit-only enum",
                target.get_trait_name(),
            );
            return Err(syn::Error::new(data.struct_token.span(), msg));
        }
        Data::Union(data) => {
            let msg = format!(
                "expected `enum`, found `union`; \
                `#[derive({})]` can only be applied to a unit-only enum",
                target.get_trait_name(),
            );
            return Err(syn::Error::new(data.union_token.span(), msg));
        }
    };

    let mut errors: Option<syn::Error> = None;
    let mut combine_error = |err| match errors.as_mut() {
        Some(errors) => errors.combine(err),
        None => errors = Some(err),
    };

    let (helper_tys, is_repr_c, repr_ty) = parse_attrs(target, &input.attrs)?;

    let mut valid_helper_tys = Vec::with_capacity(helper_tys.len());
    for ty in helper_tys {
        match validate_type(&ty) {
            Ok(valid_ty) => valid_helper_tys.push(valid_ty),
            Err(err) => combine_error(err),
        }
    }

    let mut is_unit_only = true;
    for variant in &data_enum.variants {
        match &variant.fields {
            Fields::Unit => continue,
            Fields::Named(_) => {
                let msg = format!(
                    "expected unit-like variant, found struct-like variant; \
                    `#[derive({})]` can only be applied to a unit-only enum",
                    target.get_trait_name(),
                );
                combine_error(syn::Error::new_spanned(variant, msg));
            }
            Fields::Unnamed(_) => {
                let msg = format!(
                    "expected unit-like variant, found tuple-like variant; \
                    `#[derive({})]` can only be applied to a unit-only enum",
                    target.get_trait_name(),
                );
                combine_error(syn::Error::new_spanned(variant, msg));
            }
        }

        is_unit_only = false;
    }

    if is_repr_c && is_unit_only && repr_ty.is_none() {
        let msg = "`#[repr(C)]` fieldless enums are not supported";
        return Err(syn::Error::new(input.ident.span(), msg));
    }

    if let Some(errors) = errors {
        return Err(errors);
    }

    let variants: Vec<_> = data_enum
        .variants
        .into_iter()
        .map(|variant| variant.ident)
        .collect();

    // Extract the representation passed by `#[repr(...)]` if present. If nothing is
    // specified, the default is `Rust` representation, which uses `isize` for its
    // discriminant type.
    // See: https://doc.rust-lang.org/reference/items/enumerations.html#r-items.enum.discriminant.repr-rust
    let repr_ty = repr_ty.unwrap_or_else(|| Ident::new("isize", Span::call_site()));

    Ok(derive_for_enum(
        target,
        &input.ident,
        &variants,
        repr_ty,
        valid_helper_tys,
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

fn parse_attrs(
    target: DeriveTarget,
    attrs: &[Attribute],
) -> syn::Result<(Vec<Type>, bool, Option<Ident>)> {
    let helper = target.get_helper_name();

    let mut is_repr_c = false;
    let mut repr_ty = None;
    let mut helper_tys = Vec::new();
    for attr in attrs {
        if attr.path().is_ident("repr") {
            attr.parse_nested_meta(|meta| {
                let ident = meta.path.get_ident();
                if let Some(i) = ident {
                    if is_valid_primitive(i) {
                        repr_ty = ident.cloned();
                    } else if i == "C" {
                        is_repr_c = true;
                    }
                }
                // Delegate `repr` attribute validation to rustc.
                Ok(())
            })?;
        } else if attr.path().is_ident(helper) && helper_tys.is_empty() {
            let args = attr.parse_args_with(Punctuated::<Type, Token![,]>::parse_terminated)?;
            helper_tys.extend(args);
        }
    }

    Ok((helper_tys, is_repr_c, repr_ty))
}

fn derive_for_enum(
    target: DeriveTarget,
    enum_ident: &Ident,
    variants: &[Ident],
    repr_ty: Ident,
    helper_tys: Vec<ValidTy>,
) -> TokenStream {
    let impl_fn = match target {
        DeriveTarget::Into => impl_into,
        DeriveTarget::TryFrom => impl_try_from,
    };

    let qualified_repr_ty: syn::Path = parse_quote! { ::core::primitive::#repr_ty };

    return if helper_tys.is_empty() {
        let ty = ValidTy::Primitive(repr_ty);
        let implementation = impl_fn(enum_ident, variants, &qualified_repr_ty, &ty);
        ::quote::quote! { #implementation }
    } else {
        let impls = helper_tys
            .into_iter()
            .map(|ty| impl_fn(enum_ident, variants, &qualified_repr_ty, &ty));
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
    base_ty: Ident,
    bits: LitInt,
}

impl Bounded {
    const NAME: &'static str = "Bounded";
    const QUALIFIED_NAME: &'static str = "::kernel::num::Bounded";

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

impl ::quote::ToTokens for Bounded {
    fn to_tokens(&self, tokens: &mut TokenStream) {
        let bits = &self.bits;
        let base_ty = self.emit_qualified_base_ty();
        let qualified_name: syn::Path = parse_str(Self::QUALIFIED_NAME).expect("valid path");

        tokens.extend(::quote::quote! {
            #qualified_name<#base_ty, #bits>
        });
    }
}

impl fmt::Display for Bounded {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}<{}, {}>", Self::NAME, self.base_ty, self.bits)
    }
}

fn validate_type(ty: &Type) -> syn::Result<ValidTy> {
    let Type::Path(type_path) = ty else {
        return Err(make_err(ty));
    };

    let TypePath { qself, path } = type_path;
    if qself.is_some() {
        return Err(make_err(ty));
    }

    let syn::Path {
        leading_colon,
        segments,
    } = path;
    if leading_colon.is_some() || segments.len() != 1 {
        return Err(make_err(ty));
    }

    let segment = &path.segments[0];
    if segment.ident == Bounded::NAME {
        return validate_bounded(segment);
    } else {
        return validate_primitive(&segment.ident);
    }

    fn make_err(ty: &Type) -> syn::Error {
        let msg = format!(
            "expected unqualified form of `bool`, primitive integer type, or `{}<T, N>`",
            Bounded::NAME,
        );
        syn::Error::new_spanned(ty, msg)
    }
}

fn validate_bounded(path_segment: &PathSegment) -> syn::Result<ValidTy> {
    let PathSegment { ident, arguments } = path_segment;
    return match arguments {
        PathArguments::AngleBracketed(inner) if ident == Bounded::NAME => {
            let AngleBracketedGenericArguments {
                colon2_token, args, ..
            } = inner;

            if colon2_token.is_some() {
                return Err(make_outer_err(path_segment));
            }

            if args.len() != 2 {
                return Err(make_outer_err(path_segment));
            }

            let (base_ty, bits) = (&args[0], &args[1]);
            let GenericArgument::Type(Type::Path(base_ty_lowered)) = base_ty else {
                return Err(make_base_ty_err(base_ty));
            };

            if base_ty_lowered.qself.is_some() {
                return Err(make_base_ty_err(base_ty));
            }

            let Some(base_ty_ident) = base_ty_lowered.path.get_ident() else {
                return Err(make_base_ty_err(base_ty));
            };

            if !is_valid_primitive(base_ty_ident) {
                return Err(make_base_ty_err(base_ty));
            }

            let GenericArgument::Const(Expr::Lit(ExprLit {
                lit: Lit::Int(bits),
                ..
            })) = bits
            else {
                return Err(syn::Error::new_spanned(bits, "expected integer literal"));
            };

            let bounded = Bounded {
                base_ty: base_ty_ident.clone(),
                bits: bits.clone(),
            };
            Ok(ValidTy::Bounded(bounded))
        }
        _ => Err(make_outer_err(path_segment)),
    };

    fn make_outer_err(path_segment: &PathSegment) -> syn::Error {
        let msg = format!("expected `{0}<T, N>` (e.g., {0}<u8, 4>)", Bounded::NAME);
        syn::Error::new_spanned(path_segment, msg)
    }

    fn make_base_ty_err(base_ty: &GenericArgument) -> syn::Error {
        let msg = "expected unqualified form of primitive integer type";
        syn::Error::new_spanned(base_ty, msg)
    }
}

fn validate_primitive(ident: &Ident) -> syn::Result<ValidTy> {
    if is_valid_primitive(ident) {
        return Ok(ValidTy::Primitive(ident.clone()));
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
