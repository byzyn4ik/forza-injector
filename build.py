"""Build forza_injector: the core (winmm.dll for Azimuth) and the sample plugins.

Usage: py build.py [--zig <zig.exe>] [--output out] [--test] [--game <folder>] [--rebuild]

  <output>/core/winmm.dll                 the game-independent Azimuth plugin host
  <output>/core-test/winmm.dll            the same with the core's test entry points
  <output>/samples/<id>/mods/<Mod>/...    each sample plugin
  <output>/release/forza-injector-<version>.zip
                                          winmm.dll, injector.json, LICENSE, NOTICE
tools/validate.py runs first (a structure failure stops the build). --test
then runs tests/run_tests.py (offline; the game files below --game, default
~/FH2, are optional). Zig 0.15.2: --zig, else ZIG, else zig on PATH.
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
import build_common as bc  # noqa: E402

ZIP_TIME = (2026, 1, 1, 0, 0, 0)  # fixed, so the same files give the same ZIP


def build_core(zig, output, test, rebuild):
    name = "core-test" if test else "core"
    command = [sys.executable, ROOT / "core" / "build.py", zig, "--azimuth",
               "--output", output / name / "winmm.dll", "--build-dir", output / ".build" / name]
    if test:
        command.append("--test")
    if rebuild:
        command.append("--rebuild")
    bc.run(command)


def azimuth_hosts():
    """The Azimuth executables the core accepts, read from its host table
    (core/adapters/azimuth.c host_profiles): version, file size and SHA-256 of
    the release, and its code identity (tools/azimuth_identity.py) when the
    core also accepts a copy with changed resources (icon, version details,
    manifest; renamed too)."""
    source = (ROOT / "core" / "adapters" / "azimuth.c").read_text(encoding="utf-8")
    table = source[source.index("host_profiles[] = {"):]
    table = table[:table.index("};")]
    hosts = []
    pattern = r'\{AZIMUTH_(\d)(\d)(\d),\s*(\d+)u,[^{]*\{([^}]*)\},\s*"[^"]*",\s*\{([^}]*)\}\}'
    for match in re.finditer(pattern, table):
        name = "AZIMUTH_" + "".join(match.group(i) for i in (1, 2, 3))
        digest = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", match.group(5)))
        if len(digest) != 32:
            sys.exit(f"azimuth.c: host {name} has no 32-byte SHA-256")
        code = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", match.group(6)))
        if code and len(code) != 32:
            sys.exit(f"azimuth.c: host {name} has a code identity that is not 32 bytes")
        host = {"version": ".".join(match.group(i) for i in (1, 2, 3)), "file_size": int(match.group(4)),
                "sha256": digest.hex()}
        if code:
            host["code_sha256"] = code.hex()
        hosts.append(host)
    if not hosts:
        sys.exit("azimuth.c: no host_profiles found")
    return hosts


def injector_manifest(version):
    """injector.json: what the release supports (the builder reads it).
    "azimuth": the azimuth.exe files the core loads in (size, SHA-256): a
    builder checks the chosen azimuth.exe against it before installing.
    "code_sha256" (when there): the core also loads in a copy of that release
    whose resources were changed and which may be renamed (e.g. <Game>.exe
    with the game's icon and details), when its code identity is this."""
    profiles = [bc.read_json(path) for path in sorted((ROOT / "profiles").glob("*.json"))]
    return {
        "format_version": 1,
        "version": version,
        "azimuth": azimuth_hosts(),
        "plugin_api": [1, 2],
        "runtime_api": [1],
        "any_game": True,
        "games": [{"game": p["game"], "title_id": p["title_id"], "version": p["build"]["version"],
                   "timestamp": p["build"]["timestamp"], "image_size": p["build"]["image_size"],
                   "game_runtime": p.get("game_runtime"), "runtime_hooks": sorted(p["runtime_hooks"])} for p in profiles],
    }


def package(output, version):
    release = output / "release"
    release.mkdir(parents=True, exist_ok=True)
    manifest = json.dumps(injector_manifest(version), indent=2) + "\n"
    (output / "core" / "injector.json").write_text(manifest, encoding="utf-8", newline="\n")
    archive = release / f"forza-injector-{version}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zip_file:
        for name, path in (("winmm.dll", output / "core" / "winmm.dll"), ("injector.json", output / "core" / "injector.json"),
                           ("LICENSE", ROOT / "LICENSE"), ("NOTICE", ROOT / "NOTICE"), ("README.md", ROOT / "README.md")):
            info = zipfile.ZipInfo(name, ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            zip_file.writestr(info, path.read_bytes())
    print(archive)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zig", help="zig.exe (0.15.2)")
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--test", action="store_true", help="run the offline tests after building")
    parser.add_argument("--game", type=Path, help="tests: the folder holding the game packages (default ~/FH2)")
    parser.add_argument("--rebuild", action="store_true", help="ignore the core's build cache")
    options = parser.parse_args()
    if subprocess.run([sys.executable, ROOT / "tools" / "validate.py"]).returncode:
        sys.exit("structure validation failed")
    zig = bc.find_zig(options.zig)
    output = options.output.resolve()
    version = (ROOT / "VERSION").read_text().strip()
    build_core(zig, output, False, options.rebuild)
    build_core(zig, output, True, options.rebuild)
    for sample in sorted(p for p in (ROOT / "samples").iterdir() if (p / "build.py").is_file()):
        bc.run([sys.executable, sample / "build.py", "--zig", zig, "--output", output / "samples" / sample.name])
    package(output, version)
    if options.test:
        command = [sys.executable, ROOT / "tests" / "run_tests.py", "--output", output, "--zig", zig]
        if options.game:
            command += ["--game", options.game]
        if subprocess.run([str(part) for part in command]).returncode:
            sys.exit("tests failed")


if __name__ == "__main__":
    main()
