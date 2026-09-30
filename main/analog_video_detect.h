#pragma once

#include <stddef.h>
#include <stdint.h>

/* Analog-video confidence from raw Q4/I4 (issue #128).
 *
 * Composite video repeats line by line: sync, burst and largely the same
 * picture content return every 64 us (PAL, 1280 output pairs at 20 MS/s) or
 * 63.556 us (NTSC, ~1271). The demodulated frequency therefore correlates
 * strongly with itself one line later. OFDM Wi-Fi, other data signals and
 * noise have no such periodicity. The normalized autocorrelation at one
 * line period is independent of carrier offset (mean removed) and of the
 * VTX deviation (normalized), so it separates a real analog carrier from
 * strong but invalid RF energy without absolute sync thresholds.
 *
 * Use at least ~8 KB (two descriptors, >3 lines): at a one-line lag the
 * overlap must always contain a sync pulse, which one 4 KB descriptor (766
 * overlapping pairs) does only ~60 % of the time with a moving picture.
 * `raw` holds 16-bit ring pairs; `ring_offset` is the byte offset of raw[0]
 * in the ring (the endpoint is the odd ring byte, as Phase8 reads it).
 * `phase` is the Phase8 phase LUT (256 entries). */
typedef struct {
    int confidence;   /* peak autocorrelation x100 over the line lags, <=0 = none */
    int lag;          /* best lag in output pairs (1266..1285) */
    int standard;     /* 1 = PAL, 2 = NTSC, 0 = unknown */
} analog_video_t;

analog_video_t analog_video_detect(const uint8_t *raw, size_t bytes,
                                   size_t ring_offset, const uint8_t phase[256]);

/* A channel counts as analog video when its confidence is at least this. */
/* Host test: analog video >= 31 even weak (C/N ~10 dB) or with a fully
 * random picture, carrier offset or NTSC; OFDM Wi-Fi, noise and an
 * unmodulated carrier <= 7. */
#define ANALOG_VIDEO_MIN_CONFIDENCE 20
