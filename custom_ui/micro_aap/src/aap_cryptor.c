#include "aap_cryptor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static const char kCertificate[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDJTCCAg0CAnZTMA0GCSqGSIb3DQEBCwUAMFsxCzAJBgNVBAYTAlVTMRMwEQYD\n"
"VQQIDApDYWxpZm9ybmlhMRYwFAYDVQQHDA1Nb3VudGFpbiBWaWV3MR8wHQYDVQQK\n"
"DBZHb29nbGUgQXV0b21vdGl2ZSBMaW5rMB4XDTE0MDcwODIyNDkxOFoXDTQ0MDcw\n"
"NzIyNDkxOFowVTELMAkGA1UEBhMCVVMxCzAJBgNVBAgMAkNBMRYwFAYDVQQHDA1N\n"
"b3VudGFpbiBWaWV3MSEwHwYDVQQKDBhHb29nbGUtQW5kcm9pZC1SZWZlcmVuY2Uw\n"
"ggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCpqQmvoDW/XsREoj20dRcM\n"
"qJGWh8RlUoHB8CpBpsoqV4nAuvNngkyrdpCf1yg0fVAp2Ugj5eOtzbiN6BxoNHpP\n"
"giZ64pc+JRlwjmyHpssDaHzP+zHZM7acwMcroNVyynSzpiydEDyx/KPtEz5AsKi7\n"
"c7AYYEtnCmAnK/waN1RT5KdZ9f97D9NeF7Ljdk+IKFROJh7Nv/YGiv9GdPZh/ezS\n"
"m2qhD3gzdh9PYs2cu0u+N17PYpSYB7vXPcYa/gmIVipIJ5RuMQVBWrCgtfzwKPqb\n"
"nJQVykm8LnysK+8RCgmPLN3uhsZx6Whax2TVXb1q68DoiaFPhvMfPr2i/9IKaC69\n"
"AgMBAAEwDQYJKoZIhvcNAQELBQADggEBAIpfjQriEtbpUyWLoOOfJsjFN04+ajq9\n"
"1XALCPd+2ixWHZIBJiucrrf0H7OgY7eFnNbU0cRqiDZHI8BtvzFxNi/JgXqCmSHR\n"
"rlaoIsITfqo8KHwcAMs4qWTeLQmkTXBZYz0M3HwC7N1vOGjAJJN5qENIm1Jq+/3c\n"
"fxVg2zhHPKY8qtdgl73YIXb9Xx3WmPCBeRBCKJncj0Rq14uaOjWXRyBgbmdzMXJz\n"
"FGPHx3wN04JqGyfPFlDazXExFQwuAryjoYBRdxPxGufeQCp3am4xxI2oxNIzR+4L\n"
"nOcDhgU1B7sbkVzbKj5gjdOQAmxnKCfBtUNB63a7yzGPYGPIwlBsm54=\n"
"-----END CERTIFICATE-----\n";

