/**
 * \file            lwbtn.c
 * \brief           Lightweight button system
 *
 * Copyright (c) 2024 Tilen MAJERLE (MIT License)
 * Version:         v1.1.0
 */
#include <stddef.h>
#include "string.h"  /* project SDK string.h (memset/memcpy), avoids system conflict */
#include "lwbtn.h"

#if LWBTN_CFG_GET_STATE_MODE > 2
#error "Invalid LWBTN_GET_STATE_MODE_CALLBACK configuration"
#endif

#define LWBTN_FLAG_ONPRESS_SENT ((uint16_t)0x0001)
#define LWBTN_FLAG_MANUAL_STATE ((uint16_t)0x0002)
#define LWBTN_FLAG_FIRST_INACTIVE_RCVD ((uint16_t)0x0004)
#define LWBTN_FLAG_RESET ((uint16_t)0x0008)

#if LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC
#define LWBTN_TIME_DEBOUNCE_PRESS_GET_MIN(btn) ((lwbtn_time_t)((btn)->time_debounce))
#else
#define LWBTN_TIME_DEBOUNCE_PRESS_GET_MIN(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_DEBOUNCE_PRESS)
#endif

#if LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC
#define LWBTN_TIME_DEBOUNCE_RELEASE_GET_MIN(btn) ((lwbtn_time_t)((btn)->time_debounce_release))
#else
#define LWBTN_TIME_DEBOUNCE_RELEASE_GET_MIN(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_DEBOUNCE_RELEASE)
#endif

#if LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC
#define LWBTN_TIME_CLICK_GET_PRESSED_MIN(btn) ((lwbtn_time_t)((btn)->time_click_pressed_min))
#else
#define LWBTN_TIME_CLICK_GET_PRESSED_MIN(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_CLICK_MIN)
#endif
#if LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC
#define LWBTN_TIME_CLICK_GET_PRESSED_MAX(btn) ((lwbtn_time_t)((btn)->time_click_pressed_max))
#else
#define LWBTN_TIME_CLICK_GET_PRESSED_MAX(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_CLICK_MAX)
#endif
#if LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC
#define LWBTN_TIME_CLICK_MAX_MULTI(btn) ((lwbtn_time_t)((btn)->time_click_multi_max))
#else
#define LWBTN_TIME_CLICK_MAX_MULTI(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_CLICK_MULTI_MAX)
#endif
#if LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC
#define LWBTN_TIME_KEEPALIVE_PERIOD(btn) ((lwbtn_time_t)((btn)->time_keepalive_period))
#else
#define LWBTN_TIME_KEEPALIVE_PERIOD(btn) ((lwbtn_time_t)LWBTN_CFG_TIME_KEEPALIVE_PERIOD)
#endif
#if LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC
#define LWBTN_CLICK_MAX_CONSECUTIVE(btn) ((btn)->max_consecutive)
#else
#define LWBTN_CLICK_MAX_CONSECUTIVE(btn) LWBTN_CFG_CLICK_MAX_CONSECUTIVE
#endif

#if LWBTN_CFG_GET_STATE_MODE == LWBTN_GET_STATE_MODE_CALLBACK
#define LWBTN_BTN_GET_STATE(lwobj, btn) ((lwobj)->get_state_fn((lwobj), (btn)))
#elif LWBTN_CFG_GET_STATE_MODE == LWBTN_GET_STATE_MODE_MANUAL
#define LWBTN_BTN_GET_STATE(lwobj, btn) ((btn)->curr_state)
#elif LWBTN_CFG_GET_STATE_MODE == LWBTN_GET_STATE_MODE_CALLBACK_OR_MANUAL
#define LWBTN_BTN_GET_STATE(lwobj, btn)                                                                                \
    (((btn)->flags & LWBTN_FLAG_MANUAL_STATE)                                                                          \
         ? ((btn)->curr_state)                                                                                         \
         : (((lwobj)->get_state_fn != NULL) ? ((lwobj)->get_state_fn((lwobj), (btn))) : 0))
