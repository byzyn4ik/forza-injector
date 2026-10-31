"""Offline tests of the saves location (core/adapters/azimuth_saves.c), ported from
Extra Options 0.3.2-openbox's tools/test-appdata-saves.py.

run_saves_tests(output, zig, result) builds tests/saves_probe.c (a DLL standing
for azimuth.exe's import table) and runs this file as a child process on the
core's test build. Everything happens in a temporary folder with fake save
trees in Azimuth 1.0.4's layout; the real Documents folder is only looked up
(SHGetKnownFolderPath), never opened, and no game or saves folder is touched.

Not covered offline: azimuth.exe's own storage code (its launcher and the
game's connected storage) cannot run without starting Azimuth; the probe DLL
calls the same KERNEL32 functions through a patched import table instead.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FH2, FF = "1A96545B", "5A9771A4"
NAMES = {FH2: "Forza Horizon 2", FF: "Forza Horizon 2 Presents Fast & Furious"}


def run_saves_tests(output, zig, result):
    dll = output / "core-test" / "winmm.dll"
    if not dll.is_file():
        result("skip", "saves location: core-test not built")
        return
    probe = output / "tests" / "saves-probe.dll"
    probe.parent.mkdir(parents=True, exist_ok=True)
    build = subprocess.run([str(zig), "cc", "-target", "x86_64-windows-gnu", "-std=c11", "-O2", "-Wall", "-Wextra",
                            "-Werror", "-shared", "-s", str(ROOT / "tests" / "saves_probe.c"), "-o", str(probe)],
                           capture_output=True, text=True)
    if build.returncode:
        result("FAIL", "saves location: the probe DLL builds: " + build.stderr.strip()[:400])
        return
    runs = output / "test-runs"
    runs.mkdir(parents=True, exist_ok=True)
    child = subprocess.run([sys.executable, "-I", str(Path(__file__).resolve()), str(dll), str(probe), str(runs)],
                           capture_output=True, text=True, timeout=300)
    seen = False
    for line in child.stdout.splitlines():
        kind, _, what = line.partition(" ")
        if kind in ("ok", "FAIL"):
            seen = True
            result(kind, "saves location: " + what.strip())
    if child.returncode or not seen:
        result("FAIL", f"saves location: child exit {child.returncode}: {child.stderr[-1500:]}")
    # DllMain with [Saves] Location=Documents in a process that is not Azimuth:
    # the setting is read and logged, nothing is hooked, and loading succeeds.
    with tempfile.TemporaryDirectory(prefix="saves-attach-", dir=runs) as temporary:
        folder = Path(temporary)
        shutil.copy2(dll, folder / "winmm.dll")
        (folder / "forza-injector.ini").write_text("[Saves]\nLocation=Documents\n", encoding="utf-8")
        loaded = subprocess.run([sys.executable, "-I", "-c", "import ctypes,sys; ctypes.WinDLL(sys.argv[1])",
                                 str(folder / "winmm.dll")], capture_output=True, text=True, timeout=60)
        log = folder / "forza-injector.log"
        text = log.read_text(encoding="utf-8") if log.is_file() else ""
        result("ok" if not loaded.returncode and "the host is not Azimuth 1.0.1 to 1.0.4" in text else "FAIL",
               f"saves location: DllMain reads the setting and leaves a non-Azimuth host alone ({text.strip()[:200]!r})")


# ---------------------------------------------------------------- the child


def check(condition, message):
    print(("ok " if condition else "FAIL ") + message, flush=True)
    return condition


def sha(data):
    return hashlib.sha256(data).hexdigest()


def make_revision(revisions, number, blobs, committed=True, mtime=None, damaged=False):
    """A revision folder in Azimuth 1.0.4's layout: data\\<blob>..., then commit (or pending)."""
    revision = revisions / f"{number:016x}"
    (revision / "data").mkdir(parents=True)
    lines = ["Azimuth complete save revision v2", f"blobs {len(blobs)}"]
    for name, content in blobs.items():
        (revision / "data" / name).write_bytes(content)
        lines.append(f"{sha(content)} {len(content)} {name}")
    (revision / ("commit" if committed else "pending")).write_bytes(("\n".join(lines) + "\n").encode())
    if damaged:
        first = next(iter(blobs))
        (revision / "data" / first).write_bytes(b"X" + blobs[first][1:])
    if mtime:
        for path in revision.rglob("*"):
            os.utime(path, (mtime, mtime))
    return revision


