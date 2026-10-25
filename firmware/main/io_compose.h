#ifndef AIRGUARD_IO_COMPOSE_H
#define AIRGUARD_IO_COMPOSE_H

/* FW-6 step 3: the two decisions of the io task that join the CO path
 * (FW-5b co_alert) and the FSM (components/fsm), kept here as pure C so
 * firmware/test_host runs the same code with the real co_alert.c and
 * ag_fsm.c (test_io_compose.c). Header-only: no FreeRTOS, no GPIO, and no
 * extra SRCS line in main/CMakeLists.txt.
 *
 * 🔴 CO first, always (CLAUDE.md §2, docs/03 §5): while co->active the CO
 * pattern is the whole output and the FSM's is dropped, and ACK goes to
 * co_alert before the FSM can see it. */

#include <stdbool.h>
#include <stdint.h>

#include "ag_fsm.h"
#include "co_alert.h"

enum {
    IO_OUT_GREEN  = 1U << 0,
    IO_OUT_YELLOW = 1U << 1,
    IO_OUT_RED    = 1U << 2,
    IO_OUT_BUZZER = 1U << 3,
};

static inline uint32_t io_compose_outputs(const co_alert_out_t *co, const ag_fsm_out_t *fsm)
{
    if (co->active) {
        return (co->red ? IO_OUT_RED : 0U) | (co->buzzer ? IO_OUT_BUZZER : 0U);
    }
    return (fsm->green ? IO_OUT_GREEN : 0U) | (fsm->yellow ? IO_OUT_YELLOW : 0U) |
           (fsm->red ? IO_OUT_RED : 0U) | (fsm->buzzer ? IO_OUT_BUZZER : 0U);
}

/* Next time the shown outputs can change by themselves: under CO only CO's
 * edges matter (the FSM's do not show). INT64_MAX = steady. */
static inline int64_t io_compose_next_change_us(const co_alert_out_t *co, const ag_fsm_out_t *fsm)
{
    return co->active ? co->next_change_us : fsm->next_change_us;
}

typedef enum {
    IO_ACK_NONE,        /* nothing to do (logged only) */
    IO_ACK_CO_MUTED,    /* co_alert muted the CO buzzer/chirp; CO state kept */
    IO_ACK_FSM,         /* FSM ended the demo or muted ML ALARM */
} io_ack_t;

/* co_have: a CO snapshot exists (false when the CO path is not built). */
static inline io_ack_t io_route_ack(co_alert_t *co, bool co_have, ag_fsm_t *fsm, int64_t now_us)
{
    if (co_have && co_alert_ack(co, now_us)) {
        return IO_ACK_CO_MUTED;
    }
    /* ag_fsm_ack refuses by itself while CO is shown */
    return ag_fsm_ack(fsm, now_us) ? IO_ACK_FSM : IO_ACK_NONE;
}

#endif
