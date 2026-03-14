/* echo — print arguments to stdout */
#include "../libc/libc.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(" ", 1);
        int len = 0;
        while (argv[i][len]) len++;
        sys_write(argv[i], len);
    }
    sys_write("\n", 1);
    return 0;
}
