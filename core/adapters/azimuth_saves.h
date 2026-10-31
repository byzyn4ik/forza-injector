// Saves location: Azimuth's saves in Documents\My Games\<game>\Saves\<title>.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// forza-injector.ini beside winmm.dll:
//   [Saves]
//   Location=Documents   ; Game (default): Azimuth's own data\titles\<title>\saves
//
// With Location=Documents, saves_location_attach (DllMain, before any of
// Azimuth's own code runs) prepares the Documents folder of every known
// title this install has (first start: copies the game-folder saves it does
// not have yet, verified, keeping the originals; archives revisions older than
// the newest two that verify, so Azimuth's pruning does not delete a mirror's
// history), then rewrites Azimuth's KERNEL32 file imports so every path in or
// under <exe folder>\data\titles\<title>\saves goes to the Documents folder.
// Paths Windows hands back (GetFinalPathNameByHandleW, file name info) are
// mapped back, so Azimuth keeps seeing the folder it asked for. Nothing is
// linked (Azimuth refuses links) and the game-folder saves are never written.
#ifndef FORZA_AZIMUTH_SAVES_H
#define FORZA_AZIMUTH_SAVES_H
#include "host.h"

// DllMain (DLL_PROCESS_ATTACH): reads the setting and, when on, prepares and redirects.
void saves_location_attach(HINSTANCE module);
// forza-injector.log was (re)started: write the lines kept since DllMain again.
void saves_location_log_ready(void);
#endif
