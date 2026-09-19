#include "rj_security.h"
#include "lwip/mem.h"
#include "main.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/x509_csr.h"
#include "rj_json.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static mbedtls_pk_context key;
static mbedtls_x509_crt certificate;
static char username[65], fingerprint[65], key_fingerprint[65], subject[192], boot[40];
/* Retain the staged verifier even before a login consumer is installed. */
static volatile unsigned char salt[16], password_hash[32];
static unsigned credential_revision, key_revision;
static int rng_ready, key_ready, certificate_ready;
static void *crypto_calloc(size_t n, size_t size) {
    if (size && n > 32768 / size)
        return NULL;
    return mem_calloc(n, size);
}
int rj_authorize(const char *method, const char *path) {
    (void)method;
    (void)path;
    return RJ_DEVELOPMENT_OPEN;
}
int rj_random(void *unused, unsigned char *out, size_t size) {
    (void)unused;
    if (!rng_ready)
        return -1;
    while (size) {
        uint32_t start = HAL_GetTick();
        while (!(RNG->SR & RNG_SR_DRDY)) {
            if ((RNG->SR & (RNG_SR_SECS | RNG_SR_CECS)) || (uint32_t)(HAL_GetTick() - start) > 20)
                return -1;
        }
        if (RNG->SR & (RNG_SR_SECS | RNG_SR_CECS))
            return -1;
        uint32_t v = RNG->DR;
        for (int i = 0; i < 4 && size; i++, size--) {
            *out++ = (unsigned char)v;
            v >>= 8;
        }
    }
    return 0;
}
static void digest_hex(const unsigned char *buf, size_t n, char out[65]) {
    unsigned char h[32];
    mbedtls_sha256(buf, n, h, 0);
    for (unsigned i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", h[i]);
    rj_zero(h, sizeof(h));
}
void rj_security_init(void) {
    mbedtls_platform_set_calloc_free(crypto_calloc, mem_free);
    mbedtls_pk_init(&key);
    mbedtls_x509_crt_init(&certificate);
    /* HSI48 is a documented independent RNG clock; do not alter PLL1/PLL2/PHY. */
    RCC->CR |= RCC_CR_HSI48ON;
    uint32_t start = HAL_GetTick();
    while (!(RCC->CR & RCC_CR_HSI48RDY) && (uint32_t)(HAL_GetTick() - start) < 20) {
    }
    if (RCC->CR & RCC_CR_HSI48RDY) {
        __HAL_RCC_RNG_CONFIG(RCC_RNGCLKSOURCE_HSI48);
        __HAL_RCC_RNG_CLK_ENABLE();
        RNG->CR = RNG_CR_RNGEN;
        rng_ready = 1;
    }
    unsigned char bytes[16];
    if (rj_random(NULL, bytes, sizeof(bytes)) == 0) {
        for (unsigned i = 0; i < 16; i++)
            snprintf(boot + 2 * i, 3, "%02x", bytes[i]);
    } else {
        rng_ready = 0;
        strcpy(boot, "rng-unavailable");
    }
    rj_zero(bytes, sizeof(bytes));
}
void rj_boot_id(char out[40]) { strcpy(out, boot); }
int rj_credentials(const char *name, const char *password) {
    unsigned char ns[16], hash[32];
    if (rj_random(NULL, ns, sizeof(ns)))
        return -1;
    int rc =
        mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password,
                                      strlen(password), ns, sizeof(ns), 10000, sizeof(hash), hash);
    if (!rc) {
        for (unsigned i = 0; i < sizeof(salt); i++) salt[i] = ns[i];
        for (unsigned i = 0; i < sizeof(hash); i++) password_hash[i] = hash[i];
        snprintf(username, sizeof(username), "%s", name);
        credential_revision++;
    }
    rj_zero(ns, sizeof(ns));
    rj_zero(hash, sizeof(hash));
    return rc;
}
int rj_credentials_status(char *out, size_t cap) {
    char quoted[160];
    rj_quote(quoted, sizeof(quoted), username);
    return snprintf(out, cap,
                    "{\"username\":%s,\"credential_revision\":%u,\"configured\":%s,"
                    "\"authentication_enforced\":false,\"storage\":\"volatile_ram\",\"password_"
                    "kdf\":\"PBKDF2-HMAC-SHA256\",\"iterations\":10000}",
                    quoted, credential_revision, credential_revision ? "true" : "false");
}
int rj_tls_csr(const char *cn, char *out, size_t cap) {
    mbedtls_pk_context next;
    mbedtls_pk_init(&next);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    char name[160];
    snprintf(name, sizeof(name), "CN=%s", cn);
    int rc = mbedtls_pk_setup(&next, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!rc)
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(next), rj_random, NULL);
    if (!rc)
        rc = mbedtls_x509write_csr_set_subject_name(&csr, name);
    if (!rc) {
        mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
        mbedtls_x509write_csr_set_key(&csr, &next);
        rc = mbedtls_x509write_csr_pem(&csr, (unsigned char *)out, cap, rj_random, NULL);
    }
    if (!rc) {
        unsigned char der[160];
        int n = mbedtls_pk_write_pubkey_der(&next, der, sizeof(der));
        if (n > 0)
            digest_hex(der + sizeof(der) - n, (size_t)n, key_fingerprint);
        else
            rc = n;
    }
    if (!rc) {
        mbedtls_pk_free(&key);
        key = next;
        mbedtls_pk_init(&next);
        key_ready = 1;
        key_revision++;
        mbedtls_x509_crt_free(&certificate);
        mbedtls_x509_crt_init(&certificate);
        certificate_ready = 0;
        fingerprint[0] = 0;
        snprintf(subject, sizeof(subject), "%s", cn);
    }
    mbedtls_pk_free(&next);
    mbedtls_x509write_csr_free(&csr);
    return rc;
}
int rj_tls_certificate(const char *pem, const char *chain) {
    if (!key_ready)
        return -2;
    mbedtls_x509_crt next;
    mbedtls_x509_crt_init(&next);
    int rc = mbedtls_x509_crt_parse(&next, (const unsigned char *)pem, strlen(pem) + 1);
    if (!rc && next.next)
        rc = -3; /* leaf must be exactly one certificate */
    if (!rc)
        rc = mbedtls_pk_check_pair(&next.pk, &key, rj_random, NULL);
    if (!rc && chain && *chain)
        rc = mbedtls_x509_crt_parse(&next, (const unsigned char *)chain, strlen(chain) + 1);
    /* Parse/key-match staging is not trust-chain or validity approval. */
    if (!rc) {
        mbedtls_x509_crt_free(&certificate);
        certificate = next;
        memset(&next, 0, sizeof(next));
        digest_hex(certificate.raw.p, certificate.raw.len, fingerprint);
        certificate_ready = 1;
    }
    mbedtls_x509_crt_free(&next);
    return rc;
}
int rj_tls_status(char *out, size_t cap) {
    char q[400];
    rj_quote(q, sizeof(q), subject);
    return snprintf(
        out, cap,
        "{\"ready\":false,\"https_listener\":false,\"key_present\":%s,\"certificate_present\":%s,"
        "\"key_origin\":\"%s\",\"key_revision\":%u,\"csr_common_name\":%s,\"sha256_fingerprint\":\"%s\","
        "\"public_key_sha256\":\"%s\",\"key_match_verified\":%s,\"chain_trusted\":false,\"validity_"
        "verified\":false,\"storage\":\"volatile_ram\",\"rng_ready\":%s,\"scope\":\"development_"
        "provisioning_only\"}",
        key_ready ? "true" : "false", certificate_ready ? "true" : "false",
        key_ready ? "generated_on_device" : "unknown", key_revision, q, fingerprint,
        key_fingerprint, certificate_ready ? "true" : "false", rng_ready ? "true" : "false");
}
