/* Host test for installer/src/edit.c:
 *   cc -I../src -o /tmp/test_edit test_edit.c ../src/edit.c
 *   test_edit tai-install config.txt block.txt...   (prints the new file)
 *   test_edit tai-uninstall config.txt block.txt...
 *   test_edit vg-patch patchlist.txt block.txt...
 *   test_edit vg-config config.txt block.txt...
 *   test_edit kernel-add|kernel-remove config.txt block.txt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edit.h"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    fread(s, 1, n, f);
    s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    char *text = slurp(argv[2]);
    char titles[MAX_TITLES][TITLE_LEN + 1];
    int n = 0;
    for (int i = 3; i < argc; i++) {
        char *blk = slurp(argv[i]);
        char t[MAX_TITLES][TITLE_LEN + 1];
        int bn = block_titles(blk, t, MAX_TITLES);
        for (int j = 0; j < bn; j++) strcpy(titles[n++], t[j]);
        if (!strcmp(argv[1], "vg-patch")) {
            char *out;
            if (vg_patch_replace(text, blk, &out) < 0) return 3;
            if (out) { free(text); text = out; }
        }
    }
    char *out = NULL;
    if (!strcmp(argv[1], "tai-install")) tai_install(text, titles, n, "ur0:tai/divasplit.suprx", &out);
    else if (!strcmp(argv[1], "tai-uninstall")) tai_uninstall(text, titles, n, &out);
    else if (!strcmp(argv[1], "vg-config")) vg_config_fps(text, titles, n, &out);
    else if (!strcmp(argv[1], "kernel-add")) tai_kernel_add(text, "ur0:tai/divasplit_prof.skprx", &out);
    else if (!strcmp(argv[1], "kernel-remove")) tai_kernel_remove(text, "divasplit_prof", &out);
    fputs(out ? out : text, stdout);
    fprintf(stderr, "titles:");
    for (int i = 0; i < n; i++) fprintf(stderr, " %s", titles[i]);
    fprintf(stderr, "\n");
    return 0;
}
