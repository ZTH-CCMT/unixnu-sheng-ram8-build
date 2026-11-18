// SPDX-License-Identifier: BSD-2-Clause-Patent
// Finger report ordering for Astra algorithm V3. Integer-only, bounded, no allocation.
// Keep this object off a kernel thread's stack: caller owns persistent storage.
#ifndef SHENG_TOUCH_REPORT_TIMELINE_H
#define SHENG_TOUCH_REPORT_TIMELINE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include "NvtTouchCore.h"
}

struct TouchReportTimeline {
    // The digitizer report descriptor has ten contacts.
    enum { Capacity=32, ReportCapacity=10, MaxFlush=4 };
    typedef bool (*Callback)(void *,unsigned,const nvt_contact *,uint64_t,uint32_t);
    struct Point {
        nvt_contact contact;
        uint64_t generation;
    };
    struct Frame {
        uint64_t timeUs,timeTicks;
        unsigned count;
        uint32_t cancelledIds;
        Point point[ReportCapacity];
    };
    struct PendingStart { uint64_t generation, ticks; };

    Frame frames[Capacity];
    Point emitted[ReportCapacity];
    PendingStart starts[NVT_SLOTS];
    unsigned head,used,emittedCount;
    uint64_t lastInputUs,lastInputTicks,lastAttemptedTicks;
    unsigned clockValid;
    uint64_t fed,submitted,pendingDropped,capacityFaults,clockFaults,submitFailures;

    // POD with explicit reset: valid after IOMalloc + reset(), no constructor.
    void reset() {memset(this,0,sizeof(*this));}
    Frame &at(unsigned i) {return frames[(head+i)%Capacity];}
    const Frame &at(unsigned i) const {return frames[(head+i)%Capacity];}

    static bool sameIdentity(const Point &a, const Point &b) {
        return a.contact.tracking_id==b.contact.tracking_id && a.generation==b.generation;
    }
    static bool contains(const Frame &frame, const Point &point) {
        for (unsigned i=0;i<frame.count;++i) if(sameIdentity(frame.point[i],point)) return true;
        return false;
    }
    static bool visible(const nvt_slot &slot) {return slot.active && slot.age>=2 && slot.generation;}
    static void append(Frame &frame, const nvt_slot &slot, unsigned index) {
        Point &point=frame.point[frame.count++];
        point={};
        point.contact.slot=static_cast<int>(index);
        point.contact.tracking_id=slot.tracking_id;
        point.contact.x=slot.rx;
        point.contact.y=slot.ry;
        point.generation=slot.generation;
    }

    void enqueueSnapshot(Frame &frame) {
        if (used==Capacity) {
            // Keep the head for retry, including any partial HID submission.
            // Only the first disappearance of each head identity can end it.
            // A later cancellation of a reused ID belongs to another generation.
            const Frame &first=at(0);
            uint32_t cancelledIds=0;
            for (unsigned j=0;j<first.count;++j) {
                const Point &point=first.point[j];
                for (unsigned i=1;i<=used;++i) {
                    const Frame &following=i==used ? frame : at(i);
                    if (contains(following,point)) continue;
                    cancelledIds|=following.cancelledIds & (UINT32_C(1)<<point.contact.tracking_id);
                    break;
                }
            }
            frame.cancelledIds=cancelledIds;
            used=1;
            ++capacityFaults;
        }
        at(used++)=frame;
    }

    // Synchronize HID after a clock change or an out-of-band scripted report.
    void restartReports(uint64_t nowUs, uint64_t ticks, uint32_t cancelledIds) {
        head=used=0;
        clockValid=0;
        memset(starts,0,sizeof(starts));
        Frame release={};
        release.timeUs=nowUs;
        release.timeTicks=ticks;
        release.cancelledIds=cancelledIds;
        enqueueSnapshot(release);
    }

    bool unchanged(const Frame &frame) const {
        const unsigned count=used ? at(used-1).count : emittedCount;
        const Point *prior=used ? at(used-1).point : emitted;
        if (frame.cancelledIds || frame.count!=count) return false;
        for (unsigned i=0;i<count;++i) {
            const Point &next=frame.point[i];
            if (!sameIdentity(prior[i],next) || prior[i].contact.x!=next.contact.x ||
                prior[i].contact.y!=next.contact.y) return false;
        }
        return true;
    }

    bool clockMovedBack(uint64_t nowUs, uint64_t nowTicks) const {
        return clockValid && (nowUs<lastInputUs || nowTicks<lastInputTicks);
    }

