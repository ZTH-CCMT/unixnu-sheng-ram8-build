#!/usr/bin/env python3
"""Exercise the QuartzCore BGRA text patch with deterministic Mach VM doubles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "build-sources/overlay/UniXNU/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm"

PRELUDE = r"""
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(e) do { if (!(e)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #e); std::exit(1); } } while (false)

using vm_address_t = uintptr_t;
using vm_offset_t = uintptr_t;
using vm_size_t = size_t;
using vm_prot_t = int;
using kern_return_t = int;
using natural_t = unsigned;
using mach_msg_type_number_t = unsigned;
using vm_region_recurse_info_t = int *;

struct vm_region_submap_short_info_data_64_t { vm_prot_t protection; unsigned is_submap; };
constexpr unsigned VM_REGION_SUBMAP_SHORT_INFO_COUNT_64 = 8;

constexpr vm_prot_t VM_PROT_READ = 1;
constexpr vm_prot_t VM_PROT_WRITE = 2;
constexpr vm_prot_t VM_PROT_EXECUTE = 4;
constexpr vm_prot_t VM_PROT_COPY = 16;
constexpr vm_prot_t Temporary = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY;
constexpr kern_return_t KERN_SUCCESS = 0;
constexpr unsigned FALSE = 0;
constexpr uintptr_t Page = 0x10000;
constexpr uintptr_t Site = Page + 0x928;
constexpr size_t vm_page_size = 16384;
constexpr uint32_t Branch = 0x37000608;
constexpr uint32_t Nop = 0xd503201f;

enum class Region { Full, Error, NextPage, Partial, Submap, ShortInfo };

struct Fixture {
    explicit Fixture(vm_prot_t original_flags = VM_PROT_READ | VM_PROT_EXECUTE)
        : original(original_flags), protection(original_flags) {}

    vm_prot_t original, protection;
    uint32_t instruction = Branch;
    Region region = Region::Full;
    bool mapped = true, readable = true, fail_protect = false, fail_write = false;
    bool change_before_protect_failure = false;
    unsigned failed_restores = 0, stripped_executables = 0, writes = 0, invalidations = 0;
    unsigned successful_restores = 0, unavailable_on_restore = 0;
    unsigned unavailable_queries = 0, observed_unavailable_queries = 0;
    std::vector<vm_prot_t> requests;
};

static Fixture memory;
struct Gpu {
    unsigned last_mark = 0;
    void mark(unsigned marker) { last_mark = marker; }
};

static bool readTask(uintptr_t address, void *destination, size_t size) {
    CHECK(address == Site && size == sizeof(uint32_t));
    if (!memory.mapped || !memory.readable) return false;
    std::memcpy(destination, &memory.instruction, size);
    return true;
}

static unsigned mach_task_self() { return 17; }

static kern_return_t vm_region_recurse_64(unsigned task, vm_address_t *address,
    vm_size_t *size, natural_t *depth, vm_region_recurse_info_t info,
    mach_msg_type_number_t *count) {
    CHECK(task == mach_task_self() && *address == Page && *depth == UINT32_MAX);
    CHECK(*count == VM_REGION_SUBMAP_SHORT_INFO_COUNT_64);
    if (memory.region == Region::Error) return 1;
    if (memory.unavailable_on_restore && memory.successful_restores >= memory.unavailable_on_restore &&
        memory.observed_unavailable_queries < memory.unavailable_queries) {
        ++memory.observed_unavailable_queries;
        return 1;
    }
    auto *mapping = reinterpret_cast<vm_region_submap_short_info_data_64_t *>(info);
    mapping->protection = memory.protection;
    mapping->is_submap = memory.region == Region::Submap;
    *size = memory.region == Region::Partial ? vm_page_size / 2 : vm_page_size;
    if (memory.region == Region::NextPage) *address = Page + vm_page_size;
    if (memory.region == Region::ShortInfo) --*count;
    return KERN_SUCCESS;
}

static kern_return_t vm_protect(unsigned task, vm_address_t page, size_t size, unsigned set_max, vm_prot_t flags) {
    CHECK(task == mach_task_self() && page == Page && size == vm_page_size && set_max == FALSE);
    memory.requests.push_back(flags);
    if (flags == Temporary) {
        CHECK(memory.protection == memory.original && !(flags & VM_PROT_EXECUTE));
        if (!memory.fail_protect || memory.change_before_protect_failure)
            memory.protection = VM_PROT_READ | VM_PROT_WRITE;
        if (memory.fail_protect) return 1;
    } else {
        CHECK(flags == memory.original && memory.requests.front() == Temporary);
        if (memory.failed_restores) {
            --memory.failed_restores;
            return 1;
        }
        ++memory.successful_restores;
        memory.protection = flags;
        if (memory.stripped_executables) {
            --memory.stripped_executables;
            memory.protection &= ~VM_PROT_EXECUTE;
        }
    }
    return KERN_SUCCESS;
}

static kern_return_t vm_write(unsigned task, vm_address_t address, vm_offset_t word, size_t size) {
    CHECK(task == mach_task_self() && address == Site && size == sizeof(uint32_t));
    CHECK(memory.protection == (VM_PROT_READ | VM_PROT_WRITE));
    ++memory.writes;
    if (memory.fail_write) return 1;
    std::memcpy(&memory.instruction, reinterpret_cast<const void *>(word), size);
    return KERN_SUCCESS;
}

static void sys_icache_invalidate(void *address, size_t size) {
    CHECK(reinterpret_cast<uintptr_t>(address) == Site && size == sizeof(uint32_t));
    ++memory.invalidations;
}

struct FixtureExit { int status; };
[[maybe_unused, noreturn]] static void fixtureExit(int status) { throw FixtureExit{status}; }
#define _exit fixtureExit
"""

TEST = r"""
#undef _exit

static unsigned patch(Gpu &gpu) {
    return patchFramebufferBranch(&gpu, Site);
}

static void checkRestored(vm_prot_t original) {
    CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original}));
    CHECK(memory.protection == original && memory.writes == 1);
}

