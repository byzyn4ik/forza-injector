"""Offline tests of forza_injector against the real game files (nothing is run).

Usage: py tests/run_tests.py [--game <folder>] [--output out]
  --game    the folder holding the game packages (default ~/FH2): FH2 in
            Anthem_1.0.0.64_x64__8wekyb3d8bbwe, Fast & Furious in
            Spire_1.0.0.6_x64__8wekyb3d8bbwe (directly or one folder deeper)
  --output  the build output (default out): out/core/winmm.dll is checked
Each profile's game EXE: file size, TimeDateStamp, SizeOfImage, and the
original bytes at every optional runtime hook that names them. A game whose EXE is
missing is skipped. Prints ok / FAIL / skip per test; exits 1 on a failure.
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import ctypes
import json
import re
import os
import shutil
import subprocess
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_common as bc  # noqa: E402

counts = {"ok": 0, "FAIL": 0, "skip": 0}


def result(kind, message):
    counts[kind] += 1
    print(f"  {kind:<5} {message}")


def find_exe(game_folder, package, exe):
    for base in [game_folder, *[p for p in game_folder.iterdir() if p.is_dir()]] if game_folder.is_dir() else []:
        for candidate in base.glob(f"{package}_*"):
            path = candidate / "Mount" / exe
            if path.is_file():
                return path
    return None


def test_profile(path, game_folder):
    profile = json.loads(path.read_text(encoding="utf-8"))
    build = profile["build"]
    exe = find_exe(game_folder, profile["package"], build["exe"])
    if not exe:
        result("skip", f"{path.name}: no {profile['package']} {build['exe']} below {game_folder}")
        return
    timestamp, image_size, sections = bc.pe_identity(exe)
    result("ok" if exe.stat().st_size == build["file_size"] else "FAIL",
           f"{path.name}: file size {exe.stat().st_size} (profile {build['file_size']})")
    result("ok" if timestamp == int(build["timestamp"], 16) else "FAIL",
           f"{path.name}: TimeDateStamp {timestamp:#x}")
    result("ok" if image_size == int(build["image_size"], 16) else "FAIL",
           f"{path.name}: SizeOfImage {image_size:#x}")
    for name, hook in profile["runtime_hooks"].items():
        rva = int(hook["rva"], 16)
        if "original" in hook:
            expected = bytes.fromhex(hook["original"])
            actual = bc.read_at_rva(exe, sections, rva, len(expected))
            result("ok" if actual == expected else "FAIL",
                   f"{path.name}: {name} at {rva:#x} holds {expected.hex()} (found {actual.hex() if actual else None})")
        if "original_function_rva" in hook:
            # A vtable slot in the file holds the function's preferred-base address.
            slot = bc.read_at_rva(exe, sections, rva, 8)
            target = struct.unpack("<Q", slot)[0] - 0x140000000 if slot else None
            expected = int(hook["original_function_rva"], 16)
            result("ok" if target == expected else "FAIL",
                   f"{path.name}: {name} slot {rva:#x} points to {expected:#x} (found {target:#x})"
                   if target is not None else f"{path.name}: {name} slot {rva:#x} not in the file")


def test_core(output):
    dll = output / "core" / "winmm.dll"
    if not dll.is_file():
        result("skip", "out/core/winmm.dll not built (run build.py)")
        return
    exports = set(bc.pe_exports(dll))
    # winmm's own exports (forwarded to System32), Extra Options' status, and ours.
    for export in ("timeBeginPeriod", "PlaySoundW", "Fh2TweaksStatus", "ForzaInjectorStatus"):
        result("ok" if export in exports else "FAIL", f"core winmm.dll exports {export}")
    result("ok" if len(exports) >= 180 else "FAIL", f"core winmm.dll exports {len(exports)} names (winmm has 180)")


def test_sdk(output, zig):
    """tests/sdk_test.c: forza_patch.h and forza_profile.h, compiled and run."""
    if not zig:
        result("skip", "SDK test: no zig (--zig or ZIG)")
        return
    exe = output / "tests" / "sdk_test.exe"
    exe.parent.mkdir(parents=True, exist_ok=True)
    build = subprocess.run([str(zig), "cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-I", str(bc.SDK_INCLUDE), str(ROOT / "tests" / "sdk_test.c"), "-o", str(exe)],
                           capture_output=True, text=True)
    if build.returncode:
        result("FAIL", "SDK test builds: " + build.stderr.strip()[:400])
        return
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    for line in run.stdout.splitlines():
        line = line.strip()
        if line.startswith(("ok", "FAIL")):
            kind, _, what = line.partition(" ")
            result(kind, "SDK: " + what.strip())
    if run.returncode and "FAIL" not in run.stdout:
        result("FAIL", f"SDK test exit code {run.returncode}")


def test_export_reader(output):
    """The core's forza_file_exports (early phase) on real DLLs, through the test build."""
    dll = output / "core-test" / "winmm.dll"
    hello = output / "samples" / "hello" / "mods" / "Hello" / "plugins" / "Hello.dll"
    if not dll.is_file() or not hello.is_file():
        result("skip", "export reader: core-test or the Hello sample not built")
        return
    core = ctypes.WinDLL(str(dll))
    reader = core.ForzaTestFileExports
    reader.argtypes, reader.restype = [ctypes.c_wchar_p, ctypes.c_char_p], ctypes.c_int
    for name, expected in ((b"ForzaPluginInitialize", 1), (b"FH2PluginInitialize", 1),
                           (b"ForzaPluginEarlyInitialize", 0), (b"ForzaPlugin", 0)):
        found = reader(str(hello), name)
        result("ok" if bool(found) == bool(expected) else "FAIL",
               f"export reader: Hello.dll {'exports' if expected else 'does not export'} {name.decode()}")
    result("ok" if reader(str(dll), b"ForzaInjectorStatus") else "FAIL", "export reader: winmm.dll exports ForzaInjectorStatus")
    result("ok" if not reader(str(ROOT / "README.md"), b"x") else "FAIL", "export reader: a non-PE file exports nothing")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", type=Path, default=bc.GAME_FOLDER)
    parser.add_argument("--zig", help="zig.exe for the SDK test (default: ZIG, then PATH)")
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    options = parser.parse_args()
    print("forza_injector offline tests")
    for path in sorted((ROOT / "profiles").glob("*.json")):
        test_profile(path, options.game)
    test_core(options.output.resolve())
    zig = options.zig or os.environ.get("ZIG") or shutil.which("zig")
    test_sdk(options.output.resolve(), zig)
    test_export_reader(options.output.resolve())
    from azimuth_identity_tests import run_azimuth_identity_tests
    run_azimuth_identity_tests(options.output.resolve(), options.game, result)
    from host_tests import run_host_tests
    if zig:
        run_host_tests(options.output.resolve(), zig, options.game, result)
    else:
        result("skip", "host lifecycle tests: Zig not supplied")
    from provider_integration import run_provider_integration
    run_provider_integration(options.output.resolve(), options.game, result)
    from saves_tests import run_saves_tests
    if zig:
        run_saves_tests(options.output.resolve(), zig, result)
    else:
        result("skip", "saves location tests: Zig not supplied")
    print(f"{counts['ok']} ok, {counts['FAIL']} FAIL, {counts['skip']} skip")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
