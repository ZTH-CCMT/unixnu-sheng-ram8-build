#!/usr/bin/env python3
"""Build and run the driver preemption tests with assertions disabled."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "clang++"))
    args = parser.parse_args()
    driver = args.driver.resolve()
    tests = driver / "tests"
    with tempfile.TemporaryDirectory(prefix="adreno-preempt-") as temporary:
        work = Path(temporary)
        iokit = work / "IOKit"
        iokit.mkdir()
        for header in ("IOLib.h", "IOTypes.h"):
            (iokit / header).write_text('#include "XnuMemoryFixture.h"\n', encoding="ascii")
        executable = work / ("test-preempt.exe" if os.name == "nt" else "test-preempt")
        subprocess.run(
            [args.cxx, "-std=c++17", "-O2", "-DNDEBUG",
             "-I", str(work), "-I", str(driver), "-I", str(tests),
             str(tests / "test_preempt.cpp"), str(driver / "NativeAdrenoPreempt.cpp"),
             "-o", str(executable)],
            check=True,
        )
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
