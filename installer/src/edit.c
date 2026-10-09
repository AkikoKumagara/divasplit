#include <stdlib.h>
#include <string.h>

#include "edit.h"

typedef struct {
    const char *s;
    int len;    // without the newline
} line_t;

typedef struct {
    line_t *v;
    int n;
} lines_t;

typedef struct {
    char *s;
    size_t len, cap;
    int oom;
} sb_t;

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int split(const char *in, lines_t *L) {
    int cap = 1;
    for (const char *p = in; *p; p++)
        cap += *p == '\n';
    L->v = malloc(sizeof(line_t) * cap);
    L->n = 0;
    if (!L->v)
        return -1;
    const char *p = in;
    while (*p) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        L->v[L->n].s = p;
        L->v[L->n].len = len;
        L->n++;
        p += len + (e ? 1 : 0);
    }
    return 0;
}

static void sb_add(sb_t *b, const char *s, size_t n) {
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *ns = realloc(b->s, cap);
        if (!ns) {
            b->oom = 1;
            return;
        }
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void sb_line(sb_t *b, const char *s, int len) {
    sb_add(b, s, len);
    sb_add(b, "\n", 1);
}

static void sb_str(sb_t *b, const char *s) {
    sb_line(b, s, (int)strlen(s));
}

static int sb_last_blank(const sb_t *b) {
    if (b->len == 0 || (b->len == 1 && b->s[0] == '\n'))
        return 1;
    if (b->len >= 2 && b->s[b->len - 1] == '\n') {
        size_t i = b->len - 2;
        while (i > 0 && (b->s[i] == ' ' || b->s[i] == '\t' || b->s[i] == '\r'))
            i--;
        return b->s[i] == '\n';
    }
    return 0;
}

// hand the result back, or NULL when it equals the input (ignoring a newline
// added at the very end)
static int finish(const char *in, lines_t *L, sb_t *b, char **out) {
    free(L->v);
    *out = NULL;
    if (b->oom) {
        free(b->s);
        return -1;
    }
    size_t in_len = strlen(in);
    if (!b->s || (b->len == in_len && memcmp(b->s, in, in_len) == 0)
            || (b->len == in_len + 1 && memcmp(b->s, in, in_len) == 0 && b->s[in_len] == '\n')) {
        free(b->s);
        return 0;
    }
    *out = b->s;
    return 0;
}

static void trim(const line_t *l, const char **ts, int *tl) {
    const char *s = l->s;
    int n = l->len;
    while (n > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        n--;
    *ts = s;
    *tl = n;
}

static int contains_ci(const char *s, int len, const char *needle) {
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= len; i++) {
        int k = 0;
        while (k < nl && lower(s[i + k]) == lower(needle[k]))
            k++;
        if (k == nl)
            return 1;
    }
    return 0;
}

const char *find_ci(const char *hay, const char *needle) {
    int nl = (int)strlen(needle);
    for (const char *p = hay; *p; p++) {
        int k = 0;
        while (k < nl && p[k] && lower(p[k]) == lower(needle[k]))
            k++;
        if (k == nl)
            return p;
    }
    return NULL;
}

static int eq_ci(const char *a, int alen, const char *b) {
    int bl = (int)strlen(b);
    if (alen != bl)
        return 0;
    for (int i = 0; i < alen; i++)
        if (lower(a[i]) != lower(b[i]))
            return 0;
    return 1;
}

static int title_index(const char *s, int len, char titles[][TITLE_LEN + 1], int n) {
    for (int i = 0; i < n; i++)
        if (eq_ci(s, len, titles[i]))
            return i;
    return -1;
}

static int is_blank(const line_t *l) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    return tl == 0;
}

// "*TITLE" -> TITLE, or NULL when the line isn't a taiHEN section header
static const char *tai_section(const line_t *l, int *len) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    if (tl < 1 || t[0] != '*')
        return NULL;
    t++;
    tl--;
    while (tl > 0 && (*t == ' ' || *t == '\t')) {
        t++;
        tl--;
    }
    *len = tl;
    return t;
}

static int is_comment(const line_t *l) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    return tl > 0 && t[0] == '#';
}

