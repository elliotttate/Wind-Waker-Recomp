/* Bounded Yaz0 decoding for the local texture importer. GPL-3.0-or-later. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int wwhd_yaz0_decode(const uint8_t *src, size_t src_size,
                     uint8_t *out, size_t out_size) {
    if (src_size < 16 || memcmp(src, "Yaz0", 4) != 0)
        return 1;
    size_t declared = (size_t)src[4] << 24 | (size_t)src[5] << 16 |
                      (size_t)src[6] << 8 | src[7];
    if (declared != out_size)
        return 1;
    size_t at = 16, written = 0;
    unsigned code = 0, bits = 0;
    while (written < out_size) {
        if (!bits) {
            if (at >= src_size) return 1;
            code = src[at++];
            bits = 8;
        }
        if (code & 128) {
            if (at >= src_size) return 1;
            out[written++] = src[at++];
        } else {
            if (at + 2 > src_size) return 1;
            unsigned a = src[at++], b = src[at++];
            size_t distance = ((a & 15) << 8) + b + 1;
            size_t count = a >> 4;
            if (!count) {
                if (at >= src_size) return 1;
                count = src[at++] + 18;
            } else count += 2;
            if (distance > written || count > out_size - written)
                return 1;
            for (size_t i = 0; i < count; ++i) {
                out[written] = out[written - distance];
                ++written;
            }
        }
        code <<= 1;
        --bits;
    }
    return 0;
}
