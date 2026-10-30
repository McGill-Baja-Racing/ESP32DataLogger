use core::fmt::Display;
use core::future::{poll_fn, Future};
use core::net::SocketAddr;
use core::pin::pin;
use core::ptr::NonNull;
use core::task::Poll;

use edge_nal::{Close, Readable, TcpBind, TcpShutdown, TcpSplit};

use embassy_futures::join::join;

use embassy_net::tcp::{AcceptError, Error, TcpReader, TcpWriter};
use embassy_net::Stack;

use embassy_time::Duration;

use embedded_io_async::{ErrorKind, ErrorType, Read, Write};

use crate::sealed::SealedDynPool;
use crate::{to_emb_bind_socket, to_net_socket, DynPool, Pool};

/// A type that implements the `TcpBind` factory trait from `edge-nal`
/// Uses the provided Embassy networking stack and TCP buffers pool to create TCP sockets.
///
/// The type is `Copy` and `Clone`, so it can be easily passed around.
#[derive(Copy, Clone)]
pub struct Tcp<'d> {
    /// The Embassy networking stack to use for creating TCP sockets.
    stack: Stack<'d>,
    /// The pool of TCP socket buffers to use for creating TCP sockets.
    buffers: &'d dyn DynPool<TcpSocketBuffers>,
}

impl<'d> Tcp<'d> {
    /// Create a new `Tcp` instance for the provided Embassy networking stack, using the provided TCP buffers.
    ///
    /// # Arguments
    /// - `stack`: The Embassy networking stack to use for creating TCP sockets.
    /// - `buffers`: A reference to a pool of TCP socket buffers.
    ///   NOTE: Ensure that the number of buffers in the pool is not greater than the number of sockets
    ///   supported by the provided [embassy_net::Stack], or else [smoltcp::iface::SocketSet] will panic with
    ///   `adding a socket to a full SocketSet`.
    pub fn new(stack: Stack<'d>, buffers: &'d dyn DynPool<TcpSocketBuffers>) -> Self {
        Self { stack, buffers }
    }
}

impl TcpBind for Tcp<'_> {
    type Error = TcpError;

    type Accept<'a>
        = TcpAccept<'a>
    where
        Self: 'a;

    async fn bind(&self, local: SocketAddr) -> Result<Self::Accept<'_>, Self::Error> {
        Ok(TcpAccept {
            stack: *self,
            local,
        })
    }
}

/// A type that represents an acceptor for incoming TCP client connections.
/// Implements the `TcpAccept` factory trait from `edge-nal`
///
/// The type is `Copy` and `Clone`, so it can be easily passed around.
#[derive(Copy, Clone)]
pub struct TcpAccept<'d> {
    stack: Tcp<'d>,
    local: SocketAddr,
}

impl edge_nal::TcpAccept for TcpAccept<'_> {
    type Error = TcpError;

    type Socket<'a>
        = TcpSocket<'a>
    where
        Self: 'a;

    async fn accept(&self) -> Result<(SocketAddr, Self::Socket<'_>), Self::Error> {
        let local = to_emb_bind_socket(self.local).ok_or(TcpError::UnsupportedProto)?;
        // Changed from upstream, which unwraps `local_endpoint`: a connection reset right after
        // the handshake has none, and a peer could panic the firmware with it.
        loop {
            let mut socket = TcpSocket::new(self.stack.stack, self.stack.buffers)?;

            socket.socket.accept(local).await?;

            if let Some(local_endpoint) = socket.socket.local_endpoint() {
                return Ok((to_net_socket(local_endpoint), socket));
            }
        }
    }
}

/// A type that represents a TCP socket
/// Implements the `Read` and `Write` traits from `embedded-io-async`, as well as the `TcpSplit` factory trait from `edge-nal`
pub struct TcpSocket<'d> {
    /// The underlying Embassy TCP socket.
    socket: embassy_net::tcp::TcpSocket<'d>,
    /// The pool of TCP socket buffers used by this socket.
    stack_buffers: &'d dyn DynPool<TcpSocketBuffers>,
    /// The token used to identify the socket buffers in the pool.
    buffer_token: NonNull<u8>,
}

impl<'d> TcpSocket<'d> {
    fn new(
        stack: Stack<'d>,
        stack_buffers: &'d dyn DynPool<TcpSocketBuffers>,
    ) -> Result<Self, TcpError> {
        let socket_buffers = stack_buffers.alloc().ok_or(TcpError::NoBuffers)?;

        // SAFETY: The buffers are exclusively owned by this socket until `free` is called from
        // `Drop`, and the slices are built from the raw pointers (rather than from `&mut u8`
        // references to their first byte) so that they cover the whole buffers.
        let mut socket = embassy_net::tcp::TcpSocket::new(
            stack,
            unsafe {
                core::slice::from_raw_parts_mut(
                    socket_buffers.rx_buf.as_ptr(),
                    socket_buffers.rx_buf_len,
                )
            },
            unsafe {
                core::slice::from_raw_parts_mut(
                    socket_buffers.tx_buf.as_ptr(),
                    socket_buffers.tx_buf_len,
                )
            },
        );
        // Added to upstream: without it, a peer that vanished mid-response holds the socket forever.
        socket.set_timeout(Some(Duration::from_secs(10)));

        Ok(Self {
            socket,
            stack_buffers,
            buffer_token: socket_buffers.token,
        })
    }

