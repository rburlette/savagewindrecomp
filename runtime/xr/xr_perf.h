// Optional owner-thread telemetry. No allocation, locks, payloads or clock-domain conversions.
#ifndef SAVAGEWIND_XR_PERF_H
#define SAVAGEWIND_XR_PERF_H
#include <stdint.h>

typedef struct {
    uint64_t count, sum_ns, max_ns;
    uint32_t histogram_ms[513]; // ceil milliseconds, final bucket includes values above 512 ms
} XrPerfMetric;

static inline void xr_perf_add(XrPerfMetric *metric, uint64_t ns) {
    ++metric->count;
    metric->sum_ns += ns;
    if (ns > metric->max_ns) metric->max_ns = ns;
    const uint64_t bucket_ms = ns / 1000000 + (ns % 1000000 != 0);
    ++metric->histogram_ms[bucket_ms < 512 ? bucket_ms : 512];
}

// Upper bound in ms; 512 denotes a saturated bucket, inspect exact max too.
static inline unsigned xr_perf_p95(const XrPerfMetric *metric) {
    const uint64_t target = metric->count - metric->count / 20;
    uint64_t counted = 0;
    for (unsigned i = 0; i < 513; ++i) {
        counted += metric->histogram_ms[i];
        if (counted >= target) return i;
    }
    return 0;
}
#endif
