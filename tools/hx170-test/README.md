# hx170-test

Standalone, direct test of the Hantro `hx170dec` hardware H.264 decoder
via `libmfc.so`'s real `H264DecInit`/`H264DecDecode`/`H264DecNextPicture`
API — completely bypasses `sink`, Android Auto, and the network.

## Why this exists

Built to answer a question the rest of the AA investigation couldn't:
is the decoder hardware/driver/`libmfc.so` stack itself broken, or is
the real problem upstream of it (no video frame data ever reaching
`sink` in the first place)? See
`docs/DEVICE_TEST_CHECKLIST_2026-07-18.md`'s Android Auto black-screen
sections and `strace` findings (`docs/logs/sink_strace.log`) —
`sink`'s video thread was confirmed to open `hx170dec`, run
`H264DecInit`-adjacent setup, then just sit idle for ~2m23s before
exiting cleanly, with no `SHOW_WINDOW` ever called and no evidence any
real frame data ever arrived to decode.

## Usage

```sh
hx170-test                    # uses /usr/lib/libmfc.so by default
hx170-test /path/to/libmfc.so # override the library path
```

Prints the return code of each API call (`H264DecInit`, `H264DecDecode`,
`H264DecNextPicture`) and a summary.

## How to read the result

- **`H264DecInit` fails, or anything hangs/crashes instead of
  returning**: real evidence of a hardware/driver problem, independent
  of the test stream's validity.
- **`H264DecInit` succeeds (0) but `H264DecDecode`/`H264DecNextPicture`
  return an error**: most likely this tool's hand-built test stream,
  not the hardware — this still proves decoder hardware bring-up
  itself works (register mapping, ASIC ID check, DMA context setup),
  which is the main open question.

## Known limitation — read before trusting a "decode failed" result

The embedded test H.264 stream (`g_test_stream` in `hx170-test.c`) is
a minimal baseline-profile SPS+PPS+IDR sequence, reconstructed
carefully from H.264 bitstream syntax rather than copied from a
verified reference file. It has **not** been confirmed byte-correct
against a real decoder. If `H264DecDecode`/`H264DecNextPicture` report
an error, don't treat that alone as proof the hardware is broken —
check whether the failure looks like a bitstream-parsing problem
specifically (inconclusive) versus a device/ioctl/open failure
(unambiguous). If practical, feeding it a real, known-good short H.264
clip's raw NAL stream instead of the built-in test pattern would give
a fully conclusive answer either way.

## DMA input buffer

Rather than replicating `libmfc.so`'s own internal `DWLInit()`/
`DWLMallocLinear()` (which needs an opaque DWL context this tool has
no independent way to construct correctly — `H264DecInit()` sets that
up internally, for the decoder's own hardware register context, and
doesn't expose it to the caller), this tool opens `/tmp/dev/memalloc`
directly and replicates `DWLMallocLinear`'s own ioctl sequence by hand
(confirmed via Ghidra decompile of the real deployed `libmfc.so`:
`ioctl(fd, 0xc0046b01, &busAddr)` to allocate, then `mmap()` using that
bus address as the file offset). `/tmp/dev/memalloc` itself is a real,
already hardware-confirmed-working device node (see
`docs/DEVICE_TEST_CHECKLIST_2026-07-18.md`, 2026-07-20 fix) — this
tool only needs it for staging the input bitstream, not for the
decoder's own internal hardware context.
