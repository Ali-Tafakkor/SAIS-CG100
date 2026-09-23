/* Selection is isolated from peripheral startup so it can run in ARM fault tests. */
#include "boot_format.h"
uint32_t g100_select_application(G100ImageHeader *image) {
    G100BootMeta meta;
    if(!g100_meta_read(&meta)) return G100_SLOT_NONE;
    G100_BOOT_INFO->metadata_sequence=meta.sequence;
    if(meta.trial_slot<=1u && meta.trial_attempts<G100_MAX_TRIAL_ATTEMPTS &&
       g100_image_validate(meta.trial_slot,image) && image->generation==meta.trial_generation) {
        ++meta.trial_attempts;
        if(g100_meta_write(&meta)) {
            G100_BOOT_INFO->flags|=G100_BOOT_TRIAL;
            G100_BOOT_INFO->metadata_sequence=meta.sequence;
            return meta.trial_slot;
        }
        G100_BOOT_INFO->flags|=G100_BOOT_META_DEGRADED;
    }
    if(meta.trial_slot!=G100_SLOT_NONE) {
        if(meta.trial_generation>meta.reserved[0]) meta.reserved[0]=meta.trial_generation;
        meta.trial_slot=G100_SLOT_NONE;meta.trial_generation=0;meta.trial_attempts=0;
        if(!g100_meta_write(&meta)) G100_BOOT_INFO->flags|=G100_BOOT_META_DEGRADED;
        G100_BOOT_INFO->metadata_sequence=meta.sequence;
        G100_BOOT_INFO->flags|=G100_BOOT_FALLBACK;
    }
    if(meta.confirmed_slot<=1u && g100_image_validate(meta.confirmed_slot,image) &&
       image->generation==meta.confirmed_generation) return meta.confirmed_slot;
    return G100_SLOT_NONE;
}
