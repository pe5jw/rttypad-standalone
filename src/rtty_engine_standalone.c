/* rtty_engine_standalone.c
 * FSK TX engine — extracted from deskHPSDR/src/rtty_engine.c.
 * All GTK/GLib/radio dependencies removed.
 * Cross-platform mutex via miniaudio ma_mutex (Windows + POSIX).
 *
 * Original: Copyright (C) 2024-2026 Heiko Amft, DL1BZ
 * Standalone port: pe5jw/rttypad-standalone
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Pull in only the threading/mutex primitives from miniaudio */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "rtty_engine_standalone.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                           */
/* ------------------------------------------------------------------ */
#define RTTY_QUEUE_SIZE           32768
#define RTTY_LTRS                 0x1f
#define RTTY_FIGS                 0x1b
#define RTTY_SPACE                0x04
#define RTTY_CR                   0x08
#define RTTY_LF                   0x02
#define RTTY_TWO_PI               (2.0 * M_PI)
#define RTTY_SHAPE_MS             5.0      /* soft FSK transition, ms */
#define RTTY_BUF_EMPTY_DEBOUNCE_MS 250.0

/* ------------------------------------------------------------------ */
/*  ITA2 / Baudot tables                                                */
/* ------------------------------------------------------------------ */
static const char letters[32] = {
    '\0','E','\n','A',' ','S','I','U','\r','D','R','J','N','F','C','K',
    'T','Z','L','W','H','Y','P','Q','O','B','G','\0','M','X','V','\0'
};
static const char figures[32] = {
    '\0','3','\n','-',' ','\'','8','7','\r','$','4','\a',',','!',':','(',
    '5','"',')','2','#','6','0','1','9','?','&','\0','.','/',';','\0'
};

/* ------------------------------------------------------------------ */
/*  TX phase                                                            */
/* ------------------------------------------------------------------ */
typedef enum {
    PHASE_IDLE,
    PHASE_PREFIX_MARK,
    PHASE_STREAM,
    PHASE_FILL_MARK,
    PHASE_FILL_LTRS,
    PHASE_FILL_SPACE,
    PHASE_TAIL_MARK,
    PHASE_WAIT_DONE
} TxPhase;

/* ------------------------------------------------------------------ */
/*  Config                                                              */
/* ------------------------------------------------------------------ */
static double        cfg_baud         = 45.4545454545;
static int           cfg_shift        = 170;
static int           cfg_reverse      = 0;
static double        cfg_stop_bits    = 1.5;
static int           cfg_uos          = 1;
static RTTY_FILL_MODE cfg_fill        = RTTY_FILL_LTRS;
static int           cfg_start_crlf   = 1;
static int           cfg_idle_timeout = 25;
static double        cfg_af_center    = 1500.0;
static RTTY_SIDEBAND cfg_sideband     = RTTY_SIDEBAND_DIGL;

/* ------------------------------------------------------------------ */
/*  State — protected by eng_mutex except where noted                  */
/* ------------------------------------------------------------------ */
static ma_mutex      eng_mutex;
static ma_bool32     mutex_inited = MA_FALSE;

static volatile TxPhase tx_phase       = PHASE_IDLE;
static volatile int     stopping       = 0;
static volatile int     draining       = 0;
static atomic_int       active_atomic  = 0;

/* NCO */
static double phase_acc         = 0.0;
static double shape_start_hz    = 0.0;
static double shape_target_hz   = 0.0;
static double shape_current_hz  = 0.0;
static double shape_samples     = 0.0;
static double shape_pos         = 0.0;

/* Symbol timing */
static double sym_samples_left  = 0.0;
static int    current_bit       = 1;    /* 1=MARK, 0=SPACE */
static int    bit_index         = -1;   /* -1=need new char */
static uint8_t current_code     = 0;
static int    in_figures        = 0;
static int    prefix_ltrs_pending = 0;
static int    prefix_mark_samples = 0;
static int    tail_mark_samples   = 0;

