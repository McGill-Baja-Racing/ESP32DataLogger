//! ESP-IDF's HTTP server API on edge-http. The network task accepts the connections and moves the
//! bytes. C's handlers block, so they run one request at a time on a task of their own, and talk
//! to the network task through `Exchange`s: messages the handler posts and the network task sends.

use alloc::{boxed::Box, collections::VecDeque, string::String, sync::Arc, vec::Vec};
use core::{
    cell::RefCell,
    ffi::{CStr, c_char, c_void},
    fmt::{Debug, Display},
    mem::{MaybeUninit, replace, transmute},
    net::{IpAddr, Ipv4Addr, SocketAddr},
    ptr, slice,
    sync::atomic::{AtomicBool, Ordering},
};

use critical_section::Mutex;
use edge_http::{
    Method,
    io::{
        Error,
        server::{Connection, Handler, Server},
    },
};
use edge_nal::{TcpBind, TcpSplit, WithTimeout};
use edge_nal_embassy::Tcp;
use embassy_sync::{blocking_mutex::raw::CriticalSectionRawMutex, signal::Signal};
use embedded_io_async::{Read, Write};
use esp_radio_rtos_driver::{
    self as rtos,
    semaphore::{SemaphoreHandle, SemaphoreKind},
};
use macros::export;

use crate::rtos::ISR_STACK;
use sys::{
    ESP_ERR_HTTPD_HANDLER_EXISTS, ESP_ERR_HTTPD_HANDLERS_FULL, ESP_ERR_HTTPD_RESP_HDR,
    ESP_ERR_HTTPD_RESP_SEND, ESP_ERR_HTTPD_RESULT_TRUNC, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND,
    ESP_OK, esp_err_t, httpd_config_t, httpd_method_t, httpd_req_t, httpd_uri_t,
};

/// Sockets serving at once; an idle client holds one until it closes or the I/O times out.
pub const CONNECTIONS: usize = 4;
/// The sockets' send and receive buffers.
pub const TX: usize = 2048;
pub const RX: usize = 1024;
/// A request's headers must fit in this.
const REQUEST: usize = 1024;
const HEADERS: usize = 24;
/// How long a socket may take to move a byte, like ESP-IDF's default.
const IO_TIMEOUT_MS: u32 = 5000;
/// The longest URI, as in ESP-IDF.
const URI_MAX: usize = 512;
/// How many headers a response may set, as in ESP-IDF's default.
const RESPONSE_HEADERS: usize = 8;

type Handle = unsafe extern "C" fn(*mut httpd_req_t) -> esp_err_t;

struct Uri {
    path: String,
    method: httpd_method_t,
    handler: Handle,
}

/// The server `httpd_start` makes, which `httpd_handle_t` stands for.
struct Dispatch {
    port: u16,
    max_uris: usize,
    uris: Mutex<RefCell<Vec<Uri>>>,
    /// Requests for the handler task, which `arrived` counts.
    pending: Mutex<RefCell<VecDeque<Arc<Exchange>>>>,
    arrived: SemaphoreHandle,
}

static STARTED: AtomicBool = AtomicBool::new(false);

/// `httpd_start`'s server, for the network task to listen for.
static LISTEN: Signal<CriticalSectionRawMutex, &'static Dispatch> = Signal::new();

enum Message<'a> {
    Head {
        status: &'a str,
        headers: &'a [(&'a str, &'a str)],
    },
    Body(&'a [u8]),
    /// The handler returned; whether it returned `ESP_OK`.
    Done(bool),
}

/// One request, and what its handler says in answer.
struct Exchange {
    handler: Handle,
    method: httpd_method_t,
    uri: String,
    message: Signal<CriticalSectionRawMutex, Message<'static>>,
    /// Given when `message` has been sent, for the handler to wait on: it can't await.
    sent: SemaphoreHandle,
    /// Set when the connection broke or was dropped, after which nothing is sent.
    failed: AtomicBool,
}

impl Exchange {
    /// Hands `message` to the network task and, except for the last, waits until it is sent, so
    /// that what it borrows from the handler stays valid.
    fn post(&self, message: Message<'_>) -> Result<(), esp_err_t> {
        if self.failed.load(Ordering::Acquire) {
            return Err(ESP_ERR_HTTPD_RESP_SEND);
        }
        let last = matches!(message, Message::Done(_));
        // SAFETY: the borrow ends before this returns: `forward` gives `sent` once it has sent the
        // message, and its guard gives it when `forward` is dropped, after which it reads nothing.
        let message = unsafe { transmute::<Message<'_>, Message<'static>>(message) };
        self.message.signal(message);
        if last {
            return Ok(());
        }
        self.sent.take(None);
        if self.failed.load(Ordering::Acquire) {
            return Err(ESP_ERR_HTTPD_RESP_SEND);
        }
        Ok(())
    }
}