int tai_install(const char *in, char titles[][TITLE_LEN + 1], int n, const char *plugin_line, char **out) {
    lines_t L;
    sb_t b = {0};
    int found[MAX_TITLES] = {0};
    if (split(in, &L) < 0)
        return -1;
    int ours = -1;
    for (int i = 0; i < L.n; i++) {
        const line_t *l = &L.v[i];
        int sl;
        const char *sec = tai_section(l, &sl);
        if (sec) {
            ours = title_index(sec, sl, titles, n);
            sb_line(&b, l->s, l->len);
            if (ours >= 0 && !found[ours]) {
                found[ours] = 1;
                sb_str(&b, plugin_line);
            }
            continue;
        }
        if (ours >= 0 && !is_comment(l) && contains_ci(l->s, l->len, "divasplit"))
            continue;   // replaced by plugin_line right after the header
        sb_line(&b, l->s, l->len);
    }
    int missing = 0;
    for (int i = 0; i < n; i++)
        missing += !found[i];
    if (missing && !sb_last_blank(&b))
        sb_add(&b, "\n", 1);
    for (int i = 0; i < n; i++) {
        if (found[i])
            continue;
        sb_add(&b, "*", 1);
        sb_str(&b, titles[i]);
        sb_str(&b, plugin_line);
    }
    return finish(in, &L, &b, out);
}

int tai_uninstall(const char *in, char titles[][TITLE_LEN + 1], int n, char **out) {
    lines_t L;
    sb_t b = {0};
    if (split(in, &L) < 0)
        return -1;
    char *drop = calloc(L.n + 1, 1);
    if (!drop) {
        free(L.v);
        return -1;
    }
    for (int i = 0; i < L.n; i++) {
        int sl;
        if (!tai_section(&L.v[i], &sl) && !is_comment(&L.v[i])
                && contains_ci(L.v[i].s, L.v[i].len, "divasplit"))
            drop[i] = 1;
    }
    // our title sections with nothing left in them go too
    for (int i = 0; i < L.n; i++) {
        int sl;
        const char *sec = tai_section(&L.v[i], &sl);
        if (!sec || title_index(sec, sl, titles, n) < 0)
            continue;
        int j = i + 1, empty = 1;
        while (j < L.n && !tai_section(&L.v[j], &sl)) {
            if (!drop[j] && !is_blank(&L.v[j]))
                empty = 0;
            j++;
        }
        if (empty)
            for (int k = i; k < j; k++)
                drop[k] = 1;
    }
    for (int i = 0; i < L.n; i++)
        if (!drop[i])
            sb_line(&b, L.v[i].s, L.v[i].len);
    free(drop);
    return finish(in, &L, &b, out);
}

static int is_kernel_section(const line_t *l) {
    int sl;
    const char *sec = tai_section(l, &sl);
    return sec && eq_ci(sec, sl, "KERNEL");
}

int tai_kernel_has(const char *in, const char *needle) {
    lines_t L;
    if (split(in, &L) < 0)
        return 0;
    int kernel = 0, found = 0;
    for (int i = 0; i < L.n && !found; i++) {
        int sl;
        if (tai_section(&L.v[i], &sl))
            kernel = is_kernel_section(&L.v[i]);
        else if (kernel && !is_comment(&L.v[i]) && contains_ci(L.v[i].s, L.v[i].len, needle))
            found = 1;
    }
    free(L.v);
    return found;
}

int tai_kernel_add(const char *in, const char *line, char **out) {
    lines_t L;
    sb_t b = {0};
    *out = NULL;
    if (tai_kernel_has(in, line))
        return 0;
    if (split(in, &L) < 0)
        return -1;
    int added = 0;
    for (int i = 0; i < L.n; i++) {
        sb_line(&b, L.v[i].s, L.v[i].len);
        if (!added && is_kernel_section(&L.v[i])) {
            sb_str(&b, line);
            added = 1;
        }
    }
    if (!added) {
        sb_t c = {0};
        sb_str(&c, "*KERNEL");
        sb_str(&c, line);
        if (b.s)
            sb_add(&c, b.s, b.len);
        free(b.s);
        b = c;
    }
    return finish(in, &L, &b, out);
}

