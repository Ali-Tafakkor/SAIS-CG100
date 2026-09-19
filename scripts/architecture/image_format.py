"""Host counterpart of boot_format.h. Development integrity, NOT authentication."""
import hashlib
import struct
import zlib
import warnings

BOARD = 0x75000206
SDRAM = 0xC0000000
HEADER = 4096
SLOT_BYTES = 0x400000
MAX_IMAGE = SLOT_BYTES - HEADER
FACTORY_BYTES = 0xD0000
OFFSETS = (0x100000, 0x500000, 0x10000)
META_OFFSETS = (0xF0000, 0xF1000)
NONE = 0xFFFFFFFF


def require(ok, message):
    if not ok:
        raise ValueError(message)


def admit(payload, maximum=MAX_IMAGE):
    require(1024 < len(payload) <= maximum and len(payload) % 4 == 0,
            'IMAGE_CAPACITY_EXCEEDED_OR_INVALID: no Flash write; SD is disabled. '
            'Reduce the image or explicitly redesign capacity before deployment.')
    sp, entry = struct.unpack_from('<II', payload)
    require(sp == 0x20020000 and entry & 1 and SDRAM + 1024 <= (entry & ~1) < SDRAM + len(payload),
            'Image vectors do not match the SDRAM/DTCM board ABI')
    if len(payload) >= maximum * 0.8:
        warnings.warn('IMAGE_CAPACITY_WARNING: >=80% of this slot. Plan capacity before deployment; '
                      'SD is disabled and will not be enabled automatically.', stacklevel=2)


def pack(payload, generation, maximum=MAX_IMAGE):
    admit(payload, maximum)
    require(0 < generation < NONE, 'Invalid image generation')
    head = struct.pack('<8I', 0x314D4947, 1, BOARD, SDRAM, len(payload), zlib.crc32(payload), generation, 0)
    head += hashlib.sha256(payload).digest() + bytes(60)
    head += struct.pack('<I', zlib.crc32(head))
    return head + b'\xff' * (HEADER - len(head)) + payload


def unpack(image, maximum=MAX_IMAGE):
    require(len(image) >= HEADER + 1024, 'Truncated image')
    magic, fmt, board, load, size, crc, gen, flags = struct.unpack_from('<8I', image)
    require((magic, fmt, board, load, flags) == (0x314D4947, 1, BOARD, SDRAM, 0), 'Wrong image identity')
    require(zlib.crc32(image[:124]) == struct.unpack_from('<I', image, 124)[0], 'Header CRC mismatch')
    require(len(image) == HEADER + size, 'Image length mismatch')
    payload = image[HEADER:]
    admit(payload, maximum)
    require(zlib.crc32(payload) == crc, 'Payload CRC mismatch')
    require(hashlib.sha256(payload).digest() == image[32:64], 'Payload SHA-256 mismatch')
    return {'bytes': size, 'generation': gen, 'sha256': hashlib.sha256(payload).hexdigest()}


def meta_pack(sequence, confirmed_slot, confirmed_generation, trial_slot=NONE, trial_generation=0, attempts=0):
    require(0 < sequence < NONE, 'Metadata sequence exhausted')
    require(confirmed_slot in (0, 1, NONE) and trial_slot in (0, 1, NONE) and 0 <= attempts <= 2,
            'Invalid metadata selection')
    raw = struct.pack('<15I', 0x314D4247, 1, sequence, confirmed_slot, confirmed_generation,
                      trial_slot, trial_generation, attempts, *([0] * 7))
    return raw + struct.pack('<I', zlib.crc32(raw))


def meta_unpack(raw):
    if len(raw) < 64:
        return None
    v = struct.unpack_from('<16I', raw)
    if (v[:2] != (0x314D4247, 1) or not v[2] or v[3] not in (0, 1, NONE)
            or v[5] not in (0, 1, NONE) or v[7] > 2 or zlib.crc32(raw[:60]) != v[15]):
        return None
    return dict(zip(('sequence', 'confirmed_slot', 'confirmed_generation', 'trial_slot', 'trial_generation', 'attempts'), v[2:8]))


def latest_metadata(sectors):
    valid = [(i, meta_unpack(raw)) for i, raw in enumerate(sectors)]
    valid = [(i, m) for i, m in valid if m is not None]
    require(valid, 'No valid boot metadata')
    return max(valid, key=lambda pair: pair[1]['sequence'])
