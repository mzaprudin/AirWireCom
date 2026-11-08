#!/usr/bin/env python3
"""Compile and run firmware behavior checks without boards or PlatformIO."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    failed = False
    with tempfile.TemporaryDirectory(prefix="airwirecom-tests-") as directory:
        for platform, defines in (("esp32", []), ("esp8266", ["-DESP8266"])):
            executable = Path(directory) / platform
            command = [os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra",
                       "-I", str(ROOT / "tests/fakes"), "-I", str(ROOT / "include"),
                       *defines, str(ROOT / "tests/firmware_regression.cpp"), "-o", str(executable)]
            print(f"=== {platform} ===", flush=True)
            compiled = subprocess.run(command, check=False)
            if compiled.returncode:
                failed = True
                continue
            result = subprocess.run([str(executable)], check=False)
            failed |= result.returncode != 0
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
