# Injector host

This directory contains the game-independent Azimuth plugin host. Game-specific
Extra Options implementations have moved to
`forza-plugins/plugins/xtra-options/runtime/src`.

The host accepts a valid mapped x64 PE from an inspected Azimuth build without
checking its title, executable name, timestamp, size or FH2 file hash against a
game whitelist. Host fingerprints and map-call bytes are still checked before
modifying Azimuth.

`plugins.c` discovers mods and probes optional runtime providers first.
A claiming provider owns native game hooks and deferred plugin initialization.
Otherwise the host runs the early and normal v2/v1 plugin phases itself.
There are no game patches or gameplay settings in this build.
