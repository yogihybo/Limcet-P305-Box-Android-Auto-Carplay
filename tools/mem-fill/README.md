# mem-fill

Write-side companion to `tools/mem-dump` — fills a range of physical
memory with a repeating 32-bit pattern via `mmap()`'d `/dev/mem`.
Same rationale as `mem-dump`: plain `write()`/`lseek()` into
`/dev/mem` only works for normal linear-mapped System RAM, not
reserved/no-map DMA carve-out regions (e.g. the LCDC framebuffer);
`mmap()` works for both.

```
mem-fill <phys_addr> <length> <pattern32>
```

## Why this exists

Manually staging a test frame into scratch memory and pointing a
hardware layer's `*_ADDR` register at it (via `devmem`) lets you
visually verify a display-layer fix without a real decoder session
feeding frames — useful when the actual pipeline (e.g. Android Auto's
`sink`) never reaches the point of calling `SHOW_WINDOW`/`SET_VIDEO_ADDR`
on its own (session drops, ping timeout, etc.) but you want to confirm
the *display* side of a fix independently.

## Manually forcing `VIDEO_LAYER2` on, to test the OSD1 colorkey fix

Context: `ARKFB_INIT_VIDEO_DISPLAY` already configures position/size/
blend for `VIDEO_LAYER2` correctly (confirmed via `dmesg`'s
`layer=4: init display x=0, y=0, w=800, h=480.` line) — the two calls
that never fire when the AA session drops early are `ARKFB_SHOW_WINDOW`
(sets `CONTROL` bit 6) and `ARKFB_SET_VIDEO_ADDR_RAW` (sets
`VIDEO2_ADDR1`). This replicates both by hand.

1. Fill ~1.5MB of scratch space with a solid, unmistakable color. Use
   an offset well past `/dev/fb0`'s own triple-buffered area (`800 *
   480 * 4 * 3` ≈ 4.5MB) within the LCDC's 16MB DMA carve-out (base
   confirmed via `dmesg`'s `16384KiB frame buffer at 0f000000` line)
   so this doesn't collide with OSD1's live content:
   ```sh
   mem-fill 0x0f800000 0x177000 0x00ff0000   # opaque red, RGB888 byte order
   ```
2. Point `VIDEO2_ADDR1` at that scratch buffer:
   ```sh
   devmem 0xe0500338 32 0x0f800000
   ```
3. Force `VIDEO2_CTL` to plain RGB888 (in case `INIT_VIDEO_DISPLAY`'s
   own format state is stale/unset — see
   `docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md`'s AA black-screen entries
   for the full field derivation):
   ```sh
   devmem 0xe0500320 32 0x400127
   ```
4. Enable `VIDEO_LAYER2` — read-modify-write `CONTROL`'s bit 6 (don't
   clobber OSD1's bit 7, which should stay on):
   ```sh
   devmem 0xe0500004 32 0x03600081   # OR in bit 6: 0x03600081 | 0x40 = 0x036000c1
   devmem 0xe0500004 32 0x036000c1
   ```
   (Use whatever `CONTROL`'s actual current value is instead of
   `0x03600081` if it's changed since — read it first with
   `devmem 0xe0500004 32`.)

**Expected result if the colorkey fix works**: a solid red rectangle
should appear exactly where `CarAutoWindow`'s black "video hole" is,
with the rest of OSD1's UI (if any) still visible around it. If
nothing changes, or the whole screen goes red (OSD1 not correctly
punching a hole), that's real diagnostic signal — see
`docs/historical/DEVICE_TEST_CHECKLIST_2026-07-18.md` §70 and
`project_aa_video_black_screen` (memory) for where to go next either
way.

To undo: `devmem 0xe0500004 32 0x03600081` (or whatever `CONTROL`
read as before step 4) turns `VIDEO_LAYER2` back off.
