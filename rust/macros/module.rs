// SPDX-License-Identifier: GPL-2.0

use std::fmt::Write;

use proc_macro2::{
    Literal,
    TokenStream, //
};
use syn::{
    bracketed,
    parse::{
        Parse,
        ParseStream, //
    },
    punctuated::Punctuated,
    token::Bracket,
    Error,
    Ident,
    LitStr,
    Result,
    Token, //
};

use crate::helpers::*;

struct ModInfoBuilder<'a> {
    module: &'a str,
    counter: usize,
    buffer: String,
}

impl<'a> ModInfoBuilder<'a> {
    fn new(module: &'a str) -> Self {
        ModInfoBuilder {
            module,
            counter: 0,
            buffer: String::new(),
        }
    }

    fn emit_base(&mut self, field: &str, content: &str, builtin: bool) {
        let string = if builtin {
            // Built-in modules prefix their modinfo strings by `module.`.
            format!(
                "{module}.{field}={content}\0",
                module = self.module,
                field = field,
                content = content
            )
        } else {
            // Loadable modules' modinfo strings go as-is.
            format!("{field}={content}\0")
        };

        write!(
            &mut self.buffer,
            "
                {cfg}
                #[doc(hidden)]
                #[cfg_attr(not(target_os = \"macos\"), link_section = \".modinfo\")]
                #[used(compiler)]
                pub static __{module}_{counter}: [u8; {length}] = *{string};
            ",
            cfg = if builtin {
                "#[cfg(not(MODULE))]"
            } else {
                "#[cfg(MODULE)]"
            },
            module = self.module.to_uppercase(),
            counter = self.counter,
            length = string.len(),
            string = Literal::byte_string(string.as_bytes()),
        )
        .unwrap();

        self.counter += 1;
    }

    fn emit_only_builtin(&mut self, field: &str, content: &str) {
        self.emit_base(field, content, true)
    }

    fn emit_only_loadable(&mut self, field: &str, content: &str) {
        self.emit_base(field, content, false)
    }

    fn emit(&mut self, field: &str, content: &str) {
        self.emit_only_builtin(field, content);
        self.emit_only_loadable(field, content);
    }
}

mod kw {
    syn::custom_keyword!(name);
    syn::custom_keyword!(authors);
    syn::custom_keyword!(description);
    syn::custom_keyword!(license);
    syn::custom_keyword!(alias);
    syn::custom_keyword!(firmware);
}

#[allow(dead_code, reason = "some fields are only parsed into")]
enum ModInfoField {
    Type(Token![type], Token![:], Ident),
    Name(kw::name, Token![:], AsciiLitStr),
    Authors(
        kw::authors,
        Token![:],
        Bracket,
        Punctuated<LitStr, Token![,]>,
    ),
    Description(kw::description, Token![:], LitStr),
    License(kw::license, Token![:], AsciiLitStr),
    Alias(
        kw::authors,
        Token![:],
        Bracket,
        Punctuated<LitStr, Token![,]>,
    ),
    Firmware(
        kw::firmware,
        Token![:],
        Bracket,
        Punctuated<LitStr, Token![,]>,
    ),
}

impl ModInfoField {
    /// Obtain the key identifying the field.
    fn key(&self) -> Ident {
        match self {
            ModInfoField::Type(key, ..) => Ident::new("type", key.span),
            ModInfoField::Name(key, ..) => Ident::new("name", key.span),
            ModInfoField::Authors(key, ..) => Ident::new("authors", key.span),
            ModInfoField::Description(key, ..) => Ident::new("description", key.span),
            ModInfoField::License(key, ..) => Ident::new("license", key.span),
            ModInfoField::Alias(key, ..) => Ident::new("alias", key.span),
            ModInfoField::Firmware(key, ..) => Ident::new("firmware", key.span),
        }
    }
}