int tai_kernel_remove(const char *in, const char *needle, char **out) {
    lines_t L;
    sb_t b = {0};
    if (split(in, &L) < 0)
        return -1;
    int kernel = 0;
    for (int i = 0; i < L.n; i++) {
        int sl;
        if (tai_section(&L.v[i], &sl))
            kernel = is_kernel_section(&L.v[i]);
        else if (kernel && !is_comment(&L.v[i]) && contains_ci(L.v[i].s, L.v[i].len, needle))
            continue;
        sb_line(&b, L.v[i].s, L.v[i].len);
    }
    return finish(in, &L, &b, out);
}

/* VitaGrafix */

static int vg_header(const line_t *l, char *title, unsigned long *nid) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    if (tl < 2 || t[0] != '[')
        return 0;
    const char *end = memchr(t, ']', tl);
    if (!end)
        return 0;
    const char *p = t + 1;
    char field[3][40];
    int nf = 0;
    while (p < end && nf < 3) {
        const char *c = memchr(p, ',', end - p);
        const char *fe = c ? c : end;
        while (p < fe && (*p == ' ' || *p == '\t'))
            p++;
        int fl = (int)(fe - p);
        while (fl > 0 && (p[fl - 1] == ' ' || p[fl - 1] == '\t'))
            fl--;
        if (fl > 39)
            fl = 39;
        memcpy(field[nf], p, fl);
        field[nf][fl] = 0;
        nf++;
        p = c ? c + 1 : end;
    }
    if (nf < 1 || strlen(field[0]) > TITLE_LEN)
        return 0;
    strcpy(title, field[0]);
    *nid = nf >= 3 ? strtoul(field[2], NULL, 16) : 0xFFFFFFFFul;
    return 1;
}

int block_titles(const char *block, char titles[][TITLE_LEN + 1], int max) {
    lines_t L;
    int n = 0;
    if (split(block, &L) < 0)
        return 0;
    for (int i = 0; i < L.n; i++) {
        char title[TITLE_LEN + 1];
        unsigned long nid;
        if (!vg_header(&L.v[i], title, &nid) || n >= max)
            continue;
        if (title_index(title, (int)strlen(title), titles, n) < 0)
            strcpy(titles[n++], title);
    }
    free(L.v);
    return n;
}

enum { K_BLANK, K_COMMENT, K_HEADER, K_OTHER };

static int kind(const line_t *l) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    if (tl == 0)
        return K_BLANK;
    if (t[0] == '#')
        return K_COMMENT;
    if (t[0] == '[')
        return K_HEADER;
    return K_OTHER;
}

