#include "tea_crypto.h"

/* Real key, recovered via the real firmware's .data init table -- see the
 * full derivation trail in tea_crypto.h. Not a guess. */
const uint32_t tea_real_key[4] = { 0x0000006D, 0x0000007C, 0x000000A9, 0x000000C4 };

#define TEA_DELTA     0x9E3779B9UL
#define TEA_SUM_INIT  0xC6EF3720UL /* == 32 * TEA_DELTA mod 2^32, confirmed from the
                                     * real firmware's own literal at its cipher's
                                     * sum-init load (0x080050AA), not assumed */

/* Structure matches the real firmware's decompiled decrypt loop
 * (0x080050C6-0x080050F8) instruction-for-instruction, confirmed via
 * disassembly of hardware/MCU/can_app.bin -- not a generic textbook TEA
 * pulled from memory. Key index usage (v1 uses key[2]/key[3], v0 uses
 * key[0]/key[1]) matches the real firmware's own key[] indexing exactly. */
void tea_decrypt_block(uint32_t *v0, uint32_t *v1, const uint32_t key[4]) {
    uint32_t y = *v0;
    uint32_t z = *v1;
    uint32_t sum = TEA_SUM_INIT;

    for (uint8_t i = 0; i < 32; i++) {
        z -= ((y << 4) + key[2]) ^ (y + sum) ^ ((y >> 5) + key[3]);
        y -= ((z << 4) + key[0]) ^ (z + sum) ^ ((z >> 5) + key[1]);
        sum -= TEA_DELTA;
    }

    *v0 = y;
    *v1 = z;
}
