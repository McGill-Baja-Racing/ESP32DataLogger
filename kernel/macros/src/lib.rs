//! `#[export]` turns a Rust function into the C function of the same name that ESP-IDF's headers
//! declare. It adds `#[unsafe(no_mangle)]` and `unsafe extern "C"`, translates the types below,
//! and fails the build unless the result matches the declaration bindgen generated into `sys`.
//!
//! | Rust | C |
//! |---|---|
//! | `&T`, `&mut T` | `T *` |
//! | `&mut MaybeUninit<T>` | `T *` that C hasn't filled in, for out-parameters |
//! | `Option<&T>`, `Option<&mut T>` | `T *` that may be NULL |
//! | `Box<T>`, `Option<Box<T>>` | `T *` that C hands over, or may be NULL |
//! | `&str` (parameters only) | `const char *` |
//! | `Result<(), esp_err_t>` | `esp_err_t` |
//!
//! A NULL `&T` or `Box<T>` panics, like `configASSERT`. A kernel type such as `&Queue` means the C
//! handle its `sys::CPointer` impl names. A `Box<T>` parameter frees `T` when the function ends,
//! so C must have got the pointer from a `Box<T>` the kernel leaked.
//!
//! The function may be `unsafe`. Its lifetime `'call` is the call's, and it takes no other
//! generics.

use proc_macro::TokenStream;
use quote::{format_ident, quote};
use syn::{
    FnArg, GenericArgument, GenericParam, ItemFn, Pat, PathArguments, ReturnType, Type,
    parse_macro_input, parse_quote,
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

fn is_optional_str(ty: &Type) -> bool {
    generic(ty, "Option").is_some_and(is_str)
}

/// The C type bindgen declares in place of `ty`; `MaybeUninit<T>` and `Box<T>` have `T *`'s layout.
fn c_type(ty: &Type) -> Type {
    match ty {
        _ if is_str(ty) => parse_quote!(*const ::core::ffi::c_char),
        Type::Reference(reference) => {
            let target = c_type(&reference.elem);
            match reference.mutability {
                Some(_) => parse_quote!(<#target as ::sys::CPointer>::Unique),
                None => parse_quote!(<#target as ::sys::CPointer>::Shared),
            }
        }
        _ => match (
            generic(ty, "Option"),
            generic(ty, "MaybeUninit"),
            generic(ty, "Box"),
        ) {
            (Some(inner), ..)
                if matches!(inner, Type::Reference(_)) || generic(inner, "Box").is_some() =>
            {
                c_type(inner)
            }
            (_, Some(inner), _) => c_type(inner),
            (.., Some(inner)) => {
                let target = c_type(inner);
                parse_quote!(<#target as ::sys::CPointer>::Unique)
            }
            _ => ty.clone(),
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
    let lone_call = |generics: &syn::Generics| {
        generics.where_clause.is_none()
            && generics.params.iter().all(
                |param| matches!(param, GenericParam::Lifetime(life) if life.lifetime.ident == "call"),
            )
    };
    if !lone_call(&sig.generics) {
        let error =
            syn::Error::new_spanned(&sig.generics, "an export takes no generics but `'call`");
        return error.to_compile_error().into();
    }
    let rust_output = &sig.output;
    if matches!(rust_output, ReturnType::Type(_, ty) if is_str(ty) || is_optional_str(ty)) {
        let error = syn::Error::new_spanned(rust_output, "an export can't return a `str`");
        return error.to_compile_error().into();
    }
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
            let error = syn::Error::new_spanned(input, "an export can't take `self`");
            return error.to_compile_error().into();
        };
        let Pat::Ident(ident) = &*param.pat else {
            let error =
                syn::Error::new_spanned(&param.pat, "an export's parameters must be plain names");
            return error.to_compile_error().into();
        };
        let (name, ty) = (&ident.ident, &*param.ty);
        if is_optional_str(ty) {
            let error = syn::Error::new_spanned(ty, "an export can't take an `Option<&str>`");
            return error.to_compile_error().into();
        }
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
            _ if matches!(ty, Type::Reference(_)) || generic(ty, "Box").is_some() => {
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
