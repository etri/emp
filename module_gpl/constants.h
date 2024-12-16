#ifndef __CONSTANTS_H__
#define __CONSTANTS_H__

#ifndef BITS_PER_BYTE
#define BITS_PER_BYTE (8)
#endif

#define MS_TO_NS (1000000)
#define S_TO_NS  (1000000000)

#define KB_ORDER (10)
#define MB_ORDER (20)
#define GB_ORDER (30)

#define SIZE_MEGA             (((size_t)1)<<MB_ORDER)
#define SIZE_GIGA             (((size_t)1)<<GB_ORDER)

#define MB_TO_PAGE(x)         (x << (MB_ORDER - PAGE_SHIFT))
#define GB_TO_PAGE(x)         (x << (GB_ORDER - PAGE_SHIFT))

#define PAGE_TO_MB(x)         (x >> (MB_ORDER - PAGE_SHIFT))
#define PAGE_TO_GB(x)         (x >> (GB_ORDER - PAGE_SHIFT))

#endif /* __CONSTANTS_H__ */
