#ifndef NX_CORE_HOST_KERNEL_COMPAT_H
#define NX_CORE_HOST_KERNEL_COMPAT_H
/* Load host libc definition guards before the freestanding ABI defines its
 * compiler-attribute spellings. This changes no production warning policy. */
#include <sys/cdefs.h>
#ifdef __unused
#undef __unused
#endif
#ifdef __weak
#undef __weak
#endif
#endif
