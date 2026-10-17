//! `#[export]` turns a Rust function into the C function of the same name that ESP-IDF's headers
//! declare. It adds `#[unsafe(no_mangle)]` and `unsafe extern "C"`, translates the types below the
//! way ESP-IDF's own functions treat them, and fails the build unless the result matches the
//! declaration bindgen generated into `sys` exactly.
//!
//! | Rust | C |
//! |---|---|
//! | `&T`, `&mut T` | `T *` |
//! | `&mut MaybeUninit<T>` | `T *` that C hasn't filled in, for out-parameters |
//! | `Option<&T>`, `Option<&mut T>` | `T *` that may be NULL |
//! | `&str` | `const char *` |
//! | `Result<(), esp_err_t>` | `esp_err_t` |
//!
//! RUST_KERNEL.md has the NULL rule. A kernel type such as `&Queue` means the C handle its
//! `sys::CPointer` impl names.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as Tokens;
use quote::{format_ident, quote};
use syn::{
    FnArg, GenericArgument, ItemFn, Pat, PathArguments, ReturnType, Type, parse_macro_input,
    parse_quote,
};

/// Whether `ty` is written `name` or `name<...>`, like `str` or `Result<(), esp_err_t>`.
fn is(ty: &Type, name: &str) -> bool {
    matches!(ty, Type::Path(path) if path.path.segments.last().is_some_and(|last| last.ident == name))
}

/// The `T` in `name<T>`.
fn argument<'ty>(ty: &'ty Type, name: &str) -> Option<&'ty Type> {
    let Type::Path(path) = ty else { return None };
    let PathArguments::AngleBracketed(arguments) = &path.path.segments.last()?.arguments else {
        return None;
    };
    match arguments.args.first()? {
        GenericArgument::Type(inner) if is(ty, name) => Some(inner),
        _ => None,
    }
}

/// The C type the check expects in place of `ty`; `MaybeUninit<T>` has `T`'s layout.
fn c_type(ty: &Type) -> Tokens {
    if let Some(inner) = argument(ty, "MaybeUninit") {
        return c_type(inner);
    }
    let reference = match ty {
        Type::Reference(reference) => reference,
        _ => match argument(ty, "Option") {
            Some(Type::Reference(reference)) => reference,
            _ => return quote!(#ty),
        },
    };
    let target = c_type(&reference.elem);
    if reference.mutability.is_some() {
        quote!(<#target as ::sys::CPointer>::Unique)
    } else {
        quote!(<#target as ::sys::CPointer>::Shared)
    }
}

#[proc_macro_attribute]
pub fn export(_attribute: TokenStream, item: TokenStream) -> TokenStream {
    let ItemFn {
        attrs,
        vis,
        sig,
        block,
    } = parse_macro_input!(item as ItemFn);
    let name = &sig.ident;
    let output = &sig.output;
    let result = matches!(output, ReturnType::Type(_, ty) if is(ty, "Result"));
    let (c_return, check_return) = match output {
        _ if result => (quote!(-> ::sys::esp_err_t), quote!(-> ::sys::esp_err_t)),
        ReturnType::Default => (quote!(), quote!()),
        ReturnType::Type(arrow, ty) => (quote!(#output), {
            let c = c_type(ty);
            quote!(#arrow #c)
        }),
    };

    let mut c_params = Vec::new(); // the C function's parameters
    let mut c_types = Vec::new(); // what the check compares with the header
    let mut conversions = Vec::new(); // each C argument turned into the Rust one, before the call
    let mut c_names = Vec::new();
    for input in &sig.inputs {
        let FnArg::Typed(param) = input else {
            panic!("an export can't take `self`");
        };
        let Pat::Ident(ident) = &*param.pat else {
            panic!("an export's parameters must be plain names");
        };
        let ty = &*param.ty;
        let rust_name = &ident.ident;
        // Unlike the Rust name, the compiler sees this one as generated and doesn't lint it.
        let c_name = format_ident!("c_{}", rust_name.to_string().trim_start_matches('_'));
        match ty {
            Type::Reference(reference) if is(&reference.elem, "str") => {
                c_params.push(quote!(#c_name: *const ::core::ffi::c_char));
                c_types.push(quote!(*const ::core::ffi::c_char));
                // C passes NULL or a NUL-terminated string that outlives the call.
                conversions.push(quote! {
                    let #c_name = if #c_name.is_null() {
                        "?"
                    } else {
                        unsafe { ::core::ffi::CStr::from_ptr(#c_name) }.to_str().unwrap_or("?")
                    };
                });
            }
            Type::Reference(_) => {
                let on_null = if result {
                    quote!(return ::sys::ESP_ERR_INVALID_ARG)
                } else {
                    quote!(::core::panic!(::core::concat!(
                        "NULL ",
                        ::core::stringify!(#rust_name)
                    )))
                };
                c_params.push(quote!(#c_name: ::core::option::Option<#ty>));
                c_types.push(c_type(ty));
                conversions.push(quote! {
                    let ::core::option::Option::Some(#c_name) = #c_name else { #on_null };
                });
            }
            _ => {
                c_params.push(quote!(#c_name: #ty));
                c_types.push(c_type(ty));
            }
        }
        c_names.push(c_name);
    }
    let variadic = sig.variadic.as_ref().map(|_| quote!(, ...));
    let check = quote! {
        const _: () = {
            let _: unsafe extern "C" fn(#(#c_types),* #variadic) #check_return = ::sys::#name;
        };
    };

    // A variadic Rust function must itself be the C function, so it takes C's types unchanged.
    if sig.variadic.is_some() {
        let mut c_sig = sig.clone();
        c_sig.abi = Some(parse_quote!(extern "C"));
        return quote!(#(#attrs)* #[unsafe(no_mangle)] #vis #c_sig #block #check).into();
    }
    let unsafety = &sig.unsafety;
    let call = quote!(#unsafety { inner(#(#c_names),*) });
    let finish = if result {
        quote!(match #call {
            ::core::result::Result::Ok(()) => ::sys::ESP_OK,
            ::core::result::Result::Err(error) => error,
        })
    } else {
        call
    };
    let inputs = &sig.inputs;
    quote! {
        #[unsafe(no_mangle)]
        #vis unsafe extern "C" fn #name(#(#c_params),*) #c_return {
            #(#attrs)*
            #unsafety fn inner(#inputs) #output #block
            #(#conversions)*
            #finish
        }
        #check
    }
    .into()
}
