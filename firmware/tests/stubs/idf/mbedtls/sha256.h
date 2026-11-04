#pragma once
#include <stdint.h>
#include <stddef.h>
// SHA-256 поверх CommonCrypto этой машины — НАСТОЯЩИЙ, а не пустышка. Пустышка была бы хуже
// отсутствия: путь установки обновления сверяет хеш образа, и проверка, которая всегда сходится,
// молча одобрила бы подменённый образ в тесте, который якобы это и проверяет.
#define COMMON_DIGEST_FOR_OPENSSL
#include <CommonCrypto/CommonDigest.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { CC_SHA256_CTX ctx; } mbedtls_sha256_context;
static inline void mbedtls_sha256_init(mbedtls_sha256_context *c){ (void)c; }
static inline void mbedtls_sha256_free(mbedtls_sha256_context *c){ (void)c; }
static inline int  mbedtls_sha256_starts(mbedtls_sha256_context *c, int is224){ (void)is224; return CC_SHA256_Init(&c->ctx) == 1 ? 0 : -1; }
static inline int  mbedtls_sha256_update(mbedtls_sha256_context *c, const unsigned char *d, size_t n){ return CC_SHA256_Update(&c->ctx, d, (CC_LONG)n) == 1 ? 0 : -1; }
static inline int  mbedtls_sha256_finish(mbedtls_sha256_context *c, unsigned char out[32]){ return CC_SHA256_Final(out, &c->ctx) == 1 ? 0 : -1; }
static inline int  mbedtls_sha256(const unsigned char *in, size_t n, unsigned char out[32], int is224){
    (void)is224; CC_SHA256(in, (CC_LONG)n, out); return 0; }

#ifdef __cplusplus
}
#endif
