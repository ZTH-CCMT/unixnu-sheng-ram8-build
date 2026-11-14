/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_GPU_SQE_CAPTURE_H
#define SHENG_GPU_SQE_CAPTURE_H
#include "NativeAdrenoSqeSnapshot.h"

/* Only diagnostic index ports are written. No power vote, scheduling operation,
 * allocation, wait or retry is permitted while this gate is held. Zero-initialized
 * storage makes the gate ready before driver attachment, without late allocation.
 * The board disables local interrupts before one strong CAS; genuine contention
 * reports Busy rather than waiting under the GPU lock. Strong CAS avoids losing
 * the once-only timeout capture to an uncontended spurious failure. */
struct GpuSqeCaptureGate { uint32_t held; };
struct GpuSqeCaptureIo {
    void *cookie;
    uintptr_t (*interrupt_save)(void *);
    void (*interrupt_restore)(void *, uintptr_t);
    int (*gx_available)(void *); /* 0 only when GX MMIO may be accessed */
    int (*read32)(void *, uint32_t dword_register, uint32_t *);
    int (*write32)(void *, uint32_t dword_register, uint32_t);
    void (*barrier)(void *);
};
enum {
    GpuSqeCaptureBusy = -1001,
    GpuSqeCaptureGxOff = -1002,
    GpuSqeCaptureGxReadFailed = -1003,
    GpuSqeCaptureUnavailable = -1004,
    GpuSqeCaptureInvalid = -1005
};
#ifdef __cplusplus
static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "SQE gate must be lock-free");
#else
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "SQE gate must be lock-free");
#endif

static inline int GpuSqeCapture(struct GpuSqeCaptureGate *gate,
                               const struct GpuSqeCaptureIo *io,
                               struct NativeAdrenoSqeSnapshot *out,
                               unsigned count)
{
    /* A7xx BR/BV STAT is an auto-incrementing indexed port, in DWORD units.
     * Qualcomm gen7_cp_indexed_reg_list snapshots 0x40 words from each bank.
     * Word 0 is SQE PC; word 1 is raw state, a PM4 header only if it validates.
     * Reading neither freezes SQE nor provides an atomic two-bank snapshot. */
    static const uint32_t selector[2] = {0x0908, 0x0a87};
    static const uint32_t data[2] = {0x0909, 0x0a88};
    uintptr_t irq;
    uint32_t unlocked = 0;
    unsigned b, i;
    int result;
    if (!out) return GpuSqeCaptureInvalid;
    NativeAdrenoSqeSnapshotInit(out);
    if (!gate || !io || !count || count > NativeAdrenoSqeBankWords ||
        !io->interrupt_save || !io->interrupt_restore || !io->gx_available ||
        !io->read32 || !io->write32 || !io->barrier) return GpuSqeCaptureInvalid;
    irq = io->interrupt_save(io->cookie);
    if (!__atomic_compare_exchange_n(&gate->held, &unlocked, 1, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        io->interrupt_restore(io->cookie, irq);
        return GpuSqeCaptureBusy;
    }
    result = io->gx_available(io->cookie);
    if (!result) for (b = 0; b < NativeAdrenoSqeBanks; ++b) {
        uint32_t saved = UINT32_MAX;
        int status = io->read32(io->cookie, selector[b], &saved);
        out->selector_read_status[b] = status;
        if (status) { if (!result) result = status; continue; }
        out->selector_saved[b] = saved;
        status = io->write32(io->cookie, selector[b], 0);
        out->selector_select_status[b] = status;
        io->barrier(io->cookie);
        if (!status) {
            out->read_status[b] = 0;
            for (i = 0; i < count; ++i) {
                uint32_t word = UINT32_MAX;
                status = io->read32(io->cookie, data[b], &word);
                if (status) {
                    out->read_status[b] = status;
                    out->first_failed_word[b] = i;
                    if (!result) result = status;
                    /* A failed read might have advanced DATA: do not assign
                     * later reads guessed indices. Still restore the selector. */
                    break;
                }
                out->words[b][i] = word;
                out->valid_mask[b][i / 32] |= UINT32_C(1) << (i % 32);
            }
        } else if (!result) result = status;
        /* Selection may have partially reached the device even on an error. */
        status = io->write32(io->cookie, selector[b], saved);
        out->selector_restore_status[b] = status;
        io->barrier(io->cookie);
        if (status && !result) result = status;
    }
    __atomic_store_n(&gate->held, 0, __ATOMIC_RELEASE);
    io->interrupt_restore(io->cookie, irq);
    return result;
}
#endif