#endif

static lwbtn_t lwbtn_default;
#define LWBTN_GET_LWOBJ(in_lwobj) ((in_lwobj) != NULL ? (in_lwobj) : (&lwbtn_default))

static void
prv_process_btn(lwbtn_t* lwobj, lwbtn_btn_t* btn, lwbtn_time_t mstime) {
    uint8_t new_state;

    new_state = LWBTN_BTN_GET_STATE(lwobj, btn);

    if (!(btn->flags & LWBTN_FLAG_FIRST_INACTIVE_RCVD)) {
        if (new_state) {
            return;
        }
        btn->last_state = 0;
        btn->flags = LWBTN_FLAG_FIRST_INACTIVE_RCVD;
    }

    if (new_state != btn->last_state) {
        btn->time_state_change = mstime;
    }

    else if (new_state) {
        if (!(btn->flags & LWBTN_FLAG_ONPRESS_SENT)) {
#if LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC || LWBTN_CFG_TIME_DEBOUNCE_PRESS > 0
            if ((lwbtn_time_t)(mstime - btn->time_state_change) >= LWBTN_TIME_DEBOUNCE_PRESS_GET_MIN(btn))
#endif
            {
#if !LWBTN_CFG_CLICK_MAX_CONSECUTIVE_SEND_IMMEDIATELY
                if (btn->click.cnt > 0 && btn->click.cnt == LWBTN_CLICK_MAX_CONSECUTIVE(btn)) {
                    lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONCLICK);
                    btn->click.cnt = 0;
                }
#endif
                btn->flags |= LWBTN_FLAG_ONPRESS_SENT;
                lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONPRESS);
#if LWBTN_CFG_USE_KEEPALIVE
                btn->keepalive.last_time = mstime;
                btn->keepalive.cnt = 0;
#endif
                btn->time_change = mstime;
            }
#if LWBTN_CFG_USE_KEEPALIVE
        } else {
            while ((lwbtn_time_t)(mstime - btn->keepalive.last_time) >= LWBTN_TIME_KEEPALIVE_PERIOD(btn)) {
                btn->keepalive.last_time += LWBTN_TIME_KEEPALIVE_PERIOD(btn);
                ++btn->keepalive.cnt;
                lwobj->evt_fn(lwobj, btn, LWBTN_EVT_KEEPALIVE);
            }
#endif
        }
    }

    else {
        if (btn->flags & LWBTN_FLAG_ONPRESS_SENT) {
#if LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC || LWBTN_CFG_TIME_DEBOUNCE_RELEASE > 0
            if ((mstime - btn->time_state_change) >= LWBTN_TIME_DEBOUNCE_RELEASE_GET_MIN(btn))
#endif
            {
                btn->flags &= ~LWBTN_FLAG_ONPRESS_SENT;
                lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONRELEASE);

#if LWBTN_CFG_USE_CLICK
                if ((lwbtn_time_t)(mstime - btn->time_change) >= LWBTN_TIME_CLICK_GET_PRESSED_MIN(btn)
                    && (lwbtn_time_t)(mstime - btn->time_change) <= LWBTN_TIME_CLICK_GET_PRESSED_MAX(btn)) {
                    if (btn->click.cnt > 0 && btn->click.cnt < LWBTN_CLICK_MAX_CONSECUTIVE(btn)
                        && (lwbtn_time_t)(mstime - btn->click.last_time) < LWBTN_TIME_CLICK_MAX_MULTI(btn)) {
                        ++btn->click.cnt;
                    } else {
                        if (btn->click.cnt > 0) {
                            lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONCLICK);
                        }
                        btn->click.cnt = 1;
                    }
                    btn->click.last_time = mstime;
                } else {
#if LWBTN_CFG_CLICK_CONSECUTIVE_KEEP_AFTER_SHORT_PRESS
                    if (btn->click.cnt > 0
                        && (lwbtn_time_t)(mstime - btn->time_change) < LWBTN_TIME_CLICK_GET_PRESSED_MIN(btn)) {
                        lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONCLICK);
                    }
#endif
                    btn->click.cnt = 0;
                }

