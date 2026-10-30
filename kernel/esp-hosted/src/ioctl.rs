use alloc::vec::Vec;
use core::cell::RefCell;
use core::future::{Future, poll_fn};
use core::mem::take;
use core::task::Poll;

use embassy_sync::waitqueue::WakerRegistration;

enum Ioctl {
    Idle,
    Pending(Vec<u8>),
    Sent,
    Done(Vec<u8>),
}

pub struct Shared(RefCell<SharedInner>);

struct SharedInner {
    ioctl: Ioctl,
    ready: bool,
    control_waker: WakerRegistration,
    runner_waker: WakerRegistration,
}

impl Shared {
    pub const fn new() -> Self {
        Self(RefCell::new(SharedInner {
            ioctl: Ioctl::Idle,
            ready: false,
            control_waker: WakerRegistration::new(),
            runner_waker: WakerRegistration::new(),
        }))
    }

    pub fn ioctl(&self, request: Vec<u8>) -> impl Future<Output = Vec<u8>> + '_ {
        {
            let mut this = self.0.borrow_mut();
            this.ioctl = Ioctl::Pending(request);
            this.runner_waker.wake();
        }
        poll_fn(|cx| {
            let mut this = self.0.borrow_mut();
            if let Ioctl::Done(response) = &mut this.ioctl {
                let response = take(response);
                this.ioctl = Ioctl::Idle;
                Poll::Ready(response)
            } else {
                this.control_waker.register(cx.waker());
                Poll::Pending
            }
        })
    }

    pub fn ioctl_wait_pending(&self) -> impl Future<Output = Vec<u8>> + '_ {
        poll_fn(|cx| {
            let mut this = self.0.borrow_mut();
            if let Ioctl::Pending(request) = &mut this.ioctl {
                let request = take(request);
                this.ioctl = Ioctl::Sent;
                Poll::Ready(request)
            } else {
                this.runner_waker.register(cx.waker());
                Poll::Pending
            }
        })
    }

    /// Drops a response nobody waits for.
    pub fn ioctl_done(&self, response: &[u8]) {
        let mut this = self.0.borrow_mut();
        if matches!(this.ioctl, Ioctl::Sent) {
            this.ioctl = Ioctl::Done(response.to_vec());
            this.control_waker.wake();
        }
    }

    pub fn ready(&self) -> bool {
        self.0.borrow().ready
    }

    pub fn set_ready(&self) {
        let mut this = self.0.borrow_mut();
        this.ready = true;
        this.control_waker.wake();
    }

    pub fn init_wait(&self) -> impl Future<Output = ()> + '_ {
        poll_fn(|cx| {
            let mut this = self.0.borrow_mut();
            if this.ready {
                Poll::Ready(())
            } else {
                this.control_waker.register(cx.waker());
                Poll::Pending
            }
        })
    }
}