def profile_blobs(seed, big=False):
    return {"BinarySaveState": os.urandom(3000) + seed, "PlayerDatabase": os.urandom((1 << 20) + 17 if big else 5000),
            "VersionFlags": b"\x01\x00\x00\x00"}


def contents(root):
    return sorted((str(p.relative_to(root)), p.read_bytes()) for p in Path(root).rglob("*") if p.is_file())


def revisions_of(folder):
    return sorted(p.name for p in Path(folder).iterdir()) if Path(folder).is_dir() else []


def child(dll_path, probe_path, runs):
    dll = ctypes.WinDLL(dll_path)
    probe = ctypes.WinDLL(probe_path)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    dll.ForzaTestSavesConfigure.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
    dll.ForzaTestSavesHook.argtypes, dll.ForzaTestSavesHook.restype = [ctypes.c_char_p], ctypes.c_void_p
    dll.ForzaTestSavesMap.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int]
    dll.ForzaTestSavesSetting.argtypes = [ctypes.c_wchar_p]
    dll.ForzaTestDocumentsFolder.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
    dll.ForzaTestSavesPatch.argtypes = [ctypes.c_void_p]
    dll.ForzaTestSavesTitles.argtypes = [ctypes.c_wchar_p, ctypes.c_uint]
    dll.ForzaInjectorSavesFolder.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint]
    dll.ForzaInjectorSavesFolder.restype = ctypes.c_uint
    probe.ProbeCall.argtypes = [ctypes.c_int, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, wt.DWORD]
    probe.ProbeCall.restype = wt.DWORD

    def mapped(path, reverse=0):
        out = ctypes.create_unicode_buffer(4096)
        return out.value if dll.ForzaTestSavesMap(path, out, 4096, reverse) else None

    def call(operation, path, other=None):
        out = ctypes.create_unicode_buffer(2048)
        value = probe.ProbeCall(operation, path, other, out, 2048)
        return value, out.value

    def configure(base, prepare=1):
        log = base / "forza-injector.log"
        log.unlink(missing_ok=True)
        ok = dll.ForzaTestSavesConfigure(str(base / "Game" / "data" / "titles"), str(base / "Documents" / "My Games"),
                                         str(log), prepare)
        return ok, log.read_text(encoding="utf-8") if log.is_file() else ""

    def roots(base, title):
        return (base / "Game" / "data" / "titles" / title / "saves",
                base / "Documents" / "My Games" / NAMES[title] / "Saves" / title)

    profile = Path("player-1") / "User_9000000000000" / "revisions"
    with tempfile.TemporaryDirectory(prefix="saves-", dir=runs) as temporary:
        base = Path(temporary)

        # 1. The titles table matches the FH2 titles' profiles/*.json (the redirect is verified for those
        # two only; other games' saves stay in the game folder); the Documents lookup matches Windows'.
        text = ctypes.create_unicode_buffer(4096)
        dll.ForzaTestSavesTitles(text, 4096)
        table = dict(line.split("\t") for line in text.value.splitlines())
        all_profiles = [json.loads(p.read_text(encoding="utf-8")) for p in (ROOT / "profiles").glob("*.json")]
        profiles = {p["title_id"]: p["name"] for p in all_profiles if p["game"] in ("fh2", "fh2ff")}
        check(table == profiles, f"titles table {table} = the FH2 titles' profiles/*.json")
        others = {p["title_id"]: p["game"] for p in all_profiles if p["game"] not in ("fh2", "fh2ff")}
        check(not set(table) & set(others), f"no saves redirect for {sorted(others.values())}")
        folder_id = (ctypes.c_byte * 16).from_buffer_copy(bytes.fromhex("d09ad3fd8f23af46adb46c85480369c7"))
        known = ctypes.c_wchar_p()
        ctypes.WinDLL("shell32").SHGetKnownFolderPath(ctypes.byref(folder_id), 0, None, ctypes.byref(known))
        found = ctypes.create_unicode_buffer(4096)
        dll.ForzaTestDocumentsFolder(found, 4096)
        check(found.value.lower() == (known.value or "").lower(), f"Documents folder {found.value!r} = known folder")

        # 2. The setting.
        ini = base / "forza-injector.ini"
        for body, expected in [("", 0), ("[Saves]\nLocation=Documents\n", 1), ("[Saves]\nLocation=documents\n", 1),
                               ("[Saves]\nLocation=Game\n", 0), ("[Saves]\nLocation=Elsewhere\n", -1),
                               ("[Other]\nLocation=Documents\n", 0)]:
            ini.write_text(body, encoding="utf-8")
            check(dll.ForzaTestSavesSetting(str(ini)) == expected, f"[Saves] Location {body.strip()!r} -> {expected}")

        # 3. First start: the game folder's complete revisions are copied (verified), the rest is left.
        one = base / "one"
        game, docs = roots(one, FF)
        blobs = {n: profile_blobs(bytes([n]), big=n == 6) for n in (4, 5, 6, 7)}
        for number in (4, 5, 6):
            make_revision(game / profile, number, blobs[number])
        make_revision(game / profile, 7, blobs[7], committed=False)        # still being written
        make_revision(game / "machine" / "Settings" / "revisions", 1, {"Settings": b"s" * 100})
        make_revision(game / "player-1" / "Photo_1" / "revisions", 2, {"Photo": b"p" * 100}, damaged=True)
        before = contents(game)
        ok, log = configure(one)
        check(ok == 1, "first start: prepared and on")
        check(revisions_of(docs / profile) == ["0000000000000005", "0000000000000006"],
              f"first start: the newest two complete revisions copied ({revisions_of(docs / profile)})")
        check(contents(docs / profile / "0000000000000006") == contents(game / profile / "0000000000000006"),
              "first start: copied byte for byte")
        check((docs / "machine" / "Settings" / "revisions" / "0000000000000001" / "commit").is_file(),
              "first start: the machine space too")
        check(not (docs / "player-1" / "Photo_1").exists(), "first start: a revision not matching its commit is not copied")
        check(contents(game) == before, "first start: the game-folder saves are unchanged")
        check("3 revisions copied" in log and "1 damaged" in log, "first start: the log counts copies and damaged ones")
        check(not (one / "Documents" / "My Games" / NAMES[FH2]).exists(),
              "first start: no Documents folder for a title the install does not have")
        check(not any(docs.parent.glob(FF + ".injector-staging/*")), "first start: no staging copy left")
        check((docs.parent / (FF + ".migrated.txt")).read_text(encoding="utf-8").strip().lower() == str(game).lower(),
              "first start: the migrated game folder is noted")

        # 4. Mapping, both ways, both titles.
        g, a = str(game), str(docs)
        check(mapped(g + r"\player-1\x") == a + r"\player-1\x", "map: below the saves folder")
        check(mapped(g) == a and mapped(g + "\\") == a + "\\", "map: the saves folder itself")
        check(mapped(g.upper().replace("\\", "/") + "/p/q") == a + r"\p\q", "map: case and slashes")
        check(mapped("\\\\?\\" + g + r"\p") == "\\\\?\\" + a + r"\p" and mapped("\\??\\" + g + r"\p") == "\\??\\" + a + r"\p",
              "map: \\\\?\\ and \\??\\ forms")
        check(mapped(g + "X\\p") is None and mapped(g[:-1]) is None and mapped(str(game.parent / "tmp")) is None,
              "map: look-alikes and siblings are left")
        fh2_game, fh2_docs = roots(one, FH2)
        check(mapped(str(fh2_game) + r"\player-1") == str(fh2_docs) + r"\player-1", "map: Forza Horizon 2's title too")
        check(mapped(str(one / "Game" / "data" / "titles" / "4CE1DFAB" / "saves")) is None, "map: another title is left")
        start = os.getcwd()
        os.chdir(game.parent)
        check(mapped(r"saves\player-1") == a + r"\player-1" and mapped(r"..\5A9771A4\SAVES") == a, "map: relative paths")
        os.chdir(start)
        check(mapped(g + "\\" + "\\".join(["d" * 50] * 5)).startswith("\\\\?\\" + a), "map: long paths get \\\\?\\")
        check(mapped("\\\\?\\" + a + r"\p", 1) == "\\\\?\\" + g + r"\p" and mapped(a + r"\p", 1) == g + r"\p",
              "map back: DOS forms")
        if a[0].lower() == g[0].lower():
            check(mapped(a[2:] + r"\p", 1) == g[2:] + r"\p", "map back: volume-relative")
            check(mapped("\\Device\\HarddiskVolume3" + a[2:], 1) == "\\Device\\HarddiskVolume3" + g[2:], "map back: \\Device")
        check(mapped(a + "X", 1) is None, "map back: look-alikes are left")
        folder = ctypes.create_unicode_buffer(1024)
        length = dll.ForzaInjectorSavesFolder(FF, folder, 1024)
        check(length == len(a) and folder.value == a, "ForzaInjectorSavesFolder: the Documents folder for plugins")
        check(dll.ForzaInjectorSavesFolder("4CE1DFAB", folder, 1024) == 0 and dll.ForzaInjectorSavesFolder(FF, folder, 5) == 0,
              "ForzaInjectorSavesFolder: 0 for another title or a small buffer")

        # 5. Azimuth's import table patched (the probe DLL stands for azimuth.exe).
        handle = ctypes.c_void_p(probe._handle)
        check(dll.ForzaTestSavesPatch(handle) == 1, "imports: the probe's KERNEL32 file imports are patched")
        check(dll.ForzaTestSavesPatch(handle) == 0, "imports: refused when another module already replaced them")
        new = g + r"\player-1\Livery_1"
        check(call(0, new)[0] and (docs / "player-1" / "Livery_1").is_dir() and not (game / "player-1" / "Livery_1").exists(),
              "imports: CreateDirectoryW lands in Documents")
        check(call(1, new + r"\pending", "x")[0] and (docs / "player-1" / "Livery_1" / "pending").is_file(),
              "imports: CreateFileW lands in Documents")
        check(call(3, new + r"\pending", new + r"\commit")[0] and (docs / "player-1" / "Livery_1" / "commit").is_file(),
              "imports: MoveFileExW (pending -> commit) in Documents")
        final = call(9, new + r"\commit")[1]
        check(final.lower() == ("\\\\?\\" + new + r"\commit").lower(), f"imports: the final path reads as the game folder's ({final})")
        check(call(10, new + r"\commit", "\\??\\" + new + r"\renamed")[0] and (docs / "player-1" / "Livery_1" / "renamed").is_file(),
              "imports: SetFileInformationByHandle rename to a game-folder NT path")
        check(call(2, new + r"\renamed")[0] != 0xFFFFFFFF and call(7, new + r"\renamed")[0] == 3,
              "imports: GetFileAttributesW / GetFileAttributesExW")
        check(call(6, new + r"\*")[0] == 1 and call(8, g + r"\*")[0] == 1, "imports: FindFirstFileExW / FindFirstFileW")
        check(call(4, new + r"\renamed")[0] and call(5, new)[0] and not (docs / "player-1" / "Livery_1").exists(),
              "imports: DeleteFileW / RemoveDirectoryW")
        check(call(0, str(fh2_game))[0] and fh2_docs.is_dir(),
              "imports: Azimuth creating a new title's saves folder makes My Games\\<game>\\Saves first")
        check(contents(game) == before, "imports: the game-folder saves are still unchanged")

        # 6. Next start: the game folder is not copied again (a container the game deleted stays deleted).
        shutil.rmtree(docs / "machine")
        ok, log = configure(one)
        check(ok == 1 and not (docs / "machine").exists(), "next start: nothing copied again")

        # 7. Documents holds a mirror's history: used as it is, all but the newest two move to <title>.history.
        two = base / "two"
        game, docs = roots(two, FF)
        make_revision(game / profile, 5, profile_blobs(b"5"), mtime=2_000_000_000)
        make_revision(game / profile, 6, profile_blobs(b"6"), mtime=2_000_000_100)  # saved after Documents' newest
        history = {n: profile_blobs(bytes([n % 256])) for n in range(0x70, 0x7a)}
        for number, blob in history.items():
            make_revision(docs / profile, number, blob, mtime=1_900_000_000 + number)
        mirror_before = {name: contents(docs / profile / name) for name in revisions_of(docs / profile)}
        other = roots(two, FH2)[1] / profile  # Forza Horizon 2's Documents saves; this install has no FH2
        for number in (1, 2, 3):
            make_revision(other, number, profile_blobs(bytes([number])))
        game_before = contents(game)
        ok, log = configure(two)
        kept = revisions_of(docs / profile)
        moved = revisions_of(docs.parent / (FF + ".history") / profile)
        check(ok == 1 and kept == ["0000000000000078", "0000000000000079"], f"history: the newest two stay ({kept})")
        check(len(moved) == 8 and all(contents(docs.parent / (FF + ".history") / profile / n) == mirror_before[n] for n in moved),
              "history: the 8 older revisions are kept, untouched, in <title>.history")
        check("8 older revisions" in log and "saved after Documents' revision" in log and "1 saves already in Documents" in log,
              "history: logged, with the note that the game folder's save is newer")
        check(contents(game) == game_before and not any((docs / profile).glob("0000000000000005")),
              "history: Documents' saves win; the game folder is not copied or changed")
        check(len(revisions_of(other)) == 3 and not (other.parents[2].parent / (FH2 + ".history")).exists(),
              "history: another title's Documents folder (not in this install) is left alone")
        ok, log = configure(two)
        check(ok == 1 and "older revisions" not in log, "history: nothing more to move at the next start")

        # 8. Documents' newest revision does not verify: it stays, and so do the two verified ones below it.
        three = base / "three"
        game, docs = roots(three, FF)
        game.mkdir(parents=True)  # the install has the title, with no saves yet
        for number in (1, 2, 3):
            make_revision(docs / profile, number, profile_blobs(bytes([number])))
        make_revision(docs / profile, 4, profile_blobs(b"4"), damaged=True)
        ok, log = configure(three)
        check(ok == 1 and revisions_of(docs / profile) == [f"{n:016x}" for n in (2, 3, 4)],
              "history: an unverified newest revision does not count as one of the two")

        # 9. A copy fails (a source file is locked): nothing is redirected this session, nothing is left behind.
        four = base / "four"
        game, docs = roots(four, FF)
        revision = make_revision(game / profile, 9, profile_blobs(b"9"))
        before = contents(game)
        locked = k32.CreateFileW(str(revision / "data" / "PlayerDatabase"), 0x80000000, 0, None, 3, 0x80, None)
        try:
            ok, log = configure(four)
        finally:
            k32.CloseHandle(ctypes.c_void_p(locked))
        check(ok == 0 and mapped(str(game) + r"\p") is None, "failure: the session keeps the game folder")
        check(not (docs.parent / (FF + ".migrated.txt")).exists() and not any(docs.rglob("commit")),
              "failure: not noted as migrated, no partial revision in Documents")
        check(contents(game) == before, "failure: the game-folder saves are unchanged")
        ok, log = configure(four)
        check(ok == 1 and revisions_of(docs / profile) == ["0000000000000009"], "failure: copied at the next start")

        # 10. A link in place of the Documents folder is refused (Azimuth refuses links).
        five = base / "five"
        game, docs = roots(five, FF)
        make_revision(game / profile, 1, profile_blobs(b"1"))
        target = base / "elsewhere"
        target.mkdir()
        docs.parent.mkdir(parents=True)
        made = subprocess.run(["cmd", "/c", "mklink", "/J", str(docs), str(target)], capture_output=True)
        if made.returncode == 0:
            ok, log = configure(five)
            check(ok == 0 and not any(target.iterdir()), "links: a junction as the Documents folder is refused")
            os.rmdir(docs)
        else:
            print("ok links: skipped (mklink /J unavailable)")

        # 11. Off: nothing is mapped.
        dll.ForzaTestSavesOff()
        check(mapped(g + r"\p") is None and dll.ForzaInjectorSavesFolder(FF, folder, 1024) == 0, "off: nothing mapped")


if __name__ == "__main__":
    child(sys.argv[1], sys.argv[2], sys.argv[3])
