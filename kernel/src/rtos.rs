//! Layer 1: FreeRTOS's API on top of esp-rtos.

use alloc::{boxed::Box, vec, vec::Vec};
use core::{cell::Cell, ffi::c_void, mem::MaybeUninit};

use critical_section::{CriticalSection, Mutex, RestoreState};
use esp_radio_rtos_driver::{
    self as rtos,
    queue::QueueHandle,
    semaphore::{SemaphoreHandle, SemaphoreKind},
};
use macros::export;
use sys::{
    BaseType_t, TaskFunction_t, TaskHandle_t, TickType_t, UBaseType_t, configTICK_RATE_HZ,
    errCOULD_NOT_ALLOCATE_REQUIRED_MEMORY, errQUEUE_FULL, pdFAIL, pdFALSE, pdPASS, pdTRUE,
};

const TICK_US: u64 = 1_000_000 / configTICK_RATE_HZ.unsigned_abs() as u64;

/// When a wait of `ticks` ends, in esp-rtos time; `None` waits forever, like `portMAX_DELAY`.
/// FreeRTOS counts from the last tick, so a periodic `vTaskDelay(1)` loop stays on the tick.
fn deadline(ticks: TickType_t) -> Option<u64> {
    (ticks != TickType_t::MAX).then(|| (rtos::now() / TICK_US + u64::from(ticks)) * TICK_US)
}

/// Whether the heap has a free block of `bytes`. esp-rtos panics when an allocation fails, where
/// FreeRTOS returns an error, so callers check first; its own small bookkeeping isn't counted.
fn fits(bytes: usize) -> bool {
    Vec::<u8>::new().try_reserve_exact(bytes).is_ok()
}

/// ESP-IDF gives the stack size in bytes, as esp-rtos takes it. A task that returns is deleted,
/// where ESP-IDF aborts.
#[export]
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
        return pdFAIL;
    };
    let stack_size = stack_depth as usize;
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
    Items {
        handle: QueueHandle,
        /// For `xQueueGenericReset`.
        item_size: usize,
    },
    Mutex(SemaphoreHandle),
}

sys::c_handle!(Queue, QueueHandle_t);

impl Queue {
    #[expect(clippy::panic, reason = "a mutex used as a queue is a firmware bug")]
    fn items(&self) -> (&QueueHandle, usize) {
        match self {
            Self::Items { handle, item_size } => (handle, *item_size),
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
    let new = Queue::Items {
        handle: QueueHandle::new(length, item_size),
        item_size,
    };
    Some(Box::leak(Box::new(new)))
}

/// `unsupported.h` rejects recursive mutexes, the other type.
#[export]
fn xQueueCreateMutex(_queue_type: u8) -> &'static Queue {
    Box::leak(Box::new(Queue::Mutex(SemaphoreHandle::new(
        SemaphoreKind::Mutex,
    ))))
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
    let taken = mutex.take_with_deadline(deadline(ticks));
    if taken { pdTRUE } else { pdFALSE }
}

#[export]
unsafe fn xQueueGenericSend(
    queue: &Queue,
    item: *const c_void,
    ticks: TickType_t,
    _position: BaseType_t,
) -> BaseType_t {
    let sent = match queue {
        Queue::Mutex(mutex) => mutex.give(),
        // SAFETY: `item` points to one item of the queue's item size.
        Queue::Items { handle, .. } => unsafe {
            handle.send_to_back_with_deadline(item.cast(), deadline(ticks))
        },
    };
    if sent { pdPASS } else { errQUEUE_FULL }
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
    let sent = unsafe { queue.items().0.try_send_to_back_from_isr(item.cast(), None) };
    if sent { pdPASS } else { errQUEUE_FULL }
}

#[export]
unsafe fn xQueueReceive(queue: &Queue, buffer: *mut c_void, ticks: TickType_t) -> BaseType_t {
    // SAFETY: `buffer` has room for one item of the queue's item size.
    let received = unsafe {
        queue
            .items()
            .0
            .receive_with_deadline(buffer.cast(), deadline(ticks))
    };
    if received { pdTRUE } else { pdFALSE }
}

/// esp-rtos queues have no reset, so this drains `queue`.
#[export]
fn xQueueGenericReset(queue: &Queue, _new_queue: BaseType_t) -> BaseType_t {
    let (handle, item_size) = queue.items();
    let mut item = vec![0_u8; item_size];
    // SAFETY: `item` holds one item; a zero timeout never blocks.
    while unsafe { handle.receive(item.as_mut_ptr(), Some(0)) } {}
    pdPASS
}

/// How deep the open critical sections nest, and the interrupt state the outermost one restores.
/// esp-hal's critical sections are reentrant, so only the outermost needs releasing.
static NESTING: Mutex<Cell<(u32, RestoreState)>> =
    Mutex::new(Cell::new((0, RestoreState::invalid())));

#[export]
unsafe fn vPortEnterCritical() {
    // SAFETY: C pairs every enter with an exit, which releases the outermost one.
    let restore = unsafe { critical_section::acquire() };
    // SAFETY: interrupts stay off until the matching exit.
    let nesting = NESTING.borrow(unsafe { CriticalSection::new() });
    let (depth, outer) = nesting.get();
    nesting.set((depth + 1, if depth == 0 { restore } else { outer }));
}

#[export]
unsafe fn vPortExitCritical() {
    // SAFETY: called inside the critical section its matching enter opened.
    let nesting = NESTING.borrow(unsafe { CriticalSection::new() });
    let (depth, outer) = nesting.get();
    assert!(depth > 0, "vPortExitCritical without vPortEnterCritical");
    nesting.set((depth - 1, outer));
    if depth == 1 {
        // SAFETY: `outer` came from the outermost `acquire`.
        unsafe { critical_section::release(outer) };
    }
}
