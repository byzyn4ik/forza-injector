# forza_injector

> Inspired by [Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) by Arthur
> (RokkuDayo). Its host, mod loader and plugin API are where this project
> started (see [NOTICE](NOTICE)), and Extra Options' plugins still load here
> unchanged. Thank you, Arthur, for Extra Options and for opening it up to
> plugins.

A small, game-independent plugin host for games running in supported Azimuth
builds (currently 1.0.1–1.0.4). Put its `winmm.dll` beside the original
`azimuth.exe`. The host retains Azimuth's native mapping, imports and TLS.

Azimuth 1.0.2 and 1.0.4 may also be *branded*: a copy whose resources only
(icon, version details, manifest) were changed, under any name (e.g.
`<Game>.exe`). The host is recognized by its PE TimeDateStamp, SizeOfImage
(another one is allowed when SizeOfCode and AddressOfEntryPoint are the
release's) and, once at startup, by its file: the release's size and SHA-256,
or its *code identity* (`tools/azimuth_identity.py`: SHA-256 over every
section but `.rsrc`, their headers, the entry point and image base), which
resource edits leave unchanged and any code change breaks. Another EXE that
loads this `winmm.dll` and is not a supported Azimuth build is left alone.

One code change is accepted on a branded Azimuth 1.0.4: the *launch patch*
(`tools/patch_azimuth.py <exe> <game>\game\base`). With it, the game starts
without Azimuth's launcher, and `--show-launcher` opens the launcher. The game
window takes the game's name as its title, and the version details take
`LegalCopyright` from the publisher in the game's AppxManifest. The host
accepts it only when every patched site holds exactly the patch. With the
original bytes put back, the file must have the release's code identity.

The injector does not apply Extra Options gameplay patches, read its INI,
verify an FH2 game hash, or restrict admission to the games in its profile
table. A valid game mapped by a supported Azimuth host can load plugins,
including a game the injector does not recognize. Individual plugins remain
responsible for checking their supported game and build before changing it.

## Structure

- `core/`: Azimuth handoff, system winmm forwarding, mod discovery, plugin
  contexts and lifecycle, logging, and small platform helpers.
- `sdk/include/`: the original Extra Options v1 API, injector v2 API, optional
  game-runtime provider API, profile lookup, and checked patch helpers.
- `profiles/`: known game identities and metadata about optional runtime hooks.
  These are a catalog, not a game admission whitelist.
- [FH2-ExtraOptions-Injector-Plugins](https://github.com/byzyn4ik/FH2-ExtraOptions-Injector-Plugins)
  (`forza-plugins` beside this repository): gameplay implementations, native game
  hooks, settings, and Extra Options compatibility.

Extra Options' former core has moved to
`forza-plugins/plugins/xtra-options/runtime/` and builds as
`mods/Xtra Options/plugins/XtraRuntime.dll`. Its coupled settings, menus,
file/database/vehicle hooks and gameplay modules stay together in that
runtime plugin. Independent feature DLLs in the same mod own their own patch
groups. Removing Xtra Options removes those features from the injector.

For FH2 feature parity, install the **whole Xtra Options mod**, including its
runtime, individual feature DLLs and INIs. Installing only the lean
`winmm.dll` gives a plugin host without Extra Options gameplay features.

## Plugin lifecycle and compatibility

The original [fh2_plugin.h](sdk/include/fh2_plugin.h) stays unchanged.
Existing plugins can export `FH2PluginInitialize`. Plugins exporting
`ForzaPluginInitialize` receive v2 instead; it is preferred when both exist.
Optional `ForzaPluginEarlyInitialize` runs after mapping and before game code.
Early failure suppresses that DLL's later initializer.

An optional [runtime provider](sdk/include/forza_runtime.h) exports
`ForzaGameRuntimeInitialize`. The host probes providers before ordinary
plugins, in mod load order. A provider returns 0 to decline without changing
the game, 1 to claim its native hook/event/deferred plugin lifecycle, or -1
to report a claimed failure. Once claimed, the host does not initialize the
same plugins again or fall back after failure. The FH2 Extra Options runtime
supplies the original deferred lifecycle and native events.

Without a claiming runtime, the generic host calls early initializers and
then normal v2/v1 initializers on its worker. Native event subscriptions
return FALSE because no game hooks are present. This lets independent
plugins implement their own supported-game hooks without embedding them in
the injector.

Xtra Options DLLs load safely under stock Extra Options: its built-in features
remain responsible for those patches, so the DLLs avoid applying them twice.
Other existing Extra Options plugins keep their original API and behavior.

## Mods

`mods/<Mod>/plugins/*.dll`, top-level DLLs only. Folders and DLLs default to
alphabetical, case-insensitive order. UTF-8 `mods/loadorder.txt` overrides
folder order; `# Folder` disables a folder, and new unlisted folders follow.
The generic loader reads format-1 `mod.json`, optional `enabled`, `games`
(game ids or title ids; [] means any game), and ordered `dependencies`.
Invalid manifests, unmet dependencies and duplicate IDs are refused.
Runtime-specific media/database mod handling lives in the runtime plugin.
The host log is `forza-injector.log`.

## Saves in Documents

Azimuth keeps a game's saves in `data\titles\<title>\saves` beside
`azimuth.exe`. With

```ini
; forza-injector.ini, beside winmm.dll
[Saves]
Location=Documents
```

Forza Horizon 2 (`1A96545B`) and Fast & Furious (`5A9771A4`) load and store
their saves in `Documents\My Games\<game>\Saves\<title>` instead
(`core/adapters/azimuth_saves.c`). `Location=Game` or no setting keeps them
in Azimuth's folder; the injector release ships no `forza-injector.ini`.
Builders, or the Saves in Documents plugin (its `Mode=Auto`), write it.

It happens in `DllMain`, before any of Azimuth's code runs: Azimuth prepares
its saves folder, and its launcher reads, backs up and restores saves, before
the game is mapped, which is when plugins start. Azimuth refuses a linked
saves folder, so nothing is linked: Azimuth's own KERNEL32 file imports (and
SHELL32's open-folder calls) are rewritten so every path in or below the
title's saves folder goes to Documents, and paths Windows hands back are
mapped back. The game-folder saves are not read or written while it is on.
Plugins ask `ForzaInjectorSavesFolder(title, buffer, capacity)` (exported by
`winmm.dll`) for the folder in use.

On the first start of an install with the redirect, every save (container)
that Documents has no complete revision of is copied from the game folder:
the newest two revisions whose files match their commit (SHA-256), staged in
`<title>.injector-staging` and renamed into place; the originals stay. A save
Documents already has is used as it is (Azimuth takes its newest complete
revision). `<title>.migrated.txt` notes the copied install, so a save deleted
in the game is not copied back later. A copy that fails leaves the saves in
the game folder for that session. Revisions beyond the newest two that
verify (a mirror's history) are moved, untouched, to `<title>.history` beside
the title's folder, before Azimuth would prune them. All of it is logged in
`forza-injector.log` (lines starting with `SAVES:`).

## Build and verification

```text
py build.py --zig <zig.exe> --test [--game <folder containing your game packages>]
```

Zig 0.15.2. Output: `out/core/winmm.dll` and
`out/release/forza-injector-<version>.zip`. `injector.json` lists the
accepted Azimuth executables (`azimuth`: version, file size, SHA-256 and,
when branded copies are accepted, `code_sha256`, read from the core's host
table) and advertises v1/v2, runtime API v1,
game-independent admission and known profiles.

Offline tests check the SDK, original game identities, both plugin ABIs,
ordering/disabling/filtering/dependencies, early/normal threading, provider
claim/decline/failure, and unchanged FH2, FF and unknown game images without
a provider. With the Azimuth 1.0.4 release zip (`--game` folder,
`azimuth-windows-1.0.4.zip`) they brand a copy (icon, version details,
manifest; larger, same-size and smaller `.rsrc`), check that the host accepts it, also
renamed, and refuses it after one changed `.text` byte. With the sibling Xtra Options build present, paired tests verify
the actual host/provider/individual-feature initialization on a mapped FH2
image. The saves tests (`tests/saves_tests.py`) run the redirect on fake save
trees in a temporary folder: first-start copies, a mirror's history, damaged
and locked revisions, links, path mapping both ways, and the patched import
table of a probe DLL standing for `azimuth.exe` (Azimuth's own storage code
is not run). These tests do not execute game code or establish in-game compatibility
for plugins that have not been ported.

## Thanks

To Arthur (RokkuDayo) and everyone who worked on
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions): forza_injector would not
exist without it. To the Azimuth authors, for running these games on PC.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
