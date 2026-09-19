#include "boot_format.h"
#include "board_memory.h"
#include "stm32h750xx.h"
#include <string.h>

uint32_t g100_crc32(const void *data,uint32_t size) {
    const uint8_t *p=data;
    uint32_t crc=0xFFFFFFFFu;
    for(uint32_t i=0;i<size;++i) {
        crc^=p[i];
        for(unsigned j=0;j<8;++j) crc=(crc>>1)^(0xEDB88320u & (0u-(crc&1u)));
    }
    return ~crc;
}
uint32_t g100_slot_offset(uint32_t slot) {
    return slot==0 ? G100_SLOT_A_OFFSET : slot==1 ? G100_SLOT_B_OFFSET :
           slot==G100_SLOT_FACTORY ? G100_FACTORY_OFFSET : G100_SLOT_NONE;
}
int g100_image_header_read(uint32_t slot,G100ImageHeader *h) {
    uint32_t off=g100_slot_offset(slot);
    if(off==G100_SLOT_NONE) return 0;
    memcpy(h,(const void *)(G100_NOR_BASE+off),sizeof(*h));
    uint32_t maximum=slot==G100_SLOT_FACTORY ? G100_FACTORY_BYTES-G100_IMAGE_HEADER_BYTES : G100_IMAGE_MAX;
    return h->magic==G100_IMAGE_MAGIC && h->format==1 && h->board==G100_BOARD_ID &&
           h->load_address==G100_SDRAM_BASE && h->image_bytes>=1024 &&
           h->image_bytes<=maximum && !(h->image_bytes&3u) && h->flags==0 &&
           h->header_crc==g100_crc32(h,sizeof(*h)-4);
}
int g100_image_validate(uint32_t slot,G100ImageHeader *h) {
    if(!g100_image_header_read(slot,h)) return 0;
    const uint32_t *p=(const uint32_t *)(G100_NOR_BASE+g100_slot_offset(slot)+G100_IMAGE_HEADER_BYTES);
    return p[0]==0x20020000u && (p[1]&1u) && (p[1]&~1u)>=G100_SDRAM_BASE+1024u &&
           (p[1]&~1u)<G100_SDRAM_BASE+h->image_bytes &&
           g100_crc32(p,h->image_bytes)==h->image_crc;
}
static int meta_valid(const G100BootMeta *m) {
    return m->magic==G100_META_MAGIC && m->format==1 && m->sequence!=0 &&
           (m->confirmed_slot<=1 || m->confirmed_slot==G100_SLOT_NONE) &&
           (m->trial_slot<=1 || m->trial_slot==G100_SLOT_NONE) &&
           m->trial_attempts<=G100_MAX_TRIAL_ATTEMPTS &&
           m->crc==g100_crc32(m,sizeof(*m)-4);
}
static int meta_latest(G100BootMeta *out) {
    G100BootMeta a,b;
    memcpy(&a,(const void *)(G100_NOR_BASE+G100_META0_OFFSET),sizeof(a));
    memcpy(&b,(const void *)(G100_NOR_BASE+G100_META1_OFFSET),sizeof(b));
    int av=meta_valid(&a),bv=meta_valid(&b);
    if(!av&&!bv) return -1;
    int selected=bv&&(!av||(int32_t)(b.sequence-a.sequence)>0);
    *out=selected?b:a;
    return selected;
}
int g100_meta_read(G100BootMeta *out) { return meta_latest(out)>=0; }
int g100_meta_write(G100BootMeta *m) {
    G100BootMeta old;
    int index=meta_latest(&old);
    m->magic=G100_META_MAGIC; m->format=1;
    m->sequence=index<0 ? 1 : old.sequence+1;
    if(!m->sequence) return 0; /* No silent generation wrap. */
    m->crc=g100_crc32(m,sizeof(*m)-4);
    if(!meta_valid(m)) return 0;
    uint32_t off=index==0 ? G100_META1_OFFSET:G100_META0_OFFSET;
    uint32_t mask=__get_PRIMASK(); __disable_irq();
    /* The current valid sector remains untouched. Commit magic is programmed LAST. */
    int ok=g100_qspi_sector_erase(off) &&
        g100_qspi_program(off+4,(const uint8_t *)m+4,sizeof(*m)-4);
    if(ok) ok=memcmp((const void *)(G100_NOR_BASE+off+4),(const uint8_t *)m+4,sizeof(*m)-4)==0;
    if(ok) ok=g100_qspi_program(off,m,4);
    if(ok) ok=memcmp((const void *)(G100_NOR_BASE+off),m,sizeof(*m))==0;
    __set_PRIMASK(mask);
    return ok;
}
int g100_boot_confirm(void) {
    volatile G100BootInfo *info=G100_BOOT_INFO;
    if(info->magic!=G100_INFO_MAGIC) return 0;
    if(!(info->flags&G100_BOOT_TRIAL)) { info->confirmed=1; return 1; }
    G100BootMeta meta;
    if(!g100_meta_read(&meta) || meta.trial_slot!=info->selected_slot ||
       meta.trial_generation!=info->generation) return 0;
    meta.confirmed_slot=info->selected_slot;
    meta.confirmed_generation=info->generation;
    meta.trial_slot=G100_SLOT_NONE; meta.trial_generation=0; meta.trial_attempts=0;
    if(!g100_meta_write(&meta)) return 0;
    info->metadata_sequence=meta.sequence; info->confirmed=1;
    return 1;
}
