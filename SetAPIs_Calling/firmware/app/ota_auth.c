/* Per-board, provisioned HMAC key. No plaintext password, shared fleet secret or
 * unauthenticated write route. Nonces fail closed when the hardware RNG fails. */
#include "ota_auth.h"
#include "boot_format.h"
#include "main.h"
#include "rj_security.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdio.h>
static uint8_t nonce[32];
static uint32_t sequence, accepted_sequence;
static int ready, accepted;
static const G100Provision *provision(void) {
    const G100Provision *p=(const void *)(G100_NOR_BASE+G100_PROVISION_OFFSET);
    if(p->magic!=G100_PROVISION_MAGIC || p->format!=1 ||
       p->uid[0]!=HAL_GetUIDw0() || p->uid[1]!=HAL_GetUIDw1() || p->uid[2]!=HAL_GetUIDw2() ||
       p->reserved[0] || p->reserved[1] || p->crc!=g100_crc32(p,60)) return NULL;
    return p;
}
static void hex(const uint8_t *src,char *dst) {
    for(unsigned i=0;i<32;++i) (void)snprintf(dst+2*i,3,"%02x",src[i]);
}
static int mac(uint32_t seq,const char *path,const uint8_t *data,size_t bytes,uint8_t digest[32]) {
    const G100Provision *p=provision();
    if(!p) return 0;
    uint8_t le[4]={(uint8_t)seq,(uint8_t)(seq>>8),(uint8_t)(seq>>16),(uint8_t)(seq>>24)};
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc=mbedtls_md_setup(&ctx,mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),1);
    if(!rc) rc=mbedtls_md_hmac_starts(&ctx,p->key,32);
    if(!rc) rc=mbedtls_md_hmac_update(&ctx,nonce,32);
    if(!rc) rc=mbedtls_md_hmac_update(&ctx,le,4);
    if(!rc) rc=mbedtls_md_hmac_update(&ctx,(const uint8_t *)path,strlen(path)+1);
    if(!rc) rc=mbedtls_md_hmac_update(&ctx,data,bytes);
    if(!rc) rc=mbedtls_md_hmac_finish(&ctx,digest);
    mbedtls_md_free(&ctx);
    return !rc;
}
int g100_auth_challenge(char out[65],uint32_t *seq) {
    if(!provision()) return 0;
    if(!ready) {
        if(rj_random(NULL,nonce,sizeof(nonce))) return 0;
        sequence=1; ready=1;
    }
    hex(nonce,out); *seq=sequence;
    return sequence!=UINT32_MAX;
}
int g100_auth_request(const char *path,const uint8_t *body,size_t bytes) {
    accepted=0;
    if(!ready || bytes<G100_AUTH_BYTES || sequence==UINT32_MAX) return 0;
    uint32_t seq=(uint32_t)body[0]|((uint32_t)body[1]<<8)|((uint32_t)body[2]<<16)|((uint32_t)body[3]<<24);
    uint8_t digest[32];
    if(seq!=sequence || !mac(seq,path,body+G100_AUTH_BYTES,bytes-G100_AUTH_BYTES,digest)) return 0;
    unsigned diff=0;
    for(unsigned i=0;i<32;++i) diff|=digest[i]^body[4+i];
    memset(digest,0,sizeof(digest));
    if(diff) return 0;
    accepted_sequence=sequence++; accepted=1;
    return 1;
}
int g100_auth_response(const char *body,char signature[65]) {
    uint8_t digest[32];
    if(!accepted || !mac(accepted_sequence,"response",(const uint8_t *)body,strlen(body),digest)) return 0;
    hex(digest,signature); memset(digest,0,sizeof(digest)); accepted=0;
    return 1;
}