    async fn close(&mut self, what: Close) -> Result<(), TcpError> {
        async fn discard_all_data(rx: &mut TcpReader<'_>) -> Result<(), TcpError> {
            let mut buf = [0; 32];

            while rx.read(&mut buf).await? > 0 {}

            Ok(())
        }

        if matches!(what, Close::Both | Close::Write) {
            self.socket.close();
        }

        let (mut rx, mut tx) = self.socket.split();

        match what {
            Close::Read => discard_all_data(&mut rx).await?,
            Close::Write => tx.flush().await?,
            Close::Both => {
                let mut flush = pin!(tx.flush());
                let mut read = pin!(discard_all_data(&mut rx));

                match join(&mut flush, &mut read).await {
                    (Err(e), _) => Err(e)?,
                    (_, Err(e)) => Err(e)?,
                    _ => (),
                }
            }
        }

        Ok(())
    }

    async fn abort(&mut self) -> Result<(), TcpError> {
        self.socket.abort();
        self.socket.flush().await?;

        Ok(())
    }
}

impl Drop for TcpSocket<'_> {
    fn drop(&mut self) {
        self.socket.close();
        unsafe {
            self.stack_buffers.free(self.buffer_token);
        }
    }
}

impl ErrorType for TcpSocket<'_> {
    type Error = TcpError;
}

impl Read for TcpSocket<'_> {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
        Ok(self.socket.read(buf).await?)
    }
}

impl Write for TcpSocket<'_> {
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
        Ok(self.socket.write(buf).await?)
    }

    async fn flush(&mut self) -> Result<(), Self::Error> {
        self.socket.flush().await?;

        Ok(())
    }
}

impl Readable for TcpSocket<'_> {
    async fn readable(&mut self) -> Result<(), Self::Error> {
        // embassy-net's wait_read_ready() only returns when can_recv() is true (data available),
        // but does not return when the peer closes the connection (FIN received) with empty buffer.
        // We need to also check may_recv() to detect when peer has sent FIN.
        //
        // This implementation relies on smoltcp waking the recv waker when the socket state
        // changes such that may_recv() becomes false (i.e., when FIN is received).
        //
        // Evidence from smoltcp 0.12.0 source (src/socket/tcp.rs):
        // - When FIN is received in ESTABLISHED state (line 1853-1858):
        //   https://github.com/smoltcp-rs/smoltcp/blob/d2d647090d544b1e7c142571da9d55f7280f664b/src/socket/tcp.rs#L1853-L1858
        //   `self.set_state(State::CloseWait)` is called
        // - The `set_state` method (lines 1315-1329) unconditionally calls:
        //   https://github.com/smoltcp-rs/smoltcp/blob/d2d647090d544b1e7c142571da9d55f7280f664b/src/socket/tcp.rs#L1315-L1329
        //   `self.rx_waker.wake()` with comment "Wake all tasks waiting. Even if we
        //   haven't received/sent data, this is needed because return values of
        //   functions may change depending on the state."
        //
        // Therefore, when FIN is received, the waker registered by wait_read_ready()
        // WILL be triggered, causing our poll_fn to be re-polled, at which point
        // may_recv() will return false and we'll return Ready.

        poll_fn(|cx| {
            // Check if peer has closed the connection (FIN received)
            if !self.socket.may_recv() {
                // Peer has closed, socket is "readable" (will return EOF on subsequent read)
                return Poll::Ready(());
            }

            // Check if data is available by polling wait_read_ready future
            let read_ready_future = core::pin::pin!(self.socket.wait_read_ready());
            read_ready_future.poll(cx)
        })
        .await;

        Ok(())
    }
}

impl TcpShutdown for TcpSocket<'_> {
    async fn close(&mut self, what: Close) -> Result<(), Self::Error> {
        TcpSocket::close(self, what).await
    }

    async fn abort(&mut self) -> Result<(), Self::Error> {
        TcpSocket::abort(self).await
    }
}

/// A type that represents the read half of a split TCP socket.
/// Implements the `Read` trait from `embedded-io-async`.
pub struct TcpSocketRead<'a>(TcpReader<'a>);

impl ErrorType for TcpSocketRead<'_> {
    type Error = TcpError;
}

impl Read for TcpSocketRead<'_> {
    async fn read(&mut self, buf: &mut [u8]) -> Result<usize, Self::Error> {
        self.0.read(buf).await.map_err(TcpError::from)
    }
}

impl Readable for TcpSocketRead<'_> {
    /// Wait until the socket becomes readable.
    ///
    /// Note: TcpReader doesn't expose may_recv(), so we cannot detect FIN closure here.
    /// This means readable() will not return when peer closes with empty buffer.
    /// Callers using split sockets should handle 0-byte reads (EOF) appropriately.
    async fn readable(&mut self) -> Result<(), Self::Error> {
        self.0.wait_read_ready().await;
        Ok(())
    }
}

