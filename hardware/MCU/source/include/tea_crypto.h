#ifndef TEA_CRYPTO_H
#define TEA_CRYPTO_H

#include "stm32f105.h"

/* Real, disassembly-verified 32-round TEA (Tiny Encryption Algorithm) decrypt,
 * matching the real firmware's CMD 0x88 handshake routine at 0x080050A0.
 * Confirmed via direct disassembly of hardware/MCU/can_app.bin this session:
 *   - DELTA = 0x9E3779B9 (standard TEA constant)
 *   - sum_init = 0xC6EF3720 (== 32 * DELTA mod 2^32, the correct initial sum
 *     for a 32-round TEA *decrypt* -- confirmed by the real firmware's own
 *     literal, not assumed from a textbook)
 *   - 32 rounds, real v0/v1/sum decrement pattern (sub, not add -- decrypt
 *     direction, not encrypt)
 *
 * REAL KEY RECOVERED. The cipher reads its key from SRAM 0x20000098 as 4
 * sequential 32-bit words. That address is NOT a flash literal at the
 * cipher call site -- it's populated from the real firmware's .data init
 * table at boot. Traced the whole chain end to end, not guessed:
 *
 *   1. Reset vector's second jump target (0x08004198) is exactly the
 *      function that calls the data/bss init-table walker (0x080041A0) as
 *      its first instruction -- confirms this whole chain is genuinely on
 *      the real boot path, not unreached code.
 *   2. The walker reads a 2-entry table (embedded right after itself,
 *      PC-relative offsets from 0x080041CC): entry 1 = {src=0x0800BC14,
 *      dst=0x20000000, size=0xE8, copy_fn}, entry 2 = {src=0x0800BCFC,
 *      dst=0x200000E8, size=0x2CD8, zero_fn} -- the standard .data-copy +
 *      .bss-clear pair, with .data occupying RAM 0x20000000-0x200000E8.
 *   3. 0x20000098 falls inside that .data range at offset 0x98, so its
 *      real flash source is 0x0800BC14 + 0x98 = 0x0800BCAC. Read directly:
 *      key[0]=0x0000006D, key[1]=0x0000007C, key[2]=0x000000A9,
 *      key[3]=0x000000C4 (all surrounding .data bytes are zero, confirming
 *      this isn't an alignment artifact).
 *
 * Worth flagging plainly: each word has only its low byte set. The real
 * effective key entropy is 32 bits, not the full 128 bits the field width
 * implies -- a genuinely weak scheme, not a derivation error (the
 * surrounding zeroed .data bytes rule out an off-by-a-word misread). */

extern const uint32_t tea_real_key[4]; /* Real key, see derivation above */

/* Decrypts an 8-byte (2x uint32) TEA block in place, using the given
 * 128-bit key. */
void tea_decrypt_block(uint32_t *v0, uint32_t *v1, const uint32_t key[4]);

#endif /* TEA_CRYPTO_H */
