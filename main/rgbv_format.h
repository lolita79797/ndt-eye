#ifndef RGBV_FORMAT_H
#define RGBV_FORMAT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Packed struct - EXACTLY 32 BYTES
// magic[4] + version(u16) + header_len(u16) + width(u16) + height(u16) + fps(u16) + total_frames(u32) + thumb_size(u32) + reserved[10]
typedef struct __attribute__((packed)) {
    char     magic[4];       // "RGBV"
    uint16_t version;        // 1
    uint16_t header_len;     // sizeof(rgbv_header_t) + thumb_size (Offset of frame 0)
    uint16_t width;          // 360
    uint16_t height;         // 360
    uint16_t fps;            // 60
    uint32_t total_frames;   // Total frames count
    uint32_t thumb_size;     // Byte size of JPEG thumbnail
    uint8_t  reserved[10];   // Padding
} rgbv_header_t;

_Static_assert(sizeof(rgbv_header_t) == 32, "rgbv_header_t struct size must be exactly 32 bytes");

#ifdef __cplusplus
}
#endif

#endif // RGBV_FORMAT_H