/// C's strings stay valid until the response is sent, as ESP-IDF stores them without a copy.
fn keep(text: &str) -> &'static str {
    // SAFETY: `Response` holds the result only while the handler runs, which C's strings outlive.
    unsafe { transmute::<&str, &'static str>(text) }
}

/// The response a handler is building, which `httpd_req_t::aux` points to.
struct Response {
    exchange: Arc<Exchange>,
    status: &'static str,
    content_type: &'static str,
    headers: Vec<(&'static str, &'static str)>,
    started: bool,
}

impl Response {
    /// Sends the status and headers, with the body's length or chunked.
    fn start(&mut self, length: Option<usize>) -> Result<(), esp_err_t> {
        if replace(&mut self.started, true) {
            return Ok(());
        }
        let length = length.map(|length| alloc::format!("{length}"));
        let mut headers = Vec::with_capacity(RESPONSE_HEADERS + 2);
        headers.push(("Content-Type", self.content_type));
        headers.extend_from_slice(&self.headers);
        if let Some(length) = &length {
            headers.push(("Content-Length", length));
        }
        self.exchange.post(Message::Head {
            status: self.status,
            headers: &headers,
        })
    }

    fn body(&self, data: &[u8]) -> Result<(), esp_err_t> {
        if data.is_empty() {
            return Ok(());
        }
        self.exchange.post(Message::Body(data))
    }
}

fn response(request: &mut httpd_req_t) -> &mut Response {
    // SAFETY: `respond` points `aux` to a `Response` that outlives the handler, which is the only
    // user of it.
    unsafe { &mut *request.aux.cast::<Response>() }
}

#[export]
fn httpd_start(
    handle: &mut MaybeUninit<*mut c_void>,
    config: &httpd_config_t,
) -> Result<(), esp_err_t> {
    assert!(
        !STARTED.swap(true, Ordering::Relaxed),
        "the kernel starts the HTTP server once"
    );
    let dispatch = &*Box::leak(Box::new(Dispatch {
        port: config.server_port,
        max_uris: usize::from(config.max_uri_handlers),
        uris: Mutex::new(RefCell::new(Vec::new())),
        pending: Mutex::new(RefCell::new(VecDeque::with_capacity(CONNECTIONS))),
        arrived: SemaphoreHandle::new(SemaphoreKind::Counting {
            max: u32::MAX,
            initial: 0,
        }),
    }));
    let dispatch_ptr = ptr::from_ref(dispatch).cast_mut().cast();
    // SAFETY: the task only uses the leaked `Dispatch`.
    unsafe {
        rtos::task_create(
            "httpd",
            run_handlers,
            dispatch_ptr,
            config.task_priority,
            None,
            config.stack_size + ISR_STACK,
        );
    }
    LISTEN.signal(dispatch);
    handle.write(dispatch_ptr);
    Ok(())
}

/// The server a handle from `httpd_start` stands for.
fn dispatch<'a>(handle: *mut c_void) -> Result<&'a Dispatch, esp_err_t> {
    // SAFETY: `httpd_start` leaked the `Dispatch` its handles point to.
    unsafe { handle.cast::<Dispatch>().as_ref() }.ok_or(ESP_ERR_INVALID_ARG)
}

/// Only unregisters the handlers: the listener and the handler task keep running, and answer 404.
#[export]
fn httpd_stop(handle: *mut c_void) -> Result<(), esp_err_t> {
    let dispatch = dispatch(handle)?;
    critical_section::with(|cs| dispatch.uris.borrow_ref_mut(cs).clear());
    Ok(())
}

#[export]
fn httpd_register_uri_handler(
    handle: *mut c_void,
    uri_handler: &httpd_uri_t,
) -> Result<(), esp_err_t> {
    let dispatch = dispatch(handle)?;
    if uri_handler.uri.is_null() {
        return Err(ESP_ERR_INVALID_ARG);
    }
    // SAFETY: C passes a NUL-terminated URI.
    let path = unsafe { CStr::from_ptr(uri_handler.uri) }
        .to_str()
        .map_err(|_utf8| ESP_ERR_INVALID_ARG)?;
    let uri = Uri {
        path: path.into(),
        method: uri_handler.method,
        handler: uri_handler.handler.ok_or(ESP_ERR_INVALID_ARG)?,
    };
    critical_section::with(|cs| {
        let mut uris = dispatch.uris.borrow_ref_mut(cs);
        if uris
            .iter()
            .any(|known| known.path == uri.path && known.method == uri.method)
        {
            return Err(ESP_ERR_HTTPD_HANDLER_EXISTS);
        }
        if uris.len() >= dispatch.max_uris {
            return Err(ESP_ERR_HTTPD_HANDLERS_FULL);
        }
        uris.push(uri);
        Ok(())
    })
}