static const char kPrivateKey[] =
"-----BEGIN PRIVATE KEY-----\n"
"MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCpqQmvoDW/XsRE\n"
"oj20dRcMqJGWh8RlUoHB8CpBpsoqV4nAuvNngkyrdpCf1yg0fVAp2Ugj5eOtzbiN\n"
"6BxoNHpPgiZ64pc+JRlwjmyHpssDaHzP+zHZM7acwMcroNVyynSzpiydEDyx/KPt\n"
"Ez5AsKi7c7AYYEtnCmAnK/waN1RT5KdZ9f97D9NeF7Ljdk+IKFROJh7Nv/YGiv9G\n"
"dPZh/ezSm2qhD3gzdh9PYs2cu0u+N17PYpSYB7vXPcYa/gmIVipIJ5RuMQVBWrCg\n"
"tfzwKPqbnJQVykm8LnysK+8RCgmPLN3uhsZx6Whax2TVXb1q68DoiaFPhvMfPr2i\n"
"/9IKaC69AgMBAAECggEAbBoW3963IG6jpA+0PW11+EzYJw/u5ZiCsS3z3s0Fd6E7\n"
"VqBIQyXU8FOlpxMSvQ8zqtaVjroGLlIsS88feo4leM+28Qm70I8W/I7jPDPcmxlS\n"
"nbqycnDu5EY5IeVi27eAUI+LUbBs3APb900Rl2p4uKfoBkAlC0yjI5J1GcczZhf7\n"
"RDh1wGgFWZI+ljiSrfpdiA4XmcZ9c7FlO5+NTotZzYeNx1iZprajV1/dlDy8UWEk\n"
"woWtppeGzUf3HHgl8yay62ub2vo5I1Z7Z98Roq8KC1o7k2IXOrHztCl3X03gMwlI\n"
"F4WQ6Fx5LZDU9dfaPhzkutekVgbtO9SzHgb3NXCZwQKBgQDcSS/OLll18ssjBwc7\n"
"PsdaIFIPlF428Tk8qezEnDmHS6xeztkGnpOlilk9jYSsVUbQmq8MwBSjfMVH95B0\n"
"w0yyfOYqjgTocg4lRCoPuBdnuBY/lU1Lws4FoGsGMNFkHWjHzl622mavkJiDzWA+\n"
"CORPUllS/DnPKJnZk2n0zZRKaQKBgQDFKqvePMx/a/ayQ09UZYxov0vwRyNkHevm\n"
"wEGQjOiHKozWvLqWhCvFtwo+VqHqmCw95cYUpg1GvppB6Lnw2uHgWAWxr3ugDjaR\n"
"YSqG/L7FG6FDF+1sPvBuxNpBmto59TI1fBFmU9VBGLDnr1M27qH3KTWlA3lCsovV\n"
"6Dbk7D+vNQKBgE6GgFYdS6KyFBu+a6OA84t7LgWDvDoVr3Oil1ZW4mMKZL2/OroT\n"
"WUqPkNRSWFMeawn9uhzvc+v7lE/dPk+BNxwBTgMpcTJzRfue2ueTljRQ+Q1daZpy\n"
"LQLwdnZUfLAVk752IGlKXYSEJPoHAiHbBZgJIPJmGy1vqbhXxlOP3SbRAoGBAJoA\n"
"Q2/5gy0/sdf5FRxxmOM0D+dkWTNY36pDnrJ+LR1uUcVkckUghWQQHRMl7aBkLaJH\n"
"N5lnPdV1CN3UHnAPNwBZIFFyJJiWoW6aO3JmNceVVjcmmE7FNlz+qw81GaDNcOMv\n"
"vhN0BYyr8Xl1iwTMDXwVFw6FkRBUjz6L+1yBXxjFAoGAJZcU+tEM1+gHPCqHK2bP\n"
"kfYOCyEAro4zY/VWXZKHgCoPau8Uc9+vFu2QVMb5kVyLTdyRLQKpooR6f8En6utS\n"
"/G15YuqRYqzSTrMBzpRrqIwbgKI9RHNPAvhtVAmXnwsYDPIQ1rrELK6WzTjUySRd\n"
"7gyCoq+DlY7ZKDa7FUz05Ek=\n"
"-----END PRIVATE KEY-----\n";

struct aap_cryptor {
    SSL_CTX *ctx;
    SSL *ssl;
    BIO *read_bio;   /* Input to SSL */
    BIO *write_bio;  /* Output from SSL */
    bool is_active;
};

aap_cryptor_t *aap_cryptor_create(void) {
    aap_cryptor_t *cryptor = (aap_cryptor_t *)calloc(1, sizeof(aap_cryptor_t));
    if (!cryptor) return NULL;

    const SSL_METHOD *method = TLS_client_method();
    cryptor->ctx = SSL_CTX_new(method);
    if (!cryptor->ctx) {
        free(cryptor);
        return NULL;
    }

    /* Load certificate */
    BIO *cert_bio = BIO_new_mem_buf(kCertificate, sizeof(kCertificate) - 1);
    X509 *cert = PEM_read_bio_X509_AUX(cert_bio, NULL, NULL, NULL);
    BIO_free(cert_bio);
    if (!cert) {
        SSL_CTX_free(cryptor->ctx);
        free(cryptor);
        return NULL;
    }
    SSL_CTX_use_certificate(cryptor->ctx, cert);
    X509_free(cert);

    /* Load private key */
    BIO *key_bio = BIO_new_mem_buf(kPrivateKey, sizeof(kPrivateKey) - 1);
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(key_bio);
    if (!pkey) {
        SSL_CTX_free(cryptor->ctx);
        free(cryptor);
        return NULL;
    }
    SSL_CTX_use_PrivateKey(cryptor->ctx, pkey);
    EVP_PKEY_free(pkey);

    cryptor->ssl = SSL_new(cryptor->ctx);
    if (!cryptor->ssl) {
        SSL_CTX_free(cryptor->ctx);
        free(cryptor);
        return NULL;
    }

    cryptor->read_bio = BIO_new(BIO_s_mem());
    cryptor->write_bio = BIO_new(BIO_s_mem());
    SSL_set_bio(cryptor->ssl, cryptor->read_bio, cryptor->write_bio);

    /* Set 20KB buffer size on BIOs */
    BIO_set_write_buf_size(cryptor->read_bio, 20480);
    BIO_set_write_buf_size(cryptor->write_bio, 20480);

    SSL_set_connect_state(cryptor->ssl);
    SSL_set_verify(cryptor->ssl, SSL_VERIFY_NONE, NULL);

    cryptor->is_active = false;
    return cryptor;
}

