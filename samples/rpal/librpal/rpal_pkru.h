#include <x86intrin.h>
#include "private.h"

#define RPAL_PKRU_BASE_CODE_READ 0xAAAAAAAA
#define RPAL_PKRU_BASE_CODE 0xFFFFFFFF
#define RPAL_NO_PKEY -1

typedef uint32_t u32;
/*
 * extern __inline unsigned int
 * __attribute__((__gnu_inline__, __always_inline__, __artificial__))
 * _rdpkru_u32 (void)
 * {
 *   return __builtin_ia32_rdpkru ();
 * }
 *
 * extern __inline void
 * __attribute__((__gnu_inline__, __always_inline__, __artificial__))
 * _wrpkru (unsigned int __key)
 * {
 *   __builtin_ia32_wrpkru (__key);
 * }
 */
// #define rdpkru _rdpkru_u32
// #define wrpkru _wrpkru
static inline uint32_t rdpkru(void)
{
	uint32_t ecx = 0;
	uint32_t edx, pkru;

	/*
	 * "rdpkru" instruction.  Places PKRU contents in to EAX,
	 * clears EDX and requires that ecx=0.
	 */
	asm volatile(".byte 0x0f,0x01,0xee\n\t"
		     : "=a"(pkru), "=d"(edx)
		     : "c"(ecx));
	return pkru;
}

static inline void wrpkru(uint32_t pkru)
{
	uint32_t ecx = 0, edx = 0;

	/*
	 * "wrpkru" instruction.  Loads contents in EAX to PKRU,
	 * requires that ecx = edx = 0.
	 */
	asm volatile(".byte 0x0f,0x01,0xef\n\t"
		     :
		     : "a"(pkru), "c"(ecx), "d"(edx));
}

static inline u32 rpal_pkey_to_pkru(int pkey)
{
	int offset = pkey * 2;
	u32 mask = 0x3 << offset;

	return RPAL_PKRU_BASE_CODE & ~mask;
}

static inline u32 rpal_pkey_to_pkru_read(int pkey)
{
	int offset = pkey * 2;
	u32 mask = 0x3 << offset;

	return RPAL_PKRU_BASE_CODE_READ & ~mask;
}

static inline u32 rpal_pkru_union(u32 pkru0, u32 pkru1)
{
	return pkru0 & pkru1;
}

static inline u32 rpal_pkru_intersect(u32 pkru0, u32 pkru1)
{
	return pkru0 | pkru1;
}