/* Queue */
static uint8_t  tx_queue[RTTY_QUEUE_SIZE];
static volatile int tx_head = 0;
static volatile int tx_tail = 0;

/* Idle / debounce */
static double idle_samples      = 0.0;
static double buf_empty_samples = 0.0;
static int    buf_empty_armed   = 0;

/* Callbacks */
static void (*buffer_empty_cb)(void) = NULL;
static void (*tx_done_cb)(void)      = NULL;

/* ------------------------------------------------------------------ */
/*  Queue helpers                                                        */
/* ------------------------------------------------------------------ */
static inline int q_empty(void)  { return tx_head == tx_tail; }
static inline void q_push(uint8_t v) {
    int next = (tx_tail + 1) % RTTY_QUEUE_SIZE;
    if (next != tx_head) { tx_queue[tx_tail] = v; tx_tail = next; }
}
static inline int q_pop(uint8_t *v) {
    if (tx_head == tx_tail) return 0;
    *v = tx_queue[tx_head];
    tx_head = (tx_head + 1) % RTTY_QUEUE_SIZE;
    return 1;
}

/* ------------------------------------------------------------------ */
/*  ITA2 lookup                                                          */
/* ------------------------------------------------------------------ */
static uint8_t char_to_ita2(char c, int *need_shift, int cur_figs) {
    *need_shift = -1;
    c = (char)toupper((unsigned char)c);
    switch (c) {
        case '\r': return RTTY_CR;
        case '\n': return RTTY_LF;
        case ' ':  return RTTY_SPACE;
    }
    for (int i = 1; i < 32; i++) {
        if (letters[i] == c) {
            if (cur_figs) *need_shift = 0; /* need LTRS shift */
            return (uint8_t)i;
        }
    }
    for (int i = 1; i < 32; i++) {
        if (figures[i] == c) {
            if (!cur_figs) *need_shift = 1; /* need FIGS shift */
            return (uint8_t)i;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Shaped frequency (5 ms linear ramp on tone change)                  */
/* ------------------------------------------------------------------ */
static double shaped_frequency(double target_hz, int sr) {
    if (fabs(target_hz - shape_target_hz) > 0.01) {
        shape_start_hz  = shape_current_hz;
        shape_target_hz = target_hz;
        shape_pos       = 0.0;
        shape_samples   = (RTTY_SHAPE_MS / 1000.0) * (double)sr;
    }
    if (shape_pos < shape_samples) {
        double t = shape_pos / shape_samples;
        shape_current_hz = shape_start_hz + t * (shape_target_hz - shape_start_hz);
        shape_pos += 1.0;
    } else {
        shape_current_hz = shape_target_hz;
    }
    return shape_current_hz;
}

/* ------------------------------------------------------------------ */
/*  Mark / space frequencies for AFSK                                   */
/* ------------------------------------------------------------------ */
static void get_mark_space_hz(double *mark_hz, double *space_hz) {
    double half = (double)cfg_shift / 2.0;
    if (cfg_sideband == RTTY_SIDEBAND_DIGL) {
        *mark_hz  = cfg_af_center + half;
        *space_hz = cfg_af_center - half;
    } else {
        *mark_hz  = cfg_af_center - half;
        *space_hz = cfg_af_center + half;
    }
    if (cfg_reverse) { double t = *mark_hz; *mark_hz = *space_hz; *space_hz = t; }
}

/* ------------------------------------------------------------------ */
/*  Internal start (call with mutex held)                               */
/* ------------------------------------------------------------------ */
static void start_locked(void) {
    tx_head = tx_tail = 0;
    in_figures           = 0;
    tx_phase             = PHASE_PREFIX_MARK;
    stopping             = 0;
    draining             = 0;
    phase_acc            = 0.0;
    shape_start_hz       = shape_target_hz = shape_current_hz = 0.0;
    shape_pos            = shape_samples = 0.0;
    sym_samples_left     = 0.0;
    bit_index            = -1;
    idle_samples         = 0.0;
    buf_empty_armed      = 0;
    buf_empty_samples    = 0.0;
    prefix_ltrs_pending  = 3;
    prefix_mark_samples  = 0;
    tail_mark_samples    = 0;

    if (cfg_start_crlf) { q_push(RTTY_CR); q_push(RTTY_LF); }
    atomic_store(&active_atomic, 1);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
void rttye_init(void) {
    if (!mutex_inited) {
        ma_mutex_init(&eng_mutex);
        mutex_inited = MA_TRUE;
    }
}

void rttye_destroy(void) {
    rttye_abort();
    if (mutex_inited) {
        ma_mutex_uninit(&eng_mutex);
        mutex_inited = MA_FALSE;
    }
}

#define LOCK()   ma_mutex_lock(&eng_mutex)
#define UNLOCK() ma_mutex_unlock(&eng_mutex)

void   rttye_set_baud(double b)          { LOCK(); cfg_baud=b;          UNLOCK(); }
double rttye_get_baud(void)              { return cfg_baud; }
void   rttye_set_shift(int s)            { LOCK(); cfg_shift=s;         UNLOCK(); }
int    rttye_get_shift(void)             { return cfg_shift; }
void   rttye_set_reverse(int r)          { LOCK(); cfg_reverse=r;       UNLOCK(); }
int    rttye_get_reverse(void)           { return cfg_reverse; }
void   rttye_set_stop_bits(double sb)    { LOCK(); cfg_stop_bits=sb;    UNLOCK(); }
double rttye_get_stop_bits(void)         { return cfg_stop_bits; }
void   rttye_set_uos(int e)              { LOCK(); cfg_uos=e;           UNLOCK(); }
int    rttye_get_uos(void)               { return cfg_uos; }
void   rttye_set_fill(RTTY_FILL_MODE f)  { LOCK(); cfg_fill=f;          UNLOCK(); }
RTTY_FILL_MODE rttye_get_fill(void)      { return cfg_fill; }
void   rttye_set_start_crlf(int e)       { LOCK(); cfg_start_crlf=e;    UNLOCK(); }
int    rttye_get_start_crlf(void)        { return cfg_start_crlf; }
void   rttye_set_idle_timeout(int s)     { LOCK(); cfg_idle_timeout=s;  UNLOCK(); }
int    rttye_get_idle_timeout(void)      { return cfg_idle_timeout; }
void   rttye_set_af_center(double hz)    { LOCK(); cfg_af_center=hz;    UNLOCK(); }
double rttye_get_af_center(void)         { return cfg_af_center; }
void   rttye_set_sideband(RTTY_SIDEBAND sb){ LOCK(); cfg_sideband=sb;   UNLOCK(); }
RTTY_SIDEBAND rttye_get_sideband(void)   { return cfg_sideband; }

void rttye_set_buffer_empty_callback(void (*cb)(void)) { buffer_empty_cb = cb; }
void rttye_set_tx_done_callback(void (*cb)(void))      { tx_done_cb = cb; }

int rttye_start(void) {
    LOCK();
    if (atomic_load(&active_atomic)) { UNLOCK(); return -1; }
    start_locked();
    UNLOCK();
    return 0;
}

int rttye_queue_text(const char *text) {
    if (!text) return 0;
    LOCK();
    if (!atomic_load(&active_atomic)) start_locked();
    int count = 0;
    int cur_figs = in_figures;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '\r' && *(p+1) == '\n') continue; /* collapse CRLF */
        int need_shift;
        uint8_t code = char_to_ita2(c, &need_shift, cur_figs);
        if (code == 0 && need_shift == -1) continue;
        if (need_shift == 1) { q_push(RTTY_FIGS); cur_figs = 1; }
        else if (need_shift == 0) { q_push(RTTY_LTRS); cur_figs = 0; }
        if (cfg_uos && c == ' ') { q_push(RTTY_LTRS); cur_figs = 0; }
        q_push(code);
        count++;
    }
    if (count > 0) {
        buf_empty_armed = 1;
        idle_samples    = 0.0;
        if (tx_phase == PHASE_FILL_LTRS ||
            tx_phase == PHASE_FILL_MARK ||
            tx_phase == PHASE_FILL_SPACE)
            tx_phase = PHASE_STREAM;
    }
    UNLOCK();
    return count;
}

void rttye_drain(void) {
    LOCK();
    draining = 1;
    if (tx_phase == PHASE_FILL_LTRS || tx_phase == PHASE_FILL_MARK || tx_phase == PHASE_FILL_SPACE)
        tx_phase = PHASE_TAIL_MARK;
    UNLOCK();
}

void rttye_stop(void) {
    LOCK();
    tx_head = tx_tail = 0;
    stopping = 1;
    tx_phase = PHASE_TAIL_MARK;
    tail_mark_samples = 0;
    UNLOCK();
}

void rttye_abort(void) {
    if (!mutex_inited) return;
    LOCK();
    atomic_store(&active_atomic, 0);
    tx_phase = PHASE_IDLE;
    stopping = draining = 0;
    tx_head  = tx_tail = 0;
    phase_acc = 0.0;
    UNLOCK();
    if (tx_done_cb) tx_done_cb();
}

int rttye_is_active(void) { return atomic_load(&active_atomic); }

/* ------------------------------------------------------------------ */
/*  Render — called from miniaudio data callback (audio thread)         */
/* ------------------------------------------------------------------ */
int rttye_render(float *out, int frames, int sr, int mono) {
    LOCK();

    if (tx_phase == PHASE_IDLE || !atomic_load(&active_atomic)) {
        UNLOCK();
        memset(out, 0, (size_t)frames * (mono ? 1 : 2) * sizeof(float));
        return 0;
    }

    double mark_hz, space_hz;
    get_mark_space_hz(&mark_hz, &space_hz);

    const double samples_per_bit   = (double)sr / cfg_baud;
    const double prefix_dur        = 0.300 * (double)sr;
    const double tail_dur          = 0.300 * (double)sr;

    for (int n = 0; n < frames; n++) {
        double freq = mark_hz;

        switch (tx_phase) {

        /* ---- prefix: 300 ms MARK preamble ----------------------- */
        case PHASE_PREFIX_MARK:
            freq = mark_hz;
            if (++prefix_mark_samples >= (int)prefix_dur) {
                prefix_mark_samples = 0;
                tx_phase  = PHASE_STREAM;
                bit_index = -1;
            }
            break;

        /* ---- stream: emit symbols from queue --------------------- */
        case PHASE_STREAM: {
            if (bit_index < 0 || sym_samples_left <= 0.0) {
                if (bit_index < 0) {
                    /* Need a new character */
                    uint8_t code = 0;
                    int got = 0;
                    if (prefix_ltrs_pending > 0) {
                        code = RTTY_LTRS; prefix_ltrs_pending--; got = 1;
                    } else {
                        got = q_pop(&code);
                    }
                    if (!got) {
                        /* Queue empty */
                        if (draining || stopping) {
                            tx_phase = PHASE_TAIL_MARK; tail_mark_samples = 0;
                        } else {
                            switch (cfg_fill) {
                            case RTTY_FILL_MARK:  tx_phase = PHASE_FILL_MARK;  break;
                            case RTTY_FILL_SPACE: tx_phase = PHASE_FILL_SPACE; break;
                            default:              tx_phase = PHASE_FILL_LTRS;  break;
                            }
                        }
                        freq = mark_hz; break;
                    }
                    /* Debounce: schedule buffer_empty callback */
                    if (q_empty() && buf_empty_armed) {
                        buf_empty_samples = RTTY_BUF_EMPTY_DEBOUNCE_MS / 1000.0 * (double)sr;
                        buf_empty_armed = 0;
                    }
                    current_code = code;
                    bit_index    = 0;
                    sym_samples_left = 0.0;
                }
                /* Advance frame: 0=start, 1-5=data bits, 6=stop */
                if (bit_index == 0) {
                    current_bit = 0; /* START = SPACE */
                    sym_samples_left += samples_per_bit;
                } else if (bit_index >= 1 && bit_index <= 5) {
                    current_bit = (current_code >> (bit_index - 1)) & 1;
                    sym_samples_left += samples_per_bit;
                } else { /* stop bit */
                    current_bit = 1; /* MARK */
                    sym_samples_left += samples_per_bit * cfg_stop_bits;
                    if (current_code == RTTY_FIGS) in_figures = 1;
                    if (current_code == RTTY_LTRS) in_figures = 0;
                }
                bit_index++;
                if (bit_index > 6) bit_index = -1;
            }
            freq = current_bit ? mark_hz : space_hz;
            sym_samples_left -= 1.0;

            /* Buffer-empty debounce countdown */
            if (buf_empty_samples > 0.0) {
                buf_empty_samples -= 1.0;
                if (buf_empty_samples <= 0.0) {
                    UNLOCK();
                    if (buffer_empty_cb) buffer_empty_cb();
                    LOCK();
                }
            }
            break;
        }

        /* ---- fill modes ----------------------------------------- */
        case PHASE_FILL_MARK:
            freq = mark_hz;
            idle_samples += 1.0;
            if (!q_empty()) { tx_phase = PHASE_STREAM; bit_index = -1; }
            else if (idle_samples >= (double)cfg_idle_timeout * sr)
                { tx_phase = PHASE_TAIL_MARK; tail_mark_samples = 0; }
            break;

        case PHASE_FILL_SPACE:
            freq = space_hz;
            idle_samples += 1.0;
            if (!q_empty()) { tx_phase = PHASE_STREAM; bit_index = -1; }
            else if (idle_samples >= (double)cfg_idle_timeout * sr)
                { tx_phase = PHASE_TAIL_MARK; tail_mark_samples = 0; }
            break;

        case PHASE_FILL_LTRS:
            freq = mark_hz;
            idle_samples += 1.0;
            if (!q_empty()) { tx_phase = PHASE_STREAM; bit_index = -1; }
            else if (idle_samples >= (double)cfg_idle_timeout * sr)
                { tx_phase = PHASE_TAIL_MARK; tail_mark_samples = 0; }
            break;

        /* ---- tail: 300 ms MARK tail ------------------------------ */
        case PHASE_TAIL_MARK:
            freq = mark_hz;
            if (++tail_mark_samples >= (int)tail_dur)
                tx_phase = PHASE_WAIT_DONE;
            break;

        /* ---- done: fire callback, go idle ------------------------ */
        case PHASE_WAIT_DONE:
            freq = mark_hz;
            if (n == frames - 1) {
                atomic_store(&active_atomic, 0);
                tx_phase = PHASE_IDLE;
                UNLOCK();
                if (tx_done_cb) tx_done_cb();
                /* Write last sample then return */
                freq = shaped_frequency(freq, sr);
                phase_acc += RTTY_TWO_PI * freq / (double)sr;
                if (phase_acc > RTTY_TWO_PI) phase_acc -= RTTY_TWO_PI;
                if (mono) out[n] = (float)sin(phase_acc);
                else { out[2*n] = (float)cos(phase_acc); out[2*n+1] = (float)sin(phase_acc); }
                return 0;
            }
            break;

        default:
            freq = mark_hz;
            break;
        }

        /* NCO */
        freq = shaped_frequency(freq, sr);
        phase_acc += RTTY_TWO_PI * freq / (double)sr;
        if (phase_acc > RTTY_TWO_PI) phase_acc -= RTTY_TWO_PI;

        if (mono) {
            out[n] = (float)sin(phase_acc);
        } else {
            out[2*n]   = (float)cos(phase_acc);
            out[2*n+1] = (float)sin(phase_acc);
        }
    }

    UNLOCK();
    return 1;
}