void aap_cryptor_destroy(aap_cryptor_t *cryptor) {
    if (!cryptor) return;
    if (cryptor->ssl) {
        SSL_free(cryptor->ssl); /* Automatically frees BIOs */
        cryptor->ssl = NULL;
    }
    if (cryptor->ctx) {
        SSL_CTX_free(cryptor->ctx);
        cryptor->ctx = NULL;
    }
    free(cryptor);
}

bool aap_cryptor_is_active(const aap_cryptor_t *cryptor) {
    return cryptor ? cryptor->is_active : false;
}

bool aap_cryptor_do_handshake(aap_cryptor_t *cryptor) {
    if (!cryptor || !cryptor->ssl) return false;

    int ret = SSL_do_handshake(cryptor->ssl);
    if (ret == 1) {
        cryptor->is_active = true;
        return true;
    }

    int err = SSL_get_error(cryptor->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return false;
    }

    fprintf(stderr, "aap_cryptor: SSL_do_handshake error %d\n", err);
    return false;
}

size_t aap_cryptor_read_handshake(aap_cryptor_t *cryptor, uint8_t *out_buf, size_t max_len) {
    if (!cryptor || !cryptor->write_bio || !out_buf) return 0;
    size_t pending = (size_t)BIO_ctrl_pending(cryptor->write_bio);
    if (pending == 0) return 0;

    size_t to_read = pending < max_len ? pending : max_len;
    int read_bytes = BIO_read(cryptor->write_bio, out_buf, (int)to_read);
    return read_bytes > 0 ? (size_t)read_bytes : 0;
}

size_t aap_cryptor_write_handshake(aap_cryptor_t *cryptor, const uint8_t *in_buf, size_t in_len) {
    if (!cryptor || !cryptor->read_bio || !in_buf || in_len == 0) return 0;
    int written = BIO_write(cryptor->read_bio, in_buf, (int)in_len);
    return written > 0 ? (size_t)written : 0;
}

size_t aap_cryptor_encrypt(aap_cryptor_t *cryptor, const uint8_t *plain, size_t plain_len,
                           uint8_t *cipher_buf, size_t max_cipher_len) {
    if (!cryptor || !cryptor->ssl || !plain || plain_len == 0 || !cipher_buf) return 0;

    size_t total_written = 0;
    while (total_written < plain_len) {
        int written = SSL_write(cryptor->ssl, plain + total_written, (int)(plain_len - total_written));
        if (written <= 0) {
            int err = SSL_get_error(cryptor->ssl, written);
            fprintf(stderr, "aap_cryptor: SSL_write error %d\n", err);
            return 0;
        }
        total_written += (size_t)written;
    }

    size_t pending = (size_t)BIO_ctrl_pending(cryptor->write_bio);
    if (pending > max_cipher_len) {
        fprintf(stderr, "aap_cryptor: cipher buffer overflow (pending %zu > max %zu)\n", pending, max_cipher_len);
        return 0;
    }

    int read_bytes = BIO_read(cryptor->write_bio, cipher_buf, (int)pending);
    return read_bytes > 0 ? (size_t)read_bytes : 0;
}

size_t aap_cryptor_decrypt(aap_cryptor_t *cryptor, const uint8_t *cipher, size_t cipher_len,
                           uint8_t *plain_buf, size_t max_plain_len) {
    if (!cryptor || !cryptor->ssl || !cipher || cipher_len == 0 || !plain_buf) return 0;

    int written = BIO_write(cryptor->read_bio, cipher, (int)cipher_len);
    if (written <= 0) {
        fprintf(stderr, "aap_cryptor: BIO_write failed\n");
        return 0;
    }

    size_t total_read = 0;
    bool first_read = true;
    int pending = 0;

    while (first_read || pending > 0) {
        first_read = false;
        size_t space_left = max_plain_len - total_read;
        if (space_left == 0) break;

        size_t req_bytes = pending > 0 ? (size_t)(pending < (int)space_left ? pending : (int)space_left) : space_left;
        if (req_bytes > 2048) req_bytes = 2048;

        int read_bytes = SSL_read(cryptor->ssl, plain_buf + total_read, (int)req_bytes);
        if (read_bytes <= 0) {
            int err = SSL_get_error(cryptor->ssl, read_bytes);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                break;
            }
            fprintf(stderr, "aap_cryptor: SSL_read error %d\n", err);
            return 0;
        }

        total_read += (size_t)read_bytes;
        pending = SSL_pending(cryptor->ssl);
    }

    return total_read;
}
