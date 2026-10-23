/* rtty_engine_standalone.h
 * FSK TX engine — extracted from deskHPSDR/src/rtty_engine.c and adapted
 * for standalone use (no GTK/GLib, no radio hardware, no PortAudio).
 * Uses miniaudio for cross-platform audio (Windows/Linux/macOS).
 *
 * Original: Copyright (C) 2024-2026 Heiko Amft, DL1BZ
 * Standalone port: pe5jw/rttypad-standalone
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTTY_ENGINE_STANDALONE_H
#define RTTY_ENGINE_STANDALONE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTTY_FILL_MARK  = 0,
    RTTY_FILL_LTRS  = 1,
    RTTY_FILL_SPACE = 2
} RTTY_FILL_MODE;

typedef enum {
    RTTY_SIDEBAND_DIGL = 0,  /* LSB — mark is upper audio tone */
    RTTY_SIDEBAND_DIGU = 1   /* USB — mark is lower audio tone */
} RTTY_SIDEBAND;

/* Lifecycle */
void rttye_init(void);
void rttye_destroy(void);

/* Config — safe to call any time */
void           rttye_set_baud(double baud);        /* default 45.4545 */
double         rttye_get_baud(void);
void           rttye_set_shift(int shift_hz);      /* default 170 */
int            rttye_get_shift(void);
void           rttye_set_reverse(int reverse);     /* default 0 */
int            rttye_get_reverse(void);
void           rttye_set_stop_bits(double sb);     /* default 1.5 */
double         rttye_get_stop_bits(void);
void           rttye_set_uos(int enabled);         /* unshift-on-space, default 1 */
int            rttye_get_uos(void);
void           rttye_set_fill(RTTY_FILL_MODE fill); /* default LTRS */
RTTY_FILL_MODE rttye_get_fill(void);
void           rttye_set_start_crlf(int enabled);  /* default 1 */
int            rttye_get_start_crlf(void);
void           rttye_set_idle_timeout(int seconds); /* default 25 */
int            rttye_get_idle_timeout(void);
void           rttye_set_af_center(double hz);     /* AFSK centre tone, default 1500 Hz */
double         rttye_get_af_center(void);
void           rttye_set_sideband(RTTY_SIDEBAND sb);
RTTY_SIDEBAND  rttye_get_sideband(void);

/* Callbacks — fired from audio render thread; keep short, no blocking */
void rttye_set_buffer_empty_callback(void (*cb)(void));
void rttye_set_tx_done_callback(void (*cb)(void));

/* Control */
int  rttye_start(void);                   /* 0=OK, -1=already active */
int  rttye_queue_text(const char *text);  /* returns chars enqueued */
void rttye_drain(void);                   /* finish buffer, then tail-mark */
void rttye_stop(void);                    /* graceful stop */
void rttye_abort(void);                   /* immediate silence */
int  rttye_is_active(void);

/* Audio render — called from miniaudio data callback.
 *   out    : float buffer
 *             mono=1 → float[frames]     (AFSK real signal)
 *             mono=0 → float[frames*2]   (interleaved IQ, L=I R=Q)
 *   frames : frame count requested
 *   sr     : sample rate in Hz
 *   mono   : 1=mono AFSK,  0=stereo IQ
 * Returns 1 while TX active, 0 when done (silence was written).       */
int rttye_render(float *out, int frames, int sr, int mono);

#ifdef __cplusplus
}
#endif

#endif /* RTTY_ENGINE_STANDALONE_H */
