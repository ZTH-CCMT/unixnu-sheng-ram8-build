// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TouchAstraProfile.h"
#include "TouchEngine.h"
#include "TouchReportTimeline.h"
#include "NativeHidTouchPlan.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>

static constexpr uint64_t scan = 8333;
static constexpr uint64_t last = 2 * scan;

struct Panel {
    std::unique_ptr<nvt_touch_core> core{new nvt_touch_core{}};
    std::unique_ptr<nvt_partial_state> partial{new nvt_partial_state{}};
    TouchEngine engine{core.get(), partial.get()};
    std::array<int16_t, NVT_NODES> input{}, matrix{};
    std::array<uint8_t, NVT_THP_FRAME_LEN> raw{};
    nvt_contact out[NVT_SLOTS]{};
    uint16_t sequence = 0;

    Panel() {
        const auto profile = touchAstraReportExperimentProfile();
        assert(nvt_partial_configure(partial.get(), &profile));
        input.fill(10000);
        assert(frame(0, 0, 2) == -1);
    }

    int frame(uint64_t when, unsigned block = 0, unsigned type = 4) {
        raw.fill(0);
        auto *header = raw.data() + NVT_THP_HEADER_LEN;
        ++sequence;
        header[2] = static_cast<uint8_t>(sequence);
        header[3] = static_cast<uint8_t>(sequence >> 8);
        header[12] = header[13] = 255;
        for (unsigned i = 16; i < 20; ++i) header[i] = 255;
        header[24] = static_cast<uint8_t>(type);
        if (block) {
            raw[313] = 0x1d;
            raw[332] = static_cast<uint8_t>(block);
        }
        const unsigned start = block ? 4217 : NVT_THP_MATRIX_OFFSET;
        const unsigned count = block ? 600 : NVT_NODES;
        const unsigned first = block ? (block - 1) * 600 : 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto sample = static_cast<uint16_t>(input[first + i]);
            raw[start + 2 * i] = static_cast<uint8_t>(sample);
            raw[start + 2 * i + 1] = static_cast<uint8_t>(sample >> 8);
        }
        return engine.frame(raw.data(), raw.size(), when, matrix.data(), out, NVT_SLOTS);
    }

    void finger(double column) {
        for (unsigned r = 0; r < NVT_ROWS; ++r) {
            for (unsigned c = 0; c < NVT_COLUMNS; ++c) {
                const double x = c - column, y = (r - 14.5) / .85;
                input[r * NVT_COLUMNS + c] =
                    static_cast<int16_t>(10000 + std::lround(1000 * std::exp(-.5 * (x * x + y * y))));
            }
        }
    }

    nvt_slot& begin() {
        finger(25.2);
        frame(scan);
        assert(frame(last) == 1);
        return core->slots[out[0].slot];
    }
};

static void check_stripe() {
    {
        Panel panel;
        auto& contact = panel.begin();
        panel.input.fill(10000);
        assert(panel.frame(last + scan, 2) == 0);
        assert(contact.last_covered_negative_us == last + scan);
        assert(!panel.core->cancelled_ids);
    }
    {
        Panel panel;
        auto& contact = panel.begin();
        panel.input.fill(10000);
        assert(panel.frame(last + scan, 1) == TouchEngine::SpecialP);
        assert(contact.active && contact.last_observed_us == last);
    }
    {
        Panel panel;
        auto& contact = panel.begin();
        contact.dy = 1200;
        panel.input.fill(10000);
        assert(panel.frame(last + scan, 2) == TouchEngine::SpecialP);
        assert(contact.active);
    }
    {
        Panel panel;
        auto& contact = panel.begin();
        panel.input.fill(10000);
        ++panel.sequence;
        assert(panel.frame(last + 2 * scan, 2) == TouchEngine::SpecialP);
        assert(contact.active);
    }
}

static void check_deadline() {
    for (unsigned block : {0u, 2u}) {
        Panel panel;
        auto& contact = panel.begin();
        const auto expected = contact.generation;
        assert(panel.frame(last + NVT_OBSERVATION_MAX_US, block) == 1);
        assert(contact.generation == expected && contact.last_observed_us == last + NVT_OBSERVATION_MAX_US);
        assert(!panel.core->cancelled_ids);
        panel.input.fill(10000);
        assert(panel.frame(last + NVT_OBSERVATION_MAX_US + scan) == 0);
        assert(!panel.core->cancelled_ids);
    }
    Panel panel;
    auto& contact = panel.begin();
    const auto id = contact.tracking_id;
    const auto deadline = last + NVT_OBSERVATION_MAX_US;
    assert(nvt_touch_note_presence(panel.core.get(), id, contact.generation, deadline - 5000));
    assert(panel.engine.expire(deadline, panel.out, NVT_SLOTS) == 0);
    assert(panel.core->cancelled_ids == (1u << id));
    panel.frame(deadline + scan);
    assert(panel.frame(deadline + 2 * scan) == 1);
    assert(panel.out[0].tracking_id != id);
}

