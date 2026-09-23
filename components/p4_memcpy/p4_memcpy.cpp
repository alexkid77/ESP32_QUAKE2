/*
 * ESP32-P4 SIMD (PIE) memcpy implementation.
 *
 * Author: Alejandro Villegas Alonso
 *         https://www.linkedin.com/in/alejandro-villegas-alonso-825041b1
 *
 * Dispatches to mem_cpy::cpy_mem (fastest, from the ctag-fh-kiel benchmark)
 * or to libc memcpy depending on P4_MEMCPY_IMPL.
 */
#include "p4_memcpy.h"

#include <stdint.h>
#include <string.h>

#if P4_MEMCPY_IMPL == P4_MEMCPY_IMPL_SIMD
#include "mem_cpy_p4.hpp"
#endif

extern "C" void p4_memcpy(void *dst, const void *src, size_t n) {
#if P4_MEMCPY_IMPL == P4_MEMCPY_IMPL_SIMD
    if (n >= P4_MEMCPY_MIN_SIMD_BYTES && n <= UINT32_MAX) {
        mem_cpy::cpy_mem(src, dst, (uint32_t)n);
        return;
    }
#endif
    memcpy(dst, src, n);
}