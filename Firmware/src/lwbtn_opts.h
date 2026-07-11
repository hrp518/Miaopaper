/**
 * \file            lwbtn_opts.h
 * \brief           LwBTN application config (minimal, tuned for TLSR825x ebook)
 *
 * Button mapping:
 *   - Single click  : page turn / navigate / confirm
 *   - Double click  : reserved (click.cnt == 2)
 *   - Long press    : via KEEPALIVE event count (>= threshold)
 */
#ifndef LWBTN_OPTS_H
#define LWBTN_OPTS_H

/* Feature switches */
#define LWBTN_CFG_USE_KEEPALIVE                         1   /* needed for long-press */
#define LWBTN_CFG_USE_CLICK                            1   /* needed for single/double click */

/* Timing (milliseconds) */
#define LWBTN_CFG_TIME_DEBOUNCE_PRESS                  20
#define LWBTN_CFG_TIME_DEBOUNCE_RELEASE                0
#define LWBTN_CFG_TIME_CLICK_MIN                       10
#define LWBTN_CFG_TIME_CLICK_MAX                       400     /* max press duration for a click */
#define LWBTN_CFG_TIME_CLICK_MULTI_MAX                 350     /* max gap between consecutive clicks */
#define LWBTN_CFG_TIME_KEEPALIVE_PERIOD                100     /* keepalive event every 100ms */

/* Click management */
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE                2       /* up to double-click */
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE_SEND_IMMEDIATELY 1
#define LWBTN_CFG_CLICK_CONSECUTIVE_KEEP_AFTER_SHORT_PRESS 0

/* All DYNAMIC options off -> smaller struct, less RAM */
#define LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC          0
#define LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC        0
#define LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC               0
#define LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC               0
#define LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC         0
#define LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC        0
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC        0

/* State acquisition: callback reads GPIO */
#define LWBTN_CFG_GET_STATE_MODE                       LWBTN_GET_STATE_MODE_CALLBACK

/* Time type */
#define LWBTN_CFG_TYPE_VARTYPE                         uint32_t

#endif /* LWBTN_OPTS_H */
