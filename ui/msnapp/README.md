# msnapp/

Tracks every distinct `MsnCoreApp` binary found across this project's
firmware dumps, one subfolder per variant, so future sessions can compare
versions instead of re-extracting from each dump's `rootfs.img` (UBI
image — needs `ubireader_extract_files` to pull a single file back out).

## Why this exists

2026-07-17: `bootusb` (this fork's own kernel + `firmware_source/
prado_reconstructed` rootfs) crashed on boot — `MsnCoreApp` segfaulted
inside `MCUAdapter_BoxP300::onInited()` → `ProtocolUtils::writeDatas()`
(NULL pointer, fault address `0xc`), then got respawned and crashed
again in a tight loop. Comparing against `docs/logs/boot log.txt` (a
captured real stock boot, confirmed *not* crashing) found the
`MsnCoreApp` binary shipped in `firmware_source/prado_reconstructed`
wasn't just misconfigured — it's a completely different file: different
MD5, reports `AppVer:V3.7.2.1203` in its own crash dump vs. the working
reference's `V3.21.09.0219`, and is 14x smaller (348KB vs 4.86MB, far
more than stripping alone explains). `git log` on that file shows only a
generic "restructure root directory" commit touching it — it looks like
the wrong binary got carried into the reconstructed rootfs at some point
rather than a deliberate substitution.

## Variants

| Folder | Source | Size | MD5 | Confirmed status |
|---|---|---|---|---|
| `prado_dump_V3.21.09.0219/` | `firmware_dumps/Prado firmware dump/mtd6_rootfs/usr/bin/MsnCoreApp` (already extracted, not a UBI image) | 4,855,912 | `0aa7ab650d1f5afe97113c02e2c3852e` | **Confirmed working** — matches `docs/logs/boot log.txt`'s `MsnCoreApp version: V3.21.09.0219`, that capture boots clean with no crash anywhere in the log. Used to replace the broken copy in `firmware_source/prado_reconstructed`. |
| `holden/` | `firmware_dumps/Holden firmware update/rootfs.img` (UBI, extracted via `ubireader_extract_files`) | 4,881,480 | `c96ed1af2c99094063322a64ea4c39df` | Not boot-tested from this project. Byte-identical to `Prado firmware recovery holden based/rootfs.img`'s copy (same MD5) — that dump's name literally says "holden based", consistent. |
| `cstech/` | `firmware_dumps/CarSyncTech Toyota/CSTech-202511-IP17/rootfs.img` (UBI) | 4,886,320 | `79d3cd3f2f97edda7fae60ddf79caf77` | Not boot-tested from this project. Different vendor build (CarSyncTech), similar size to the Prado/Holden variants but distinct MD5. |
| `p306/` | `firmware_dumps/P306 2025 Firmware Update/rootfs.img` (UBI) | 663,860 | `0a0eb603677bb38e61c9ca7b09bfbbfa` | Not boot-tested. Notably small (~15% of the Prado/Holden/CSTech size) — worth checking before ever using this one; could be a genuinely lighter build or could have the same kind of problem the broken reconstructed copy had. |
| `broken_reconstructed_V3.7.2.1203/` | `firmware_source/prado_reconstructed/mtd6_rootfs/rootfs/usr/bin/MsnCoreApp`, as found before the 2026-07-17 fix | 347,964 | `fdd14327fd16ebce9ba73e1b284d4ceb` | **Confirmed crashing** — the NULL-pointer segfault described above. Kept here only as a reference for what NOT to ship; replaced in the rootfs with `prado_dump_V3.21.09.0219/`. |

`Prado firmware recovery holden based/rootfs.img` isn't given its own
folder since it's byte-identical to `holden/` — see the MD5 column.

## Extracting from a UBI `rootfs.img`

```sh
ubireader_extract_files -o /tmp/extract "firmware_dumps/<dump>/rootfs.img"
# binary lands at /tmp/extract/<seq>/rootfs/usr/bin/MsnCoreApp
```

## Regenerating this comparison

```sh
md5sum msnapp/*/MsnCoreApp
strings msnapp/*/MsnCoreApp | grep -B2 "MsnCoreApp version:"  # version string is assembled at runtime, not a static literal -- easier to read AppVer from an actual crash dump/boot log instead
```
