/* Hardware boundary for executing the real updater, HMAC and boot policy on ARM.
 * No connected board, OpenOCD or real network is used by this test harness. */
#include "main.h"
#include "boot_format.h"
#include "board_memory.h"
#include "board_status.h"
#include "ota_update.h"
#include "ota_auth.h"
#include "config_store.h"
#include <string.h>
#include <stddef.h>
#include <errno.h>
volatile BoardStatus g_board_status;
static uint32_t tick=6000, seed=1;
static int fail_after=-1, writes, random_failure, healthy=1;
static uint8_t heap[32768];static ptrdiff_t heap_used;
void *_sbrk(ptrdiff_t n) {
    if(n<0 || n>(ptrdiff_t)sizeof(heap)-heap_used) {errno=ENOMEM;return (void *)-1;}
    void *p=heap+heap_used;heap_used+=n;return p;
}
uint32_t HAL_GetTick(void) {return tick;}
uint32_t HAL_GetUIDw0(void) {return 0x00112233;}
uint32_t HAL_GetUIDw1(void) {return 0x44556677;}
uint32_t HAL_GetUIDw2(void) {return 0x8899AABB;}
void g100_watchdog_feed(void) {}
int g100_architecture_ready(void) {return healthy;}
int rj_random(void *unused,unsigned char *out,size_t bytes) {
    (void)unused;if(random_failure) return -1;
    for(size_t i=0;i<bytes;++i) out[i]=(uint8_t)(seed+i);
    ++seed;return 0;
}
static int allow(void) {return fail_after<0 || writes++<fail_after;}
static int meta_range(uint32_t off,uint32_t n) {
    return (off>=G100_META0_OFFSET && off+n<=G100_META0_OFFSET+4096) ||
           (off>=G100_META1_OFFSET && off+n<=G100_META1_OFFSET+4096);
}
static int image_range(uint32_t off,uint32_t n) {
    return (off>=G100_SLOT_A_OFFSET && off+n<=G100_SLOT_A_OFFSET+G100_IMAGE_SLOT_BYTES) ||
           (off>=G100_SLOT_B_OFFSET && off+n<=G100_SLOT_B_OFFSET+G100_IMAGE_SLOT_BYTES);
}
static int program(uint32_t off,const void *data,uint32_t n) {
    if(!allow()) return 0;
    if(!n || n>256 || (off&255)+n>256) return 0;
    uint8_t *dst=(void *)(G100_NOR_BASE+off);const uint8_t *src=data;
    for(uint32_t i=0;i<n;++i) {if((dst[i]&src[i])!=src[i]) return 0;dst[i]&=src[i];}
    return 1;
}
int g100_qspi_sector_erase(uint32_t off) {
    if((off&4095) || !meta_range(off,4096) || !allow()) return 0;
    memset((void *)(G100_NOR_BASE+off),255,4096);return 1;
}
int g100_qspi_image_sector_erase(uint32_t off) {
    if((off&4095) || !image_range(off,4096) || !allow()) return 0;
    memset((void *)(G100_NOR_BASE+off),255,4096);return 1;
}
int g100_qspi_program(uint32_t off,const void *p,uint32_t n) {return meta_range(off,n)&&program(off,p,n);}
int g100_qspi_image_program(uint32_t off,const void *p,uint32_t n) {return image_range(off,n)&&program(off,p,n);}
typedef struct {uint32_t size;char method[8],path[160];uint8_t body[16420];char output[1536],mac[65];} Io;
#define IO ((Io *)0x24001000u)
int test_http(void) {
    IO->output[0]=IO->mac[0]=0;
    int code=g100_ota_handle(IO->method,IO->path,IO->body,IO->size,IO->output,sizeof(IO->output));
    if(!strcmp(IO->method,"POST")) g100_auth_response(IO->output,IO->mac);
    return code;
}
int test_discovery(void) {return g100_ota_discovery(IO->body,IO->size,IO->output,sizeof(IO->output));}
void test_init(uint32_t slot,uint32_t generation,uint32_t flags,uint32_t random_seed) {
    memset((void *)G100_BOOT_INFO,0,sizeof(*G100_BOOT_INFO));
    G100_BOOT_INFO->magic=G100_INFO_MAGIC;G100_BOOT_INFO->selected_slot=slot;
    G100_BOOT_INFO->generation=generation;G100_BOOT_INFO->flags=flags;
    G100_BOOT_INFO->confirmed=!(flags&G100_BOOT_TRIAL);
    g_board_status.stage=BOARD_STAGE_RUNNING;seed=random_seed;
}
void test_fault(int count) {fail_after=count;writes=0;}
void test_health(int value) {healthy=value;}
void test_rng_fail(void) {random_failure=1;}
uint32_t test_boot(void) {
    G100ImageHeader h;G100_BOOT_INFO->flags=0;
    uint32_t selected=g100_select_application(&h);
    if(selected!=G100_SLOT_NONE) {G100_BOOT_INFO->selected_slot=selected;G100_BOOT_INFO->generation=h.generation;}
    return selected;
}
int test_valid(uint32_t slot) {G100ImageHeader h;return g100_image_validate(slot,&h);}

int g100_qspi_config_sector_erase(uint32_t off) {
    if((off&4095) || off<0xE1000 || off>=0xE9000 || !allow()) return 0;
    memset((void *)(G100_NOR_BASE+off),255,4096);return 1;
}
int g100_qspi_config_program(uint32_t off,const void *p,uint32_t n) {
    return off>=0xE1000 && off+n<=0xE9000 && program(off,p,n);
}
int test_config_save(void) {return g100_config_save((const char *)IO->body,(const char *)IO->body+2048);}
int test_config_load(void) {return g100_config_load((char *)IO->body,2048,(char *)IO->body+2048,8193);}
