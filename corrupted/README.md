# Corrupted / unsafe files — do not flash

Files here are **known corrupted** and must not be flashed or booted. Kept for
reference only, not for use.

| File | Problem |
|------|---------|
| `uboot_sdboot.bin` | Built by running a buggy `patch_uboot.py` directly against Holden's stock `uboot.bin`. The patch's blind 4096-byte env-block clear wiped a real command table (`set_default_env`, `env_import`, `saveenv` strings + function-pointer data) that sat right after the tiny original env. |
| `uboot_final.bin` | Produced from the corrupted `uboot_sdboot.bin` above via `--patch-nand-offset`. Inherits the same corruption. |

Full investigation, evidence, and what to do instead:
[`docs/UBOOT_SDBOOT_INVESTIGATION.md`](../docs/UBOOT_SDBOOT_INVESTIGATION.md).

The `patch_uboot.py` bug itself is fixed (it now refuses to write rather than
silently corrupting), but these two specific files were already produced by
the old buggy version before the fix landed, so the fix doesn't retroactively
repair them.