/// Runs C's handlers, one request at a time.
extern "C" fn run_handlers(dispatch: *mut c_void) {
    // SAFETY: `httpd_start` leaked this `Dispatch`.
    let dispatch = unsafe { &*dispatch.cast::<Dispatch>() };
    loop {
        dispatch.arrived.take(None);
        let exchange = critical_section::with(|cs| dispatch.pending.borrow_ref_mut(cs).pop_front());
        // A client that left while queued doesn't run its handler.
        if let Some(exchange) = exchange
            && !exchange.failed.load(Ordering::Acquire)
        {
            respond(dispatch, &exchange);
        }
    }
}

fn respond(dispatch: &Dispatch, exchange: &Arc<Exchange>) {
    // SAFETY: all-zero is a valid `httpd_req_t`: null pointers and zero counts.
    let mut request: httpd_req_t = unsafe { core::mem::zeroed() };
    request.handle = ptr::from_ref(dispatch).cast_mut().cast();
    request.method = exchange.method.0.cast_signed();
    // The last byte stays NUL.
    let room = request.uri.len() - 1;
    for (to, from) in request.uri.iter_mut().take(room).zip(exchange.uri.bytes()) {
        *to = from;
    }
    let mut response = Response {
        exchange: Arc::clone(exchange),
        status: "200 OK",
        content_type: "text/html",
        headers: Vec::new(),
        started: false,
    };
    request.aux = ptr::from_mut(&mut response).cast();
    // SAFETY: C registered `handler` for this request, and `request` and `response` outlive it.
    let result = unsafe { (exchange.handler)(&raw mut request) };
    exchange.post(Message::Done(result == ESP_OK)).ok();
}

#[export]
fn httpd_resp_set_status(r: &mut httpd_req_t, status: &str) -> Result<(), esp_err_t> {
    response(r).status = keep(status);
    Ok(())
}

#[export]
fn httpd_resp_set_type(r: &mut httpd_req_t, content_type: &str) -> Result<(), esp_err_t> {
    response(r).content_type = keep(content_type);
    Ok(())
}

#[export]
fn httpd_resp_set_hdr(r: &mut httpd_req_t, field: &str, value: &str) -> Result<(), esp_err_t> {
    let headers = &mut response(r).headers;
    if headers.len() >= RESPONSE_HEADERS {
        return Err(ESP_ERR_HTTPD_RESP_HDR);
    }
    headers.push((keep(field), keep(value)));
    Ok(())
}

/// The bytes C passes with a length, or with `HTTPD_RESP_USE_STRLEN`.
///
/// # Safety
///
/// `buf` points to `buf_len` bytes, or to a NUL-terminated string if `buf_len` is -1.
unsafe fn bytes<'a>(buf: *const c_char, buf_len: isize) -> &'a [u8] {
    match usize::try_from(buf_len) {
        Ok(0) => &[],
        // SAFETY: as the caller promises.
        Ok(length) => unsafe { slice::from_raw_parts(buf.cast(), length) },
        // SAFETY: as the caller promises.
        Err(_use_strlen) => unsafe { CStr::from_ptr(buf) }.to_bytes(),
    }
}

#[export]
unsafe fn httpd_resp_send(
    r: &mut httpd_req_t,
    buf: *const c_char,
    buf_len: isize,
) -> Result<(), esp_err_t> {
    // SAFETY: C passes `buf_len` bytes at `buf`, or a string.
    let data = unsafe { bytes(buf, buf_len) };
    let response = response(r);
    response.start(Some(data.len()))?;
    response.body(data)
}

/// An empty chunk ends the response, which the handler returning completes.
#[export]
unsafe fn httpd_resp_send_chunk(
    r: &mut httpd_req_t,
    buf: *const c_char,
    buf_len: isize,
) -> Result<(), esp_err_t> {
    // SAFETY: C passes `buf_len` bytes at `buf`, or a string.
    let data = unsafe { bytes(buf, buf_len) };
    let response = response(r);
    response.start(None)?;
    response.body(data)
}

/// Copies `text` and a NUL to C's buffer of `size` bytes, or none of it if it doesn't fit.
fn copy(text: &str, buffer: *mut c_char, size: usize) -> Result<(), esp_err_t> {
    if text.len() >= size {
        return Err(ESP_ERR_HTTPD_RESULT_TRUNC);
    }
    // SAFETY: C gives room for `size` bytes at `buffer`, and the text and its NUL are fewer.
    let out = unsafe { slice::from_raw_parts_mut(buffer.cast::<u8>(), text.len() + 1) };
    for (to, from) in out.iter_mut().zip(text.bytes().chain([0])) {
        *to = from;
    }
    Ok(())
}

