/* SPDX-License-Identifier: MIT */
/*
 * extrace_sink_text.h - lossy record sink that prints one line per record through
 * a non-blocking write hook, for example a UART (DESIGN §12.8):
 *
 *     [ring] t=<ticks> <event> <KIND> <payload>
 *
 * INTS print decoded values, STR its text, PRINTF the site ID and the raw argument
 * bytes, FTRACE "fn 0x<address field>" and the arguments, everything else the
 * first bytes in hex. The host can decode such a log with the image metadata. No
 * C library is needed.
 */
#ifndef EXTRACE_SINK_TEXT_H
#define EXTRACE_SINK_TEXT_H

#include "extrace_disp.h"

#define EXTRACE_TEXT_LINE_MAX 160u

/* Writes up to len bytes without blocking; returns how many were taken. */
typedef uint32_t (*extrace_text_write_fn)(void *ctx, const char *buf, uint32_t len);

struct extrace_text_sink {
    struct extrace_sink sink;
    extrace_text_write_fn write;
    void *ctx;
    char *fifo;                     /* caller-provided line buffer */
    uint32_t size, head, tail;      /* free-running indices into fifo */
    uint32_t dropped;               /* lines dropped because the FIFO was full */
};

/* fifo_size must be a power of two of at least EXTRACE_TEXT_LINE_MAX */
int extrace_text_sink_init(struct extrace_text_sink *t, char *fifo, uint32_t fifo_size,
                           extrace_text_write_fn write, void *ctx);

#endif /* EXTRACE_SINK_TEXT_H */
