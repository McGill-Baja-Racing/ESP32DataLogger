#ifndef ECVT_FSM_H
#define ECVT_FSM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ECVT_STATUSWORD_TARGET_REACHED (1U << 10)

typedef enum {
    ECVT_STATE_STOPPED = 0,
    ECVT_STATE_RUNNING,
    ECVT_STATE_BRAKE,
    ECVT_STATE_ERROR,
} ecvt_state_t;

typedef struct {
    ecvt_state_t state;
    int64_t entered_at_us;
} ecvt_fsm_t;

typedef struct {
    ecvt_fsm_t *fsm;
    volatile uint16_t *latest_tpdo1_statusword;
    volatile int64_t *latest_tpdo1_rx_us;
} ecvt_fsm_user_ctx_t;

void ecvt_fsm_init(ecvt_fsm_t *fsm, int64_t now_us);
const char *ecvt_state_name(ecvt_state_t state);
bool ecvt_fsm_transition_to(ecvt_fsm_t *fsm,
                            ecvt_state_t requested_state,
                            int64_t now_us,
                            void *user_ctx);
void ecvt_fsm_dispatch_state_entry(ecvt_state_t new_state, ecvt_state_t old_state, void *user_ctx);
void ecvt_fsm_on_transition(ecvt_state_t new_state, ecvt_state_t old_state, void *user_ctx);
bool ecvt_fsm_motor_actions_enabled(void);
void ecvt_fsm_on_enter_stopped(void *user_ctx, ecvt_state_t old_state);
void ecvt_fsm_on_enter_running(void *user_ctx, ecvt_state_t old_state);
void ecvt_fsm_on_enter_brake(void *user_ctx, ecvt_state_t old_state);
void ecvt_fsm_on_enter_error(void *user_ctx, ecvt_state_t old_state);

#ifdef __cplusplus
}
#endif

#endif