static void check_hid() {
    nht_state ack{};
    ack.count = 1;
    ack.point[0] = {1, 500, 500};
    nht_plan plan{};
    const nht_point next{2, 520, 500};
    assert(nht_build_with_cancel(&ack, &next, 1, 2, 1u << 1, &plan) == 3);
    assert(plan.packet[0].bytes[1] == NHT_CANCEL);
    assert(plan.packet[1].bytes[1] == NHT_UP);
    assert(plan.packet[2].bytes[1] == NHT_DOWN);
    assert(nht_commit(&ack, &plan, 0, 1));
    assert(!nht_commit(&ack, &plan, 1, 0));
    assert(ack.cancel_sent && ack.count == 1 && ack.sequence == 0);
    assert(nht_build_with_cancel(&ack, &next, 1, 4, 1u << 1, &plan) == 2);
    assert(plan.packet[0].bytes[1] == NHT_UP);
    assert(nht_commit(&ack, &plan, 0, 1));
    assert(!nht_commit(&ack, &plan, 1, 0));
    assert(!ack.cancel_sent && !ack.count);
    assert(nht_build_with_cancel(&ack, &next, 1, 6, 1u << 1, &plan) == 1);
    assert(plan.packet[0].bytes[1] == NHT_DOWN);

    ack.count = 2;
    ack.point[0] = {1, 500, 500};
    ack.point[1] = next;
    assert(nht_build_with_cancel(&ack, &next, 1, 8, 1u << 1, &plan) == 1);
    assert(plan.packet[0].bytes[1] == NHT_DOWN && plan.packet[0].bytes[7] == NHT_UP);
    assert(nht_build_with_cancel(&ack, nullptr, 0, 10, 0, &plan) == 1);
    assert(plan.packet[0].bytes[1] == NHT_UP);
}

static void check_queue() {
    Panel panel;
    const auto id = panel.begin().tracking_id;
    auto timeline = std::make_unique<TouchReportTimeline>();
    timeline->reset();
    struct Sink { bool ready; uint32_t mask; } sink{false, 0};
    const auto submit = [](void *data, unsigned, const nvt_contact *, uint64_t, uint32_t mask) {
        auto& s = *static_cast<Sink *>(data);
        s.mask = mask;
        return s.ready;
    };
    const auto deadline = last + NVT_OBSERVATION_MAX_US;
    assert(panel.engine.expire(deadline, panel.out, NVT_SLOTS) == 0);
    assert(timeline->feed(deadline, deadline, *panel.core));
    assert(timeline->flush(deadline, submit, &sink) == 0 && timeline->used == 1);
    panel.core->cancelled_ids = 0;
    sink.ready = true;
    assert(timeline->flush(deadline + 1, submit, &sink) == 1);
    assert(sink.mask == (1u << id));
}

static void check_motion() {
    {
        Panel panel;
        const auto& contact = panel.begin();
        const int origin = contact.x;
        panel.finger(26.0);
        assert(panel.frame(last + scan) == 1);
        const int raw_step = contact.x - origin;
        const int reported_step = panel.out[0].x - origin;
        assert(raw_step > 32 * NVT_SUPER_RESOLUTION);
        assert(reported_step * 2 >= raw_step && reported_step <= raw_step);
    }
    {
        Panel panel;
        const auto& contact = panel.begin();
        nvt_contact measured = panel.out[0];
        uint64_t time = last;
        const auto measure = [&](int x) {
            measured.x = x;
            time += scan;
            assert(nvt_touch_process_partial(panel.core.get(), &measured, 1, time, panel.out, NVT_SLOTS) == 1);
            return panel.out[0].x;
        };
        int x = contact.x;
        int reported = x;
        const int step = 25; // 300 px/s at 120 Hz
        for (unsigned i = 0; i < 36; ++i) {
            x += step;
            reported = measure(x);
            assert(reported <= x);
        }
        assert(x - reported <= step);
        x -= step;
        int next = measure(x);
        assert(next < reported && next >= x);
        for (unsigned i = 0; i < 12; ++i) {
            reported = next;
            next = measure(x);
            assert(next <= reported && next >= x);
        }
        for (unsigned i = 0; i < 36; ++i) {
            x -= 40; // Establish motion in the old direction at 480 px/s.
            measure(x);
        }
        for (unsigned i = 0; i < 48; ++i) {
            x += step;
            // The signed speed passes through zero during this steady return.
            assert(x - measure(x) <= 3 * step);
        }
        measure(x);
        assert(contact.motion_weight_x == 0);
    }
    {
        Panel panel;
        const int origin = panel.begin().x;
        nvt_contact measured = panel.out[0];
        measured.x = origin + 4 * NVT_FUZZ;
        assert(nvt_touch_process_partial(panel.core.get(), &measured, 1, last + scan, panel.out, NVT_SLOTS) == 1);
        assert(panel.out[0].x - origin <= 35);
        measured.x = origin;
        assert(nvt_touch_process_partial(panel.core.get(), &measured, 1, last + 2 * scan, panel.out, NVT_SLOTS) == 1);
        assert(panel.out[0].x - origin <= 35);
    }
}

int main() {
    check_stripe();
    std::puts("PASS covered, unrelated, moving, and skipped P stripes");
    check_deadline();
    std::puts("PASS same-ID recovery at the deadline and cancellation after silence");
    check_hid();
    std::puts("PASS cancel, tip-state clear, retry, new down, and surviving finger reports");
    check_queue();
    std::puts("PASS queued cancellation survives an unsuccessful publication");
    check_motion();
    std::puts("PASS initial motion, sustained drag, reversal, stop, and isolated noise");
}
