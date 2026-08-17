#ifndef __MACROS_H__
#define __MACROS_H__

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define offset_of(type, member) ((size_t) &((type *)0)->member)
#define container_of(ptr, type, member) ({          \
    const typeof(((type *)0)->member) *__mptr = (ptr); \
    (type *)((char *)__mptr - offset_of(type, member)); })

#define __likely(x) __builtin_expect(!!(x), 1)
#define __unlikely(x) __builtin_expect(!!(x), 0)

#endif /* __MACROS_H__ */