int vg_patch_replace(const char *in, const char *block, char **out) {
    lines_t L, B;
    sb_t b = {0};
    if (split(block, &B) < 0)
        return -1;
    // the block's own keys
    char keys_t[16][TITLE_LEN + 1];
    unsigned long keys_n[16];
    int nkeys = 0;
    int blen = B.n;
    while (blen > 0 && is_blank(&B.v[blen - 1]))
        blen--;
    for (int i = 0; i < blen && nkeys < 16; i++)
        if (vg_header(&B.v[i], keys_t[nkeys], &keys_n[nkeys]))
            nkeys++;
    if (split(in, &L) < 0) {
        free(B.v);
        return -1;
    }
    int *k = malloc(sizeof(int) * (L.n + 1));
    if (!k) {
        free(B.v);
        free(L.v);
        return -1;
    }
    for (int i = 0; i < L.n; i++)
        k[i] = kind(&L.v[i]);

    int inserted = 0;
    int i = 0;
    // block starts: a run of headers, plus the comment lines right above it
    while (i < L.n) {
        int g = i;
        while (g < L.n && !(k[g] == K_HEADER && (g == 0 || k[g - 1] != K_HEADER)))
            g++;
        int start = g;
        while (start > i && k[start - 1] == K_COMMENT)
            start--;
        // lines before this block (preamble or the previous block's tail) stay
        for (int j = i; j < start; j++)
            sb_line(&b, L.v[j].s, L.v[j].len);
        if (g >= L.n)
            break;
        int hend = g;
        while (hend < L.n && k[hend] == K_HEADER)
            hend++;
        // the block ends where the next block's lead comments or headers begin
        int next = hend;
        while (next < L.n && !(k[next] == K_HEADER))
            next++;
        int end = next;
        while (end > hend && k[end - 1] == K_COMMENT)
            end--;

        int matched = 0;
        char match[64] = {0};
        for (int j = g; j < hend; j++) {
            char t[TITLE_LEN + 1];
            unsigned long nid;
            if (!vg_header(&L.v[j], t, &nid))
                continue;
            for (int q = 0; q < nkeys; q++) {
                if (eq_ci(t, (int)strlen(t), keys_t[q]) && nid == keys_n[q]) {
                    if (j - g < 64)
                        match[j - g] = 1;
                    matched++;
                    break;
                }
            }
        }
        if (matched && !inserted) {
            for (int j = 0; j < blen; j++)
                sb_line(&b, B.v[j].s, B.v[j].len);
            sb_add(&b, "\n", 1);
            inserted = 1;
        }
        if (matched < hend - g) {
            for (int j = start; j < end; j++)
                if (!(j >= g && j < hend && j - g < 64 && match[j - g]))
                    sb_line(&b, L.v[j].s, L.v[j].len);
        }
        i = end;
    }
    if (!inserted) {
        if (!sb_last_blank(&b))
            sb_add(&b, "\n", 1);
        for (int j = 0; j < blen; j++)
            sb_line(&b, B.v[j].s, B.v[j].len);
    }
    free(k);
    free(B.v);
    return finish(in, &L, &b, out);
}

// "[TITLE]" -> TITLE
static const char *ini_section(const line_t *l, int *len) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    if (tl < 2 || t[0] != '[')
        return NULL;
    const char *e = memchr(t, ']', tl);
    if (!e)
        return NULL;
    const char *s = t + 1;
    int n = (int)(e - s);
    while (n > 0 && (*s == ' ' || *s == '\t')) {
        s++;
        n--;
    }
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        n--;
    *len = n;
    return s;
}

static int is_fps_line(const line_t *l) {
    const char *t;
    int tl;
    trim(l, &t, &tl);
    if (tl < 4 || lower(t[0]) != 'f' || lower(t[1]) != 'p' || lower(t[2]) != 's')
        return 0;
    int p = 3;
    while (p < tl && (t[p] == ' ' || t[p] == '\t'))
        p++;
    return p < tl && t[p] == '=';
}

int vg_config_fps(const char *in, char titles[][TITLE_LEN + 1], int n, char **out) {
    lines_t L;
    sb_t b = {0};
    int found[MAX_TITLES] = {0};
    if (split(in, &L) < 0)
        return -1;
    int i = 0;
    while (i < L.n) {
        int sl;
        const char *sec = ini_section(&L.v[i], &sl);
        int t = sec ? title_index(sec, sl, titles, n) : -1;
        sb_line(&b, L.v[i].s, L.v[i].len);
        i++;
        if (t < 0)
            continue;
        found[t] = 1;
        int end = i;
        while (end < L.n && !ini_section(&L.v[end], &sl))
            end++;
        int fps = -1;
        for (int j = i; j < end; j++)
            if (is_fps_line(&L.v[j]))
                fps = j;
        if (fps < 0)
            sb_str(&b, "FPS=60");
        for (int j = i; j < end; j++) {
            if (j == fps)
                sb_str(&b, "FPS=60");
            else if (!is_fps_line(&L.v[j]))
                sb_line(&b, L.v[j].s, L.v[j].len);
        }
        i = end;
    }
    for (int t = 0; t < n; t++) {
        if (found[t])
            continue;
        if (!sb_last_blank(&b))
            sb_add(&b, "\n", 1);
        sb_add(&b, "[", 1);
        sb_add(&b, titles[t], strlen(titles[t]));
        sb_str(&b, "]");
        sb_str(&b, "FPS=60");
    }
    return finish(in, &L, &b, out);
}
