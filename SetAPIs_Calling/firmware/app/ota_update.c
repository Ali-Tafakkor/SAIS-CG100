/* Authenticated binary update service shared by application and factory recovery. */
#include "ota_update.h"
#include "ota_auth.h"
#include "architecture.h"
#include "board_memory.h"
#include "boot_format.h"
#include "boot_request.h"
#include "board_status.h"
#include "device_config.h"
#include "main.h"
#include "mbedtls/sha256.h"
#include <stdio.h>
#include <string.h>
#define CHUNK_BYTES 16384u
#define PATH_PREFIX "/api/v1/firmware/"
static struct {
    G100ImageHeader header;
    uint32_t slot, base, received, metadata_sequence, reset_at, last_write;
    unsigned state; /* 0 idle, 1 receiving, 2 verified, 3 activated, 4 failed, 5 recovery reset */
} upload;
static int error_json(int code,const char *reason,char *out,size_t cap) {
    (void)snprintf(out,cap,"{\"ok\":false,\"error\":\"%s\"}",reason); return code;
}
static int parse_uint(const char *s,uint32_t *value) {
    if(!*s) return 0;
    uint32_t n=0;
    for(;*s;++s) { if(*s<'0'||*s>'9'||n>(UINT32_MAX-(uint32_t)(*s-'0'))/10u) return 0;
        n=n*10u+(uint32_t)(*s-'0'); }
    *value=n;return 1;
}
static void uid_string(char s[25]) {
    (void)snprintf(s,25,"%08lX%08lX%08lX",(unsigned long)HAL_GetUIDw0(),
        (unsigned long)HAL_GetUIDw1(),(unsigned long)HAL_GetUIDw2());
}
int g100_ota_discovery(const uint8_t *query,size_t bytes,char *out,size_t cap) {
    static const char signature[]="G100_UPDATE_DISCOVER_V2";
    if(bytes!=256u || memcmp(query,signature,sizeof(signature)-1u)) return 0;
    for(size_t i=sizeof(signature)-1u;i<bytes;++i) if(query[i]) return 0;
    char uid[25];uid_string(uid);
    int n=snprintf(out,cap,"{\"protocol\":2,\"uid\":\"%s\",\"firmware\":\"%s\"}",uid,G100_FIRMWARE_VERSION);
    return n>0 && (size_t)n<cap && n<=256 ? n:0;
}
static int header_valid(const G100ImageHeader *h) {
    if(h->magic!=G100_IMAGE_MAGIC || h->format!=1u || h->board!=G100_BOARD_ID ||
       h->load_address!=G100_SDRAM_BASE || h->flags || h->image_bytes<1024u ||
       h->image_bytes>G100_IMAGE_MAX || (h->image_bytes&3u) || !h->generation ||
       h->generation==G100_SLOT_NONE || h->header_crc!=g100_crc32(h,sizeof(*h)-4u)) return 0;
    for(unsigned i=0;i<15u;++i) if(h->reserved[i]) return 0;
    return 1;
}
static void empty_meta(G100BootMeta *meta) {
    memset(meta,0,sizeof(*meta)); meta->confirmed_slot=G100_SLOT_NONE;meta->trial_slot=G100_SLOT_NONE;
}
static uint32_t generation_floor(const G100BootMeta *meta) {
    G100ImageHeader h;
    uint32_t n=meta->confirmed_generation;
    if(meta->trial_generation>n) n=meta->trial_generation;
    if(meta->reserved[0]>n) n=meta->reserved[0];
    for(unsigned slot=0;slot<2;++slot)
        if(g100_image_header_read(slot,&h) && h.generation>n) n=h.generation;
    return n;
}
static int status_json(char *out,size_t cap) {
    G100BootMeta meta;G100ImageHeader active;
    int valid=g100_meta_read(&meta);if(!valid) empty_meta(&meta);
    char nonce[65]="", uid[25], digest[65]="";uint32_t sequence=0;
    uid_string(uid);
    int auth=g100_auth_challenge(nonce,&sequence);
    if(g100_image_header_read(G100_BOOT_INFO->selected_slot,&active))
        for(unsigned i=0;i<32;++i) (void)snprintf(digest+2*i,3,"%02x",active.sha256[i]);
    (void)snprintf(out,cap,
        "{\"ok\":true,\"protocol\":2,\"uid\":\"%s\",\"version\":\"%s\",\"role\":\"%s\","
        "\"auth_ready\":%s,\"nonce\":\"%s\",\"sequence\":%lu,\"chunk_bytes\":%u,"
        "\"metadata_valid\":%s,\"confirmed_slot\":%lu,\"confirmed_generation\":%lu,"
        "\"metadata_sequence\":%lu,\"trial_slot\":%lu,\"trial_generation\":%lu,"
        "\"generation_floor\":%lu,\"running_slot\":%lu,\"running_generation\":%lu,"
        "\"running_sha256\":\"%s\",\"boot_confirmed\":%s,\"healthy\":%s,"
        "\"upload_state\":%u,\"target_slot\":%lu,\"received\":%lu,\"image_bytes\":%lu}",
        uid,G100_FIRMWARE_VERSION,G100_BOOT_INFO->selected_slot==G100_SLOT_FACTORY?"recovery":"application",
        auth?"true":"false",nonce,(unsigned long)sequence,CHUNK_BYTES,valid?"true":"false",
        (unsigned long)meta.confirmed_slot,(unsigned long)meta.confirmed_generation,
        (unsigned long)meta.sequence,(unsigned long)meta.trial_slot,(unsigned long)meta.trial_generation,
        (unsigned long)generation_floor(&meta),(unsigned long)G100_BOOT_INFO->selected_slot,
        (unsigned long)G100_BOOT_INFO->generation,digest,G100_BOOT_INFO->confirmed?"true":"false",
        g100_architecture_ready()?"true":"false",
        upload.state,(unsigned long)upload.slot,(unsigned long)upload.received,(unsigned long)upload.header.image_bytes);
    return 200;
}
static int begin(const char *uid,const uint8_t *body,size_t bytes,char *out,size_t cap) {
    char expected[25];uid_string(expected);
    if(strcmp(uid,expected)) return error_json(412,"UID_MISMATCH",out,cap);
    G100ImageHeader h,old;G100BootMeta meta;
    if(bytes!=sizeof(h)) return error_json(422,"IMAGE_HEADER_INVALID",out,cap);
    memcpy(&h,body,sizeof(h));
    if(!header_valid(&h)) return error_json(422,"IMAGE_HEADER_INVALID",out,cap);
    if((upload.state==1u || upload.state==2u) && !memcmp(&h,&upload.header,sizeof(h))) {
        (void)snprintf(out,cap,"{\"ok\":true,\"target_slot\":%lu,\"chunk_bytes\":%u}",(unsigned long)upload.slot,CHUNK_BYTES);
        return 202;
    }
    if(upload.state==1u || upload.state==2u || upload.state==3u)
        return error_json(409,"UPLOAD_BUSY_ABORT_FIRST",out,cap);
    int recovery=G100_BOOT_INFO->selected_slot==G100_SLOT_FACTORY;
    if(!g100_meta_read(&meta)) {
        if(!recovery) return error_json(409,"RECOVERY_REQUIRED",out,cap);
        empty_meta(&meta);
        if(!g100_meta_write(&meta)) return error_json(500,"METADATA_REPAIR_FAILED",out,cap);
    }
    if(recovery && meta.trial_slot!=G100_SLOT_NONE &&
       (meta.trial_attempts>=G100_MAX_TRIAL_ATTEMPTS || !g100_image_validate(meta.trial_slot,&old))) {
        if(meta.trial_generation>meta.reserved[0]) meta.reserved[0]=meta.trial_generation;
        meta.trial_slot=G100_SLOT_NONE;meta.trial_generation=0;meta.trial_attempts=0;
        if(!g100_meta_write(&meta)) return error_json(500,"METADATA_REPAIR_FAILED",out,cap);
    }
    if(meta.trial_slot!=G100_SLOT_NONE) return error_json(409,"TRIAL_PENDING",out,cap);
    if(meta.confirmed_slot>1u && !recovery) return error_json(409,"RECOVERY_REQUIRED",out,cap);
    if(h.generation<=generation_floor(&meta)) return error_json(409,"GENERATION_NOT_NEWER",out,cap);
    /* Repair a corrupt confirmed image only while executing the independent service. */
    if(recovery && meta.confirmed_slot<=1u && !g100_image_validate(meta.confirmed_slot,&old)) {
        meta.confirmed_slot=G100_SLOT_NONE;
        if(!g100_meta_write(&meta)) return error_json(500,"METADATA_REPAIR_FAILED",out,cap);
    }
    uint32_t slot=meta.confirmed_slot<=1u?1u-meta.confirmed_slot:0u;
    if(slot==G100_BOOT_INFO->selected_slot) return error_json(409,"ACTIVE_SLOT_PROTECTED",out,cap);
    uint32_t base=g100_slot_offset(slot);
    if(!g100_qspi_image_sector_erase(base)) return error_json(500,"HEADER_ERASE_FAILED",out,cap);
    memset(&upload,0,sizeof(upload));upload.header=h;upload.slot=slot;upload.base=base;
    upload.metadata_sequence=meta.sequence;upload.state=1;upload.last_write=HAL_GetTick();
    (void)snprintf(out,cap,"{\"ok\":true,\"target_slot\":%lu,\"chunk_bytes\":%u}",(unsigned long)slot,CHUNK_BYTES);
    return 202;
}
static int append(uint32_t offset,const uint8_t *body,size_t bytes,char *out,size_t cap) {
    if(upload.state!=1u) return error_json(409,"NO_ACTIVE_UPLOAD",out,cap);
    if(!bytes || bytes>CHUNK_BYTES || (bytes&3u) || (offset&3u) || offset>upload.header.image_bytes ||
       bytes>upload.header.image_bytes-offset) return error_json(422,"CHUNK_INVALID",out,cap);
    uint32_t address=upload.base+G100_IMAGE_HEADER_BYTES+offset;
    /* A lost response can be retried with a fresh MAC without writing a page twice. */
    if(offset<upload.received && bytes<=upload.received-offset &&
       !memcmp((const void *)(G100_NOR_BASE+address),body,bytes)) goto reply;
    if(offset!=upload.received) return error_json(409,"OFFSET_MISMATCH",out,cap);
    for(unsigned pos=0;pos<bytes;) {
        uint32_t at=address+pos;
        if(!(at&4095u) && !g100_qspi_image_sector_erase(at)) {
            upload.state=4;return error_json(500,"PAYLOAD_ERASE_FAILED",out,cap);
        }
        unsigned count=256u-(at&255u);if(count>bytes-pos) count=(unsigned)bytes-pos;
        if(!g100_qspi_image_program(at,body+pos,count) || memcmp((const void *)(G100_NOR_BASE+at),body+pos,count)) {
            upload.state=4;return error_json(500,"PAYLOAD_VERIFY_FAILED",out,cap);
        }
        pos+=count;g100_watchdog_feed();
    }
    upload.received+=(uint32_t)bytes;
reply:
    upload.last_write=HAL_GetTick();
    (void)snprintf(out,cap,"{\"ok\":true,\"received\":%lu}",(unsigned long)upload.received);return 200;
}
static int verify_payload(char *out,size_t cap) {
    if(upload.state==2u) { (void)snprintf(out,cap,"{\"ok\":true,\"verified\":true}");return 200; }
    if(upload.state!=1u || upload.received!=upload.header.image_bytes)
        return error_json(409,"UPLOAD_INCOMPLETE",out,cap);
    mbedtls_sha256_context sha;
    uint8_t digest[32];
    uint32_t crc=0xFFFFFFFFu;
    const uint8_t *p=(const uint8_t *)(G100_NOR_BASE+upload.base+G100_IMAGE_HEADER_BYTES);
    mbedtls_sha256_init(&sha);
    if(mbedtls_sha256_starts(&sha,0)) {
        mbedtls_sha256_free(&sha);
        return error_json(500,"HASH_START_FAILED",out,cap);
    }
    for(uint32_t off=0;off<upload.header.image_bytes;) {
        uint32_t n=upload.header.image_bytes-off;
        if(n>4096u) n=4096u;
        if(mbedtls_sha256_update(&sha,p+off,n)) {
            mbedtls_sha256_free(&sha);
            return error_json(500,"HASH_UPDATE_FAILED",out,cap);
        }
        for(uint32_t i=0;i<n;++i) {
            crc^=p[off+i];
            for(unsigned j=0;j<8u;++j) crc=(crc>>1)^(0xEDB88320u&(0u-(crc&1u)));
        }
        off+=n;
        g100_watchdog_feed();
    }
    if(mbedtls_sha256_finish(&sha,digest)) {
        mbedtls_sha256_free(&sha);
        return error_json(500,"HASH_FINISH_FAILED",out,cap);
    }
    mbedtls_sha256_free(&sha);
    if((~crc)!=upload.header.image_crc || memcmp(digest,upload.header.sha256,32u)) {
        upload.state=4;
        return error_json(422,"PAYLOAD_DIGEST_MISMATCH",out,cap);
    }
    uint32_t base=upload.base;
    const uint8_t *head=(const uint8_t *)&upload.header;
    if(!g100_qspi_image_program(base+4u,head+4u,sizeof(upload.header)-4u) ||
       memcmp((const void *)(G100_NOR_BASE+base+4u),head+4u,sizeof(upload.header)-4u) ||
       !g100_qspi_image_program(base,head,4u) ||
       !g100_image_validate(upload.slot,&upload.header)) {
        upload.state=4;
        return error_json(500,"HEADER_COMMIT_FAILED",out,cap);
    }
    upload.state=2;
    (void)snprintf(out,cap,"{\"ok\":true,\"verified\":true,\"target_slot\":%lu}",
                   (unsigned long)upload.slot);
    return 200;
}
static int activate(char *out,size_t cap) {
    G100BootMeta meta;
    if(upload.state!=2u || !g100_image_validate(upload.slot,&upload.header))
        return error_json(409,"IMAGE_NOT_VERIFIED",out,cap);
    if(!g100_meta_read(&meta) || meta.sequence!=upload.metadata_sequence ||
       meta.confirmed_slot==upload.slot || meta.trial_slot!=G100_SLOT_NONE)
        return error_json(409,"BOOT_METADATA_CHANGED",out,cap);
    meta.trial_slot=upload.slot;meta.trial_generation=upload.header.generation;meta.trial_attempts=0;
    if(!g100_meta_write(&meta)) return error_json(500,"TRIAL_METADATA_WRITE_FAILED",out,cap);
    upload.state=3;upload.reset_at=HAL_GetTick()+800u;
    (void)snprintf(out,cap,"{\"ok\":true,\"reset_pending\":true}");return 202;
}
int g100_ota_handle(const char *method,const char *path,const uint8_t *body,size_t bytes,char *out,size_t cap) {
    if(strncmp(path,PATH_PREFIX,sizeof(PATH_PREFIX)-1u)) return 0;
    const char *action=path+sizeof(PATH_PREFIX)-1u;
    if(!strcmp(action,"status") && !strcmp(method,"GET")) return status_json(out,cap);
    if(strcmp(method,"POST")) return error_json(405,"METHOD_NOT_ALLOWED",out,cap);
    if(!g100_auth_request(path,body,bytes)) return error_json(401,"AUTHENTICATION_FAILED",out,cap);
    body+=G100_AUTH_BYTES;bytes-=G100_AUTH_BYTES;
    if(!strcmp(action,"status") && !bytes) return status_json(out,cap);
    if(!strncmp(action,"begin?uid=",10u)) return begin(action+10,body,bytes,out,cap);
    if(!strncmp(action,"chunk?offset=",13u)) {
        uint32_t off;if(!parse_uint(action+13,&off)) return error_json(400,"OFFSET_INVALID",out,cap);
        return append(off,body,bytes,out,cap);
    }
    if(!strcmp(action,"finish") && !bytes) return verify_payload(out,cap);
    if(!strcmp(action,"activate") && !bytes) return activate(out,cap);
    if(!strcmp(action,"abort") && !bytes && upload.state!=3u) {
        memset(&upload,0,sizeof(upload));return status_json(out,cap);
    }
    if(!strcmp(action,"confirm") && bytes==32u) {
        G100ImageHeader running;
        if(G100_BOOT_INFO->selected_slot>1u || !g100_image_header_read(G100_BOOT_INFO->selected_slot,&running) ||
           memcmp(body,running.sha256,32) || !g100_architecture_ready()) return error_json(409,"RUNNING_IMAGE_NOT_READY",out,cap);
        if(!g100_boot_confirm()) return error_json(500,"CONFIRM_FAILED",out,cap);
        return status_json(out,cap);
    }
    if(!strcmp(action,"recovery") && !bytes) {
        upload.state=5;upload.reset_at=HAL_GetTick()+800u;
        (void)snprintf(out,cap,"{\"ok\":true,\"reset_pending\":true}");return 202;
    }
    return error_json(404,"FIRMWARE_ROUTE_NOT_FOUND",out,cap);
}
void g100_ota_process(uint32_t now) {
    if(upload.state==3u && (int32_t)(now-upload.reset_at)>=0) g100_request_application();
    if(upload.state==5u && (int32_t)(now-upload.reset_at)>=0) NVIC_SystemReset();
    if((upload.state==1u || upload.state==2u) && (uint32_t)(now-upload.last_write)>60000u) upload.state=0;
#ifdef G100_RECOVERY
    /* Leave a rescue window after every reset, including confirmed application faults.
     * Empty/corrupt A+B stays in recovery indefinitely. Discovery does not delay boot. */
    if(now>=20000u && (upload.state==0u || upload.state==4u)) {
        static int checked;
        G100BootMeta meta;G100ImageHeader h;
        if(!checked && g100_meta_read(&meta)) {
            checked=1;
            if((meta.trial_slot<=1u && g100_image_validate(meta.trial_slot,&h)) ||
               (meta.confirmed_slot<=1u && g100_image_validate(meta.confirmed_slot,&h)))
                g100_request_application();
        }
    }
#endif
}
