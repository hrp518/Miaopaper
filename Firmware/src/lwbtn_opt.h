/**
 * \file            lwbtn_opt.h
 * \brief           LwBTN options (defaults, overridable by lwbtn_opts.h)
 */
#ifndef LWBTN_OPT_HDR_H
#define LWBTN_OPT_HDR_H

/* Uncomment to ignore user options (or set macro in compiler flags) */
/* #define LWBTN_IGNORE_USER_OPTS */

/* Include application options */
#ifndef LWBTN_IGNORE_USER_OPTS
#include "lwbtn_opts.h"
#endif /* LWBTN_IGNORE_USER_OPTS */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * \defgroup        LWBTN_OPT Configuration
 * \brief           Default configuration setup
 * \{
 */

/**
 * \brief           Memory set function (footprint same as memset)
 */
#ifndef LWBTN_MEMSET
#define LWBTN_MEMSET(dst, val, len) memset((dst), (val), (len))
#endif

/**
 * \brief           Memory copy function (footprint same as memcpy)
 */
#ifndef LWBTN_MEMCPY
#define LWBTN_MEMCPY(dst, src, len) memcpy((dst), (src), (len))
#endif

/**
 * \brief           Enables `1` or disables `0` periodic keep alive events.
 */
#ifndef LWBTN_CFG_USE_KEEPALIVE
#define LWBTN_CFG_USE_KEEPALIVE 1
#endif

/**
 * \brief           Enables `1` or disables `0` click event management.
 */
#ifndef LWBTN_CFG_USE_CLICK
#define LWBTN_CFG_USE_CLICK 1
#endif

/**
 * \brief           Minimum debounce time for press event in ms
 */
#ifndef LWBTN_CFG_TIME_DEBOUNCE_PRESS
#define LWBTN_CFG_TIME_DEBOUNCE_PRESS 20
#endif

/**
 * \brief           Enables `1` or disables `0` dynamic settable time debounce
 */
#ifndef LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC
#define LWBTN_CFG_TIME_DEBOUNCE_PRESS_DYNAMIC 0
#endif

/**
 * \brief           Minimum debounce time for release event in ms
 */
#ifndef LWBTN_CFG_TIME_DEBOUNCE_RELEASE
#define LWBTN_CFG_TIME_DEBOUNCE_RELEASE 0
#endif

/**
 * \brief           Enables `1` or disables `0` dynamic settable time debounce for release event
 */
#ifndef LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC
#define LWBTN_CFG_TIME_DEBOUNCE_RELEASE_DYNAMIC 0
#endif

/**
 * \brief           Minimum active input time for valid click event, in ms.
 *                  Set to `0` to disable.
 */
#ifndef LWBTN_CFG_TIME_CLICK_MIN
#define LWBTN_CFG_TIME_CLICK_MIN 20
#endif

#ifndef LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC
#define LWBTN_CFG_TIME_CLICK_MIN_DYNAMIC 0
#endif

/**
 * \brief           Maximum active input time for valid click event, in ms.
 *                  Set to `-1` to allow any time.
 */
#ifndef LWBTN_CFG_TIME_CLICK_MAX
#define LWBTN_CFG_TIME_CLICK_MAX 300
#endif

#ifndef LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC
#define LWBTN_CFG_TIME_CLICK_MAX_DYNAMIC 0
#endif

/**
 * \brief           Maximum allowed time between last on-release and next valid on-press,
 *                  to still allow multi-click events, in ms.
 */
#ifndef LWBTN_CFG_TIME_CLICK_MULTI_MAX
#define LWBTN_CFG_TIME_CLICK_MULTI_MAX 250
#endif

#ifndef LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC
#define LWBTN_CFG_TIME_CLICK_MULTI_MAX_DYNAMIC 0
#endif

/**
 * \brief           Maximum number of allowed consecutive click events
 */
#ifndef LWBTN_CFG_CLICK_MAX_CONSECUTIVE
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE 2
#endif

#ifndef LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE_DYNAMIC 0
#endif

/**
 * \brief           Keep-alive event period, in ms
 */
#ifndef LWBTN_CFG_TIME_KEEPALIVE_PERIOD
#define LWBTN_CFG_TIME_KEEPALIVE_PERIOD 100
#endif

#ifndef LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC
#define LWBTN_CFG_TIME_KEEPALIVE_PERIOD_DYNAMIC 0
#endif

/**
 * \brief           Enables `1` or disables `0` immediate onclick event
 *                  after on-release event, if number of consecutive
 *                  clicks reaches max value.
 */
#ifndef LWBTN_CFG_CLICK_MAX_CONSECUTIVE_SEND_IMMEDIATELY
#define LWBTN_CFG_CLICK_MAX_CONSECUTIVE_SEND_IMMEDIATELY 1
#endif

/**
 * \brief           Get button state options
 */
#define LWBTN_GET_STATE_MODE_CALLBACK           0 /*!< Callback-only state mode */
#define LWBTN_GET_STATE_MODE_MANUAL             1 /*!< Manual-only state mode */
#define LWBTN_GET_STATE_MODE_CALLBACK_OR_MANUAL 2 /*!< Callback or manual state mode */

/**
 * \brief           Sets the mode how new button state is acquired.
 */
#ifndef LWBTN_CFG_GET_STATE_MODE
#define LWBTN_CFG_GET_STATE_MODE LWBTN_GET_STATE_MODE_CALLBACK
#endif

/**
 * \brief           Keeps the consecutive click event group if last
 *                  sequence of *onpress* and *onrelease* was too short.
 */
#ifndef LWBTN_CFG_CLICK_CONSECUTIVE_KEEP_AFTER_SHORT_PRESS
#define LWBTN_CFG_CLICK_CONSECUTIVE_KEEP_AFTER_SHORT_PRESS 0
#endif

/**
 * \brief           Default variable type for the time values (in ms)
 */
#ifndef LWBTN_CFG_TYPE_VARTYPE
#define LWBTN_CFG_TYPE_VARTYPE uint32_t
#endif

/**
 * \}
 */

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LWBTN_OPT_HDR_H */
