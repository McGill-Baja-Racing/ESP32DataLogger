#![no_std]

use core::cell::{Cell, UnsafeCell};
use core::mem::MaybeUninit;
use core::net::{IpAddr, SocketAddr};
use core::ptr::NonNull;

use embassy_net::{IpAddress, IpEndpoint, IpListenEndpoint};

pub use tcp::*;

use crate::sealed::SealedDynPool;

mod tcp;

/// A const-generics-erased trait variant of `Pool`
///
/// Allows for types like `Tcp` and `TcpSocket` that do reference the
/// pool to erase the const-generics set on the Pool object type when used for TCP buffers.
///
/// To erase the type of the pool itself, these types use `&dyn DynPool<B>`
pub trait DynPool<B>: SealedDynPool<B> {}

impl<T, B> DynPool<B> for &T where T: DynPool<B> {}

mod sealed {
    use core::ptr::NonNull;

    /// The sealed trait variant of `DynPool`.
    pub trait SealedDynPool<B> {
        /// Allocate an object from the pool.
        ///
        /// Returns `None` if the pool is exhausted.
        fn alloc(&self) -> Option<B>;

        /// Free an object back to the pool.
        ///
        /// # Safety
        /// - `buffer_token` must be a pointer obtained from `alloc` that hasn't been freed yet.
        unsafe fn free(&self, buffer_token: NonNull<u8>);
    }

    impl<T, B> SealedDynPool<B> for &T
    where
        T: SealedDynPool<B>,
    {
        fn alloc(&self) -> Option<B> {
            (**self).alloc()
        }

        unsafe fn free(&self, buffer_token: NonNull<u8>) {
            (**self).free(buffer_token)
        }
    }
}

/// A simple fixed-size pool allocator for `T`.
pub struct Pool<T, const N: usize> {
    used: [Cell<bool>; N],
    data: [UnsafeCell<MaybeUninit<T>>; N],
}

impl<T, const N: usize> Pool<T, N> {
    #[expect(
        clippy::declare_interior_mutable_const,
        reason = "array repeat expressions need a const"
    )]
    const VALUE: Cell<bool> = Cell::new(false);
    #[expect(
        clippy::declare_interior_mutable_const,
        reason = "array repeat expressions need a const"
    )]
    const UNINIT: UnsafeCell<MaybeUninit<T>> = UnsafeCell::new(MaybeUninit::uninit());

    /// Create a new pool.
    pub const fn new() -> Self {
        Self {
            used: [Self::VALUE; N],
            data: [Self::UNINIT; N],
        }
    }
}

impl<T, const N: usize> Default for Pool<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, const N: usize> Pool<T, N> {
    /// Allocate an object from the pool.
    ///
    /// # Returns
    /// - `Some(NonNull<T>)` if an object was successfully allocated.
    /// - `None` if the pool is exhausted.
    fn alloc(&self) -> Option<NonNull<T>> {
        for n in 0..N {
            // this can't race because Pool is not Sync.
            if !self.used[n].get() {
                self.used[n].set(true);
                let p = self.data[n].get() as *mut T;
                return Some(unsafe { NonNull::new_unchecked(p) });
            }
        }
        None
    }

    /// Free an object back to the pool.
    ///
    /// Safety: p must be a pointer obtained from `alloc` that hasn't been freed yet.
    ///
    /// # Arguments
    /// - `p`: A pointer to the object to free.
    unsafe fn free(&self, p: NonNull<T>) {
        let origin = self.data.as_ptr() as *mut T;
        let n = p.as_ptr().offset_from(origin);
        assert!(n >= 0);
        assert!((n as usize) < N);
        self.used[n as usize].set(false);
    }
}

/// Convert an embassy-net `IpEndpoint` to a standard library `SocketAddr`.
pub(crate) fn to_net_socket(socket: IpEndpoint) -> SocketAddr {
    SocketAddr::new(socket.addr.into(), socket.port)
}

/// Convert a standard library `SocketAddr` to an embassy-net `IpListenEndpoint`.
pub(crate) fn to_emb_bind_socket(socket: SocketAddr) -> Option<IpListenEndpoint> {
    let addr = if socket.ip().is_unspecified() {
        None
    } else {
        Some(to_emb_addr(socket.ip())?)
    };

    Some(IpListenEndpoint {
        addr,
        port: socket.port(),
    })
}

/// Convert an embassy-net `IpAddress` to a standard library `IpAddr`.
pub(crate) fn to_emb_addr(addr: IpAddr) -> Option<IpAddress> {
    match addr {
        IpAddr::V4(addr) => Some(addr.into()),
        IpAddr::V6(_) => None,
    }
}
