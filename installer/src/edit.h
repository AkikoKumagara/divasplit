#ifndef DIVASPLIT_EDIT_H
#define DIVASPLIT_EDIT_H

/*
 * Text edits for the installer, plain C with no Vita calls so they can be
 * tested on a PC (installer/test). Every function takes the old file
 * contents and returns a new malloc'd string through *out, or NULL when
 * nothing changes. They return -1 when out of memory, else 0.
 */

#define TITLE_LEN 9
#define MAX_TITLES 32

// title IDs in a VitaGrafix block's [TITLEID, self, NID] headers; returns the count
int block_titles(const char *block, char titles[][TITLE_LEN + 1], int max);

// taiHEN config.txt: put plugin_line under each *TITLE section (creating it if
// missing) and drop any other divasplit line there (old paths, typos)
int tai_install(const char *in, char titles[][TITLE_LEN + 1], int n, const char *plugin_line, char **out);

// taiHEN config.txt: remove every divasplit line, and title sections that end up empty
int tai_uninstall(const char *in, char titles[][TITLE_LEN + 1], int n, char **out);

// VitaGrafix patchlist.txt: replace the headers of block (same title ID and NID)
// with block. A block that also covers other games or versions keeps those headers.
int vg_patch_replace(const char *in, const char *block, char **out);

// VitaGrafix config.txt: FPS=60 in each [TITLE] section (created if missing)
int vg_config_fps(const char *in, char titles[][TITLE_LEN + 1], int n, char **out);

// case-insensitive substring search
const char *find_ci(const char *hay, const char *needle);

#endif
