#!/usr/bin/env python3
"""Run the button sampling loop and HID handoff with scripted inputs."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


DEVICE = Path(__file__).resolve().parents[1] / 'build-sources/overlay/device'
SOURCE = DEVICE / 'package/ButtonProbe.cpp'
HID_SOURCE = DEVICE / 'drivers/ShengHidInput/NativeHidInput.cpp'

MOCK = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using event_t = const volatile void *;
using wait_result_t = int;
constexpr int THREAD_UNINT = 0, THREAD_AWAKENED = 1, THREAD_CONTINUE_NULL = 0;
constexpr uint32_t NSEC_PER_MSEC = 1000000;
constexpr int gPon = 1, gGpio = 2;
struct {volatile uint32_t *va = nullptr;} gObs;
int gArb;
volatile uint32_t gEvents;
uint64_t gIrqTicks;
uint32_t gIrqStatus;
bool gTrace;
volatile int32_t native_buttons_stats[10];
void *gSinkContext;
void (*gSink)(void *, uint32_t);

enum Wake { NextIrq, Timeout, IrqDuringRead, Finish };
struct Frame {
    uint8_t pon, gpio;
    int ponError, gpioError;
    unsigned waitMs;
    Wake wake;
};
struct Finished {};
static std::vector<Frame> frames;
static std::vector<uint32_t> reports;
static unsigned pos;
static unsigned readCount;
static bool waiting;

static int br_read8(volatile uint32_t *, const int *, int apid, uint8_t, uint8_t *out) {
    const Frame &frame = frames.at(pos);
    ++readCount;
    if (apid == gPon) {
        if (frame.wake == IrqDuringRead) __atomic_add_fetch(&gEvents, 1, __ATOMIC_RELEASE);
        if (!frame.ponError) *out = frame.pon;
        return frame.ponError;
    }
    assert(apid == gGpio);
    if (!frame.gpioError) *out = frame.gpio;
    return frame.gpioError;
}

static void enterWait(event_t address, unsigned ms) {
    assert(address == &gEvents && !waiting && readCount == 2);
    const Frame &frame = frames.at(pos);
    if (frame.waitMs != ms) std::fprintf(stderr, "sample %u: expected %u ms wait, got %u\n", pos, frame.waitMs, ms);
    assert(frame.waitMs == ms);
    waiting = true;
}

static void assert_wait_timeout(event_t e, int, unsigned ms, uint32_t scale) {
    assert(scale == NSEC_PER_MSEC && ms > 0);
    enterWait(e, ms);
}
static void assert_wait(event_t e, int) { enterWait(e, 0); }

static void finishWait(bool blocked) {
    assert(waiting);
    const Frame &frame = frames.at(pos);
    assert(blocked == (frame.wake != IrqDuringRead));
    if (frame.wake == Finish) throw Finished{};
    if (frame.wake == Timeout) assert(frame.waitMs);
    if (frame.wake == NextIrq) __atomic_add_fetch(&gEvents, 1, __ATOMIC_RELEASE);
    ++pos;
    readCount = 0;
    waiting = false;
}

static void clear_wait(int, int) { finishWait(false); }
static void thread_block(int) { finishWait(true); }
static int current_thread() { return 0; }
static void thread_set_thread_name(int, const char *) {}
static bool setup() { return true; }
static uint64_t mach_absolute_time() { return 0; }
static void absolutetime_to_nanoseconds(uint64_t in, uint64_t *out) { *out = in; }
template <class... Args> static void IOLog(const char *, Args...) {}
'''

CASES = r'''
static void test(const char *name, std::vector<Frame> inputs, std::vector<uint32_t> expected) {
    frames = inputs;
    reports.clear();
    gEvents = 0;
    native_buttons_stats[2] = 0;
    pos = readCount = 0;
    waiting = false;
    gSink = [](void *, uint32_t mask) { reports.push_back(mask); };
    try {
        buttonsThread(nullptr, 0);
        assert(false && "button worker returned before finishing the scripted wait");
    } catch (const Finished &) {
        assert(pos + 1 == frames.size());
        assert(reports == expected);
        assert(native_buttons_stats[2] == static_cast<int32_t>(expected.size()));
    }
    std::printf("PASS %s\n", name);
}

int main() {
    test("release read error recovers on timeout without another IRQ", {
        {0x80, 0xff, 0, 0, 0, NextIrq},
        {0x00, 0xff, 2, 0, 2, Timeout},
        {0x00, 0xff, 0, 0, 0, Finish},
    }, {0x2, 0x0});

    test("PON and GPIO preserve confirmed bits independently", {
        {0xc0, 0xff, 0, 0, 0, NextIrq},
        {0x00, 0xfe, 2, 0, 2, Timeout},
        {0x00, 0xff, 0, 1, 4, Timeout},
        {0x00, 0xff, 0, 0, 0, Finish},
    }, {0xa, 0xe, 0x4, 0x0});

    test("unresolved release uses bounded-rate backoff until the real up", {
        {0x80, 0xff, 0, 0, 0, NextIrq},
        {0x00, 0xff, 1, 0, 2, Timeout},
        {0x00, 0xff, 1, 0, 4, Timeout},
        {0x00, 0xff, 1, 0, 8, Timeout},
        {0x00, 0xff, 1, 0, 16, Timeout},
        {0x00, 0xff, 1, 0, 100, Timeout},
        {0x00, 0xff, 1, 0, 100, Timeout},
        {0x00, 0xff, 0, 0, 0, Finish},
    }, {0x2, 0x0});

    test("persistent read failure while idle returns to IRQ-only waiting", {
        {0x00, 0xff, 1, 0, 2, Timeout},
        {0x00, 0xff, 1, 0, 4, Timeout},
        {0x00, 0xff, 1, 0, 8, Timeout},
        {0x00, 0xff, 1, 0, 16, Timeout},
        {0x00, 0xff, 1, 0, 0, Finish},
    }, {});

    test("IRQ during sampling skips sleep and resets the retry burst", {
        {0x00, 0xff, 1, 0, 2, Timeout},
        {0x00, 0xff, 1, 0, 4, IrqDuringRead},
        {0x00, 0xff, 1, 0, 2, Timeout},
        {0x80, 0xff, 0, 0, 0, Finish},
    }, {0x2});
}
'''


def extract_source():
    source = SOURCE.read_text(encoding='utf-8')
    constants = '\n'.join(re.search(r'^constexpr \w+ ' + name + r'\b.*;$', source, re.M)[0]
                          for name in ('kPowerBit', 'kReportPower'))
    constants += '\n' + re.search(r'^constexpr uint8_t kRtSts = [^,;]+', source, re.M)[0] + ';'
    sample = source[source.index('struct ButtonSample {'):source.index('\nbool setup()')]
    start = source.index('void buttonsThread(')
    worker = source[start:source.index('\n}  // namespace', start)]
    return MOCK + constants + '\n' + sample + worker + '\n' + CASES


HID_MOCK = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -1, kIOReturnNoMemory = -2;
constexpr int kIODirectionNone = 0, gIOServicePlane = 0;
struct OSObject { virtual ~OSObject() = default; };
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSSafeReleaseNULL(object) do { if (object) (object)->release(); (object) = nullptr; } while (false)
struct OSIterator {
    const std::vector<OSObject *> &children;
    unsigned index = 0;
    OSObject *getNextObject() { return index < children.size() ? children[index++] : nullptr; }
    void release() { delete this; }
};
struct IORegistryEntry : OSObject {
    std::vector<OSObject *> children;
    OSIterator *getChildIterator(int) { return new OSIterator{children}; }
};
struct IOService : IORegistryEntry {
    bool eventDriver = false, inactive = false;
    const void *metaCast(const char *) const { return eventDriver ? this : nullptr; }
    bool isInactive() const { return inactive; }
    void retain() {}
    void release() {}
};
struct IOTimerEventSource {
    unsigned delay = 0;
    void setTimeoutMS(unsigned ms) { delay = ms; }
};
struct IOBufferMemoryDescriptor {
    unsigned mask;
    static IOBufferMemoryDescriptor *withBytes(const unsigned char *bytes, unsigned size, int) {
        assert(size == 2 && bytes[0] == 2);
        return new IOBufferMemoryDescriptor{bytes[1]};
    }
    void release() { delete this; }
};
static void native_hid_input_observed(unsigned, int) {}
static void IOLog(const char *, ...) {}
static uint64_t nowUs;
static uint64_t mach_absolute_time() { return nowUs * 1000; }
static void absolutetime_to_nanoseconds(uint64_t ticks, uint64_t *ns) { *ns = ticks; }

struct NativeHidInput : IOService {
    IOTimerEventSource retryTimer;
    IOService *openClient = nullptr;
    bool buttonsTrace = false;
    uint32_t buttonSequence = 0, buttonSnapshot = UINT32_MAX;
    int rejectedMask = -1;
    std::vector<unsigned> delivered;
    NativeHidInput() { buttonRetryTimer = &retryTimer; }
    bool isOpen(const IOService *client) const { return openClient == client; }
    IOReturn submitReport(IOBufferMemoryDescriptor *report) {
        assert(openClient && !openClient->inactive);
        if (int(report->mask) == rejectedMask) return kIOReturnNoMemory;
        delivered.push_back(report->mask);
        return kIOReturnSuccess;
    }
    IOReturn press(unsigned mask) {
        return buttonsAction(this, reinterpret_cast<void *>(uintptr_t(mask)), nullptr, nullptr, nullptr);
    }
    void retry() { retryButtons(this, buttonRetryTimer); }
'''

HID_CASES = r'''
};

int main() {
    IOService original, replacement;
    original.eventDriver = replacement.eventDriver = true;

    NativeHidInput late;
    assert(late.press(2) == kIOReturnNotReady && late.retryTimer.delay == 20);
    late.children = {&original};
    nowUs = 120000;
    assert(late.press(0) == kIOReturnNotReady && late.delivered.empty());
    late.openClient = &original;
    late.retry();
    assert((late.delivered == std::vector<unsigned>{2, 0}));
    std::puts("PASS HID late open preserves down and up in order");

    late.openClient = nullptr;
    assert(late.press(4) == kIOReturnNotReady);
    late.children.push_back(&replacement);
    late.openClient = &replacement;
    late.retry();
    assert(late.press(0) == kIOReturnSuccess);
    assert((late.delivered == std::vector<unsigned>{2, 0, 4, 0}));
    std::puts("PASS HID reconnect selects the currently open driver");

    NativeHidInput failed;
    failed.children = {&original};
    failed.openClient = &original;
    failed.rejectedMask = 2;
    assert(failed.press(2) == kIOReturnNoMemory);
    failed.rejectedMask = 0;
    assert(failed.press(0) == kIOReturnNoMemory && failed.buttons == 2);
    failed.rejectedMask = -1;
    failed.retry();
    assert((failed.delivered == std::vector<unsigned>{2, 0} && failed.buttons == 0));
    std::puts("PASS HID failed submission retries only the undelivered report");

    NativeHidInput expired;
    assert(expired.press(2) == kIOReturnNotReady);
    assert(expired.press(0) == kIOReturnNotReady);
    nowUs += 300000;
    expired.children = {&original};
    expired.openClient = &original;
    expired.retry();
    assert(expired.delivered == std::vector<unsigned>{0});
    std::puts("PASS HID stale tap reconciles to the current released state");

    NativeHidInput overflow;
    for (unsigned i = 0; i <= NativeHidInput::kPendingButtons; ++i)
        assert(overflow.press(i % 2 ? 0 : 2) == kIOReturnNotReady);
    overflow.children = {&original};
    overflow.openClient = &original;
    overflow.retry();
    assert(overflow.press(0) == kIOReturnSuccess);
    assert((overflow.delivered == std::vector<unsigned>{2, 0}));
    std::puts("PASS HID full queue reconciles to the current held state");
}
'''


def extract_hid_source():
    source = HID_SOURCE.read_text(encoding='utf-8')
    fields = source[source.index('    IOTimerEventSource *buttonRetryTimer'):source.index('    bool touchEnabled')]
    handoff = source[source.index('    static uint64_t buttonUs()'):source.index('    bool hasEventDriverCached()')]
    discovery = source[source.index('    static IOService *copyEventDriver('):source.index('    static IOReturn stopButtonRetry(')]
    return HID_MOCK + fields + handoff + discovery + HID_CASES


def compiler_and_environment():
    env = os.environ.copy()
    vs = Path(env.get('ProgramFiles', 'C:/Program Files')) / 'Microsoft Visual Studio/2022'
    compiler = env.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if os.name == 'nt' and not compiler:
        compiler = next(vs.glob('*/VC/Tools/Llvm/x64/bin/clang++.exe'), None)
    if not compiler:
        raise SystemExit('Set CXX to a C++17 clang++ or g++ executable.')
    compiler = Path(compiler)
    if os.name == 'nt' and 'Microsoft Visual Studio' in str(compiler):
        toolset = sorted((compiler.parents[3] / 'MSVC').iterdir())[-1]
        kits = Path(env.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Windows Kits/10'
        sdk = sorted(kits.glob('Include/*/ucrt'))[-1].parent
        lib = kits / 'Lib' / sdk.name
        for key, paths in (
            ('INCLUDE', (toolset / 'include', sdk / 'ucrt', sdk / 'shared', sdk / 'um')),
            ('LIB', (toolset / 'lib/x64', lib / 'ucrt/x64', lib / 'um/x64')),
        ):
            env[key] = os.pathsep.join(dict.fromkeys(filter(None, [env.get(key), *map(str, paths)])))
    return str(compiler), env


def main():
    compiler, env = compiler_and_environment()
    with tempfile.TemporaryDirectory(prefix='sheng-buttons-') as temp:
        for name, contents in [('button-probe', extract_source()), ('button-handoff', extract_hid_source())]:
            source = Path(temp) / f'{name}.cpp'
            binary = source.with_suffix('.exe' if os.name == 'nt' else '')
            source.write_text(contents, encoding='utf-8')
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(binary)],
                           check=True, env=env)
            subprocess.run([str(binary)], check=True, env=env)


if __name__ == '__main__':
    main()
