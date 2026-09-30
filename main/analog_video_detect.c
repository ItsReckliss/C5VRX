#include "analog_video_detect.h"

#define AVD_MAX_PAIRS 4096u
#define AVD_LAG_MIN   1266
#define AVD_LAG_MAX   1285

analog_video_t analog_video_detect(const uint8_t *raw, size_t bytes,
                                   size_t ring_offset, const uint8_t phase[256])
{
    static int8_t d[AVD_MAX_PAIRS];
    analog_video_t out = {0, 0, 0};
    if (!raw || !phase || bytes < 4u) return out;
    /* Endpoint = odd ring byte: first endpoint index inside `raw`. */
    size_t first = (ring_offset & 1u) ? 0u : 1u;
    size_t n = 0;
    uint8_t prev = phase[raw[first]];
    for (size_t i = first + 2u; i < bytes && n < AVD_MAX_PAIRS; i += 2u) {
        uint8_t p = phase[raw[i]];
        d[n++] = (int8_t)(uint8_t)(p - prev);      /* wrapped 50 ns step */
        prev = p;
    }
    if (n < (size_t)AVD_LAG_MAX + 256u) return out;

    int32_t sum = 0;
    for (size_t k = 0; k < n; ++k) sum += d[k];
    int32_t mean_q4 = (int32_t)((sum * 16) / (int32_t)n);
    int64_t var = 0;
    for (size_t k = 0; k < n; ++k) {
        int32_t x = d[k] * 16 - mean_q4;
        var += (int64_t)x * x;
    }
    var /= (int64_t)n;
    if (var <= 0) return out;

    int best = -1000, best_lag = 0;
    for (int lag = AVD_LAG_MIN; lag <= AVD_LAG_MAX; ++lag) {
        int64_t cov = 0;
        size_t m = n - (size_t)lag;
        for (size_t k = 0; k < m; ++k)
            cov += (int64_t)(d[k] * 16 - mean_q4) * (d[k + (size_t)lag] * 16 - mean_q4);
        cov /= (int64_t)m;
        int rho = (int)(cov * 100 / var);
        if (rho > best) { best = rho; best_lag = lag; }
    }
    out.confidence = best;
    out.lag = best_lag;
    out.standard = best_lag >= 1277 ? 1 : 2;
    return out;
}
