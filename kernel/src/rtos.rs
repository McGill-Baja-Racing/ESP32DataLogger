//! FreeRTOS's API on top of esp-rtos.

use alloc::{boxed::Box, vec::Vec};
use core::{
    cell::{Cell, UnsafeCell},
    ffi::{c_int, c_void},
    mem::MaybeUninit,
};

use critical_section::{CriticalSection, Mutex, RestoreState};
use esp_radio_rtos_driver::{
    self as rtos,
    queue::QueueHandle,
    semaphore::{SemaphoreHandle, SemaphoreKind},
};
use macros::export;
use sys::{
    BaseType_t, TaskFunction_t, TaskHandle_t, TickType_t, UBaseType_t, configTICK_RATE_HZ,
    errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY, pdFALSE, pdPASS, pdTRUE,
};

const TICK_US: u64 = 1_000_000 / configTICK_RATE_HZ.unsigned_abs() as u64;

#[derive(Clone, Copy)]
pub struct Context(pub *mut c_void);

// SAFETY: the kernel never dereferences it; it only passes it back to C's callback.
unsafe impl Send for Context {}

/// When a wait of `ticks` ends, in esp-rtos time; `None` waits forever, like `portMAX_DELAY`.
/// FreeRTOS counts from the last tick, so a periodic `vTaskDelay(1)` loop stays on the tick.
pub fn deadline(ticks: TickType_t) -> Option<u64> {
    (ticks != TickType_t::MAX).then(|| (rtos::now() / TICK_US + u64::from(ticks)) * TICK_US)
}

/// When a wait of `timeout_ms` ends, in esp-rtos time; `-1` waits forever, like ESP-IDF's drivers.
pub fn deadline_ms(timeout_ms: c_int) -> Option<u64> {
    u64::try_from(timeout_ms)
        .ok()
        .map(|ms| rtos::now() + ms * 1000)
}

/// A semaphore that is given once and taken once.
pub fn binary_semaphore() -> SemaphoreHandle {
    SemaphoreHandle::new(SemaphoreKind::Counting { max: 1, initial: 0 })
}

/// Data that tasks use only while holding its mutex, which can span slow I/O unlike a critical
/// section.
pub struct Locked<T> {
    lock: SemaphoreHandle,
    data: UnsafeCell<T>,
}

// SAFETY: `with` and `with_deadline` are the only access to `data`, and they hold `lock`, so
// tasks take turns with a `T` they may move between them.
unsafe impl<T: Send> Sync for Locked<T> {}

/// Gives the lock back when dropped, so a panic or early return can't leak it.
struct Held<'a>(&'a SemaphoreHandle);

impl Drop for Held<'_> {
    fn drop(&mut self) {
        self.0.give();
    }
}

impl<T> Locked<T> {
    pub fn new(data: T) -> Self {
        Self {
            lock: SemaphoreHandle::new(SemaphoreKind::Mutex),
            data: UnsafeCell::new(data),
        }
    }

    /// Runs `f` on the data, waiting for the lock as long as it takes.
    pub fn with<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        self.lock.take(None);
        // SAFETY: the task holds `lock`.
        unsafe { self.run(f) }
    }

    /// Like `with`, but gives up and returns `None` when the lock isn't free by `deadline`.
    #[cfg_attr(
        esp32p4,
        expect(dead_code, reason = "only the C3's I2C driver waits for a deadline")
    )]
    pub fn with_deadline<R>(
        &self,
        deadline: Option<u64>,
        f: impl FnOnce(&mut T) -> R,
    ) -> Option<R> {
        // SAFETY: the task holds `lock` once `take_with_deadline` succeeds.
        self.lock
            .take_with_deadline(deadline)
            .then(|| unsafe { self.run(f) })
    }

    /// # Safety
    ///
    /// The calling task holds `lock`, which this gives back.
    unsafe fn run<R>(&self, f: impl FnOnce(&mut T) -> R) -> R {
        let _held = Held(&self.lock);
        // SAFETY: the caller holds `lock`, so this is the only access to `data`.
        f(unsafe { &mut *self.data.get() })
    }
}

/// A `&'static T` that is set after start-up and then read from tasks and interrupts.
pub struct Global<T: 'static>(Mutex<Cell<Option<&'static T>>>);

impl<T> Global<T> {
    pub const fn new() -> Self {
        Self(Mutex::new(Cell::new(None)))
    }

    pub fn get(&self) -> Option<&'static T> {
        critical_section::with(|cs| self.0.borrow(cs).get())
    }

    pub fn set(&self, value: &'static T) {
        critical_section::with(|cs| self.0.borrow(cs).set(Some(value)));
    }
}

