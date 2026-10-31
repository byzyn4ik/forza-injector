"""Build the Hello sample plugin.

Usage: py samples/hello/build.py [--zig <zig.exe>] [--output out]
Writes <output>/mods/Hello/mod.json and <output>/mods/Hello/plugins/Hello.dll.
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
import build_common as bc  # noqa: E402

MOD_NAME = "Hello"
DLL_NAME = "Hello.dll"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zig", help="zig.exe (0.15.2)")
    parser.add_argument("--output", type=Path, default=HERE / "out")
    options = parser.parse_args(argv)
    zig = bc.find_zig(options.zig)
    output = options.output.resolve()
    build, plugins = output / "obj", output / "mods" / MOD_NAME / "plugins"
    for folder in (build, plugins):
        folder.mkdir(parents=True, exist_ok=True)
    version = bc.read_json(HERE / "mod" / "mod.json")["version"]
    bc.version_rc(build / "version.rc", version, DLL_NAME, "Hello (forza_injector sample plugin)", "forza_injector")
    bc.run([*bc.plugin_compiler(zig), "-shared", "-s", HERE / "src" / "plugin.c", build / "version.rc",
            "-Wl,--out-implib," + str(build / "plugin.lib"), "-o", plugins / DLL_NAME])
    for stale in plugins.glob("*.lib"):
        stale.unlink()
    shutil.copy2(HERE / "mod" / "mod.json", plugins.parent / "mod.json")
    print(plugins / DLL_NAME)


if __name__ == "__main__":
    main()
