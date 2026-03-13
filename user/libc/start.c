/*
 * Limnx libc — portable program entry point
 *
 * The arch-specific crt_arch.h provides the _start symbol via inline
 * assembly. _start calls __libc_start(argc, argv) which calls main().
 */

/* Include arch-specific _start stub */
#if defined(__x86_64__)
#include "arch/x86_64/crt_arch.h"
#elif defined(__aarch64__)
#include "arch/arm64/crt_arch.h"
#endif

extern int main(int argc, char **argv);

/* Called by arch _start stub */
int __libc_start(long argc, char **argv) {
    return main((int)argc, argv);
}