/// What esp-rtos allocates besides the memory the caller asks for: a task's record and stack
/// guard, or a queue's or mutex's semaphores and wait queues.
const OVERHEAD: usize = 512;

/// Whether the heap has a free block of `bytes` and room for esp-rtos's own. esp-rtos panics when
/// an allocation fails, where FreeRTOS returns an error, so callers check first.
fn fits(bytes: usize) -> bool {
    bytes
        .checked_add(OVERHEAD)
        .is_some_and(|bytes| Vec::<u8>::new().try_reserve_exact(bytes).is_ok())
}

fn pd(ok: bool) -> BaseType_t {
    if ok { pdTRUE } else { pdFALSE }
}

/// What ESP-IDF's separate interrupt stack holds, which here lands on the interrupted task's
/// stack; the firmware's stack sizes leave it out.
pub const ISR_STACK: usize = 1536;

/// ESP-IDF gives the stack size in bytes, as esp-rtos takes it.
#[export]
#[expect(clippy::panic, reason = "FreeRTOS faults on a NULL task function too")]
unsafe fn xTaskCreatePinnedToCore(
    task_code: TaskFunction_t,
    name: &str,
    stack_depth: u32,
    param: *mut c_void,
    priority: UBaseType_t,
    created: Option<&mut MaybeUninit<TaskHandle_t>>,
    _core: BaseType_t,
) -> BaseType_t {
    let Some(function) = task_code else {
        panic!("NULL task_code");
    };
    let stack_size = stack_depth as usize + ISR_STACK;
    if !fits(stack_size) {
        return errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY;
    }
    // SAFETY: C asks for `function` to run with `param` in its own task.
    let function: extern "C" fn(*mut c_void) = unsafe { core::mem::transmute(function) };
    // SAFETY: C lets the new task use `param`.
    let task = unsafe { rtos::task_create(name, function, param, priority, None, stack_size) };
    if let Some(handle) = created {
        handle.write(task.as_ptr().cast());
    }
    pdPASS
}

#[export]
#[expect(
    clippy::cast_possible_truncation,
    reason = "wraps like FreeRTOS's tick count"
)]
fn xTaskGetTickCount() -> TickType_t {
    (rtos::now() / TICK_US) as TickType_t
}

/// Only a task deleting itself; esp-rtos never returns from that.
#[export]
fn vTaskDelete(task: TaskHandle_t) {
    assert!(
        task.is_null(),
        "the kernel supports only a task deleting itself"
    );
    crate::libc::forget_task();
    // SAFETY: `None` is the current task. The firmware never calls this from `app_main`, and
    // esp-rtos panics on deleting the main task.
    unsafe { rtos::schedule_task_deletion(None) };
}

#[export]
fn vTaskDelay(ticks: TickType_t) {
    if ticks == 0 {
        rtos::yield_task();
    } else {
        rtos::usleep_until(deadline(ticks).unwrap_or(u64::MAX));
    }
}

/// FreeRTOS's mutexes are queues too.
enum Queue {
    Items(Items),
    Mutex(SemaphoreHandle),
}

struct Items {
    handle: QueueHandle,
    item_size: usize,
    /// esp-rtos queues report only how many items they hold.
    length: usize,
}

sys::c_handle!(Queue, sys::QueueHandle_t);

impl Queue {
    #[expect(clippy::panic, reason = "a mutex used as a queue is a firmware bug")]
    fn items(&self) -> &Items {
        match self {
            Self::Items(items) => items,
            Self::Mutex(_) => panic!("a FreeRTOS mutex used as a queue"),
        }
    }
}

#[export]
fn xQueueGenericCreate(
    length: UBaseType_t,
    item_size: UBaseType_t,
    _queue_type: u8,
) -> Option<&'static Queue> {
    let (length, item_size) = (length as usize, item_size as usize);
    if !length.checked_mul(item_size).is_some_and(fits) {
        return None;
    }
    let new = Queue::Items(Items {
        handle: QueueHandle::new(length, item_size),
        item_size,
        length,
    });
    Some(Box::leak(Box::new(new)))
}

/// `unsupported.h` rejects recursive mutexes, the other type.
#[export]
fn xQueueCreateMutex(_queue_type: u8) -> Option<&'static Queue> {
    fits(0).then(|| {
        let mutex = Queue::Mutex(SemaphoreHandle::new(SemaphoreKind::Mutex));
        &*Box::leak(Box::new(mutex))
    })
}

#[export]
#[expect(
    clippy::panic,
    reason = "`unsupported.h` rejects the other semaphores, so only a firmware bug gets here"
)]
fn xQueueSemaphoreTake(queue: &Queue, ticks: TickType_t) -> BaseType_t {
    let Queue::Mutex(mutex) = queue else {
        panic!("the kernel supports only mutexes as semaphores");
    };
    pd(mutex.take_with_deadline(deadline(ticks)))
}

