// The vendored libevdev linux/input.h wants <linux/types.h>, which only
// Linux has. Elsewhere (e.g. macOS) provide the few types it uses, so that
// the host test also runs there.
#if defined(__linux__)
#include_next <linux/types.h>
#else
#ifndef MINITOUCH_TEST_LINUX_TYPES_H
#define MINITOUCH_TEST_LINUX_TYPES_H
#include <stdint.h>
typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;
#endif
#endif
