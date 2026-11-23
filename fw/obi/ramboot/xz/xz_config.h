/* The Linux kernel's XZ decoder (lib/xz) built in user space for kjump:
 * single-call mode (the whole payload in one buffer) and the ARM branch
 * filter, the macros lib/xz/xz_private.h expects outside the kernel. */
#ifndef XZ_CONFIG_H
#define XZ_CONFIG_H
#define XZ_DEC_SINGLE
#define XZ_DEC_ARM
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "xz.h"
#define kmalloc(size, flags) malloc(size)
#define kfree(ptr) free(ptr)
#define vmalloc(size) malloc(size)
#define vfree(ptr) free(ptr)
#define memeq(a, b, size) (memcmp(a, b, size) == 0)
#define memzero(buf, size) memset(buf, 0, size)
#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#define min_t(type, x, y) min((type)(x), (type)(y))
#ifndef __always_inline
#define __always_inline inline __attribute__((__always_inline__))
#endif
#ifndef fallthrough
#define fallthrough __attribute__((__fallthrough__))
#endif
static inline uint32_t get_unaligned_le32(const uint8_t *b)
{
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static inline uint32_t get_unaligned_be32(const uint8_t *b)
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
}
static inline void put_unaligned_le32(uint32_t v, uint8_t *b)
{
	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
static inline void put_unaligned_be32(uint32_t v, uint8_t *b)
{
	b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}
#define get_le32 get_unaligned_le32
#endif
