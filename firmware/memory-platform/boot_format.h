#ifndef G100_BOOT_FORMAT_H
#define G100_BOOT_FORMAT_H
#include <stdint.h>

#define G100_BOARD_ID 0x75000206u
#define G100_IMAGE_MAGIC 0x314D4947u
#define G100_META_MAGIC 0x314D4247u
#define G100_INFO_MAGIC 0x31494247u
#define G100_NOR_BASE 0x90000000u
#define G100_NOR_BYTES 0x01000000u
#define G100_SDRAM_BASE 0xC0000000u
#define G100_SDRAM_BYTES 0x01000000u
#define G100_IMAGE_HEADER_BYTES 0x1000u
#define G100_IMAGE_SLOT_BYTES 0x400000u
#define G100_IMAGE_MAX (G100_IMAGE_SLOT_BYTES - G100_IMAGE_HEADER_BYTES)
#define G100_FACTORY_OFFSET 0x10000u
#define G100_FACTORY_BYTES 0xD0000u
#define G100_META0_OFFSET 0xF0000u
#define G100_META1_OFFSET 0xF1000u
#define G100_SLOT_A_OFFSET 0x100000u
#define G100_SLOT_B_OFFSET 0x500000u
#define G100_SLOT_NONE 0xFFFFFFFFu
#define G100_SLOT_FACTORY 2u
#define G100_SLOT_RAM 3u
#define G100_BOOT_TRIAL 1u
#define G100_BOOT_FALLBACK 2u
#define G100_BOOT_RAM_TEST 4u
#define G100_BOOT_META_DEGRADED 8u
#define G100_MAX_TRIAL_ATTEMPTS 2u

typedef struct {
    uint32_t magic, format, board, load_address, image_bytes, image_crc;
    uint32_t generation, flags;
    uint8_t sha256[32]; /* Host/release identity; CRC is the development boot integrity gate. */
    uint32_t reserved[15], header_crc;
} G100ImageHeader;

typedef struct {
    uint32_t magic, format, sequence, confirmed_slot, confirmed_generation;
    uint32_t trial_slot, trial_generation, trial_attempts;
    uint32_t reserved[7], crc;
} G100BootMeta;

typedef struct {
    uint32_t magic, version, stage, error, selected_slot, generation, image_bytes;
    uint32_t image_crc, boot_cycles, memory_errors, jedec, metadata_sequence;
    uint32_t flags, reset_cause, confirmed, cpu_hz;
    uint32_t reserved[16];
} G100BootInfo;
#define G100_BOOT_INFO ((volatile G100BootInfo *)0x24000080u)
_Static_assert(sizeof(G100ImageHeader) == 128, "image format ABI");
_Static_assert(sizeof(G100BootMeta) == 64, "metadata format ABI");
_Static_assert(sizeof(G100BootInfo) == 128, "handoff ABI");

uint32_t g100_crc32(const void *data, uint32_t bytes);
uint32_t g100_slot_offset(uint32_t slot);
int g100_image_header_read(uint32_t slot, G100ImageHeader *out);
int g100_image_validate(uint32_t slot, G100ImageHeader *out);
int g100_meta_read(G100BootMeta *out);
int g100_meta_write(G100BootMeta *meta);
int g100_boot_confirm(void);
uint32_t g100_select_application(G100ImageHeader *image);
#endif
