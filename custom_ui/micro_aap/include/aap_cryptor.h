#ifndef AAP_CRYPTOR_H
#define AAP_CRYPTOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aap_cryptor aap_cryptor_t;

/* Initialize OpenSSL TLS 1.2 context, certificate, and memory BIOs */
aap_cryptor_t *aap_cryptor_create(void);
void aap_cryptor_destroy(aap_cryptor_t *cryptor);

bool aap_cryptor_is_active(const aap_cryptor_t *cryptor);

/* Perform TLS handshake step. Returns true if handshake is complete, false if needs more I/O */
bool aap_cryptor_do_handshake(aap_cryptor_t *cryptor);

/* Read outgoing TLS handshake bytes from write BIO into buffer */
size_t aap_cryptor_read_handshake(aap_cryptor_t *cryptor, uint8_t *out_buf, size_t max_len);

/* Write incoming TLS handshake bytes into read BIO */
size_t aap_cryptor_write_handshake(aap_cryptor_t *cryptor, const uint8_t *in_buf, size_t in_len);

/* Encrypt plaintext buffer. Returns ciphertext bytes written to cipher_buf, or 0 on error */
size_t aap_cryptor_encrypt(aap_cryptor_t *cryptor, const uint8_t *plain, size_t plain_len,
                           uint8_t *cipher_buf, size_t max_cipher_len);

/* Decrypt ciphertext buffer. Returns decrypted plaintext bytes written to plain_buf, or 0 on error */
size_t aap_cryptor_decrypt(aap_cryptor_t *cryptor, const uint8_t *cipher, size_t cipher_len,
                           uint8_t *plain_buf, size_t max_plain_len);

#ifdef __cplusplus
}
#endif

#endif /* AAP_CRYPTOR_H */
