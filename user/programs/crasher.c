/* Minimal program that exits with non-zero status (for supervisor restart test) */
#include "libc/libc.h"

int main(void) {
    sys_exit(1);
    return 1;
}