impl Parse for ModInfoField {
    fn parse(input: ParseStream<'_>) -> Result<Self> {
        let key = input.lookahead1();
        if key.peek(Token![type]) {
            Ok(Self::Type(input.parse()?, input.parse()?, input.parse()?))
        } else if key.peek(kw::name) {
            Ok(Self::Name(input.parse()?, input.parse()?, input.parse()?))
        } else if key.peek(kw::authors) {
            let list;
            Ok(Self::Authors(
                input.parse()?,
                input.parse()?,
                bracketed!(list in input),
                Punctuated::parse_terminated(&list)?,
            ))
        } else if key.peek(kw::description) {
            Ok(Self::Description(
                input.parse()?,
                input.parse()?,
                input.parse()?,
            ))
        } else if key.peek(kw::license) {
            Ok(Self::License(
                input.parse()?,
                input.parse()?,
                input.parse()?,
            ))
        } else if key.peek(kw::alias) {
            let list;
            Ok(Self::Alias(
                input.parse()?,
                input.parse()?,
                bracketed!(list in input),
                Punctuated::parse_terminated(&list)?,
            ))
        } else if key.peek(kw::firmware) {
            let list;
            Ok(Self::Firmware(
                input.parse()?,
                input.parse()?,
                bracketed!(list in input),
                Punctuated::parse_terminated(&list)?,
            ))
        } else {
            Err(key.error())
        }
    }
}

#[derive(Debug, Default)]
pub(crate) struct ModuleInfo {
    type_: String,
    license: String,
    name: String,
    authors: Option<Vec<String>>,
    description: Option<String>,
    alias: Option<Vec<String>>,
    firmware: Option<Vec<String>>,
}

