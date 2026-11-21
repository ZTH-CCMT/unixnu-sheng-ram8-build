#!/usr/bin/env python3
"""Exercise the production AGX close state machine with deterministic host doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build-sources/overlay/UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp"

PRELUDE = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#define CHECK(e) do { if (!(e)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #e); std::exit(1); } } while (false)

using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnTimeout, kIOReturnBusy, kIOReturnIOError, THREAD_UNINT };

struct IOLock { bool held = false; };
void IOLockLock(IOLock *lock) { CHECK(!lock->held); lock->held = true; }
void IOLockUnlock(IOLock *lock) { CHECK(lock->held); lock->held = false; }
void IOLockSleep(IOLock *, uint32_t *, int) { CHECK(false); }

struct Clock {
    void *cookie = this;
    uint64_t now = 1;
    uint64_t (*now_us)(void *) = [](void *cookie) { return static_cast<Clock *>(cookie)->now; };
    std::vector<std::function<bool()>> pauses;
    size_t next = 0;
    bool waitForPoll(uint64_t began, uint32_t timeout);
};
struct Space {
    void *vm() { return this; }
};
struct Core {
    uint64_t peak = 17ull << 20;
    unsigned peak_reads = 0, releases = 0;
    IOReturn release_result = kIOReturnSuccess;
    std::vector<uint32_t> fences;
    uint32_t oldestOwnFence() const;
    uint64_t windowPeak() { ++peak_reads; return peak; }
    IOReturn releaseAll(Clock *, uint32_t);
};
struct Submit {
    Core *core = nullptr;
    Space *space = nullptr;
    IOReturn observe_error = kIOReturnSuccess, forget_result = kIOReturnSuccess;
    unsigned forgets = 0;
    uint32_t signaled = 0;
    std::vector<uint32_t> observations, waits;
    IOReturn observeFence(uint32_t fence, Clock *, uint32_t);
    IOReturn wait(uint32_t fence, Clock *, uint32_t);
    IOReturn forgetVm(void *vm, Clock *, uint32_t);
};
struct Context {
    unsigned destroys = 0;
    Space *space = nullptr;
    IOReturn result = kIOReturnSuccess;
    IOReturn destroyVm(Space *);
};
struct State {
    IOLock *lock;
    Submit *submit;
    Context *context;
    bool kernel_client = true;
    bool rows[4] = {false, false, true, false};
    unsigned closed = 0;
    Clock clock{};
};
static State g{};
static struct { unsigned closes; } native_adreno_metal_diag{};
struct NativeAdrenoClientRecord { unsigned closes = 0, slot = 2; uint32_t peak_mib = 3; };
static NativeAdrenoClientRecord record;
NativeAdrenoClientRecord *clientRecord(const char *) { return &record; }
struct IOBufferMemoryDescriptor {
    unsigned releases = 0;
    void release() { CHECK(!g.lock->held); ++releases; }
};
static struct AGXDeviceUserClient *gMailboxOwner;
static IOBufferMemoryDescriptor *gMailbox;
static uint32_t *gMailboxWords;
static bool gMailboxMapped;

bool Clock::waitForPoll(uint64_t began, uint32_t timeout)
{
    CHECK(!g.lock->held && began <= now && timeout == 2000000);
    CHECK(next < pauses.size());
    ++now;
    return pauses[next++]();
}
uint32_t Core::oldestOwnFence() const
{
    CHECK(g.lock->held);
    return fences.empty() ? 0 : fences.front();
}
IOReturn Core::releaseAll(Clock *clock, uint32_t timeout)
{
    CHECK(g.lock->held && clock == &g.clock && !timeout && fences.empty());
    ++releases;
    if (!release_result) peak = 0;
    return release_result;
}
IOReturn Submit::observeFence(uint32_t fence, Clock *clock, uint32_t timeout)
{
    CHECK(g.lock->held && clock == &g.clock && !timeout && fence);
    observations.push_back(fence);
    if (observe_error) return observe_error;
    return fence == signaled ? kIOReturnSuccess : kIOReturnTimeout;
}
IOReturn Submit::wait(uint32_t fence, Clock *clock, uint32_t timeout)
{
    CHECK(g.lock->held && clock == &g.clock && !timeout && fence);
    waits.push_back(fence);
    if (fence != signaled) return kIOReturnTimeout;
    if (core->oldestOwnFence() == fence) core->fences.erase(core->fences.begin());
    return kIOReturnSuccess;
}
IOReturn Submit::forgetVm(void *vm, Clock *clock, uint32_t timeout)
{
    CHECK(g.lock->held && vm == space && clock == &g.clock && timeout == 2000000);
    ++forgets;
    return forget_result;
}
IOReturn Context::destroyVm(Space *requested)
{
    CHECK(g.lock->held && requested == space);
    ++destroys;
    return result;
}
"""

CLASS_FIELDS = r"""
    bool live_ = true, closing_ = false, kernel_ = true;
    Core *core_;
    Space *space_;
    unsigned row_ = 2, unwiring_bos_ = 0, retiring_view_token_ = 0;
    char name_[20] = "test";
    unsigned terminates = 0, releases = 0;
    IOReturn clientClose();
    IOReturn waitForCloseWork();
    void release() { CHECK(!g.lock->held); ++releases; }
    void dropViews() { CHECK(!g.lock->held && closing()); }
    void terminate() { CHECK(!g.lock->held && !live_ && !space_); ++terminates; }
};
"""

