//! Layer 1: FreeRTOS's API on top of esp-rtos.

use alloc::{boxed::Box, vec};
use core::{cell::Cell, ffi::c_void, mem::MaybeUninit};

use critical_section::{CriticalSection, Mutex, RestoreState};
use esp_radio_rtos_driver::{self as rtos, queue::QueueHandle};
use macros::export;
use sys::{
    BaseType_t, TaskFunction_t, TaskHandle_t, TickType_t, UBaseType_t, configTICK_RATE_HZ,
    errQUEUE_FULL, pdFAIL, pdFALSE, pdPASS, pdTRUE, queueQUEUE_TYPE_BASE, queueSEND_TO_BACK,
    queueSEND_TO_FRONT, tskNO_AFFINITY,
};

#[expect(
    clippy::integer_division,
    clippy::as_conversions,
    reason = "ESP-IDF's tick rates are positive and divide one second exactly"
)]
const TICK_US: u64 = 1_000_000 / configTICK_RATE_HZ.unsigned_abs() as u64;

/// When a wait of `ticks` ends, in esp-rtos time; `None` waits forever, like `portMAX_DELAY`.
fn deadline(ticks: TickType_t) -> Option<u64> {
    (ticks != TickType_t::MAX).then(|| after(ticks))
}

/// The esp-rtos time `ticks` ticks after the last one; FreeRTOS counts waits from the last tick.
#[expect(clippy::integer_division, reason = "rounds down to the last tick")]
fn after(ticks: TickType_t) -> u64 {
    (rtos::now() / TICK_US)
        .saturating_add(u64::from(ticks))
        .saturating_mul(TICK_US)
}

/// A C task function and the parameter C asked to run it with.
type Entry = (unsafe extern "C" fn(*mut c_void), *mut c_void);

/// Every task starts here, so a returning task panics like ESP-IDF instead of esp-rtos deleting it.
extern "C" fn run(entry: *mut c_void) {
    // SAFETY: `entry` is the `Box<Entry>` that `xTaskCreatePinnedToCore` leaked for this task alone.
    let (function, param) = *unsafe { Box::from_raw(entry.cast::<Entry>()) };
    // SAFETY: C asked for `function` to run with `param` in its own task.
    unsafe { function(param) };
    #[expect(clippy::panic, reason = "same failure as ESP-IDF")]
    {
        panic!("a FreeRTOS task returned");
    }
}

/// ESP-IDF gives the stack size in bytes, as esp-rtos takes it.
#[export]
unsafe fn xTaskCreatePinnedToCore(
    task_code: TaskFunction_t,
    name: &str,
    stack_depth: u32,
    param: *mut c_void,
    priority: UBaseType_t,
    created: Option<&mut MaybeUninit<TaskHandle_t>>,
    core: BaseType_t,
) -> BaseType_t {
    let (Some(function), Ok(stack_size)) = (task_code, usize::try_from(stack_depth)) else {
        return pdFAIL;
    };
    let pinned = u32::try_from(core).ok().filter(|_| core != tskNO_AFFINITY);
    let entry: *mut Entry = Box::into_raw(Box::new((function, param)));
    // SAFETY: `run` takes ownership of `entry`, and C lets the new task use `param`.
    let task = unsafe { rtos::task_create(name, run, entry.cast(), priority, pinned, stack_size) };
    if let Some(handle) = created {
        handle.write(task.as_ptr().cast());
    }
    pdPASS
}

#[export]
fn vTaskDelay(ticks: TickType_t) {
    if ticks == 0 {
        rtos::yield_task();
    } else {
        rtos::usleep_until(after(ticks));
    }
}

/// What a C `QueueHandle_t` points to.
struct Queue {
    handle: QueueHandle,
    /// esp-rtos queues have no reset, so `xQueueGenericReset` drains them item by item.
    item_size: usize,
}

// C shares one handle between tasks.
impl sys::CPointer for Queue
where
    Self: Sync,
{
    type Shared = sys::QueueHandle_t;
    type Unique = sys::QueueHandle_t;
}

#[export]
fn xQueueGenericCreate(
    length: UBaseType_t,
    item_size: UBaseType_t,
    queue_type: u8,
) -> Option<&'static Queue> {
    assert_eq!(
        i32::from(queue_type),
        queueQUEUE_TYPE_BASE,
        "only plain queues are supported, not semaphores"
    );
    let (Ok(capacity), Ok(size)) = (usize::try_from(length), usize::try_from(item_size)) else {
        return None;
    };
    let new = Queue {
        handle: QueueHandle::new(capacity, size),
        item_size: size,
    };
    Some(Box::leak(Box::new(new)))
}

#[export]
unsafe fn xQueueGenericSend(
    queue: &Queue,
    item: *const c_void,
    ticks: TickType_t,
    position: BaseType_t,
) -> BaseType_t {
    let target = &queue.handle;
    let until = deadline(ticks);
    #[expect(non_upper_case_globals, reason = "the names are ESP-IDF's")]
    let sent = match position {
        // SAFETY: `item` points to one item of the queue's item size.
        queueSEND_TO_BACK => unsafe { target.send_to_back_with_deadline(item.cast(), until) },
        // SAFETY: as above.
        queueSEND_TO_FRONT => unsafe { target.send_to_front_with_deadline(item.cast(), until) },
        #[expect(
            clippy::unimplemented,
            reason = "the firmware doesn't overwrite queues yet"
        )]
        _ => unimplemented!("xQueueOverwrite"),
    };
    if sent { pdPASS } else { errQUEUE_FULL }
}

#[export]
unsafe fn xQueueReceive(queue: &Queue, buffer: *mut c_void, ticks: TickType_t) -> BaseType_t {
    // SAFETY: `buffer` has room for one item of the queue's item size.
    let received = unsafe {
        queue
            .handle
            .receive_with_deadline(buffer.cast(), deadline(ticks))
    };
    if received { pdTRUE } else { pdFALSE }
}

#[export]
fn xQueueGenericReset(queue: &Queue, _new_queue: BaseType_t) -> BaseType_t {
    let mut item = vec![0_u8; queue.item_size];
    // FreeRTOS resets atomically, so no task or interrupt can add items mid-drain.
    critical_section::with(|_| {
        // SAFETY: `item` holds one item; a zero timeout never blocks.
        while unsafe { queue.handle.receive(item.as_mut_ptr(), Some(0)) } {}
    });
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
    nesting.set((
        depth.saturating_add(1),
        if depth == 0 { restore } else { outer },
    ));
}

#[export]
unsafe fn vPortExitCritical() {
    // SAFETY: called inside the critical section its matching enter opened.
    let nesting = NESTING.borrow(unsafe { CriticalSection::new() });
    let (depth, outer) = nesting.get();
    #[expect(clippy::expect_used, reason = "an unpaired exit is a firmware bug")]
    let remaining = depth
        .checked_sub(1)
        .expect("vPortExitCritical without vPortEnterCritical");
    nesting.set((remaining, outer));
    if remaining == 0 {
        // SAFETY: `outer` came from the outermost `acquire`.
        unsafe { critical_section::release(outer) };
    }
}
