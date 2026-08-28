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
 * NOT YET RESOLVED: the real 128-bit key. The real firmware reads it from
 * SRAM (0x20000098), not a flash literal directly visible at the cipher
 * call site -- almost certainly populated from the .data section at startup
 * (the standard flash->RAM data-copy every C startup does), but the exact
 * flash source bytes were NOT located in this pass. tea_key_placeholder[]
 * below is explicitly NOT the real key -- it is zeroed and clearly marked,
 * so nobody mistakes a guess for a verified value. Do not fill this in
 * without independently verifying it against a real captured challenge/
 * response pair from live hardware (or locating the real .data copy table
 * in can_app.bin) -- a wrong key that "looks like" it works is worse than
 * leaving CMD 0x88 unimplemented, since it would silently fail the real
 * handshake while looking complete. */

extern const uint32_t tea_key_placeholder[4]; /* NOT the real key -- see above */

/* Decrypts an 8-byte (2x uint32) TEA block in place, using the given
 * 128-bit key. Algorithm structure is verified real; correctness for an
 * actual challenge/response depends entirely on using the real key. */
void tea_decrypt_block(uint32_t *v0, uint32_t *v1, const uint32_t key[4]);

#endif /* TEA_CRYPTO_H */