static void testPatchWithProtection(vm_prot_t original) {
    Gpu gpu;
    memory = Fixture{original};
    CHECK(patch(gpu) == 0x6E42);
    checkRestored(original);
    CHECK(memory.instruction == Nop && memory.invalidations == 1);

    memory = Fixture{original};
    memory.instruction = Nop;
    memory.region = Region::Error;
    CHECK(patch(gpu) == 0x6E42);
    CHECK(memory.requests.empty() && memory.writes == 0);
}

static void testRefusals() {
    Gpu gpu;

    memory = Fixture{};
    CHECK(patchFramebufferBranch(&gpu, Site + 1) == 0x6E40);
    CHECK(memory.requests.empty() && memory.writes == 0);

    memory = Fixture{};
    memory.mapped = false;
    CHECK(patch(gpu) == 0x6E40);
    CHECK(memory.requests.empty() && memory.writes == 0 && memory.instruction == Branch);

    memory = Fixture{};
    memory.readable = false;
    CHECK(patch(gpu) == 0x6E40);
    CHECK(memory.requests.empty() && memory.writes == 0 && memory.instruction == Branch);

    memory = Fixture{};
    memory.instruction = 0xd65f03c0;
    CHECK(patch(gpu) == 0x6E41);
    CHECK(memory.requests.empty() && memory.writes == 0 && memory.instruction == 0xd65f03c0);

    for (Region region : {Region::Error, Region::NextPage, Region::Partial,
                          Region::Submap, Region::ShortInfo}) {
        memory = Fixture{};
        memory.region = region;
        CHECK(patch(gpu) == 0x6E43);
        CHECK(memory.requests.empty() && memory.writes == 0 && memory.instruction == Branch);
    }
}

static void testPostRestoreQueryUnavailable() {
    const auto original = VM_PROT_READ | VM_PROT_EXECUTE;
    for (unsigned unavailable : {2u, 1u}) {
        Gpu gpu;
        memory = Fixture{original};
        memory.unavailable_on_restore = 1;
        memory.unavailable_queries = unavailable;
        unsigned result = 0;
        try {
            result = patch(gpu);
        } catch (const FixtureExit &exit) {
            std::fprintf(stderr, "restore succeeded but query was unavailable; exited with %d, marker %04x\n",
                         exit.status, gpu.last_mark);
            std::exit(1);
        }
        CHECK(result == (unavailable == 2 ? 0x6E54u : 0x6E42u));
        CHECK(memory.observed_unavailable_queries == unavailable);
        checkRestored(original);
        CHECK(memory.instruction == Nop && memory.invalidations == 1);
    }
}

