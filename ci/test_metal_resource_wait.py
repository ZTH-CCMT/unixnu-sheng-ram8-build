#!/usr/bin/env python3
"""Run the frontend resource-wait control flow with host encoding/GPU gates."""
import argparse
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build-sources/overlay/UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm"
PRELUDE = r"""
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)
using namespace std::chrono_literals;
using ull=unsigned long long;

struct Group {
    std::condition_variable changed;
    std::mutex mutex;
    bool ready=false;
    unsigned waits=0;

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        ++waits;
        changed.notify_all();
        CHECK(changed.wait_for(lock, 2s, [&]{ return ready; }));
    }
    void awaitWaiter() {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, 2s, [&]{ return waits!=0; }));
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        ready=true;
        changed.notify_all();
    }
};
using dispatch_group_t=Group *;
static constexpr auto DISPATCH_TIME_FOREVER=0;
static void dispatch_group_wait(Group *group, int timeout) {
    CHECK(timeout==DISPATCH_TIME_FOREVER);
    group->wait();
}

struct Resource { enum class Kind {Buffer,Texture,Unknown}; Kind kind; int handle=7; };
using id=Resource *;
using NBBuffer=Resource;
using NBTexture=Resource;

struct NBCommandBuffer {
    std::atomic<bool> submitted{false},failedLocally{false};
    Group runtime;
    Group *runtimeCommit=&runtime,*resourceEncoding=nullptr;
    NBCommandBuffer *handle=this;
    int dependency=0;
    unsigned queries=0,completions=0;
    void waitUntilCompleted() { ++completions; }
};
using NSArray=std::vector<NBCommandBuffer *>;
static std::mutex snapshotLock;
static NSArray registered;
static NSArray inFlightSnapshot() {
    std::lock_guard<std::mutex> lock(snapshotLock);
    return registered;
}
static void setRegistered(NSArray buffers) {
    std::lock_guard<std::mutex> lock(snapshotLock);
    registered=std::move(buffers);
}
static bool worker=false;
static bool onEncodeQueue() { return worker; }
static std::atomic<uint64_t> timeline{100},gResourceChecks{0},gResourceSkipped{0};
static uint64_t nowUs() { return timeline.load(); }
struct Timed {
    uint64_t elapsed=0;
    void add(uint64_t since) { elapsed=nowUs()-since; }
};
static Timed gTimeResourceWait;

static int uses(NBCommandBuffer *command, int resource) {
    CHECK(resource==7);
    CHECK(command->submitted || command->resourceEncoding);
    ++command->queries;
    return command->dependency;
}
static int nmb_command_buffer_uses_buffer(NBCommandBuffer *command, int resource) { return uses(command,resource); }
static int nmb_command_buffer_uses_texture(NBCommandBuffer *command, int resource) { return uses(command,resource); }
"""

TEST = r"""
int main() {
    Resource texture{Resource::Kind::Texture}, buffer{Resource::Kind::Buffer}, unknown{Resource::Kind::Unknown};
    NBCommandBuffer prior, later;
    setRegistered({&prior});
    std::thread waiter([&]{ waitForResource(&texture); });
    prior.runtime.awaitWaiter();
    setRegistered({&prior,&later});
    timeline.fetch_add(50);
    prior.submitted=true;
    prior.runtime.release();
    waiter.join();
    CHECK(prior.queries==1 && prior.completions==0);
    CHECK(later.runtime.waits==0 && later.queries==0);
    CHECK(gTimeResourceWait.elapsed==50);

    NBCommandBuffer parked;
    Group encoding;
    parked.resourceEncoding=&encoding;
    setRegistered({&parked});
    std::thread parkedWaiter([&]{ waitForResource(&buffer); });
    encoding.awaitWaiter();
    encoding.release();
    parkedWaiter.join();
    CHECK(parked.queries==1 && parked.completions==0 && parked.runtime.waits==0);

    NBCommandBuffer refusal;
    setRegistered({&refusal});
    std::thread rejectedWaiter([&]{ waitForResource(&buffer); });
    refusal.runtime.awaitWaiter();
    refusal.failedLocally=true;
    refusal.submitted=true;
    refusal.runtime.release();
    rejectedWaiter.join();
    CHECK(refusal.queries==0 && refusal.completions==0);

    NBCommandBuffer matching;
    matching.dependency=1;
    matching.submitted=true;
    setRegistered({&later,&matching});
    worker=true;
    waitForResource(&texture);
    CHECK(later.runtime.waits==0 && later.queries==0);
    CHECK(matching.queries==1 && matching.completions==1);
    worker=false;
    setRegistered({&matching});
    waitForResource(&unknown);
    CHECK(matching.queries==1 && matching.completions==2);
    std::puts("PASS resource waits keep published commits and actual dependencies, skip unrelated future work and refusals, and handle parked/worker paths");
}
"""


def read_implementation():
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("static std::mutex gRuntimeHandleLock;")
    end = source.index("\n// /private/var/ngpu/debug/draw-stats", start)
    body = source[start:end]
    # Replace Objective-C messaging only. The wait/snapshot ordering remains
    # the production implementation compiled against synchronous host gates.
    replacements = {
        "NSArray *pending": "const NSArray pending",
        "for(NBCommandBuffer *commandBuffer in pending)": "for(NBCommandBuffer *commandBuffer : pending)",
        "commandBuffer.resourceEncoding?:commandBuffer.runtimeCommit": "commandBuffer->resourceEncoding ? commandBuffer->resourceEncoding : commandBuffer->runtimeCommit",
        "commandBuffer.submitted": "commandBuffer->submitted",
        "commandBuffer.failedLocally": "commandBuffer->failedLocally",
        "commandBuffer.handle": "commandBuffer->handle",
        "[resource isKindOfClass:[NBBuffer class]]": "resource->kind==Resource::Kind::Buffer",
        "[resource isKindOfClass:[NBTexture class]]": "resource->kind==Resource::Kind::Texture",
        "[(NBBuffer *)resource handle]": "resource->handle",
        "[(NBTexture *)resource handle]": "resource->handle",
        "[(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted]": "commandBuffer->waitUntilCompleted()",
    }
    for before, after in replacements.items():
        if before not in body:
            raise ValueError(f"Update the host messaging stub for {before!r}")
        body = body.replace(before, after)
    return body


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--work-dir", type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="metal-resource-wait-", dir=args.work_dir) as tmp:
        test = pathlib.Path(tmp) / "resource_wait.cpp"
        exe = test.with_suffix(".exe")
        test.write_text(PRELUDE + read_implementation() + TEST, encoding="utf-8")
        subprocess.run([args.cxx,"-std=c++17","-O2","-Wall","-Wextra","-Werror","-pthread",test,"-o",exe],check=True)
        subprocess.run([exe],check=True,timeout=12)


if __name__ == "__main__":
    main()