TEST = r"""
struct Fixture {
    IOLock lock;
    Space space;
    Core core;
    Submit submit;
    Context context;
    AGXDeviceUserClient client;
    IOBufferMemoryDescriptor mailbox;
    uint32_t words[4]{};

    Fixture() {
        submit.core = &core;
        submit.space = &space;
        context.space = &space;
        client.core_ = &core;
        client.space_ = &space;
        g = State{&lock, &submit, &context};
        g.clock.cookie = &g.clock;
        record = {};
        native_adreno_metal_diag = {};
        gMailboxOwner = &client;
        gMailbox = &mailbox;
        gMailboxWords = words;
        gMailboxMapped = true;
    }

    void stillOwned(bool active) {
        CHECK(client.live_ && client.space_ == &space && g.kernel_client && g.rows[2]);
        CHECK(client.active() == active && client.closing() != active);
        CHECK(!g.closed && !record.closes && record.slot == 2 && record.peak_mib == 3);
        CHECK(!native_adreno_metal_diag.closes && !client.terminates);
    }
    void closed(unsigned peak) {
        CHECK(!client.active() && client.closing() && !client.live_ && !client.space_);
        CHECK(!g.kernel_client && !g.rows[2] && client.row_ == 4);
        CHECK(g.closed == 1 && record.closes == 1 && record.slot == 0xff);
        CHECK(record.peak_mib == peak && native_adreno_metal_diag.closes == 1 && client.terminates == 1);
        CHECK(!gMailboxOwner && !gMailbox && !gMailboxWords && !gMailboxMapped);
        CHECK(mailbox.releases == 1 && client.releases == 1);
        CHECK(client.clientClose() == kIOReturnBusy && g.closed == 1 && client.terminates == 1);
    }
};

int main() {
    {
        Fixture f;
        f.core.release_result = kIOReturnIOError;
        CHECK(f.client.clientClose() == kIOReturnIOError);
        f.stillOwned(true);
        CHECK(f.core.releases == 1 && !f.submit.forgets && !f.context.destroys);
        f.core.peak = 23ull << 20;
        f.core.release_result = kIOReturnSuccess;
        CHECK(f.client.clientClose() == kIOReturnSuccess);
        f.closed(23);
        CHECK(f.core.releases == 2 && f.core.peak_reads == 2);
    }
    {
        Fixture f;
        f.submit.forget_result = kIOReturnIOError;
        CHECK(f.client.clientClose() == kIOReturnIOError);
        f.stillOwned(false);
        CHECK(f.core.releases == 1 && f.submit.forgets == 1 && !f.context.destroys);
        f.submit.forget_result = kIOReturnSuccess;
        f.context.result = kIOReturnIOError;
        CHECK(f.client.clientClose() == kIOReturnIOError);
        f.stillOwned(false);
        CHECK(f.core.releases == 1 && f.submit.forgets == 2 && f.context.destroys == 1);
        f.context.result = kIOReturnSuccess;
        CHECK(f.client.clientClose() == kIOReturnSuccess);
        f.closed(17);
        CHECK(f.core.releases == 1 && f.core.peak_reads == 1);
        CHECK(f.submit.forgets == 2 && f.context.destroys == 2);
    }
    {
        Fixture f;
        f.core.fences = {12, 19};
        g.clock.pauses = {
            [&] {
                CHECK(!f.client.active() && f.client.closing());
                CHECK(f.client.clientClose() == kIOReturnBusy);
                f.submit.signaled = 40; // Another connection's fence.
                return true;
            },
            [&] {
                f.submit.signaled = 12;
                IOLockLock(g.lock);
                f.core.fences.erase(f.core.fences.begin()); // Simulate completion on another caller.
                IOLockUnlock(g.lock);
                return true;
            },
            [&] { f.submit.signaled = 19; return true; }
        };
        CHECK(f.client.clientClose() == kIOReturnSuccess);
        CHECK((f.submit.observations == std::vector<uint32_t>{12, 12, 12, 19, 19}));
        CHECK((f.submit.waits == std::vector<uint32_t>{12, 19}));
        CHECK(g.clock.next == 3 && f.core.releases == 1);
        f.closed(17);
    }
    {
        Fixture f;
        f.core.fences = {12};
        g.clock.pauses = {[] { return false; }}; // Poll deadline elapsed.
        CHECK(f.client.clientClose() == kIOReturnTimeout);
        f.stillOwned(true);
        CHECK((f.submit.waits == std::vector<uint32_t>{12}));
        f.submit.observe_error = kIOReturnIOError;
        CHECK(f.client.clientClose() == kIOReturnIOError);
        f.stillOwned(true);
        CHECK((f.submit.waits == std::vector<uint32_t>{12}));
        CHECK(!f.core.releases && !f.submit.forgets && !f.context.destroys);
        f.submit.observe_error = kIOReturnSuccess;
        f.submit.signaled = 12;
        CHECK(f.client.clientClose() == kIOReturnSuccess);
        f.closed(17);
    }
    std::puts("PASS close waits outside the global lock and retries each teardown stage exactly once");
}
"""


def read_implementation(source):
    text = source.read_text(encoding="utf-8")
    fields = text[text.index("    enum : uint8_t {CloseCore"):text.index("    bool handoffOnly_")]
    start = text.index("static constexpr uint32_t CloseFenceTimeoutUs")
    end = text.index("void AGXDeviceUserClient::free()", start)
    return "struct AGXDeviceUserClient {\n" + fields + CLASS_FIELDS + text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=SOURCE)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="gpu-client-close-") as tmp:
        src = Path(tmp) / "client_close.cpp"
        exe = src.with_suffix(".exe")
        src.write_text(PRELUDE + read_implementation(args.source) + TEST, encoding="utf-8")
        subprocess.run(
            [args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", src, "-o", exe],
            check=True,
        )
        subprocess.run([exe], check=True, timeout=10)


if __name__ == "__main__":
    main()