    bool feed(uint64_t nowUs,uint64_t nowTicks,const nvt_touch_core &core) {
        if (clockMovedBack(nowUs,nowTicks)) {
            ++clockFaults;
            restartReports(nowUs,nowTicks,UINT32_MAX);
        }
        clockValid=1;
        lastInputUs=nowUs;
        lastInputTicks=nowTicks;
        ++fed;

        for (unsigned s=0;s<NVT_SLOTS;++s) {
            const auto &slot=core.slots[s];
            auto &start=starts[s];
            if (start.generation && (!slot.active || slot.generation!=start.generation)) {
                start={};
                ++pendingDropped;
            }
            if (slot.active && slot.age<2 && !start.generation)
                start={slot.generation,nowTicks};
        }

        Frame frame={};
        frame.timeUs=nowUs;
        frame.timeTicks=nowTicks;
        frame.cancelledIds=core.cancelled_ids;
        const unsigned priorCount=used ? at(used-1).count : emittedCount;
        const Point *prior=used ? at(used-1).point : emitted;
        uint32_t selected=0;
        // Keep active members of the previous snapshot before filling vacancies.
        for (unsigned j=0;j<priorCount;++j) {
            const unsigned s=static_cast<unsigned>(prior[j].contact.slot);
            const auto &slot=core.slots[s];
            if (!visible(slot) || slot.generation!=prior[j].generation ||
                slot.tracking_id!=prior[j].contact.tracking_id) continue;
            append(frame,slot,s);
            selected|=UINT32_C(1)<<s;
        }
        for (unsigned s=0;s<NVT_SLOTS && frame.count<ReportCapacity;++s) {
            if (!(selected & (UINT32_C(1)<<s)) && visible(core.slots[s])) append(frame,core.slots[s],s);
        }

        // A lone confirmation can retain its original DOWN. Pending points
        // stay outside the FIFO and never delay an already visible contact.
        if (!priorCount && frame.count==1 && !frame.cancelledIds) {
            const unsigned s=static_cast<unsigned>(frame.point[0].contact.slot);
            const auto &slot=core.slots[s];
            if (starts[s].generation==slot.generation && nowUs>=slot.born_us &&
                nowUs-slot.born_us<NVT_PENDING_MAX_US) {
                Frame origin=frame;
                origin.timeTicks=starts[s].ticks;
                origin.point[0].contact.x=slot.origin_x;
                origin.point[0].contact.y=slot.origin_y;
                enqueueSnapshot(origin);
            }
        }
        for (unsigned s=0;s<NVT_SLOTS;++s) if (visible(core.slots[s])) starts[s]={};
        if (!unchanged(frame)) enqueueSnapshot(frame);
        return true;
    }

    bool deliver(Callback callback,void *context,const Point *points,unsigned count,uint64_t ticks,uint32_t cancelledIds) {
        if (ticks<lastAttemptedTicks) ticks=lastAttemptedTicks;
        lastAttemptedTicks=ticks;
        nvt_contact contacts[ReportCapacity];
        for (unsigned j=0;j<count;++j) contacts[j]=points[j].contact;
        if (!callback || !callback(context,count,contacts,ticks,cancelledIds)) {
            ++submitFailures;
            return false;
        }
        emittedCount=count;
        for (unsigned j=0;j<count;++j) emitted[j]=points[j];
        ++submitted;
        return true;
    }

    unsigned flush(uint64_t nowUs,Callback callback,void *context) {
        unsigned released=0;
        while (used && released<MaxFlush) {
            const Frame &frame=at(0);
            if (nowUs<frame.timeUs) break;
            // HID has no generations. Clear a reused ID before its next DOWN.
            Point kept[ReportCapacity];
            unsigned keep=0;
            uint32_t ending=0;
            bool reused=false;
            for (unsigned j=0;j<emittedCount;++j) {
                const auto &old=emitted[j];
                const int id=old.contact.tracking_id;
                bool survives=false;
                for (unsigned k=0;k<frame.count;++k) {
                    if (frame.point[k].contact.tracking_id!=id) continue;
                    survives=frame.point[k].generation==old.generation;
                    if (!survives) reused=true;
                    break;
                }
                if (survives) kept[keep++]=old;
                else ending|=UINT32_C(1)<<id;
            }
            if (reused && !deliver(callback,context,kept,keep,frame.timeTicks,frame.cancelledIds & ending)) break;
            if (!deliver(callback,context,frame.point,frame.count,frame.timeTicks,frame.cancelledIds)) break;
            head=(head+1)%Capacity;
            --used;
            ++released;
        }
        return released;
    }

    // 1 means the queued snapshot can be retried now.
    uint64_t waitUs(uint64_t nowUs) const {
        if (clockValid && nowUs<lastInputUs) return 1;
        if (!used) return 0;
        return at(0).timeUs>nowUs ? at(0).timeUs-nowUs : 1;
    }
};
static_assert(sizeof(TouchReportTimeline)<32768,"Timeline must stay bounded and off-stack");
#endif
