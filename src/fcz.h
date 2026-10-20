#ifndef FCZ_H
#define FCZ_H
#include <stddef.h>
#include <stdint.h>

#define FCZ_MAGIC          "FCZ1"
#define FCZ_VERSION        1u
#define FCZ_HEADER_SIZE    24u
#define FCZ_CHUNK_HDR_SIZE 12u
#define FCZ_DEFAULT_CHUNK  (1u << 20)    /* 1 MiB  */
#define FCZ_MIN_CHUNK      (4u << 10)    /* 4 KiB  */
#define FCZ_MAX_CHUNK      (16u << 20)   /* 16 MiB */
#define FCZ_MAX_THREADS    64u
#define FCZ_LEVEL          6

typedef enum {
    FCZ_OK = 0, FCZ_ERR_ARG, FCZ_ERR_IO, FCZ_ERR_MEM,
    FCZ_ERR_FORMAT, FCZ_ERR_ZLIB, FCZ_ERR_THREAD
} fcz_status;

typedef struct {
    double   seconds;
    uint64_t in_bytes;    /* bytes read from the input file   */
    uint64_t out_bytes;   /* bytes written to the output file */
} fcz_stats;

const char *fcz_strerror(fcz_status s);

/* Explicit little-endian serialization: no struct padding, no host-endian issues. */
static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
#endif
