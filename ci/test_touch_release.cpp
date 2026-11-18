// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TouchAstraProfile.h"
#include "TouchEngine.h"
#include "TouchReportTimeline.h"
#include "NativeHidTouchPlan.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
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

    explicit Panel(uint16_t initialSequence = 0) : sequence(initialSequence) {
        const auto profile = touchAstraReportExperimentProfile();
        assert(nvt_partial_configure(partial.get(), &profile));
        input.fill(10000);
        assert(frame(0, 0, 2, 0) == -1);
    }

    void encode(unsigned block, unsigned type, unsigned dataType) {
        raw.fill(0);
        auto *header = raw.data() + NVT_THP_HEADER_LEN;
        ++sequence;
        header[2] = static_cast<uint8_t>(sequence);
        header[3] = static_cast<uint8_t>(sequence >> 8);
        header[12] = header[13] = 255;
        for (unsigned i = 16; i < 20; ++i) header[i] = 255;
        header[24] = static_cast<uint8_t>(type);
        raw[NVT_THP_DATA_TYPE_OFFSET] = static_cast<uint8_t>(dataType);
        if (block) {
            raw[NVT_THP_DATA_TYPE_OFFSET] = 0x1d;
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
    }

    int receive(uint64_t when) {
        return engine.frame(raw.data(), raw.size(), when, matrix.data(), out, NVT_SLOTS);
    }

    int frame(uint64_t when, unsigned block = 0, unsigned type = 4, unsigned dataType = 0x03) {
        encode(block, type, dataType);
        return receive(when);
    }

    void finger(double column, int peak = 1000, double row = 14.5) {
        for (unsigned r = 0; r < NVT_ROWS; ++r) {
            for (unsigned c = 0; c < NVT_COLUMNS; ++c) {
                const double x = c - column, y = (r - row) / .85;
                input[r * NVT_COLUMNS + c] =
                    static_cast<int16_t>(10000 + std::lround(peak * std::exp(-.5 * (x * x + y * y))));
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

static void check_frame_admission() {
    Panel panel;
    const auto &contact = panel.begin();
    const auto token = panel.engine.sampleToken;
    const auto counter = panel.engine.streamCounter;
    panel.input.fill(10000);
    for (unsigned tag : {0x00u, 0x1cu, 0x7eu}) {
        assert(panel.frame(last + scan, 0, 4, tag) == TouchEngine::Rejected);
        assert(panel.engine.streamUs == last && panel.engine.sampleToken == token);
        assert(panel.engine.streamCounter == counter && contact.last_observed_us == last && contact.active);
    }
    panel.encode(2, 4, 0x03);
    for (unsigned invalidBlock : {0u, 5u}) {
        panel.raw[332] = static_cast<uint8_t>(invalidBlock);
        assert(panel.receive(last + scan) == TouchEngine::Rejected);
        assert(panel.engine.streamUs == last && panel.engine.sampleToken == token && contact.active);
    }
}

static void check_counter_order() {
    {
        Panel panel(97);
        const auto &contact = panel.begin();
        const auto token = panel.engine.sampleToken;
        assert(panel.sequence == 100);
        panel.sequence = 97;
        assert(panel.frame(last + scan) == TouchEngine::Stale);
        panel.sequence = 99;
        assert(panel.frame(last + 2 * scan) == TouchEngine::Stale);
        assert(contact.last_observed_us == last && panel.engine.sampleToken == token);
        panel.sequence = 101;
        assert(panel.frame(last + 3 * scan) == 1);
        assert(panel.engine.streamCounter == 102 && panel.engine.sampleToken == token + 1);
    }
    {
        Panel panel(65531);
        // Exercise the modular comparison across a wrap with real +2 strides.
        panel.finger(25.2);
        panel.sequence = 65533;
        assert(panel.frame(scan) >= 0 && panel.engine.streamCounter == 65534);
        panel.sequence = 65535;
        assert(panel.frame(last) == 1 && panel.engine.streamCounter == 0);
        panel.sequence = 1;
        assert(panel.frame(last + scan) == 1 && panel.engine.streamCounter == 2);
    }
    {
        Panel panel(97);
        const auto &old = panel.begin();
        const int id = old.tracking_id;
        const auto generation = old.generation;
        const uint64_t idle = last + NVT_OBSERVATION_MAX_US;
        const auto token = panel.engine.sampleToken;

        panel.sequence = 1;
        assert(panel.frame(idle) == TouchEngine::Stale);
        assert(panel.receive(idle + scan) == TouchEngine::Stale);
        assert(panel.engine.streamCounter == 100 && panel.engine.sampleToken == token);

        panel.sequence = 3;
        assert(panel.frame(idle + 2 * scan) >= 0);
        assert(panel.engine.streamCounter == 4 && panel.engine.sampleToken == token + 1);
        assert(panel.core->cancelled_ids == (1u << id));
        assert(panel.frame(idle + 3 * scan) == 1);
        assert(panel.out[0].tracking_id != id && panel.core->slots[panel.out[0].slot].generation != generation);
    }
}

static void check_baseline() {
    constexpr int center = 15 * NVT_COLUMNS + 25;
    for (const unsigned pressed : {37u, 53u}) {
        Panel panel;
        nvt_touch_reset(panel.core.get());
        for (unsigned sample = 1; sample <= NVT_STARTUP_FRAMES; ++sample) {
            if (sample == NVT_STARTUP_FRAMES - pressed + 1) panel.finger(25, 1000, 15);
            assert(panel.frame(sample * scan) == -1);
        }
        assert(panel.core->reference[center] == 10000);
        assert(panel.frame((NVT_STARTUP_FRAMES + 1) * scan) == 1);
    }
    {
        Panel panel;
        int id = 0;
        for (unsigned sample = 1; sample <= 120; ++sample) {
            panel.finger(25, 5 * sample, 15);
            const int count = panel.frame(sample * scan);
            assert(count == 0 || count == 1);
            if (id) assert(count == 1 && panel.out[0].tracking_id == id);
            if (count) id = panel.out[0].tracking_id;
        }
        assert(id && panel.core->delta[center] > 500);
        panel.input.fill(10000);
        assert(panel.frame(121 * scan) == 0);
        assert(!panel.core->cancelled_ids);
    }
    {
        Panel panel;
        const int id = panel.begin().tracking_id;
        const auto contact = panel.input;
        constexpr int distant = 30 * NVT_COLUMNS + 50;
        for (int sample = 1; sample <= 65; ++sample) {
            for (int node = 0; node < NVT_NODES; ++node)
                panel.input[node] = static_cast<int16_t>(contact[node] + sample);
            panel.input[distant] += 5 * sample;
            assert(panel.frame(last + sample * scan) == 1);
            assert(panel.out[0].tracking_id == id);
        }
        assert(panel.core->reference[center] == 10000);
        assert(panel.core->reference[0] == 10065);
        assert(panel.core->reference[distant] > 10150);
        assert(panel.core->delta[distant] < NVT_UP_THRESHOLD);
    }
    {
        Panel panel;
        panel.finger(25, 1000, 15);
        const auto first = panel.input;
        panel.finger(40, 850, 27);
        auto both = panel.input;
        for (int node = 0; node < NVT_NODES; ++node) both[node] += first[node] - 10000;
        uint32_t noise = 73;
        for (unsigned sample = 1; sample <= 512; ++sample) {
            for (int node = 0; node < NVT_NODES; ++node) {
                noise = 1664525u * noise + 1013904223u;
                panel.input[node] = static_cast<int16_t>(both[node] + int((noise >> 16) % 21) - 10);
            }
            assert(panel.frame(sample * scan) == (sample == 1 ? 0 : 2));
        }
        assert(panel.core->reference[center] == 10000);
        assert(panel.core->reference[27 * NVT_COLUMNS + 40] == 10000);
    }
}

static void check_candidate_geometry() {
    {
        Panel panel;
        const int center = 15 * NVT_COLUMNS + 25;
        panel.input[center] += 800;
        panel.input[center + 2] += 800;
        assert(panel.frame(scan) == 0 && panel.frame(last) == 0);
        assert(panel.core->next_generation == 0);
        nvt_region_candidates region{};
        assert(nvt_touch_extract_region(panel.core.get(), panel.input.data() + 10 * NVT_COLUMNS,
                                        10, 10, &region) == 0);
    }
    {
        Panel panel;
        panel.input[NVT_COLUMNS - 1] += 500;
        panel.input[NVT_COLUMNS - 2] += 200;
        assert(panel.frame(scan) == 0 && panel.frame(last) == 1);
        const int id = panel.out[0].tracking_id;
        for (unsigned sample = 3; sample <= 64; ++sample) {
            assert(panel.frame(sample * scan) == 1 && panel.out[0].tracking_id == id);
        }
        assert(panel.core->reference[NVT_COLUMNS - 1] == 10000);
    }
    {
        Panel panel;
        // Establish two contacts so that the nearest weak peak keeps its group.
        nvt_region_candidates history{};
        history.row_count = NVT_ROWS;
        history.count = 2;
        history.contact[0].x = nvt_touch_column_center(20) * NVT_SUPER_RESOLUTION;
        history.contact[1].x = nvt_touch_column_center(40) * NVT_SUPER_RESOLUTION;
        for (auto &contact : history.contact) {
            contact.y = nvt_touch_row_center(15) * NVT_SUPER_RESOLUTION;
            contact.peak = 800;
        }
        assert(nvt_touch_process_region(panel.core.get(), &history, scan, panel.out, NVT_SLOTS) == 0);
        assert(nvt_touch_process_region(panel.core.get(), &history, last, panel.out, NVT_SLOTS) == 2);
        panel.input[15 * NVT_COLUMNS + 20] += 250;
        panel.input[14 * NVT_COLUMNS + 20] += 150;
        panel.input[15 * NVT_COLUMNS + 22] += 850;
        panel.input[14 * NVT_COLUMNS + 22] += 400;
        panel.input[15 * NVT_COLUMNS + 40] += 750;
        panel.input[14 * NVT_COLUMNS + 40] += 400;
        nvt_region_candidates region{};
        assert(nvt_touch_extract_region(panel.core.get(), panel.input.data(), 0, NVT_ROWS, &region) == 2);
        assert(region.contact[0].peak == 850 && region.contact[0].area == 4);
        assert(region.contact[0].weight == (850 - 255) + (400 - 255));
        assert(region.contact[0].x == nvt_touch_column_center(22) * NVT_SUPER_RESOLUTION);
    }
    {
        Panel panel;
        nvt_region_candidates region{};
        for (const int fringe : {100, 400}) {
            for (int r = -4; r <= 4; ++r) {
                for (int c = -4; c <= 4; ++c) {
                    const int distance = std::abs(r) + std::abs(c);
                    panel.input[(15 + r) * NVT_COLUMNS + 25 + c] =
                        static_cast<int16_t>(10000 + fringe + 3 * (8 - distance));
                }
            }
            panel.input[15 * NVT_COLUMNS + 25] = 10800;
            const int count = nvt_touch_extract_region(panel.core.get(), panel.input.data(), 0, NVT_ROWS, &region);
            if (fringe == 100) {
                assert(count == 1 && region.contact[0].area == 81 && region.contact[0].weight == 560);
            } else {
                assert(count == 0);
            }
        }
    }
}

static nvt_contact point(int x, int y, int peak = 900, int area = 8) {
    nvt_contact contact{};
    contact.x = x * NVT_SUPER_RESOLUTION;
    contact.y = y * NVT_SUPER_RESOLUTION;
    contact.peak = peak;
    contact.area = area;
    return contact;
}

static int observe_points(Panel &panel, uint64_t time, std::initializer_list<nvt_contact> points) {
    nvt_region_candidates region{};
    region.row_count = NVT_ROWS;
    for (const auto &contact : points) region.contact[region.count++] = contact;
    return nvt_touch_process_region(panel.core.get(), &region, time, panel.out, NVT_SLOTS);
}

static const nvt_slot *find_track(const Panel &panel, int id) {
    for (const auto &slot : panel.core->slots)
        if (slot.active && slot.tracking_id == id) return &slot;
    return nullptr;
}

static void check_rejoin() {
    for (const bool delayed : {false, true}) {
        Panel panel;
        panel.finger(25, 1000, 15);
        panel.frame(scan);
        assert(panel.frame(last) == 1);
        auto &slot = panel.core->slots[panel.out[0].slot];
        const int original_x = slot.x, id = slot.tracking_id;
        const auto generation = slot.generation;
        panel.input.fill(10000);
        panel.input[15 * NVT_COLUMNS + 25] += 180;
        assert(panel.frame(last + scan) == 1);
        assert(slot.missed == 1 && slot.last_observed_us == last);
        if (delayed) ++panel.sequence;
        const uint64_t time = last + (delayed ? 3 : 2) * scan;
        panel.finger(25.55, 1000, 15);
        assert(panel.frame(time) == 1);
        assert(slot.x - original_x > 24 * NVT_SUPER_RESOLUTION);
        assert(slot.last_observed_us == time && slot.missed == 0);
        assert(slot.tracking_id == id && slot.generation == generation && !panel.core->cancelled_ids);
    }
    {
        Panel panel;
        panel.finger(25, 1000, 15);
        panel.frame(scan);
        assert(panel.frame(last) == 1);
        const int id = panel.out[0].tracking_id;
        panel.input.fill(10000);
        panel.input[15 * NVT_COLUMNS + 25] += 180;
        assert(panel.frame(last + scan) == 1);
        panel.finger(26.6, 1000, 15);
        assert(panel.frame(last + 2 * scan) == 0 && !find_track(panel, id));
        assert(panel.frame(last + 3 * scan) == 1 && panel.out[0].tracking_id != id);
    }
    {
        Panel panel;
        const nvt_contact initial = point(1048, 806);
        assert(observe_points(panel, scan, {initial}) == 0);
        assert(observe_points(panel, last, {initial}) == 1);
        const auto &slot = panel.core->slots[panel.out[0].slot];
        panel.input[15 * NVT_COLUMNS + 20] += 180;
        assert(panel.frame(last + scan) == 1 && slot.missed == 1);
        nvt_contact fitted = point(1075, 806, 250, 2);
        fitted.tracking_id = slot.tracking_id;
        assert(nvt_touch_process_partial(panel.core.get(), &fitted, 1, last + 2 * scan,
                                         panel.out, NVT_SLOTS) == 1);
        assert(slot.x == fitted.x && slot.shape_peak == initial.peak && slot.shape_area == initial.area);
    }
}

static void check_assignment() {
    {
        Panel panel;
        panel.finger(20, 1000, 15);
        panel.frame(scan);
        assert(panel.frame(last) == 1);
        const int id = panel.out[0].tracking_id;
        const auto generation = find_track(panel, id)->generation;
        panel.finger(21, 1000, 15);
        panel.input[15 * NVT_COLUMNS + 24] += 450;
        panel.input[15 * NVT_COLUMNS + 25] += 200;
        assert(panel.frame(last + scan) == 1);
        assert(panel.core->next_generation == generation + 1);
        panel.finger(23.8, 1000, 15);
        assert(panel.frame(last + 2 * scan) == 1);
        assert(panel.out[0].tracking_id == id && !panel.core->cancelled_ids);
    }
    {
        Panel panel;
        const auto a = point(1000, 500), b = point(830, 500);
        assert(observe_points(panel, scan, {a, b}) == 0);
        assert(observe_points(panel, last, {a, b}) == 2);
        const int first = panel.out[0].tracking_id, second = panel.out[1].tracking_id;
        assert(observe_points(panel, last + scan, {point(1010, 500), point(1190, 500)}) == 2);
        assert(find_track(panel, first)->x == point(1190, 500).x);
        assert(find_track(panel, second)->x == point(1010, 500).x);
    }
    for (const unsigned gap : {16666u, 25000u, 25001u}) {
        Panel panel;
        const auto a = point(996, 806), b = point(360, 410), c = point(1730, 650);
        assert(observe_points(panel, scan, {a, b, c}) == 0);
        assert(observe_points(panel, last, {a, b, c}) == 3);
        const int id = panel.out[0].tracking_id;
        assert(observe_points(panel, last + scan, {point(1048, 806), b, c, point(330, 1500, 500)}) == 3);
        const auto end = point(1249, 806);
        assert(observe_points(panel, last + scan + gap, {end, b, c}) == 3);
        const auto *track = find_track(panel, id);
        assert(track && track->x == end.x && track->last_observed_us == last + scan + gap);
    }
    for (const bool shape_required : {false, true}) {
        Panel panel;
        const int step = shape_required ? 15 : 30;
        const int a_peak = shape_required ? 1000 : 900, a_area = shape_required ? 7 : 8;
        const int b_peak = shape_required ? 500 : 600, b_area = shape_required ? 15 : 12;
        assert(observe_points(panel, scan, {point(1000 - step, 500, a_peak, a_area),
                                            point(1220 + step, 650, b_peak, b_area)}) == 0);
        assert(observe_points(panel, last, {point(1000, 500, a_peak, a_area),
                                            point(1220, 650, b_peak, b_area)}) == 2);
        const int first = panel.out[0].tracking_id, second = panel.out[1].tracking_id;
        const auto a = point(1205, 500, a_peak, a_area), b = point(1015, 650, b_peak, b_area);
        assert(observe_points(panel, last + 2 * scan, {b, a}) == 2);
        const auto *first_track = find_track(panel, first), *second_track = find_track(panel, second);
        assert(first_track && first_track->x == a.x && first_track->y == a.y);
        assert(second_track && second_track->x == b.x && second_track->y == b.y);
    }
}

static void check_pending_region() {
    for (const bool expired : {false, true}) {
        Panel panel;
        panel.finger(25.2, 500);
        assert(panel.frame(scan) == 0);
        const auto generation = panel.core->next_generation;
        assert(generation == 1);
        const uint64_t time = scan + NVT_PENDING_MAX_US - (expired ? 0 : 1);
        assert(panel.frame(time, 2) == (expired ? 0 : 1));
        assert(panel.core->pending_expired == unsigned(expired));
        assert(panel.core->next_generation == generation + unsigned(expired));
        assert(panel.frame(time + scan, 2) == 1);
        assert(panel.core->slots[panel.out[0].slot].generation == generation + unsigned(expired));
    }
}

static void check_moving_region_peaks() {
    Panel panel;
    assert(observe_points(panel, scan, {point(1300, 806), point(1400, 806, 800)}) == 0);
    assert(observe_points(panel, last, {point(1340, 806), point(1440, 806, 800)}) == 2);
    const int first = panel.out[0].tracking_id, second = panel.out[1].tracking_id;
    panel.input[15 * NVT_COLUMNS + 30] += 900;
    panel.input[14 * NVT_COLUMNS + 30] += 350;
    panel.input[15 * NVT_COLUMNS + 32] += 800;
    panel.input[14 * NVT_COLUMNS + 32] += 300;
    const uint64_t time = last + scan;
    assert(panel.frame(time, 2) == 2);
    assert(find_track(panel, first)->x == point(1549, 806).x);
    assert(find_track(panel, second)->x == point(1649, 806).x);
}

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
        assert(panel.frame(last + 2 * scan, 2) == 0);
        assert(!contact.active);
        assert(!panel.core->cancelled_ids);
        assert(contact.last_covered_negative_us == last + 2 * scan);
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
    check_frame_admission();
    check_counter_order();
    std::puts("PASS known frame formats, stale frame rejection, counter wrap, and restart");
    check_baseline();
    std::puts("PASS startup contact, slow press, and per-region baseline drift");
    check_candidate_geometry();
    std::puts("PASS isolated peaks, narrow edge touches, merged amplitude, and palm area");
    check_rejoin();
    check_assignment();
    check_pending_region();
    check_moving_region_peaks();
    std::puts("PASS elapsed-time rejoin, confirmed-slot priority, crossing IDs, and pending P expiry");
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
