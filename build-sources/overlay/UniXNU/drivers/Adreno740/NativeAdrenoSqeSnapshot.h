/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_SQE_SNAPSHOT_H
#define NATIVE_ADRENO_SQE_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>

/* V5.3 evidence only. All fields are 32-bit words; no pointers, host ABI types,
 * kernel includes or implicit packing. Status 0 means completed; -1 means not
 * attempted. Values are UINT32_MAX until successful, with validity authoritative.
 * SQE index 0/1 is BR/BV, selector starts at zero (PC/raw state are words 0/1; word 1 is not
 * assumed to be an executed packet).
 * A DATA read failure ends that bank: an unsuccessful read may have advanced its
 * indexed port. read_status/first_failed_word describe the first failed read.
 * Selector restore is attempted only if the original selector was read and the
 * selection attempted. The board serializes every user of these indexed ports. */
enum { NativeAdrenoSqeBanks=2, NativeAdrenoSqeBankWords=64,
       NativeAdrenoPublishedTailWords=64, NativeAdrenoEvidenceRegs=9,
       NativeAdrenoSwitchEvidenceSlots=2, NativeAdrenoSwitchEvidenceVersion=1 };
struct NativeAdrenoSqeSnapshot {
    uint32_t words[2][64];
    uint32_t valid_mask[2][2];
    uint32_t selector_saved[2];
    int32_t selector_read_status[2];
    int32_t selector_select_status[2];
    int32_t read_status[2];
    uint32_t first_failed_word[2];
    int32_t selector_restore_status[2];
};
static inline void NativeAdrenoSqeSnapshotInit(struct NativeAdrenoSqeSnapshot *out)
{
    unsigned b,i;
    for(b=0;b<2;++b) {
        for(i=0;i<64;++i)out->words[b][i]=UINT32_MAX;
        out->valid_mask[b][0]=out->valid_mask[b][1]=0;
        out->selector_saved[b]=out->first_failed_word[b]=UINT32_MAX;
        out->selector_read_status[b]=out->selector_select_status[b]=-1;
        out->read_status[b]=out->selector_restore_status[b]=-1;
    }
}
/* A raw 64-dword window ending immediately BEFORE the actual CP_RB_WPTR read
 * from hardware while this is the loaded ring. The boundary is never next_.
 * Unpublished [hw_wptr, software_next) words are excluded even across wrap.
 * The window may contain older wrapped commands; it is not a parsed IB or a
 * claim that all fetched commands executed. valid_mask bits 0/1: RPTR/WPTR,
 * bit 2: tail copied. Bad WPTR or barrier failure leaves count=0. */
struct NativeAdrenoPublishedTail {
    uint32_t ring_words,software_next,hw_rptr,hw_wptr;
    int32_t read_status[2];
    uint32_t valid_mask,first,count;
    int32_t barrier_status;
    uint32_t begin_us[2],end_us[2];
    uint32_t words[64];
};
/* CPU views of the first 64 record bytes and first 28 SMMU-info bytes. These
 * are already driver-owned coherent/uncached mappings. A successful barrier
 * orders these reads but cannot make a GPU-updated page an atomic snapshot.
 * valid_mask bits 0/1 record pages, bits 2/3 SMMU pages; missing page is invalid. */
struct NativeAdrenoPreemptPages {
    uint32_t record[2][16];
    uint32_t smmu[2][7];
    uint32_t valid_mask;
    int32_t barrier_status;
};
struct NativeAdrenoSwitchEvidence {
    uint32_t version,word_count,sequence,from_ring,to_ring,reason,clock_present;
    uint32_t trigger_us[2],begin_us[2],end_us[2];
    uint32_t record_iova[2][2],smmu_iova[2][2];
    /* CNTL, SMMU lo/hi, NONSEC restore lo/hi, LEVEL_STATUS, RPTR, WPTR,
     * CNTL again after all other timeout reads. Registers 0..7 precede SQE;
     * register 8 follows SQE and page capture. Bit i is valid iff status[i]==0. */
    uint32_t registers[9];
    int32_t register_status[9];
    uint32_t register_valid_mask;
    int32_t sqe_status;
    struct NativeAdrenoPublishedTail published_tail;
    struct NativeAdrenoPreemptPages before,after;
    struct NativeAdrenoSqeSnapshot sqe;
};
enum { NativeAdrenoSwitchEvidenceWords=sizeof(struct NativeAdrenoSwitchEvidence)/sizeof(uint32_t) };
#ifdef __cplusplus
extern "C" {
#endif
/* No MMIO, locks, allocations or state changes: acquire a fully frozen first or
 * second timeout. Returns its word count, or 0 (out remains untouched). */
uint32_t native_adreno_switch_evidence_copy(uint32_t index,uint32_t *out,size_t capacity_words);
#ifdef __cplusplus
}
static_assert(sizeof(NativeAdrenoSwitchEvidence)==NativeAdrenoSwitchEvidenceWords*4,"evidence has padding");
static_assert(NativeAdrenoSwitchEvidenceWords<=512,"BootLog evidence budget");
#endif
#endif