impl Parse for ModuleInfo {
    fn parse(input: ParseStream<'_>) -> Result<Self> {
        let mut info = Self::default();

        let span = input.span();
        let fields = Punctuated::<ModInfoField, Token![,]>::parse_terminated(input)?;
        let mut errors = Vec::new();

        const EXPECTED_KEYS: &[&str] = &[
            "type",
            "name",
            "authors",
            "description",
            "license",
            "alias",
            "firmware",
        ];
        const REQUIRED_KEYS: &[&str] = &["type", "name", "license"];
        let mut seen_keys = Vec::new();

        for field in fields {
            let key = field.key();

            if seen_keys.contains(&key) {
                errors.push(Error::new_spanned(
                    &key,
                    format!(r#"duplicated key "{key}". Keys can only be specified once."#),
                ));
                continue;
            }
            seen_keys.push(key);

            match field {
                ModInfoField::Type(_, _, ty) => info.type_ = ty.to_string(),
                ModInfoField::Name(_, _, name) => info.name = name.value(),
                ModInfoField::Authors(_, _, _, list) => {
                    info.authors = Some(list.into_iter().map(|x| x.value()).collect())
                }
                ModInfoField::Description(_, _, desc) => info.description = Some(desc.value()),
                ModInfoField::License(_, _, license) => info.license = license.value(),
                ModInfoField::Alias(_, _, _, list) => {
                    info.alias = Some(list.into_iter().map(|x| x.value()).collect())
                }
                ModInfoField::Firmware(_, _, _, list) => {
                    info.firmware = Some(list.into_iter().map(|x| x.value()).collect())
                }
            }
        }

        for key in REQUIRED_KEYS {
            if !seen_keys.iter().any(|e| e == key) {
                errors.push(Error::new(span, format!(r#"missing required key "{key}""#)));
            }
        }

        let mut ordered_keys: Vec<&str> = Vec::new();
        for key in EXPECTED_KEYS {
            if seen_keys.iter().any(|e| e == key) {
                ordered_keys.push(key);
            }
        }

        if seen_keys != ordered_keys {
            errors.push(Error::new(
                span,
                format!(r#"keys are not ordered as expected. Order them like: {ordered_keys:?}."#),
            ));
        }

        if let Some(err) = errors.into_iter().reduce(|mut e1, e2| {
            e1.combine(e2);
            e1
        }) {
            return Err(err);
        }

        Ok(info)
    }
}

pub(crate) fn module(info: ModuleInfo) -> Result<TokenStream> {
    // Rust does not allow hyphens in identifiers, use underscore instead.
    let ident = info.name.replace('-', "_");
    let mut modinfo = ModInfoBuilder::new(ident.as_ref());
    if let Some(authors) = info.authors {
        for author in authors {
            modinfo.emit("author", &author);
        }
    }
    if let Some(description) = info.description {
        modinfo.emit("description", &description);
    }
    modinfo.emit("license", &info.license);
    if let Some(aliases) = info.alias {
        for alias in aliases {
            modinfo.emit("alias", &alias);
        }
    }
    if let Some(firmware) = info.firmware {
        for fw in firmware {
            modinfo.emit("firmware", &fw);
        }
    }

    // Built-in modules also export the `file` modinfo string.
    let file =
        std::env::var("RUST_MODFILE").expect("Unable to fetch RUST_MODFILE environmental variable");
    modinfo.emit_only_builtin("file", &file);

    Ok(format!(
        "
            /// The module name.
            ///
            /// Used by the printing macros, e.g. [`info!`].
            const __LOG_PREFIX: &[u8] = b\"{name}\\0\";

            // SAFETY: `__this_module` is constructed by the kernel at load time and will not be
            // freed until the module is unloaded.
            #[cfg(MODULE)]
            static THIS_MODULE: ::kernel::ThisModule = unsafe {{
                extern \"C\" {{
                    static __this_module: ::kernel::types::Opaque<::kernel::bindings::module>;
                }}

                ::kernel::ThisModule::from_ptr(__this_module.get())
            }};
            #[cfg(not(MODULE))]
            static THIS_MODULE: ::kernel::ThisModule = unsafe {{
                ::kernel::ThisModule::from_ptr(::core::ptr::null_mut())
            }};

            /// The `LocalModule` type is the type of the module created by `module!`,
            /// `module_pci_driver!`, `module_platform_driver!`, etc.
            type LocalModule = {type_};

            impl ::kernel::ModuleMetadata for {type_} {{
                const NAME: &'static ::kernel::str::CStr = c\"{name}\";
            }}

            // Double nested modules, since then nobody can access the public items inside.
            mod __module_init {{
                mod __module_init {{
                    use super::super::{type_};
                    use pin_init::PinInit;

                    /// The \"Rust loadable module\" mark.
                    //
                    // This may be best done another way later on, e.g. as a new modinfo
                    // key or a new section. For the moment, keep it simple.
                    #[cfg(MODULE)]
                    #[doc(hidden)]
                    #[used(compiler)]
                    static __IS_RUST_MODULE: () = ();

                    static mut __MOD: ::core::mem::MaybeUninit<{type_}> =
                        ::core::mem::MaybeUninit::uninit();

                    // Loadable modules need to export the `{{init,cleanup}}_module` identifiers.
                    /// # Safety
                    ///
                    /// This function must not be called after module initialization, because it may be
                    /// freed after that completes.
                    #[cfg(MODULE)]
                    #[doc(hidden)]
                    #[no_mangle]
                    #[link_section = \".init.text\"]
                    pub unsafe extern \"C\" fn init_module() -> ::kernel::ffi::c_int {{
                        // SAFETY: This function is inaccessible to the outside due to the double
                        // module wrapping it. It is called exactly once by the C side via its
                        // unique name.
                        unsafe {{ __init() }}
                    }}

                    #[cfg(MODULE)]
                    #[doc(hidden)]
                    #[used(compiler)]
                    #[link_section = \".init.data\"]
                    static __UNIQUE_ID___addressable_init_module: unsafe extern \"C\" fn() -> i32 = init_module;

                    #[cfg(MODULE)]
                    #[doc(hidden)]
                    #[no_mangle]
                    #[link_section = \".exit.text\"]
                    pub extern \"C\" fn cleanup_module() {{
                        // SAFETY:
                        // - This function is inaccessible to the outside due to the double
                        //   module wrapping it. It is called exactly once by the C side via its
                        //   unique name,
                        // - furthermore it is only called after `init_module` has returned `0`
                        //   (which delegates to `__init`).
                        unsafe {{ __exit() }}
                    }}

                    #[cfg(MODULE)]
                    #[doc(hidden)]
                    #[used(compiler)]
                    #[link_section = \".exit.data\"]
                    static __UNIQUE_ID___addressable_cleanup_module: extern \"C\" fn() = cleanup_module;

                    // Built-in modules are initialized through an initcall pointer
                    // and the identifiers need to be unique.
                    #[cfg(not(MODULE))]
                    #[cfg(not(CONFIG_HAVE_ARCH_PREL32_RELOCATIONS))]
                    #[doc(hidden)]
                    #[link_section = \"{initcall_section}\"]
                    #[used(compiler)]
                    pub static __{ident}_initcall: extern \"C\" fn() ->
                        ::kernel::ffi::c_int = __{ident}_init;

                    #[cfg(not(MODULE))]
                    #[cfg(CONFIG_HAVE_ARCH_PREL32_RELOCATIONS)]
                    ::core::arch::global_asm!(
                        r#\".section \"{initcall_section}\", \"a\"
                        __{ident}_initcall:
                            .long   __{ident}_init - .
                            .previous
                        \"#
                    );

                    #[cfg(not(MODULE))]
                    #[doc(hidden)]
                    #[no_mangle]
                    pub extern \"C\" fn __{ident}_init() -> ::kernel::ffi::c_int {{
                        // SAFETY: This function is inaccessible to the outside due to the double
                        // module wrapping it. It is called exactly once by the C side via its
                        // placement above in the initcall section.
                        unsafe {{ __init() }}
                    }}

                    #[cfg(not(MODULE))]
                    #[doc(hidden)]
                    #[no_mangle]
                    pub extern \"C\" fn __{ident}_exit() {{
                        // SAFETY:
                        // - This function is inaccessible to the outside due to the double
                        //   module wrapping it. It is called exactly once by the C side via its
                        //   unique name,
                        // - furthermore it is only called after `__{ident}_init` has
                        //   returned `0` (which delegates to `__init`).
                        unsafe {{ __exit() }}
                    }}

                    /// # Safety
                    ///
                    /// This function must only be called once.
                    unsafe fn __init() -> ::kernel::ffi::c_int {{
                        let initer =
                            <{type_} as ::kernel::InPlaceModule>::init(&super::super::THIS_MODULE);
                        // SAFETY: No data race, since `__MOD` can only be accessed by this module
                        // and there only `__init` and `__exit` access it. These functions are only
                        // called once and `__exit` cannot be called before or during `__init`.
                        match unsafe {{ initer.__pinned_init(__MOD.as_mut_ptr()) }} {{
                            Ok(m) => 0,
                            Err(e) => e.to_errno(),
                        }}
                    }}

                    /// # Safety
                    ///
                    /// This function must
                    /// - only be called once,
                    /// - be called after `__init` has been called and returned `0`.
                    unsafe fn __exit() {{
                        // SAFETY: No data race, since `__MOD` can only be accessed by this module
                        // and there only `__init` and `__exit` access it. These functions are only
                        // called once and `__init` was already called.
                        unsafe {{
                            // Invokes `drop()` on `__MOD`, which should be used for cleanup.
                            __MOD.assume_init_drop();
                        }}
                    }}

                    {modinfo}
                }}
            }}
        ",
        type_ = info.type_,
        name = info.name,
        ident = ident,
        modinfo = modinfo.buffer,
        initcall_section = ".initcall6.init"
    )
    .parse()
    .expect("Error parsing formatted string into token stream."))
}
