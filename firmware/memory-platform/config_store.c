/* Confirmed configuration uses two independent NOR records, commit marker last. */
#include "config_store.h"
#include "board_memory.h"
#include "boot_format.h"
#include <stdint.h>
#include <string.h>
#define CONFIG_MAGIC 0x31464347u
typedef struct {uint32_t magic,format,sequence,settings_bytes,instances_bytes,payload_crc,reserved,crc;} Header;
static uint8_t pending[2048+8193];
static const Header *record(unsigned index) {
    uint32_t off=index?G100_CONFIG1_OFFSET:G100_CONFIG0_OFFSET;
    const Header *h=(const void *)(G100_NOR_BASE+off);
    if(h->magic!=CONFIG_MAGIC || h->format!=1 || !h->sequence || h->reserved ||
       h->settings_bytes<3 || h->settings_bytes>2048 || h->instances_bytes<3 || h->instances_bytes>8193 ||
       h->crc!=g100_crc32(h,sizeof(*h)-4)) return NULL;
    const char *p=(const void *)(h+1);uint32_t n=h->settings_bytes+h->instances_bytes;
    if(p[h->settings_bytes-1] || p[n-1] || g100_crc32(p,n)!=h->payload_crc ||
       strlen(p)!=h->settings_bytes-1 || strlen(p+h->settings_bytes)!=h->instances_bytes-1) return NULL;
    return h;
}
static const Header *latest(void) {
    const Header *a=record(0),*b=record(1);
    return b && (!a || (int32_t)(b->sequence-a->sequence)>0)?b:a;
}
unsigned g100_config_revision(void) {const Header *h=latest();return h?h->sequence:0;}
int g100_config_load(char *settings,size_t sc,char *instances,size_t ic) {
    const Header *h=latest();
    if(!h || !settings || sc<h->settings_bytes || (instances && ic<h->instances_bytes)) return 0;
    const char *p=(const void *)(h+1);
    memcpy(settings,p,h->settings_bytes);
    if(instances) memcpy(instances,p+h->settings_bytes,h->instances_bytes);
    return 1;
}
int g100_config_save(const char *settings,const char *instances) {
    size_t sn=strlen(settings)+1,in=strlen(instances)+1;
    if(sn>2048 || in>8193) return 0;
    const Header *old=latest();
    if(old && old->settings_bytes==sn && old->instances_bytes==in &&
       !memcmp(old+1,settings,sn) && !memcmp((const uint8_t *)(old+1)+sn,instances,in)) return 1;
    uint32_t seq=old?old->sequence+1:1;
    if(!seq) return 0;
    memcpy(pending,settings,sn);memcpy(pending+sn,instances,in);
    Header h={CONFIG_MAGIC,1,seq,(uint32_t)sn,(uint32_t)in,g100_crc32(pending,(uint32_t)(sn+in)),0,0};
    h.crc=g100_crc32(&h,sizeof(h)-4);
    uint32_t off=old==(const Header *)(G100_NOR_BASE+G100_CONFIG0_OFFSET)?G100_CONFIG1_OFFSET:G100_CONFIG0_OFFSET;
    uint32_t bytes=(uint32_t)(sn+in);
    for(uint32_t n=0;n<sizeof(h)+bytes;n+=4096) {
        if(!g100_qspi_config_sector_erase(off+n)) return 0;
        g100_watchdog_feed();
    }
    for(uint32_t n=0;n<bytes;) {
        uint32_t at=off+sizeof(h)+n,count=256-(at&255);
        if(count>bytes-n) count=bytes-n;
        if(!g100_qspi_config_program(at,pending+n,count) ||
           memcmp((const void *)(G100_NOR_BASE+at),pending+n,count)) return 0;
        n+=count;g100_watchdog_feed();
    }
    if(!g100_qspi_config_program(off+4,(const uint8_t *)&h+4,sizeof(h)-4) ||
       memcmp((const void *)(G100_NOR_BASE+off+4),(const uint8_t *)&h+4,sizeof(h)-4) ||
       !g100_qspi_config_program(off,&h,4)) return 0;
    return record(off==G100_CONFIG1_OFFSET)!=NULL;
}
