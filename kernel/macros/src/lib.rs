//! `#[export]` turns a Rust function into the C function of the same name that ESP-IDF's headers
//! declare. It adds `#[unsafe(no_mangle)]` and `unsafe extern "C"`, translates the types below,
//! and fails the build unless the result matches the declaration bindgen generated into `sys`.
//!
//! | Rust | C |
//! |---|---|
//! | `&T`, `&mut T` | `T *` |
//! | `&mut MaybeUninit<T>` | `T *` that C hasn't filled in, for out-parameters |
//! | `Option<&T>`, `Option<&mut T>` | `T *` that may be NULL |
//! | `&str` | `const char *` |
//! | `Result<(), esp_err_t>` | `esp_err_t` |
//!
//! A NULL `&T` panics, like `configASSERT`. A kernel type such as `&Queue` means the C handle its
//! `sys::CPointer` impl names.

use proc_macro::TokenStream;
use quote::{format_ident, quote};
use syn::{
    FnArg, GenericArgument, ItemFn, Pat, PathArguments, ReturnType, Type, parse_macro_input,
};

/// The `T` in `name<T>`.
fn generic<'ty>(ty: &'ty Type, name: &str) -> Option<&'ty Type> {
    let Type::Path(path) = ty else { return None };
    let last = path.path.segments.last()?;
    let PathArguments::AngleBracketed(arguments) = &last.arguments else {
        return None;
    };
    match arguments.args.first()? {
        GenericArgument::Type(inner) if last.ident == name => Some(inner),
        _ => None,
    }
}

fn is_str(ty: &Type) -> bool {
    let Type::Reference(reference) = ty else {
        return false;
    };
    matches!(&*reference.elem, Type::Path(path) if path.path.is_ident("str"))
}

/// The C type bindgen declares in place of `ty`; `MaybeUninit<T>` has `T`'s layout.
fn c_type(ty: &Type) -> proc_macro2::TokenStream {
    match ty {
        _ if is_str(ty) => quote!(*const ::core::ffi::c_char),
        Type::Reference(reference) => {
            let target = c_type(&reference.elem);
            match reference.mutability {
                Some(_) => quote!(<#target as ::sys::CPointer>::Unique),
                None => quote!(<#target as ::sys::CPointer>::Shared),
            }
        }
        _ => match (generic(ty, "Option"), generic(ty, "MaybeUninit")) {
            (Some(inner @ Type::Reference(_)), _) | (_, Some(inner)) => c_type(inner),
            _ => quote!(#ty),
        },
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
    let rust_output = &sig.output;
    let result = matches!(rust_output, ReturnType::Type(_, ty) if generic(ty, "Result").is_some());
    let (output, c_output) = match rust_output {
        ReturnType::Default => (quote!(), quote!()),
        ReturnType::Type(..) if result => {
            (quote!(-> ::sys::esp_err_t), quote!(-> ::sys::esp_err_t))
        }
        ReturnType::Type(arrow, ty) => {
            let c = c_type(ty);
            (quote!(#rust_output), quote!(#arrow #c))
        }
    };

    let mut params = Vec::new();
    let mut c_types = Vec::new();
    let mut conversions = Vec::new();
    for input in &sig.inputs {
        let FnArg::Typed(param) = input else {
            panic!("an export can't take `self`")
        };
        let Pat::Ident(ident) = &*param.pat else {
            panic!("an export's parameters must be plain names")
        };
        let (name, ty) = (&ident.ident, &*param.ty);
        // Reading `_name` to convert it would count as using an unused parameter.
        let c = format_ident!("{}", name.to_string().trim_start_matches('_'));
        c_types.push(c_type(ty));
        match ty {
            _ if is_str(ty) => {
                params.push(quote!(#c: *const ::core::ffi::c_char));
                // C passes NULL or a NUL-terminated string that outlives the call.
                conversions.push(quote! {
                    let #name: &'call str = if #c.is_null() { "?" } else {
                        unsafe { ::core::ffi::CStr::from_ptr(#c) }.to_str().unwrap_or("?")
                    };
                });
            }
            Type::Reference(_) => {
                // ESP-IDF returns ESP_ERR_INVALID_ARG where it can; NULL is a firmware bug.
                let message = format!("NULL {c}");
                params.push(quote!(#c: ::core::option::Option<#ty>));
                conversions.push(quote! {
                    let ::core::option::Option::Some(#name) = #c else { ::core::panic!(#message) };
                });
            }
            _ => params.push(quote!(#param)),
        }
    }

    let body = if result {
        quote!((|| #rust_output #block)().map_or_else(|error| error, |()| ::sys::ESP_OK))
    } else {
        quote!(#block)
    };
    let name = &sig.ident;
    let variadic = &sig.variadic;
    quote! {
        #(#attrs)*
        #[unsafe(no_mangle)]
        #vis unsafe extern "C" fn #name<'call>(#(#params,)* #variadic) #output {
            #(#conversions)*
            #body
        }
        const _: () = {
            let _: unsafe extern "C" fn(#(#c_types,)* #variadic) #c_output = ::sys::#name;
        };
    }
    .into()
}