#[export]
fn httpd_req_get_url_query_str(
    r: &mut httpd_req_t,
    buf: *mut c_char,
    buf_len: usize,
) -> Result<(), esp_err_t> {
    let (_path, query) = response(r)
        .exchange
        .uri
        .split_once('?')
        .filter(|(_path, query)| !query.is_empty())
        .ok_or(ESP_ERR_NOT_FOUND)?;
    copy(query, buf, buf_len)
}

/// Keys match in any case, as in ESP-IDF; nothing is URL-decoded.
#[export]
fn httpd_query_key_value(
    qry: &str,
    key: &str,
    val: *mut c_char,
    val_size: usize,
) -> Result<(), esp_err_t> {
    let value = qry
        .split('&')
        .find_map(|pair| {
            let (name, value) = pair.split_once('=')?;
            name.eq_ignore_ascii_case(key).then_some(value)
        })
        .ok_or(ESP_ERR_NOT_FOUND)?;
    copy(value, val, val_size)
}

#[derive(Clone, Copy)]
struct Router(&'static Dispatch);

impl Router {
    /// The handler for a request, or the status that rejects it.
    fn find(self, uri: &str, method: Method) -> Result<(Handle, httpd_method_t), u16> {
        if uri.len() > URI_MAX {
            return Err(414);
        }
        let route = uri.split_once('?').map_or(uri, |(route, _query)| route);
        let method = match method {
            Method::Get => Some(httpd_method_t::HTTP_GET),
            Method::Post => Some(httpd_method_t::HTTP_POST),
            _ => None,
        };
        critical_section::with(|cs| {
            let uris = self.0.uris.borrow_ref(cs);
            let mut routes = uris.iter().filter(|uri| uri.path == route).peekable();
            if routes.peek().is_none() {
                return Err(404);
            }
            routes
                .find(|uri| Some(uri.method) == method)
                .map(|uri| (uri.handler, uri.method))
                .ok_or(405)
        })
    }
}

/// Sends the handler's messages to the client, and returns an error to close the connection.
async fn forward<T: Read + Write + TcpSplit, const N: usize>(
    connection: &mut Connection<'_, T, N>,
    exchange: &Exchange,
) -> Result<(), Error<T::Error>> {
    /// Frees a handler that waits for this future, however it ends.
    struct Release<'a>(&'a Exchange);

    impl Drop for Release<'_> {
        fn drop(&mut self) {
            self.0.failed.store(true, Ordering::Release);
            self.0.sent.give();
        }
    }

    let _release = Release(exchange);
    loop {
        match exchange.message.wait().await {
            Message::Head { status, headers } => {
                let (code, reason) = status.split_once(' ').unwrap_or((status, ""));
                let code = code.parse().map_err(|_code| Error::InvalidState)?;
                connection
                    .initiate_response(code, Some(reason), headers)
                    .await?;
            }
            Message::Body(data) => connection.write_all(data).await?,
            Message::Done(true) => return Ok(()),
            Message::Done(false) => return Err(Error::ConnectionClosed),
        }
        exchange.sent.give();
    }
}

impl Handler for Router {
    type Error<E: Debug> = Error<E>;

    async fn handle<T, const N: usize>(
        &self,
        _task_id: impl Display + Copy,
        connection: &mut Connection<'_, T, N>,
    ) -> Result<(), Self::Error<T::Error>>
    where
        T: Read + Write + TcpSplit,
    {
        let headers = connection.headers()?;
        let found = self.find(headers.path, headers.method);
        let (handler, method) = match found {
            Ok(found) => found,
            Err(status) => {
                connection
                    .initiate_response(status, None, &[("Content-Length", "0")])
                    .await?;
                return Ok(());
            }
        };
        let exchange = Arc::new(Exchange {
            handler,
            method,
            uri: headers.path.into(),
            message: Signal::new(),
            sent: SemaphoreHandle::new(SemaphoreKind::Counting { max: 1, initial: 0 }),
            failed: AtomicBool::new(false),
        });
        critical_section::with(|cs| {
            self.0
                .pending
                .borrow_ref_mut(cs)
                .push_back(Arc::clone(&exchange));
        });
        self.0.arrived.give();
        forward(connection, &exchange).await
    }
}

/// Listens on the port `httpd_start` chose, once it has been called.
pub async fn serve(tcp: Tcp<'_>) {
    let dispatch = LISTEN.wait().await;
    // The network task serves again after the coprocessor resets.
    LISTEN.signal(dispatch);
    let local = SocketAddr::new(IpAddr::V4(Ipv4Addr::UNSPECIFIED), dispatch.port);
    let Ok(acceptor) = tcp.bind(local).await else {
        return;
    };
    Server::<CONNECTIONS, REQUEST, HEADERS>::new()
        .run(
            None,
            WithTimeout::new(IO_TIMEOUT_MS, acceptor),
            Router(dispatch),
        )
        .await
        .ok();
}