static void testMutationFailures() {
    const auto original = VM_PROT_READ | VM_PROT_EXECUTE;
    Gpu gpu;

    memory = Fixture{original};
    memory.fail_protect = true;
    CHECK(patch(gpu) == 0x6E51);
    CHECK(memory.requests == std::vector<vm_prot_t>{Temporary});
    CHECK(memory.protection == original && memory.writes == 0 && memory.instruction == Branch);

    memory = Fixture{original};
    memory.fail_protect = true;
    memory.change_before_protect_failure = true;
    CHECK(patch(gpu) == 0x6E51);
    CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original}));
    CHECK(memory.protection == original && memory.writes == 0 && memory.instruction == Branch);

    memory = Fixture{original};
    memory.fail_write = true;
    CHECK(patch(gpu) == 0x6E52);
    checkRestored(original);
    CHECK(memory.instruction == Branch);

    memory = Fixture{original};
    memory.failed_restores = 1;
    CHECK(patch(gpu) == 0x6E42);
    CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original, original}));
    CHECK(memory.protection == original && memory.writes == 1 && memory.instruction == Nop);

    memory = Fixture{original};
    memory.stripped_executables = 1;
    CHECK(patch(gpu) == 0x6E42);
    CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original, original}));
    CHECK(memory.protection == original && memory.writes == 1 && memory.instruction == Nop);

    for (unsigned marker : {0x6E53u, 0x6E55u}) {
        memory = Fixture{original};
        memory.failed_restores = marker == 0x6E53 ? 2 : 0;
        memory.stripped_executables = marker == 0x6E55 ? 2 : 0;
        gpu.last_mark = 0;
        try {
            (void)patch(gpu);
            CHECK(false);
        } catch (const FixtureExit &exit) {
            CHECK(exit.status == 1 && gpu.last_mark == marker);
        }
        CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original, original}));
        CHECK(memory.invalidations == 0);
    }

    // A later unreadable result cannot clear an observed mismatch.
    memory = Fixture{original};
    memory.stripped_executables = 1;
    memory.unavailable_on_restore = 2;
    memory.unavailable_queries = 2;
    gpu.last_mark = 0;
    try {
        (void)patch(gpu);
        CHECK(false);
    } catch (const FixtureExit &exit) {
        CHECK(exit.status == 1 && gpu.last_mark == 0x6E55);
    }
    CHECK((memory.requests == std::vector<vm_prot_t>{Temporary, original, original}));
    CHECK(memory.observed_unavailable_queries == 2 && memory.invalidations == 0);
}

int main() {
    testPatchWithProtection(VM_PROT_READ | VM_PROT_EXECUTE);
    testPatchWithProtection(VM_PROT_WRITE | VM_PROT_EXECUTE);
    testPatchWithProtection(VM_PROT_READ | VM_PROT_WRITE);
    testRefusals();
    testPostRestoreQueryUnavailable();
    testMutationFailures();
    std::puts("PASS BGRA patch distinguishes failed restoration, unavailable verification, and a verified mismatch");
}
"""


def function(text, signature):
    start = text.index(signature)
    end = text.index("\n}", start) + len("\n}")
    return text[start:end]


def read_implementation(source):
    text = source.read_text(encoding="utf-8")
    protection = function(text, "static bool taskProtection(")
    patch = function(text, "static unsigned patchFramebufferBranch(")
    return protection + "\n" + patch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--source", type=Path, default=SOURCE)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="quartzcore-framebuffer-") as tmp:
        src = Path(tmp) / "framebuffer_patch.cpp"
        exe = src.with_suffix(".exe")
        src.write_text(PRELUDE + "\n" + read_implementation(args.source) + "\n" + TEST, encoding="utf-8")
        subprocess.run(
            [args.cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", src, "-o", exe],
            check=True,
        )
        subprocess.run([exe], check=True, timeout=10)


if __name__ == "__main__":
    main()