#[export]
unsafe fn xQueueGenericSend(
    queue: &Queue,
    item: *const c_void,
    ticks: TickType_t,
    _position: BaseType_t,
) -> BaseType_t {
    let sent = match queue {
        Queue::Mutex(mutex) => {
            assert!(mutex.give(), "gave a mutex its task doesn't hold");
            true
        }
        // SAFETY: `item` points to one item of the queue's item size.
        Queue::Items(items) => unsafe {
            items
                .handle
                .send_to_back_with_deadline(item.cast(), deadline(ticks))
        },
    };
    pd(sent)
}

/// esp-rtos switches to a woken task when the interrupt returns, so `woken` stays unset.
#[export]
unsafe fn xQueueGenericSendFromISR(
    queue: &Queue,
    item: *const c_void,
    _woken: Option<&mut BaseType_t>,
    _position: BaseType_t,
) -> BaseType_t {
    // SAFETY: `item` points to one item of the queue's item size.
    let sent = unsafe {
        queue
            .items()
            .handle
            .try_send_to_back_from_isr(item.cast(), None)
    };
    pd(sent)
}

#[export]
fn vPortYieldFromISR() {
    rtos::yield_task_from_isr();
}

#[export]
unsafe fn xQueueReceive(queue: &Queue, buffer: *mut c_void, ticks: TickType_t) -> BaseType_t {
    // SAFETY: `buffer` has room for one item of the queue's item size.
    let received = unsafe {
        queue
            .items()
            .handle
            .receive_with_deadline(buffer.cast(), deadline(ticks))
    };
    pd(received)
}

#[export]
#[expect(
    clippy::cast_possible_truncation,
    reason = "at most the `UBaseType_t` length C created the queue with"
)]
fn uxQueueSpacesAvailable(queue: &Queue) -> UBaseType_t {
    // Not atomic with a receive in flight: it can count one slot free early.
    let items = queue.items();
    items.length.saturating_sub(items.handle.messages_waiting()) as UBaseType_t
}

#[export]
#[expect(
    clippy::cast_possible_truncation,
    reason = "at most the `UBaseType_t` length C created the queue with"
)]
fn uxQueueMessagesWaiting(queue: &Queue) -> UBaseType_t {
    queue.items().handle.messages_waiting() as UBaseType_t
}

/// The largest item the firmware queues.
const MAX_RESET_ITEM: usize = 32;

/// esp-rtos queues have no reset, so this drains `queue`.
#[export]
fn xQueueGenericReset(queue: &Queue, _new_queue: BaseType_t) -> BaseType_t {
    let items = queue.items();
    let mut item = [0_u8; MAX_RESET_ITEM];
    assert!(
        items.item_size <= item.len(),
        "queue items fit the reset buffer"
    );
    // Never waits: the firmware resets queues inside portENTER_CRITICAL.
    // SAFETY: `item` holds one item.
    while unsafe { items.handle.try_receive_from_isr(item.as_mut_ptr(), None) } {}
    pdPASS
}

/// How deep the open critical sections nest, and the interrupt state the outermost one restores.
/// esp-hal's critical sections are reentrant, so only the outermost needs releasing.
#[derive(Clone, Copy)]
struct Nesting {
    depth: u32,
    outer: RestoreState,
}

static NESTING: Mutex<Cell<Nesting>> = Mutex::new(Cell::new(Nesting {
    depth: 0,
    outer: RestoreState::invalid(),
}));

#[export]
unsafe fn vPortEnterCritical() {
    // SAFETY: C pairs every enter with an exit, which releases the outermost one.
    let restore = unsafe { critical_section::acquire() };
    // SAFETY: interrupts stay off until the matching exit.
    let nesting = NESTING.borrow(unsafe { CriticalSection::new() });
    let Nesting { depth, outer } = nesting.get();
    nesting.set(Nesting {
        depth: depth + 1,
        outer: if depth == 0 { restore } else { outer },
    });
}

#[export]
unsafe fn vPortExitCritical() {
    // SAFETY: called inside the critical section its matching enter opened.
    let nesting = NESTING.borrow(unsafe { CriticalSection::new() });
    let Nesting { depth, outer } = nesting.get();
    assert!(depth > 0, "vPortExitCritical without vPortEnterCritical");
    nesting.set(Nesting {
        depth: depth - 1,
        outer,
    });
    if depth == 1 {
        // SAFETY: `outer` came from the outermost `acquire`.
        unsafe { critical_section::release(outer) };
    }
}