/// A type that represents the write half of a split TCP socket.
/// Implements the `Write` trait from `embedded-io-async`.
pub struct TcpSocketWrite<'a>(TcpWriter<'a>);

impl ErrorType for TcpSocketWrite<'_> {
    type Error = TcpError;
}

impl Write for TcpSocketWrite<'_> {
    async fn write(&mut self, buf: &[u8]) -> Result<usize, Self::Error> {
        self.0.write(buf).await.map_err(TcpError::from)
    }

    async fn flush(&mut self) -> Result<(), Self::Error> {
        self.0.flush().await.map_err(TcpError::from)
    }
}

impl TcpSplit for TcpSocket<'_> {
    type Read<'a>
        = TcpSocketRead<'a>
    where
        Self: 'a;

    type Write<'a>
        = TcpSocketWrite<'a>
    where
        Self: 'a;

    fn split(&mut self) -> (Self::Read<'_>, Self::Write<'_>) {
        let (read, write) = self.socket.split();

        (TcpSocketRead(read), TcpSocketWrite(write))
    }
}

/// A shared error type that is used by the TCP factory traits implementation as well as the TCP socket.
#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub enum TcpError {
    /// A general TCP error.
    General(Error),
    /// An error that occurred while accepting a connection.
    Accept(AcceptError),
    /// No TCP socket buffers available.
    NoBuffers,
    /// The provided socket address uses an unsupported protocol.
    UnsupportedProto,
}

impl From<Error> for TcpError {
    fn from(e: Error) -> Self {
        TcpError::General(e)
    }
}

impl From<AcceptError> for TcpError {
    fn from(e: AcceptError) -> Self {
        TcpError::Accept(e)
    }
}

impl Display for TcpError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            TcpError::General(e) => write!(f, "TCP general error: {:?}", e),
            TcpError::Accept(e) => write!(f, "TCP accept error: {:?}", e),
            TcpError::NoBuffers => write!(f, "TCP no buffers available"),
            TcpError::UnsupportedProto => write!(f, "TCP unsupported protocol"),
        }
    }
}

impl core::error::Error for TcpError {}

impl embedded_io_async::Error for TcpError {
    fn kind(&self) -> ErrorKind {
        match self {
            TcpError::General(_) => ErrorKind::Other,
            TcpError::Accept(_) => ErrorKind::Other,
            TcpError::NoBuffers => ErrorKind::OutOfMemory,
            TcpError::UnsupportedProto => ErrorKind::InvalidInput,
        }
    }
}

/// A type that holds the TCP socket buffers.
#[derive(Debug)]
pub struct TcpSocketBuffers {
    /// A token used to identify the buffer in the pool.
    token: NonNull<u8>,
    /// The buffer for receiving data.
    rx_buf: NonNull<u8>,
    /// The buffer for transmitting data.
    tx_buf: NonNull<u8>,
    /// The length of the receive buffer.
    rx_buf_len: usize,
    /// The length of the transmit buffer.
    tx_buf_len: usize,
}

/// A type alias for a pool of TCP socket buffers.
pub type TcpBuffers<const N: usize, const TX_SZ: usize = 1024, const RX_SZ: usize = 1024> =
    Pool<([u8; TX_SZ], [u8; RX_SZ]), N>;

impl<const N: usize, const TX_SZ: usize, const RX_SZ: usize> SealedDynPool<TcpSocketBuffers>
    for TcpBuffers<N, TX_SZ, RX_SZ>
{
    fn alloc(&self) -> Option<TcpSocketBuffers> {
        let socket_buffers = Pool::alloc(self)?;

        // The buffers are addressed via raw pointers projected from the pool slot rather than via
        // references to the slot as a whole: a reference to the whole slot would alias - and under
        // the aliasing rules invalidate - the other buffer handed out from the same slot.
        //
        // SAFETY: `socket_buffers` points to a live slot of the pool.
        let (tx_buf, rx_buf) = unsafe {
            let slot = socket_buffers.as_ptr();

            (
                core::ptr::addr_of_mut!((*slot).0) as *mut u8,
                core::ptr::addr_of_mut!((*slot).1) as *mut u8,
            )
        };

        Some(TcpSocketBuffers {
            token: socket_buffers.cast::<u8>(),
            rx_buf: NonNull::new(rx_buf)?,
            tx_buf: NonNull::new(tx_buf)?,
            rx_buf_len: RX_SZ,
            tx_buf_len: TX_SZ,
        })
    }

    unsafe fn free(&self, buffer_token: NonNull<u8>) {
        unsafe {
            Pool::free(self, buffer_token.cast::<([u8; TX_SZ], [u8; RX_SZ])>());
        }
    }
}

impl<const N: usize, const TX_SZ: usize, const RX_SZ: usize> DynPool<TcpSocketBuffers>
    for TcpBuffers<N, TX_SZ, RX_SZ>
{
}
