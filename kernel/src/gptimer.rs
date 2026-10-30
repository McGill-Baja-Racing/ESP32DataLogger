//! ESP-IDF's general purpose timer, on timer 0 of timer group 1 (group 0 is esp-rtos's tick). Only
//! what the firmware uses: one timer counting up at 1 MHz with one-shot absolute alarms.
//!
//! esp-hal's `Timer` can't do this: it restarts the counter and has no way to re-arm an alarm.

use core::{cell::Cell, ffi::c_void, mem::MaybeUninit, ptr};

use critical_section::Mutex;
use esp_hal::{
    handler, interrupt,
    peripherals::{Interrupt, TIMG1},
    timer::timg::TimerGroup,
};
use esp_radio_rtos_driver::yield_task_from_isr;
use macros::export;
use sys::{
    ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, esp_err_t, gptimer_alarm_config_t,
    gptimer_alarm_event_data_t, gptimer_clock_source_t, gptimer_config_t,
    gptimer_count_direction_t, gptimer_event_callbacks_t, gptimer_handle_t,
};

use crate::rtos::Context;

/// The APB clock at the CPU clock `main` sets, divided to the 1 MHz the firmware asks for.
const DIVIDER: u16 = 80;

type Callback =
    unsafe extern "C" fn(gptimer_handle_t, *const gptimer_alarm_event_data_t, *mut c_void) -> bool;

#[derive(Clone, Copy, PartialEq)]
enum Phase {
    Free,
    Created,
    Enabled,
    Running,
}

#[derive(Clone, Copy)]
struct State {
    callback: Option<(Callback, Context)>,
    /// What the alarm event reports.
    alarm: u64,
    phase: Phase,
}

impl State {
    fn advance(&mut self, from: Phase, to: Phase) -> Result<(), esp_err_t> {
        if self.phase != from {
            return Err(ESP_ERR_INVALID_STATE);
        }
        self.phase = to;
        Ok(())
    }
}

struct Timer(Mutex<Cell<State>>);

sys::c_handle!(Timer, sys::gptimer_handle_t);

static TIMER: Timer = Timer(Mutex::new(Cell::new(State {
    callback: None,
    alarm: 0,
    phase: Phase::Free,
})));

impl Timer {
    fn update<T>(&self, change: impl FnOnce(&mut State) -> T) -> T {
        critical_section::with(|cs| {
            let cell = self.0.borrow(cs);
            let mut state = cell.get();
            let result = change(&mut state);
            cell.set(state);
            result
        })
    }
}

fn count() -> u64 {
    let timer = TIMG1::regs().t(0);
    timer.update().write(|w| w.update().set_bit());
    while timer.update().read().update().bit_is_set() {}
    (u64::from(timer.hi().read().bits()) << 32) | u64::from(timer.lo().read().bits())
}

/// The hardware ends the alarm when it fires, so a callback that doesn't set another stops the
/// alarm, as in ESP-IDF; the counter keeps running.
#[handler]
fn alarm() {
    let count_value = count();
    TIMG1::regs().int_clr().write(|w| w.t(0).clear_bit_by_one());
    let state = TIMER.update(|state| *state);
    let Some((callback, Context(context))) = state.callback else {
        return;
    };
    let event = gptimer_alarm_event_data_t {
        count_value,
        alarm_value: state.alarm,
    };
    let handle = ptr::from_ref(&TIMER).cast_mut().cast();
    // SAFETY: C registered `callback` with `context` for this timer, to run in this interrupt.
    if unsafe { callback(handle, &raw const event, context) } {
        yield_task_from_isr();
    }
}

#[export]
fn gptimer_new_timer(
    config: &gptimer_config_t,
    timer: &mut MaybeUninit<&'static Timer>,
) -> Result<(), esp_err_t> {
    assert!(
        config.resolution_hz == 1_000_000
            && config.direction == gptimer_count_direction_t::GPTIMER_COUNT_UP
            && config.clk_src == gptimer_clock_source_t::GPTIMER_CLK_SRC_DEFAULT,
        "the kernel supports only a 1 MHz timer counting up from the default clock"
    );
    TIMER
        .update(|state| state.advance(Phase::Free, Phase::Created))
        .map_err(|_taken| ESP_ERR_NOT_FOUND)?;
    // SAFETY: leaving `Phase::Free` lets this run once, so nothing else uses timer group 1.
    let _ = TimerGroup::new(unsafe { TIMG1::steal() });
    let regs = TIMG1::regs().t(0);
    regs.config().write(|w| {
        w.en().clear_bit();
        w.autoreload().clear_bit();
        w.increase().set_bit();
        w.use_xtal().clear_bit();
        w.alarm_en().clear_bit();
        // SAFETY: the divider is a plain 16-bit field.
        unsafe { w.divider().bits(DIVIDER) }
    });
    regs.config().modify(|_, w| w.divcnt_rst().set_bit());
    // SAFETY: both halves of the load value are plain registers.
    unsafe {
        regs.loadlo().write(|w| w.load_lo().bits(0));
        regs.loadhi().write(|w| w.load_hi().bits(0));
    }
    // SAFETY: writing 1 loads the value above.
    regs.load().write(|w| unsafe { w.load().bits(1) });
    timer.write(&TIMER);
    Ok(())
}

#[export]
fn gptimer_register_event_callbacks(
    timer: &Timer,
    callbacks: &gptimer_event_callbacks_t,
    user_data: *mut c_void,
) -> Result<(), esp_err_t> {
    timer.update(|state| {
        if matches!(state.phase, Phase::Enabled | Phase::Running) {
            return Err(ESP_ERR_INVALID_STATE);
        }
        state.callback = callbacks
            .on_alarm
            .map(|callback| (callback, Context(user_data)));
        Ok(())
    })
}

#[export]
fn gptimer_enable(timer: &Timer) -> Result<(), esp_err_t> {
    timer.update(|state| state.advance(Phase::Created, Phase::Enabled))?;
    interrupt::bind_handler(Interrupt::TG1_T0_LEVEL, alarm);
    TIMG1::regs().int_ena().modify(|_, w| w.t(0).set_bit());
    Ok(())
}

/// Allowed in the alarm callback, so it takes no lock but the critical section. The alarm is
/// absolute: the counter keeps running from zero.
#[export]
#[expect(clippy::cast_possible_truncation, reason = "the two 32-bit halves")]
fn gptimer_set_alarm_action(
    timer: &Timer,
    config: &gptimer_alarm_config_t,
) -> Result<(), esp_err_t> {
    let regs = TIMG1::regs().t(0);
    timer.update(|state| {
        state.alarm = config.alarm_count;
        // SAFETY: both halves of the alarm value are plain registers.
        unsafe {
            regs.alarmlo()
                .write(|w| w.alarm_lo().bits(config.alarm_count as u32));
            regs.alarmhi()
                .write(|w| w.alarm_hi().bits((config.alarm_count >> 32) as u32));
        }
        regs.config().modify(|_, w| w.alarm_en().set_bit());
    });
    Ok(())
}

#[export]
fn gptimer_start(timer: &Timer) -> Result<(), esp_err_t> {
    timer.update(|state| state.advance(Phase::Enabled, Phase::Running))?;
    TIMG1::regs().t(0).config().modify(|_, w| w.en().set_bit());
    Ok(())
}
