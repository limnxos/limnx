/* ls — list directory contents */
#include "../libc/libc.h"

/* vfs_ls_dirent_t: name[256] + type(1) + pad[7] + size(8) = 272 bytes */
typedef struct {
    char name[256];
    unsigned char type;
    unsigned char pad[7];
    unsigned long size;
} ls_dirent_t;

/* vfs_stat_t: size(8) + type(1) + pad(1) + mode(2) + uid(2) + gid(2) = 16 bytes */
typedef struct {
    unsigned long size;
    unsigned char type;
    unsigned char pad1;
    unsigned short mode;
    unsigned short uid;
    unsigned short gid;
} stat_t;

static const char *type_char(unsigned char type) {
    switch (type) {
        case 0: return "-";  /* file */
        case 1: return "d";  /* directory */
        case 2: return "l";  /* symlink */
        default: return "?";
    }
}

static void mode_str(unsigned short mode, char *buf) {
    buf[0] = (mode & 0x100) ? 'r' : '-';
    buf[1] = (mode & 0x080) ? 'w' : '-';
    buf[2] = (mode & 0x040) ? 'x' : '-';
    buf[3] = (mode & 0x020) ? 'r' : '-';
    buf[4] = (mode & 0x010) ? 'w' : '-';
    buf[5] = (mode & 0x008) ? 'x' : '-';
    buf[6] = (mode & 0x004) ? 'r' : '-';
    buf[7] = (mode & 0x002) ? 'w' : '-';
    buf[8] = (mode & 0x001) ? 'x' : '-';
    buf[9] = '\0';
}

int main(int argc, char **argv) {
    int long_fmt = 0;
    int show_all = 0;
    const char *dir = (void *)0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') long_fmt = 1;
                else if (argv[i][j] == 'a') show_all = 1;
            }
        } else {
            dir = argv[i];
        }
    }

    /* Default to cwd */
    char cwd[256];
    if (!dir) {
        sys_getcwd(cwd, sizeof(cwd));
        dir = cwd;
    }

    ls_dirent_t ent;
    for (unsigned long idx = 0; ; idx++) {
        if (sys_readdir(dir, idx, &ent) < 0) break;

        /* Skip hidden files unless -a */
        if (!show_all && ent.name[0] == '.') continue;

        if (long_fmt) {
            /* Build full path for stat */
            char path[512];
            int p = 0;
            for (int j = 0; dir[j] && p < 500; j++) path[p++] = dir[j];
            if (p > 0 && path[p-1] != '/') path[p++] = '/';
            for (int j = 0; ent.name[j] && p < 510; j++) path[p++] = ent.name[j];
            path[p] = '\0';

            stat_t st;
            if (sys_stat(path, &st) == 0) {
                char m[10];
                mode_str(st.mode, m);
                printf("%s%s %3d %3d %8lu %s\n",
                       type_char(st.type), m, st.uid, st.gid, st.size, ent.name);
            } else {
                printf("%s %s\n", type_char(ent.type), ent.name);
            }
        } else {
            printf("%s\n", ent.name);
        }
    }
    return 0;
}
