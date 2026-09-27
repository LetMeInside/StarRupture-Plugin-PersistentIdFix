# PersistentIdFix

Persistent ID exhaustion prevention plugin for StarRupture.

## What it does

StarRupture uses persistent IDs for entities stored in saves. The available ID space is finite, and long-running worlds with large numbers of created entities can eventually approach the `uint32` limit.

PersistentIdFix reuses persistent IDs that are no longer present in the loaded save instead of allowing the persistent ID counter to continually increase.

Existing persistent IDs are preserved.

IDs freed during the current session are not immediately recycled.

## Tested

PersistentIdFix has been tested with:

- StarRupture client
- StarRupture dedicated server
- Small saves
- A very large save with a maximum persistent ID of `4,294,652,781`
- Save, reload, and continued play after ID reuse
- Dedicated server restart and world reload
- Continued play on a large save
- No measurable loading-time penalty observed on the tested large save

## Important

PersistentIdFix is intended to prevent persistent ID exhaustion.

It is **not** a general save-repair tool and does not claim to recover every case of missing or disappearing entities.

Always keep backups of important saves.

## Installation

Install the appropriate PersistentIdFix plugin DLL for your StarRupture installation using AlienX Mod Loader.

[Installation instructions and release files will be provided with the first release.]

## Compatibility

PersistentIdFix is built for the AlienX Mod Loader plugin system.

See the GitHub Releases page for available builds.

## License

PersistentIdFix is released under the MIT License. See [LICENSE](LICENSE).