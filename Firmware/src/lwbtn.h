/**
 * \file            lwbtn.h
 * \brief           Lightweight button manager
 *
 * Copyright (c) 2024 Tilen MAJERLE (MIT License)
 * Version:         v1.1.0
 */
#ifndef LWBTN_HDR_H
#define LWBTN_HDR_H

#include <stdint.h>
#include "lwbtn_opt.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * \defgroup        LWBTN Lightweight button manager
 * \brief           Lightweight button manager
 * \{
 */

/**
 * \brief           Custom user argument data structure
 */
typedef struct {
    void* port;    /*!< User defined GPIO port information */
    void* pin;     /*!< User defined GPIO pin information */
    uint8_t state; /*!< User defined GPIO state level when considered active */
} lwbtn_argdata_port_pin_state_t;

/* Forward declarations */
struct lwbtn_btn;
struct lwbtn;

/**
 * \brief           Time variable type
 */
typedef LWBTN_CFG_TYPE_VARTYPE lwbtn_time_t;

/**
 * \brief           List of button events
 */
typedef enum {
    LWBTN_EVT_ONPRESS = 0x00, /*!< On press event - sent when valid press is detected (after debounce if enabled) */
    LWBTN_EVT_ONRELEASE, /*!< On release event - sent when valid release event is detected (from active to inactive) */
#if LWBTN_CFG_USE_CLICK || __DOXYGEN__
    LWBTN_EVT_ONCLICK, /*!< On Click event - sent when valid sequence of on-press and on-release events occurs */
#endif                 /* LWBTN_CFG_USE_CLICK || __DOXYGEN__ */
#if LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__
    LWBTN_EVT_KEEPALIVE, /*!< Keep alive event - sent periodically when button is active */
#endif                   /* LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__ */
} lwbtn_evt_t;

/**
 * \brief           Button event function callback prototype
 */
typedef void (*lwbtn_evt_fn)(struct lwbtn* lwobj, struct lwbtn_btn* btn, lwbtn_evt_t evt);

/**
 * \brief           Get button/input state callback function
 * \return          `1` when button is considered `active`, `0` otherwise
 */
typedef uint8_t (*lwbtn_get_state_fn)(struct lwbtn* lwobj, struct lwbtn_btn* btn);

/**
 * \brief           Button/input structure
 */
typedef struct lwbtn_btn {
    uint16_t flags; /*!< Private button flags management */
#if LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_CALLBACK || __DOXYGEN__
    uint8_t curr_state;             /*!< Current button state to be processed. */
#endif                              /* LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_CALLBACK || __DOXYGEN__ */
    uint8_t last_state;             /*!< Last button state - `1` means active, `0` means inactive */
    lwbtn_time_t time_change;       /*!< Time in ms when button state got changed last time after valid debounce */
    lwbtn_time_t time_state_change; /*!< Time in ms when button state got changed last time */

#if LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__
    struct {
        lwbtn_time_t last_time; /*!< Time in ms of last send keep alive event */
        uint16_t cnt;           /*!< Number of keep alive events sent after successful on-press detection. */
    } keepalive;                /*!< Keep alive structure */
#endif                          /* LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__ */

#if LWBTN_CFG_USE_CLICK || __DOXYGEN__
    struct {
        lwbtn_time_t last_time; /*!< Time in ms of last successfully detected (not sent!) click event */
        uint8_t cnt;            /*!< Number of consecutive clicks detected, respecting maximum timeout between clicks */
    } click;                    /*!< Click event structure */
#endif                          /* LWBTN_CFG_USE_CLICK || __DOXYGEN__ */

    void* arg; /*!< User defined custom argument for callback function purpose */

#if LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC || __DOXYGEN__
    uint16_t time_debounce; /*!< Debounce time in milliseconds */
#endif                      /* LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC || __DOXYGEN__ */
#if LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC || __DOXYGEN__
    uint16_t time_debounce_release; /*!< Debounce time in milliseconds for release event  */
#endif                              /* LWBTN_CFG_TIME_DEBOUNCE_RELEASE */
#if LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC || __DOXYGEN__
    uint16_t time_click_pressed_min; /*!< Minimum pressed time for valid click event */
#endif                               /* LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC || __DOXYGEN__ */
#if LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC || __DOXYGEN__
    uint16_t time_click_pressed_max; /*!< Maximum pressed time for valid click event*/
#endif                               /* LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC || __DOXYGEN__ */
#if LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC || __DOXYGEN__
    uint16_t time_click_multi_max; /*!< Maximum time between 2 clicks to be considered consecutive click */
#endif                             /* LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC || __DOXYGEN__ */
#if LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC || __DOXYGEN__
    uint16_t time_keepalive_period; /*!< Time in ms for periodic keep alive event */
#endif                              /* LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC || __DOXYGEN__ */
#if LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC || __DOXYGEN__
    uint16_t max_consecutive; /*!< Max number of consecutive clicks */
#endif                        /* LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC || __DOXYGEN__ */
} lwbtn_btn_t;

/**
 * \brief           LwBTN group structure
 */
typedef struct lwbtn {
    lwbtn_btn_t* btns;   /*!< Pointer to buttons array */
    uint16_t btns_cnt;   /*!< Number of buttons in array */
    lwbtn_evt_fn evt_fn; /*!< Pointer to event function */
#if LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_MANUAL || __DOXYGEN__
    lwbtn_get_state_fn get_state_fn; /*!< Pointer to get state function */
#endif                               /* LWBTN_CFG_GET_STATE_MODE != LWBTN_GET_STATE_MODE_MANUAL || __DOXYGEN__ */
} lwbtn_t;

uint8_t lwbtn_init_ex(lwbtn_t* lwobj, lwbtn_btn_t* btns, uint16_t btns_cnt, lwbtn_get_state_fn get_state_fn,
                      lwbtn_evt_fn evt_fn);
uint8_t lwbtn_process_ex(lwbtn_t* lwobj, lwbtn_time_t mstime);
uint8_t lwbtn_process_btn_ex(lwbtn_t* lwobj, lwbtn_btn_t* btn, lwbtn_time_t mstime);
uint8_t lwbtn_set_btn_state(lwbtn_btn_t* btn, uint8_t state);
uint8_t lwbtn_is_btn_active(const lwbtn_btn_t* btn);
uint8_t lwbtn_reset(lwbtn_t* lwobj, lwbtn_btn_t* btn);

#define lwbtn_init(btns, btns_cnt, get_state_fn, evt_fn) lwbtn_init_ex(NULL, btns, btns_cnt, get_state_fn, evt_fn)
#define lwbtn_process(mstime)                            lwbtn_process_ex(NULL, mstime)
#define lwbtn_process_btn(btn, mstime)                   lwbtn_process_btn_ex(NULL, (btn), (mstime))

#if LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__
#if LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC || __DOXYGEN__
#define lwbtn_keepalive_get_period(btn) ((btn)->time_keepalive_period)
#else
#define lwbtn_keepalive_get_period(btn) (LWBTN_CFG_TIME_KEEPALIVE_PERIOD)
#endif /* LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC || __DOXYGEN__ */

#define lwbtn_keepalive_get_count(btn)                   ((btn)->keepalive.cnt)
#define lwbtn_keepalive_get_count_for_time(btn, ms_time) ((ms_time) / lwbtn_keepalive_get_period(btn))
#endif /* LWBTN_CFG_USE_KEEPALIVE || __DOXYGEN__ */

#define lwbtn_click_get_count(btn) ((btn)->click.cnt)

/**
 * \}
 */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LWBTN_HDR_H */
