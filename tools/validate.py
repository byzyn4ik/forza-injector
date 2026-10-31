"""Validate host sources, game identity catalog, optional runtime hooks and sample manifests.
Gameplay patch validation belongs to the owning plugin repository.
"""
# SPDX-License-Identifier: GPL-3.0-or-later

import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = ["LICENSE", "NOTICE", "README.md", "VERSION", "build.py", ".gitattributes", ".github/workflows/build.yml",
            "core/build.py", "core/host.h", "core/plugins.c", "core/runtime.c", "core/export_reader.c", "core/json.c",
            "sdk/include/fh2_plugin.h", "sdk/include/forza_plugin.h", "sdk/include/forza_profile.h",
            "sdk/include/forza_runtime.h", "sdk/include/fh2_guard.h", "sdk/include/fh2_game.h",
            "tools/build_common.py", "tools/validate.py", "tools/azimuth_identity.py", "tests/run_tests.py",
            "tests/host_tests.py", "tests/azimuth_identity_tests.py"]
HOOK_TYPES = {"vtable_function_pointer", "relative_call", "code"}
HEX = re.compile(r"^0x[0-9a-f]+$")
VERSION = re.compile(r"^\d+\.\d+\.\d+$")


class Report:
    def __init__(self):
        self.failures = 0

    def check(self, condition, message):
        print(("  ok    " if condition else "  FAIL  ") + message)
        if not condition:
            self.failures += 1
        return condition


def game_table(header):
    """[(id, title_id, timestamp, image_size)] from kForzaGames in forza_profile.h."""
    text = Path(header).read_text(encoding="utf-8")
    rows = re.findall(r'\{FORZA_GAME_\w+, "(\w+)", "(\w+)", "\w*", (0x[0-9a-f]+u|0), (0x[0-9a-f]+u|0)\}', text)
    return [(i, t, int(s.rstrip("u"), 16) if s != "0" else 0, int(z.rstrip("u"), 16) if z != "0" else 0)
            for i, t, s, z in rows]


def check_games(report, games):
    report.check(len(games) >= 2, f"forza_profile.h: {len(games)} games in kForzaGames")
    report.check(len({g[0] for g in games}) == len(games), "forza_profile.h: game ids unique")
    report.check(len({g[1] for g in games}) == len(games), "forza_profile.h: title ids unique")
    for game_id, title, _, _ in games:
        report.check(re.fullmatch(r"[0-9A-F]{8}", title) is not None, f"forza_profile.h: {game_id} title {title}")


def check_profiles(report, folder, games):
    known = {g[0]: g for g in games}
    profiles = sorted(Path(folder).glob("*.json"))
    report.check(bool(profiles), f"profiles: {len(profiles)} found")
    for path in profiles:
        name = path.name
        try:
            profile = json.loads(path.read_text(encoding="utf-8"))
        except ValueError as error:
            report.check(False, f"{name}: not JSON ({error})")
            continue
        game = known.get(profile.get("game"))
        if not report.check(game is not None, f"{name}: game {profile.get('game')!r} is in kForzaGames"):
            continue
        build = profile.get("build", {})
        report.check(name == f"{game[0]}-{game[1]}.json", f"{name}: named <game>-<title id>.json")
        report.check(profile.get("format_version") == 1, f"{name}: format_version 1")
        report.check(profile.get("title_id") == game[1], f"{name}: title id {profile.get('title_id')} = table")
        report.check(HEX.match(build.get("timestamp", "")) and int(build["timestamp"], 16) == game[2],
                     f"{name}: timestamp {build.get('timestamp')} = table")
        report.check(HEX.match(build.get("image_size", "")) and int(build["image_size"], 16) == game[3],
                     f"{name}: image size {build.get('image_size')} = table")
        hooks = profile.get("runtime_hooks")
        if report.check(isinstance(hooks, dict), f"{name}: runtime_hooks is an object ({len(hooks or {})} hooks)"):
            for hook_name, hook in hooks.items():
                report.check(hook.get("type") in HOOK_TYPES and HEX.match(hook.get("rva", "")) is not None,
                             f"{name}: hook {hook_name} has a known type and an RVA")


def check_plugin_tree(report, folders, game_ids, label):
    """Every folder in `folders` (one plugin each): build.py, src/, mod/mod.json."""
    ids = set()
    for folder in folders:
        name = f"{label}/{folder.name}"
        report.check((folder / "build.py").is_file(), f"{name}: build.py")
        report.check((folder / "src").is_dir(), f"{name}: src/")
        mod_json = folder / "mod" / "mod.json"
        if not report.check(mod_json.is_file(), f"{name}: mod/mod.json"):
            continue
        mod = json.loads(mod_json.read_text(encoding="utf-8"))
        report.check(mod.get("id") == folder.name, f"{name}: mod.json id is the folder's name")
        report.check(mod.get("id") not in ids, f"{name}: id unique")
        ids.add(mod.get("id"))
        report.check(bool(mod.get("name")) and VERSION.match(mod.get("version", "")) is not None,
                     f"{name}: name and version {mod.get('version')}")
        games = mod.get("games")
        report.check(isinstance(games, list) and set(games) <= set(game_ids),
                     f"{name}: games {games} are known game ids")


def main():
    report = Report()
    print("forza_injector structure")
    for relative in REQUIRED:
        report.check((ROOT / relative).is_file(), relative)
    report.check(VERSION.match((ROOT / "VERSION").read_text().strip() if (ROOT / "VERSION").is_file() else ""),
                 "VERSION is x.y.z")
    version = (ROOT / "VERSION").read_text().strip()
    header = (ROOT / "core/host.h").read_text()
    report.check(f'#define MOD_VERSION "{version}"' in header, "host version matches VERSION")
    games = game_table(ROOT / "sdk/include/forza_profile.h")
    check_games(report, games)
    check_profiles(report, ROOT / "profiles", games)
    build = (ROOT / "core/build.py").read_text(encoding="utf-8")
    report.check('"patch_engine.c"' not in build and '"settings.c"' not in build and '"features/' not in build,
                 "core build has no Extra Options gameplay, hooks or settings")
    adapter = (ROOT / "core/adapters/azimuth.c").read_text(encoding="utf-8")
    report.check("GAME_TIMESTAMP" not in adapter and "GAME_IMAGE_SIZE" not in adapter and "supported_title(" not in adapter,
                 "host admission has no game-specific timestamp, image size or title gate")
    check_plugin_tree(report, sorted(p for p in (ROOT / "samples").iterdir() if p.is_dir()), [g[0] for g in games],
                      "samples")
    print(f"{report.failures} failed" if report.failures else "all checks passed")
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
