/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_RING_LOG_H
#define NATIVE_ADRENO_RING_LOG_H

#include <stdint.h>

enum {
    NativeAdrenoRingLogRows = 4,
    NativeAdrenoRingLogHeaderWords = 4,
    NativeAdrenoRingLogPrefixWords = 112,
    NativeAdrenoRingLogRowWords = NativeAdrenoRingLogHeaderWords + NativeAdrenoRingLogPrefixWords
};

/* Fence, start, end, total word count, then the captured prefix. */
typedef uint32_t NativeAdrenoRingLogRow[NativeAdrenoRingLogRowWords];

#ifdef __cplusplus
extern "C" {
#endif

/* Returns up to capacity rows, newest first. */
uint32_t native_adreno_ring_log_copy(NativeAdrenoRingLogRow *out, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
