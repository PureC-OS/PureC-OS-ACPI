#pragma once
// Host-test stub shadowing kernel/diagnostics/klog.h: all logging is a no-op.
enum klog_level {
    KLOG_INFO,
    KLOG_OK,
    KLOG_WARN,
    KLOG_ERROR,
    KLOG_DEBUG
};

static inline void klog(int lvl, const char *msg) {
    (void)lvl;
    (void)msg;
}

static inline void klogf(int lvl, const char *fmt, ...) {
    (void)lvl;
    (void)fmt;
}