#if LWBTN_CFG_CLICK_MAX_CONSECUTIVE_SEND_IMMEDIATELY
                if (btn->click.cnt > 0 && btn->click.cnt == LWBTN_CLICK_MAX_CONSECUTIVE(btn)) {
                    lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONCLICK);
                    btn->click.cnt = 0;
                }
#endif
#endif /* LWBTN_CFG_USE_CLICK */

                btn->time_change = mstime;
            }
#if LWBTN_CFG_USE_CLICK
        } else {
            if (btn->click.cnt > 0) {
                if ((lwbtn_time_t)(mstime - btn->click.last_time) >= LWBTN_TIME_CLICK_MAX_MULTI(btn)) {
                    lwobj->evt_fn(lwobj, btn, LWBTN_EVT_ONCLICK);
                    btn->click.cnt = 0;
                }
            }
#endif
        }
    }

    btn->last_state = new_state;
}

uint8_t
lwbtn_init_ex(lwbtn_t* lwobj, lwbtn_btn_t* btns, uint16_t btns_cnt, lwbtn_get_state_fn get_state_fn,
              lwbtn_evt_fn evt_fn) {
    lwobj = LWBTN_GET_LWOBJ(lwobj);

    if (btns == NULL || btns_cnt == 0 || evt_fn == NULL
#if LWBTN_CFG_GET_STATE_MODE == LWBTN_GET_STATE_MODE_CALLBACK
        || get_state_fn == NULL
#endif
    ) {
        return 0;
    }

    LWBTN_MEMSET(lwobj, 0x00, sizeof(*lwobj));
    lwobj->btns = btns;
    lwobj->btns_cnt = btns_cnt;
    lwobj->evt_fn = evt_fn;
#if LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_MANUAL
    lwobj->get_state_fn = get_state_fn;
#else
    (void)get_state_fn;
#endif

    return 1;
}

uint8_t
lwbtn_process_ex(lwbtn_t* lwobj, lwbtn_time_t mstime) {
    lwobj = LWBTN_GET_LWOBJ(lwobj);
    for (size_t index = 0; index < lwobj->btns_cnt; ++index) {
        prv_process_btn(lwobj, &lwobj->btns[index], mstime);
    }
    return 1;
}

uint8_t
lwbtn_process_btn_ex(lwbtn_t* lwobj, lwbtn_btn_t* btn, lwbtn_time_t mstime) {
    if (btn != NULL) {
        prv_process_btn(LWBTN_GET_LWOBJ(lwobj), btn, mstime);
        return 1;
    }
    return 0;
}

uint8_t
lwbtn_set_btn_state(lwbtn_btn_t* btn, uint8_t state) {
#if LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_CALLBACK
    btn->curr_state = state;
    btn->flags |= LWBTN_FLAG_MANUAL_STATE;
    return 1;
#else
    (void)btn;
    (void)state;
    return 0;
#endif
}

uint8_t
lwbtn_is_btn_active(const lwbtn_btn_t* btn) {
    return btn != NULL && (btn->flags & LWBTN_FLAG_ONPRESS_SENT);
}

uint8_t
lwbtn_reset(lwbtn_t* lwobj, lwbtn_btn_t* btn) {
    for (size_t idx = 0; idx < (lwobj != NULL ? lwobj->btns_cnt : 0); ++idx) {
        lwobj->btns[idx].flags &= ~LWBTN_FLAG_FIRST_INACTIVE_RCVD;
    }
    if (btn != NULL) {
        btn->flags &= ~LWBTN_FLAG_FIRST_INACTIVE_RCVD;
    }
    return 1;
}
