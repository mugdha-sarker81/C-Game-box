/*
 * puzzle_game.c - Sliding Image Puzzle (classic 8-puzzle) for a terminal Game Box.
 *
 * Highlights
 *  - 3x3 board, 8 image tiles + 1 blank, real pictures drawn with 24-bit
 *    half-block characters (falls back to 256 colours automatically).
 *  - Shuffles are NOT random permutations.  A breadth-first search from the
 *    solved board (all legal slides) gives the exact optimal solution length
 *    ("par") of every reachable position, so every level picks a position
 *    whose optimal solution is within a fixed range.  Always solvable, and
 *    higher levels really are harder.
 *  - 6 levels, moves counter, timer, target preview, hint numbers, star
 *    rating, persistent best scores, win screen.
 *  - Any *.ppm (P6) dropped into assets/puzzle/ becomes a selectable image.
 *
 * Public API: puzzle_game_run()  (see puzzle_game.h)
 * Build standalone:  cc -O2 -DPUZZLE_STANDALONE puzzle_game.c -o puzzle
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "puzzle_game.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#else
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

typedef unsigned char u8;

/* ======================================================================== */
/*  Small utilities: output buffer, colours, terminal                        */
/* ======================================================================== */

typedef struct { char *d; size_t n, cap; } Buf;

static void buf_puts(Buf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->n + l + 1 > b->cap) {
        b->cap = (b->n + l + 1) * 2;
        b->d = realloc(b->d, b->cap);
    }
    memcpy(b->d + b->n, s, l);
    b->n += l;
    b->d[b->n] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    buf_puts(b, tmp);
}

static void buf_flush(Buf *b)
{
    size_t off = 0;
    while (off < b->n) {
#ifdef _WIN32
        DWORD written;
        DWORD chunk = b->n - off > 0xFFFFFFFFu ? 0xFFFFFFFFu : (DWORD)(b->n - off);
        if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), b->d + off, chunk, &written, NULL) || !written) break;
        off += written;
#else
        ssize_t w = write(STDOUT_FILENO, b->d + off, b->n - off);
        if (w <= 0) break;
        off += (size_t)w;
#endif
    }
    b->n = 0;
}

static int g_truecolor = 1;

static const u8 COL_LCD_DARK[3]  = { 15, 56, 15 };
static const u8 COL_LCD_LIGHT[3] = { 155, 188, 15 };
static const u8 COL_FRAME[3]     = { 88, 98, 130 };
static const u8 COL_TEXT[3]      = { 225, 228, 240 };
static const u8 COL_DIM[3]       = { 130, 136, 160 };
static const u8 COL_GOLD[3]      = { 255, 215, 80 };
static const u8 COL_WHITE[3]     = { 255, 255, 255 };
static const u8 COL_BLACK[3]     = { 0, 0, 0 };
static const u8 COL_GOOD[3]      = { 120, 235, 150 };

static int c256(const u8 *c)
{
    return 16 + 36 * ((c[0] + 25) / 51) + 6 * ((c[1] + 25) / 51) + (c[2] + 25) / 51;
}

static void set_fg(Buf *b, const u8 *c)
{
    if (g_truecolor) buf_printf(b, "\033[38;2;%d;%d;%dm", c[0], c[1], c[2]);
    else buf_printf(b, "\033[38;5;%dm", c256(c));
}

static void set_bg(Buf *b, const u8 *c)
{
    if (g_truecolor) buf_printf(b, "\033[48;2;%d;%d;%dm", c[0], c[1], c[2]);
    else buf_printf(b, "\033[48;5;%dm", c256(c));
}

static void put_at(Buf *b, int row, int col) { buf_printf(b, "\033[%d;%dH", row, col); }

static int utf8_len(const char *s)
{
    int n = 0;
    for (; *s; s++) if (((u8)*s & 0xC0) != 0x80) n++;
    return n;
}

/* Text at (row,col), padded with spaces up to `width` columns. */
static void put_text(Buf *b, int row, int col, const u8 *fg, const u8 *bg,
                     int bold, const char *s, int width)
{
    put_at(b, row, col);
    if (bg) set_bg(b, bg);
    if (fg) set_fg(b, fg);
    if (bold) buf_puts(b, "\033[1m");
    buf_puts(b, s);
    for (int i = utf8_len(s); i < width; i++) buf_puts(b, " ");
    buf_puts(b, "\033[0m");
}

static void term_size(int *rows, int *cols)
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        *cols = info.srWindow.Right - info.srWindow.Left + 1;
    } else {
        *rows = 24; *cols = 80;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row; *cols = ws.ws_col;
    } else { *rows = 24; *cols = 80; }
#endif
}

#ifdef _WIN32
static HANDLE g_console_out = INVALID_HANDLE_VALUE;
static DWORD g_old_console_mode;
static UINT g_old_output_cp;
static int g_console_mode_saved = 0;
static int g_output_cp_saved = 0;
#else
static struct termios g_old_tio;
#endif
static int g_raw_on = 0;

static void term_restore(void)
{
    if (!g_raw_on) return;
    const char *s = "\033[0m\033[?25h\033[?1049l";
#ifdef _WIN32
    DWORD written;
    WriteFile(g_console_out, s, (DWORD)strlen(s), &written, NULL);
    if (g_console_mode_saved) SetConsoleMode(g_console_out, g_old_console_mode);
    if (g_output_cp_saved) SetConsoleOutputCP(g_old_output_cp);
    g_console_mode_saved = 0;
    g_output_cp_saved = 0;
#else
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* ignore */ }
    tcsetattr(STDIN_FILENO, TCSANOW, &g_old_tio);
#endif
    g_raw_on = 0;
}

static void on_signal(int sig)
{
    term_restore();
    _exit(128 + sig);
}

static int term_raw(void)
{
    const char *ct = getenv("COLORTERM");
    g_truecolor = (ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"))) ? 1 : 0;
    if (!ct) g_truecolor = 1; /* most modern terminals; set COLORTERM=256 to force fallback */
    if (ct && strcmp(ct, "256") == 0) g_truecolor = 0;
#ifdef _WIN32
    g_console_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_console_out == INVALID_HANDLE_VALUE || !GetConsoleMode(g_console_out, &g_old_console_mode)) return 0;
    g_console_mode_saved = 1;
    g_old_output_cp = GetConsoleOutputCP();
    g_output_cp_saved = g_old_output_cp != 0;
    if (!SetConsoleOutputCP(CP_UTF8) ||
        !SetConsoleMode(g_console_out, g_old_console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        if (g_output_cp_saved) SetConsoleOutputCP(g_old_output_cp);
        g_console_mode_saved = 0;
        g_output_cp_saved = 0;
        return 0;
    }
    g_raw_on = 1;
    DWORD written;
    const char *s = "\033[?1049h\033[?25l\033[2J";
    if (!WriteFile(g_console_out, s, (DWORD)strlen(s), &written, NULL)) {
        term_restore();
        return 0;
    }
#else
    struct termios t;
    tcgetattr(STDIN_FILENO, &g_old_tio);
    t = g_old_tio;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    g_raw_on = 1;
    const char *s = "\033[?1049h\033[?25l\033[2J";
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* ignore */ }
#endif
    return 1;
}

static void screen_clear(void)
{
    const char *s = "\033[0m\033[2J\033[H";
#ifdef _WIN32
    DWORD written;
    WriteFile(g_console_out, s, (DWORD)strlen(s), &written, NULL);
#else
    if (write(STDOUT_FILENO, s, strlen(s)) < 0) { /* ignore */ }
#endif
}

enum { K_NONE = 0, K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_ESC };

static int read_byte(int timeout_ms)
{
#ifdef _WIN32
    DWORD started = GetTickCount();
    while (!_kbhit()) {
        if ((DWORD)(GetTickCount() - started) >= (DWORD)timeout_ms) return -1;
        Sleep(1);
    }
    return _getch();
#else
    fd_set fs;
    struct timeval tv;
    FD_ZERO(&fs);
    FD_SET(STDIN_FILENO, &fs);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(STDIN_FILENO + 1, &fs, NULL, NULL, &tv) <= 0) return -1;
    u8 c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return c;
#endif
}

/* Returns a K_* code, a lower-cased ASCII char, or K_NONE on timeout. */
static int read_key(int timeout_ms)
{
    int c = read_byte(timeout_ms);
    if (c < 0) return K_NONE;
#ifdef _WIN32
    if (c == 0 || c == 224) {
        switch (read_byte(40)) {
            case 72: return K_UP;
            case 80: return K_DOWN;
            case 77: return K_RIGHT;
            case 75: return K_LEFT;
            default: return K_NONE;
        }
    }
    if (c == 27) return K_ESC;
#else
    if (c == 27) {
        int c2 = read_byte(40);
        if (c2 < 0) return K_ESC;
        if (c2 != '[' && c2 != 'O') return K_ESC;
        int fin = 0, c3;
        while ((c3 = read_byte(40)) >= 0) {
            if (c3 >= 0x40 && c3 <= 0x7E) { fin = c3; break; }
        }
        switch (fin) {
            case 'A': return K_UP;
            case 'B': return K_DOWN;
            case 'C': return K_RIGHT;
            case 'D': return K_LEFT;
            default:  return K_NONE;
        }
    }
#endif
    if (c == '\r' || c == '\n') return K_ENTER;
    return tolower(c);
}

static double now_sec(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

/* ======================================================================== */
/*  Images: PPM loading, scaling, image list                                 */
/* ======================================================================== */

typedef struct { int w, h; u8 *rgb; } Image;

typedef struct {
    char file[64];
    char label[32];
    Image img;
} PuzzleImage;

static PuzzleImage g_images[PUZZLE_MAX_IMAGES];
static int g_nimg = 0;

static int ppm_int(FILE *f)
{
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == EOF) return -1;
        if (c == '#') { while ((c = fgetc(f)) != '\n' && c != EOF) {} continue; }
        if (!isspace(c)) break;
    }
    int v = 0;
    if (!isdigit(c)) return -1;
    while (isdigit(c)) { v = v * 10 + (c - '0'); c = fgetc(f); }
    return v;
}

static int ppm_load(const char *path, Image *im)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fgetc(f) != 'P' || fgetc(f) != '6') { fclose(f); return 0; }
    int w = ppm_int(f), h = ppm_int(f), mv = ppm_int(f);
    if (w < 3 || h < 3 || w > 4096 || h > 4096 || mv < 1 || mv > 255) { fclose(f); return 0; }
    size_t n = (size_t)w * h * 3;
    u8 *p = malloc(n);
    if (!p || fread(p, 1, n, f) != n) { free(p); fclose(f); return 0; }
    fclose(f);
    if (mv != 255) for (size_t i = 0; i < n; i++) p[i] = (u8)(p[i] * 255 / mv);
    im->w = w; im->h = h; im->rgb = p;
    return 1;
}

/* Box-filter resample of `src` (centre-cropped to the aspect tw:th) into out. */
static void scale_image(const Image *src, int tw, int th, u8 *out)
{
    int cw, ch;
    if ((long)src->w * th > (long)src->h * tw) { ch = src->h; cw = (int)((long)src->h * tw / th); }
    else { cw = src->w; ch = (int)((long)src->w * th / tw); }
    int ox = (src->w - cw) / 2, oy = (src->h - ch) / 2;
    for (int y = 0; y < th; y++) {
        int y0 = oy + (int)((long)y * ch / th), y1 = oy + (int)((long)(y + 1) * ch / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = ox + (int)((long)x * cw / tw), x1 = ox + (int)((long)(x + 1) * cw / tw);
            if (x1 <= x0) x1 = x0 + 1;
            long s[3] = { 0, 0, 0 };
            int cnt = 0;
            for (int yy = y0; yy < y1 && yy < src->h; yy++)
                for (int xx = x0; xx < x1 && xx < src->w; xx++) {
                    const u8 *p = src->rgb + ((size_t)yy * src->w + xx) * 3;
                    s[0] += p[0]; s[1] += p[1]; s[2] += p[2]; cnt++;
                }
            if (!cnt) cnt = 1;
            u8 *o = out + ((size_t)y * tw + x) * 3;
            o[0] = (u8)(s[0] / cnt); o[1] = (u8)(s[1] / cnt); o[2] = (u8)(s[2] / cnt);
        }
    }
}

static const struct { const char *key, *label; } THEMES[] = {
    { "sakura", "Nature" }, { "space", "Space" }, { "supercar", "Supercar" },
    { "character", "Anime" }, { "city", "City" }, { "wildlife", "Wildlife" },
    { "technology", "Gaming & Tech" },
};
#define N_THEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

static int theme_index(const char *file)
{
    for (int i = 0; i < N_THEMES; i++) if (strcmp(file, THEMES[i].key) == 0) return i;
    return 100;
}

static int image_cmp(const void *a, const void *b)
{
    const PuzzleImage *x = a, *y = b;
    int tx = theme_index(x->file), ty = theme_index(y->file);
    if (tx != ty) return tx - ty;
    return strcmp(x->file, y->file);
}

static const char *asset_dir(void)
{
    const char *e = getenv("PUZZLE_ASSETS");
    return (e && *e) ? e : PUZZLE_ASSET_DIR;
}

static void image_add(const char *name)
{
    size_t l = strlen(name);
    if (g_nimg >= PUZZLE_MAX_IMAGES || l < 5 || l > 60 ||
        strcmp(name + l - 4, ".ppm") != 0) return;

    char path[512];
#ifdef _WIN32
    snprintf(path, sizeof path, "%s\\%s", asset_dir(), name);
#else
    snprintf(path, sizeof path, "%s/%s", asset_dir(), name);
#endif
    PuzzleImage *pi = &g_images[g_nimg];
    if (!ppm_load(path, &pi->img)) return;
    snprintf(pi->file, sizeof pi->file, "%.*s", (int)(l - 4), name);
    int ti = theme_index(pi->file);
    if (ti < N_THEMES) snprintf(pi->label, sizeof pi->label, "%s", THEMES[ti].label);
    else {
        size_t fl = strlen(pi->file);
        if (fl > sizeof pi->label - 1) fl = sizeof pi->label - 1;
        memcpy(pi->label, pi->file, fl);
        pi->label[fl] = 0;
        for (char *c = pi->label; *c; c++) if (*c == '_' || *c == '-') *c = ' ';
        pi->label[0] = (char)toupper((u8)pi->label[0]);
    }
    g_nimg++;
}

static int images_scan(void)
{
    if (g_nimg) return g_nimg;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s\\*.ppm", asset_dir());
    for (char *c = pattern; *c; c++) if (*c == '/') *c = '\\';
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        image_add(data.cFileName);
    } while (g_nimg < PUZZLE_MAX_IMAGES && FindNextFileA(find, &data));
    FindClose(find);
#else
    DIR *d = opendir(asset_dir());
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_nimg < PUZZLE_MAX_IMAGES) image_add(e->d_name);
    closedir(d);
#endif
    qsort(g_images, (size_t)g_nimg, sizeof g_images[0], image_cmp);
    return g_nimg;
}

/* ======================================================================== */
/*  Puzzle logic: solver tables, levels, board, movement, solvability        */
/* ======================================================================== */

#define MAXDIST 31
static const struct { const char *name; int lo, hi; } LEVELS[PUZZLE_LEVELS] = {
    { "Easy",     4,  8 },
    { "Medium",   9, 14 },
    { "Hard",    15, 19 },
    { "Advanced",20, 24 },
    { "Expert",  25, 27 },
    { "Master",  28, 31 },
};

static unsigned char *g_dist;   /* optimal solution length for each permutation rank */
static int *g_order;            /* reachable ranks, sorted by distance (BFS order)   */
static int g_count;
static int g_start[MAXDIST + 3];/* first index in g_order for each distance          */

static int perm_rank(const int *p)
{
    int r = 0;
    for (int i = 0; i < 9; i++) {
        int c = 0;
        for (int j = i + 1; j < 9; j++) if (p[j] < p[i]) c++;
        r = r * (9 - i) + c;
    }
    return r;
}

static void perm_unrank(int r, int *p)
{
    int c[9], used[9] = { 0 };
    for (int i = 8; i >= 0; i--) { int radix = 9 - i; c[i] = r % radix; r /= radix; }
    for (int i = 0; i < 9; i++) {
        int k = c[i];
        for (int v = 0; v < 9; v++) {
            if (used[v]) continue;
            if (k == 0) { p[i] = v; used[v] = 1; break; }
            k--;
        }
    }
}

static const int DR[4] = { -1, 1, 0, 0 };   /* UP, DOWN, LEFT, RIGHT */
static const int DC[4] = { 0, 0, -1, 1 };
enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* BFS over the 181,440 reachable positions, starting from the solved board. */
static void solver_init(void)
{
    if (g_dist) return;
    g_dist = malloc(362880);
    g_order = malloc(181440 * sizeof(int));
    memset(g_dist, 255, 362880);
    int solved[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 0 };
    int head = 0, tail = 0;
    int s0 = perm_rank(solved);
    g_dist[s0] = 0;
    g_order[tail++] = s0;
    while (head < tail) {
        int s = g_order[head++], p[9];
        perm_unrank(s, p);
        int b = 0;
        while (p[b] != 0) b++;
        int br = b / 3, bc = b % 3;
        for (int d = 0; d < 4; d++) {
            int nr = br + DR[d], nc = bc + DC[d];
            if (nr < 0 || nr > 2 || nc < 0 || nc > 2) continue;
            int n = nr * 3 + nc;
            p[b] = p[n]; p[n] = 0;
            int r = perm_rank(p);
            if (g_dist[r] == 255) { g_dist[r] = (unsigned char)(g_dist[s] + 1); g_order[tail++] = r; }
            p[n] = p[b]; p[b] = 0;
        }
    }
    g_count = tail;
    for (int d = 0; d <= MAXDIST + 2; d++) g_start[d] = g_count;
    for (int i = g_count - 1; i >= 0; i--) g_start[g_dist[g_order[i]]] = i;
    for (int d = MAXDIST + 1; d >= 0; d--) if (g_start[d] > g_start[d + 1]) g_start[d] = g_start[d + 1];
}

typedef struct {
    int tile[9];        /* 0 = empty, 1..8 = image pieces (piece k belongs at index k-1) */
    int start_tile[9];  /* shuffled layout, used by "Restart"                            */
    int blank;
    int moves;
    int level;          /* 0..5 */
    int img;
    int par;            /* optimal number of moves for the current shuffle */
    int started, won;
    double t0, t_final;
    int hints;
    /* cached scaled pictures */
    u8 *scaled, *winimg;
    int cache_tp, cache_img;
} Game;

static void board_init(Game *g)             /* solved board */
{
    for (int i = 0; i < 8; i++) g->tile[i] = i + 1;
    g->tile[8] = 0;
    g->blank = 8;
}

static int board_is_solved(const Game *g)
{
    for (int i = 0; i < 8; i++) if (g->tile[i] != i + 1) return 0;
    return 1;
}

/* Parity test: a 3x3 position is solvable iff the inversion count is even. */
#ifdef PUZZLE_STANDALONE
static int board_is_solvable(const int *tile)
{
    int inv = 0;
    for (int i = 0; i < 9; i++)
        for (int j = i + 1; j < 9; j++)
            if (tile[i] && tile[j] && tile[i] > tile[j]) inv++;
    return inv % 2 == 0;
}
#endif

/* Slide the tile that is next to the empty space in the direction `dir`
 * (so "up" moves the tile below the gap up into it).  Returns 1 if moved. */
static int board_move(Game *g, int dir)
{
    int br = g->blank / 3, bc = g->blank % 3;
    int tr = br - DR[dir], tc = bc - DC[dir];
    if (tr < 0 || tr > 2 || tc < 0 || tc > 2) return 0;
    int t = tr * 3 + tc;
    g->tile[g->blank] = g->tile[t];
    g->tile[t] = 0;
    g->blank = t;
    return 1;
}

static void level_range(int level, int *lo, int *hi)
{
    *lo = LEVELS[level].lo;
    *hi = LEVELS[level].hi;
}

/* Pick a random reachable position whose optimal solution length is in the
 * level's range - i.e. "valid moves from the solved board", at least lo deep. */
static void board_shuffle(Game *g)
{
    solver_init();
    int lo, hi;
    level_range(g->level, &lo, &hi);
    int a = g_start[lo], b = g_start[hi + 1];
    int idx = a + rand() % (b - a);
    perm_unrank(g_order[idx], g->tile);
    for (g->blank = 0; g->tile[g->blank] != 0; g->blank++) {}
    g->par = g_dist[g_order[idx]];
    memcpy(g->start_tile, g->tile, sizeof g->tile);
    g->moves = 0; g->started = 0; g->won = 0; g->t0 = g->t_final = 0;
}

static void game_restart(Game *g)
{
    memcpy(g->tile, g->start_tile, sizeof g->tile);
    for (g->blank = 0; g->tile[g->blank] != 0; g->blank++) {}
    g->moves = 0; g->started = 0; g->won = 0;
}

static void game_new(Game *g, int img, int level)
{
    free(g->scaled); free(g->winimg);
    memset(g, 0, sizeof *g);
    g->img = img; g->level = level; g->cache_img = -1;
    board_init(g);
    board_shuffle(g);
}

static void game_new_same(Game *g)
{
    int hints = g->hints;
    int img = g->img, level = g->level;
    game_new(g, img, level);
    g->hints = hints;
}

static int game_seconds(const Game *g)
{
    if (!g->started) return 0;
    double t = g->won ? g->t_final : now_sec() - g->t0;
    return (int)(t + 0.0001);
}

static int game_stars(const Game *g)
{
    if (g->moves <= g->par + g->par / 2 + 2) return 3;
    if (g->moves <= g->par * 3) return 2;
    return 1;
}

/* ---- best scores (persistent) ------------------------------------------ */

typedef struct { char name[32]; int level, moves, secs; } Best;
static Best g_best[256];
static int g_nbest = 0, g_best_loaded = 0;
#define BEST_FILE "puzzle_best.txt"

static void best_load(void)
{
    if (g_best_loaded) return;
    g_best_loaded = 1;
    FILE *f = fopen(BEST_FILE, "r");
    if (!f) return;
    while (g_nbest < 256 && fscanf(f, "%31s %d %d %d", g_best[g_nbest].name, &g_best[g_nbest].level,
                                   &g_best[g_nbest].moves, &g_best[g_nbest].secs) == 4)
        g_nbest++;
    fclose(f);
}

static Best *best_find(const char *name, int level)
{
    best_load();
    for (int i = 0; i < g_nbest; i++)
        if (g_best[i].level == level && strcmp(g_best[i].name, name) == 0) return &g_best[i];
    return NULL;
}

/* Returns 1 when this result is a new record. */
static int best_update(const char *name, int level, int moves, int secs)
{
    Best *b = best_find(name, level);
    int record = 0;
    if (!b) {
        if (g_nbest >= 256) return 0;
        b = &g_best[g_nbest++];
        snprintf(b->name, sizeof b->name, "%s", name);
        b->level = level; b->moves = moves; b->secs = secs;
        record = 1;
    } else if (moves < b->moves || (moves == b->moves && secs < b->secs)) {
        b->moves = moves; b->secs = secs;
        record = 1;
    }
    if (record) {
        FILE *f = fopen(BEST_FILE, "w");
        if (f) {
            for (int i = 0; i < g_nbest; i++)
                fprintf(f, "%s %d %d %d\n", g_best[i].name, g_best[i].level, g_best[i].moves, g_best[i].secs);
            fclose(f);
        }
    }
    return record;
}

/* ======================================================================== */
/*  Rendering                                                                */
/* ======================================================================== */

typedef void (*PixFn)(const void *ctx, int x, int y, u8 *rgb);

/* Draw a w x h pixel canvas with half-block characters at text row/col. */
static void emit_pixels(Buf *b, int row, int col, int w, int h, PixFn fn, const void *ctx)
{
    for (int y = 0; y < h; y += 2) {
        put_at(b, row + y / 2, col);
        u8 pf[3] = { 0, 0, 0 }, pb[3] = { 0, 0, 0 };
        int have = 0;
        for (int x = 0; x < w; x++) {
            u8 t[3], bt[3];
            fn(ctx, x, y, t);
            fn(ctx, x, y + 1, bt);
            if (!have || memcmp(t, pf, 3) || memcmp(bt, pb, 3)) {
                set_fg(b, t); set_bg(b, bt);
                memcpy(pf, t, 3); memcpy(pb, bt, 3);
                have = 1;
            }
            buf_puts(b, "\xE2\x96\x80");          /* U+2580 upper half block */
        }
        buf_puts(b, "\033[0m");
    }
}

/* ---- board canvas ------------------------------------------------------- */

typedef struct {
    const Game *g;
    int tp;
    int win;
} BoardCtx;

#define PV 16   /* preview size in pixels */

static void clamp_add(u8 *c, int d)
{
    for (int i = 0; i < 3; i++) { int v = c[i] + d; c[i] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }
}

static void board_px(const void *vc, int x, int y, u8 *o)
{
    const BoardCtx *c = vc;
    const Game *g = c->g;
    int tp = c->tp;
    int iw = 3 * tp + 2, ih = 3 * tp + 4;
    int sx = x - 1, sy = y - 2;
    if (sx < 0 || sy < 0 || sx >= iw || sy >= ih) { memcpy(o, COL_FRAME, 3); return; }
    if (c->win) { memcpy(o, g->winimg + ((size_t)sy * iw + sx) * 3, 3); return; }
    int px = tp + 1, py = tp + 2;
    int tc = sx / px, u = sx % px, tr = sy / py, v = sy % py;
    if (u >= tp || v >= tp) { o[0] = 14; o[1] = 16; o[2] = 26; return; }
    int t = g->tile[tr * 3 + tc];
    if (t == 0) {
        int chk = ((u / 2) + (v / 2)) & 1;
        o[0] = chk ? 30 : 22; o[1] = chk ? 33 : 24; o[2] = chk ? 48 : 36;
        return;
    }
    int sr = (t - 1) / 3, scn = (t - 1) % 3;
    memcpy(o, g->scaled + ((size_t)(sr * tp + v) * (3 * tp) + (scn * tp + u)) * 3, 3);
    if (u == 0 || v == 0) clamp_add(o, 28);
    else if (u == tp - 1 || v == tp - 1) clamp_add(o, -38);
}

typedef struct { const u8 *pv; } PvCtx;

static void pv_px(const void *vc, int x, int y, u8 *o)
{
    const PvCtx *c = vc;
    if (x < 1 || x >= PV + 1 || y < 2 || y >= PV + 2) { memcpy(o, COL_FRAME, 3); return; }
    memcpy(o, c->pv + ((size_t)(y - 2) * PV + (x - 1)) * 3, 3);
}

static void game_cache(Game *g, int tp)
{
    if (g->cache_tp == tp && g->cache_img == g->img && g->scaled) return;
    free(g->scaled); free(g->winimg);
    g->scaled = malloc((size_t)(3 * tp) * (3 * tp) * 3);
    g->winimg = malloc((size_t)(3 * tp + 2) * (3 * tp + 4) * 3);
    scale_image(&g_images[g->img].img, 3 * tp, 3 * tp, g->scaled);
    scale_image(&g_images[g->img].img, 3 * tp + 2, 3 * tp + 4, g->winimg);
    g->cache_tp = tp; g->cache_img = g->img;
}

static int choose_tp(int rows, int cols)
{
    return (rows >= 33 && cols >= 72) ? 14 : 10;
}

static void fmt_time(char *out, size_t n, int secs)
{
    if (secs > 5999) secs = 5999;
    snprintf(out, n, "%02d:%02d", secs / 60, secs % 60);
}

static void draw_bar(Buf *b, int row, int col, int width, const u8 *fg, const u8 *bg,
                     int bold, const char *s, int center)
{
    char line[256];
    int len = utf8_len(s);
    int left = center ? (width - len) / 2 : 1;
    if (left < 0) left = 0;
    snprintf(line, sizeof line, "%*s%s", left, "", s);
    put_text(b, row, col, fg, bg, bold, line, width);
}

static void draw_game(Game *g, int blink)
{
    static int last_rows, last_cols;
    int rows, cols;
    term_size(&rows, &cols);
    int tp = choose_tp(rows, cols);
    if (rows != last_rows || cols != last_cols) { screen_clear(); last_rows = rows; last_cols = cols; }
    game_cache(g, tp);

    int bw = 3 * tp + 4, bh = 3 * tp + 8, brows = bh / 2;
    int pw = 22, cw = bw + 2 + pw;
    int col0 = (cols - cw) / 2 + 1;
    if (col0 < 1) col0 = 1;
    int top = 1;
    int board_row = top + 3;
    int pcol = col0 + bw + 2;

    Buf b = { 0 };
    char s[256], tbuf[16];

    draw_bar(&b, top, col0, cw, COL_LCD_LIGHT, COL_LCD_DARK, 1, "\xE2\x96\xA6  P U Z Z L E   G A M E  \xE2\x96\xA6", 1);
    fmt_time(tbuf, sizeof tbuf, game_seconds(g));
    snprintf(s, sizeof s, "Image: %s   Level: %d %s   Moves: %d   Time: %s",
             g_images[g->img].label, g->level + 1, LEVELS[g->level].name, g->moves, tbuf);
    draw_bar(&b, top + 1, col0, cw, COL_LCD_DARK, COL_LCD_LIGHT, 0, s, 0);
    put_text(&b, top + 2, col0, NULL, NULL, 0, "", cw);

    BoardCtx bc = { g, tp, g->won };
    emit_pixels(&b, board_row, col0, bw, bh, board_px, &bc);

    if (g->hints && !g->won) {
        for (int i = 0; i < 9; i++) {
            if (!g->tile[i]) continue;
            snprintf(s, sizeof s, " %d ", g->tile[i]);
            put_text(&b, board_row + (2 + (i / 3) * (tp + 2)) / 2,
                     col0 + 1 + (i % 3) * (tp + 1), COL_WHITE, COL_BLACK, 1, s, 3);
        }
    }

    /* right panel: target preview */
    u8 pvbuf[PV * PV * 3];
    scale_image(&g_images[g->img].img, PV, PV, pvbuf);
    PvCtx pc = { pvbuf };
    put_text(&b, board_row, pcol, COL_DIM, NULL, 0, " TARGET", pw);
    emit_pixels(&b, board_row + 1, pcol, PV + 2, PV + 4, pv_px, &pc);
    for (int r = 0; r < (PV + 4) / 2; r++) put_text(&b, board_row + 1 + r, pcol + PV + 2, NULL, NULL, 0, "", pw - PV - 2);

    int lr = board_row + 1 + (PV + 4) / 2 + 1;
    const u8 *tx = COL_TEXT;
    if (!g->won) {
        const char *help[] = {
            " Arrows / WASD  Move",
            " N  New game",
            " R  Restart",
            " I  Image & level",
            " L  Level up",
            " H  Hint numbers",
            " Q  Back to menu",
        };
        for (int i = 0; i < 7; i++) put_text(&b, lr + i, pcol, tx, NULL, 0, help[i], pw);
        snprintf(s, sizeof s, " Par: %d moves", g->par);
        put_text(&b, lr + 7, pcol, COL_GOLD, NULL, 0, s, pw);
    } else {
        int secs = game_seconds(g);
        int stars = game_stars(g);
        fmt_time(tbuf, sizeof tbuf, secs);
        Best *bst = best_find(g_images[g->img].file, g->level);
        const u8 *tc = blink ? COL_GOLD : COL_WHITE;
        put_text(&b, lr, pcol, tc, NULL, 1, " \xE2\x98\x85 PUZZLE SOLVED! \xE2\x98\x85", pw);
        snprintf(s, sizeof s, " Moves: %d  (par %d)", g->moves, g->par);
        put_text(&b, lr + 1, pcol, tx, NULL, 0, s, pw);
        snprintf(s, sizeof s, " Time:  %s", tbuf);
        put_text(&b, lr + 2, pcol, tx, NULL, 0, s, pw);
        snprintf(s, sizeof s, " Rating: %s%s%s", "\xE2\x98\x85", stars >= 2 ? "\xE2\x98\x85" : "\xE2\x98\x86",
                 stars >= 3 ? "\xE2\x98\x85" : "\xE2\x98\x86");
        put_text(&b, lr + 3, pcol, COL_GOLD, NULL, 0, s, pw);
        if (bst && bst->moves == g->moves && bst->secs == secs) snprintf(s, sizeof s, " NEW BEST!");
        else if (bst) snprintf(s, sizeof s, " Best: %d moves", bst->moves);
        else s[0] = 0;
        put_text(&b, lr + 4, pcol, COL_GOOD, NULL, 1, s, pw);
        if (g->level < PUZZLE_LEVELS - 1) snprintf(s, sizeof s, " N  Next level (%d)", g->level + 2);
        else snprintf(s, sizeof s, " N  New game");
        put_text(&b, lr + 5, pcol, tx, NULL, 0, s, pw);
        put_text(&b, lr + 6, pcol, tx, NULL, 0, " R  Replay   I  Image", pw);
        put_text(&b, lr + 7, pcol, tx, NULL, 0, " Q  Back to menu", pw);
    }

    int fr = board_row + brows;
    if (fr < lr + 8) fr = lr + 8;       /* small terminals: keep the footer below the side panel */
    if (g->won) snprintf(s, sizeof s, "Well done! The picture is restored.");
    else {
        Best *bst = best_find(g_images[g->img].file, g->level);
        if (bst) snprintf(s, sizeof s, "Slide tiles into the gap.   Best here: %d moves", bst->moves);
        else snprintf(s, sizeof s, "Slide tiles into the gap to rebuild the picture.");
    }
    put_text(&b, fr, col0, COL_DIM, NULL, 0, s, cw);
    buf_flush(&b);
    free(b.d);
}

/* ======================================================================== */
/*  Screens: title menu, image/level picker, how-to                          */
/* ======================================================================== */

static void draw_preview_of(Buf *b, int img, int row, int col)
{
    u8 pvbuf[PV * PV * 3];
    scale_image(&g_images[img].img, PV, PV, pvbuf);
    PvCtx pc = { pvbuf };
    emit_pixels(b, row, col, PV + 2, PV + 4, pv_px, &pc);
}

static int layout_col(int width)
{
    int rows, cols;
    term_size(&rows, &cols);
    int c = (cols - width) / 2 + 1;
    return c < 1 ? 1 : c;
}

/* Returns 1 to start playing, 0 on cancel. Updates *img and *level. */
static int select_image_screen(int *img, int *level)
{
    screen_clear();
    int w = 56, col0 = layout_col(w);
    for (;;) {
        Buf b = { 0 };
        char s[128];
        draw_bar(&b, 1, col0, w, COL_LCD_LIGHT, COL_LCD_DARK, 1, "SELECT IMAGE & LEVEL", 1);
        put_text(&b, 3, col0, COL_DIM, NULL, 0, " Image                      Preview", w);
        for (int i = 0; i < PUZZLE_MAX_IMAGES; i++) {
            int row = 4 + i;
            if (i >= g_nimg) { put_text(&b, row, col0, NULL, NULL, 0, "", 26); continue; }
            snprintf(s, sizeof s, " %s %d. %s", i == *img ? "\xE2\x96\xB6" : " ", i + 1, g_images[i].label);
            if (i == *img) put_text(&b, row, col0, COL_LCD_DARK, COL_LCD_LIGHT, 1, s, 26);
            else put_text(&b, row, col0, COL_TEXT, NULL, 0, s, 26);
        }
        draw_preview_of(&b, *img, 4, col0 + 30);
        int lr = 4 + PV / 2 + 6;
        if (lr < 4 + g_nimg + 1) lr = 4 + g_nimg + 1;
        char pips[160] = "";
        for (int i = 0; i < PUZZLE_LEVELS; i++) strcat(pips, i <= *level ? "\xE2\x97\x8F " : "\xE2\x97\x8B ");
        snprintf(s, sizeof s, " Level  \xE2\x97\x80 %d - %s \xE2\x96\xB6    %s", *level + 1, LEVELS[*level].name, pips);
        put_text(&b, lr, col0, COL_GOLD, NULL, 1, s, w);
        snprintf(s, sizeof s, " Optimal solution: %d-%d moves", LEVELS[*level].lo, LEVELS[*level].hi);
        put_text(&b, lr + 1, col0, COL_TEXT, NULL, 0, s, w);
        Best *bst = best_find(g_images[*img].file, *level);
        if (bst) snprintf(s, sizeof s, " Your best: %d moves", bst->moves); else snprintf(s, sizeof s, " No record yet");
        put_text(&b, lr + 2, col0, COL_DIM, NULL, 0, s, w);
        put_text(&b, lr + 4, col0, COL_DIM, NULL, 0, " \xE2\x86\x91\xE2\x86\x93 image    \xE2\x86\x90\xE2\x86\x92 level    Enter play    Esc back", w);
        buf_flush(&b);
        free(b.d);

        int k = read_key(500);
        switch (k) {
            case K_UP: case 'w': *img = (*img + g_nimg - 1) % g_nimg; break;
            case K_DOWN: case 's': *img = (*img + 1) % g_nimg; break;
            case K_LEFT: case 'a': if (*level > 0) (*level)--; break;
            case K_RIGHT: case 'd': if (*level < PUZZLE_LEVELS - 1) (*level)++; break;
            case K_ENTER: case ' ': return 1;
            case K_ESC: case 'q': case 'b': return 0;
            default:
                if (k >= '1' && k <= '9' && k - '1' < g_nimg) *img = k - '1';
        }
    }
}

static void how_to_screen(void)
{
    screen_clear();
    int w = 60, col0 = layout_col(w);
    Buf b = { 0 };
    draw_bar(&b, 1, col0, w, COL_LCD_LIGHT, COL_LCD_DARK, 1, "HOW TO PLAY", 1);
    const char *t[] = {
        "",
        " The picture is cut into 9 tiles: 8 pieces and 1 gap.",
        " Slide the tiles into the gap until the picture is",
        " rebuilt. Only tiles touching the gap can move.",
        "",
        " Arrow keys / WASD   move the tile in that direction",
        " N new   R restart   I image   L level up   H hints",
        "",
        " Every shuffle is solvable. 'Par' is the fewest moves",
        " that can solve it - beat it for 3 stars!",
        " Level 1 is a quick warm-up, Level 6 needs ~30 moves.",
        "",
        " Put your own pictures (P6 .ppm) in assets/puzzle/.",
        "",
        " Press any key...",
    };
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) put_text(&b, 3 + i, col0, COL_TEXT, NULL, 0, t[i], w);
    buf_flush(&b);
    free(b.d);
    while (read_key(1000) == K_NONE) {}
}

static void play_game(int *img, int *level)
{
    Game g;
    memset(&g, 0, sizeof g);
    game_new(&g, *img, *level);
    screen_clear();
    int blink = 0;
    double last_blink = now_sec();
    for (;;) {
        draw_game(&g, blink);
        int k = read_key(250);
        double t = now_sec();
        if (t - last_blink > 0.4) { blink = !blink; last_blink = t; }
        int dir = -1;
        switch (k) {
            case K_UP: case 'w': dir = DIR_UP; break;
            case K_DOWN: case 's': dir = DIR_DOWN; break;
            case K_LEFT: case 'a': dir = DIR_LEFT; break;
            case K_RIGHT: case 'd': dir = DIR_RIGHT; break;
            case 'h': g.hints = !g.hints; break;
            case 'r': game_restart(&g); break;
            case 'n':
                if (g.won && g.level < PUZZLE_LEVELS - 1) g.level++;
                game_new_same(&g);
                *level = g.level;
                break;
            case 'l':
                g.level = (g.level + 1) % PUZZLE_LEVELS;
                game_new_same(&g);
                *level = g.level;
                break;
            case 'i': {
                int ni = g.img, nl = g.level;
                if (select_image_screen(&ni, &nl)) {
                    g.img = ni; g.level = nl;
                    game_new_same(&g);
                    *img = ni; *level = nl;
                }
                screen_clear();
                break;
            }
            case 'q': case 'b': case K_ESC:
                *img = g.img; *level = g.level;
                free(g.scaled); free(g.winimg);
                return;
            default: break;
        }
        if (dir >= 0 && !g.won && board_move(&g, dir)) {
            if (!g.started) { g.started = 1; g.t0 = now_sec(); }
            g.moves++;
            if (board_is_solved(&g)) {
                g.won = 1;
                g.t_final = now_sec() - g.t0;
                best_update(g_images[g.img].file, g.level, g.moves, game_seconds(&g));
            }
        }
    }
}

void puzzle_game_run(void)
{
#ifdef _WIN32
    srand((unsigned)(time(NULL) ^ (unsigned)GetCurrentProcessId() * 2654435761u));
#else
    srand((unsigned)(time(NULL) ^ (unsigned)getpid() * 2654435761u));
#endif
    if (!images_scan()) {
        fprintf(stderr, "Puzzle: no .ppm images found in '%s'.\n"
                        "Run:  python3 tools/make_images.py\n", asset_dir());
        return;
    }
    solver_init();

    void (*old_int)(int) = signal(SIGINT, on_signal);
    void (*old_term)(int) = signal(SIGTERM, on_signal);
    if (!term_raw()) {
        signal(SIGINT, old_int);
        signal(SIGTERM, old_term);
        fprintf(stderr, "Puzzle: unable to enable Windows Terminal virtual-terminal mode.\n");
        return;
    }

    int img = 0, level = 0, sel = 0;
    const char *items[] = { "New Game", "Select Image & Level", "How to Play", "Back to Game Box" };
    const int nitems = 4;
    screen_clear();
    for (;;) {
        int w = 56, col0 = layout_col(w);
        Buf b = { 0 };
        char s[128];
        draw_bar(&b, 1, col0, w, COL_LCD_LIGHT, COL_LCD_DARK, 1, "\xE2\x96\xA6  P U Z Z L E   G A M E  \xE2\x96\xA6", 1);
        put_text(&b, 2, col0, COL_DIM, NULL, 0, " Slide the tiles. Restore the picture.", w);
        for (int i = 0; i < nitems; i++) {
            snprintf(s, sizeof s, " %s %s", i == sel ? "\xE2\x96\xB6" : " ", items[i]);
            if (i == sel) put_text(&b, 5 + i * 2, col0, COL_LCD_DARK, COL_LCD_LIGHT, 1, s, 28);
            else put_text(&b, 5 + i * 2, col0, COL_TEXT, NULL, 0, s, 28);
        }
        snprintf(s, sizeof s, " Image: %s", g_images[img].label);
        put_text(&b, 15, col0, COL_GOLD, NULL, 0, s, 28);
        snprintf(s, sizeof s, " Level: %d (%s)", level + 1, LEVELS[level].name);
        put_text(&b, 16, col0, COL_GOLD, NULL, 0, s, 28);
        draw_preview_of(&b, img, 4, col0 + 32);
        put_text(&b, 22, col0, COL_DIM, NULL, 0, " \xE2\x86\x91\xE2\x86\x93 choose    Enter select    Q back", w);
        buf_flush(&b);
        free(b.d);

        int k = read_key(1000);
        int choice = -1;
        switch (k) {
            case K_UP: case 'w': sel = (sel + nitems - 1) % nitems; break;
            case K_DOWN: case 's': sel = (sel + 1) % nitems; break;
            case K_ENTER: case ' ': choice = sel; break;
            case 'q': case K_ESC: choice = 3; break;
            case 'n': choice = 0; break;
            default: break;
        }
        if (choice == 0) { play_game(&img, &level); screen_clear(); }
        else if (choice == 1) { int ni = img, nl = level; if (select_image_screen(&ni, &nl)) { img = ni; level = nl; } screen_clear(); }
        else if (choice == 2) { how_to_screen(); screen_clear(); }
        else if (choice == 3) break;
    }

    term_restore();
    signal(SIGINT, old_int);
    signal(SIGTERM, old_term);
}

/* ======================================================================== */
/*  Self test + standalone entry point                                       */
/* ======================================================================== */

#ifdef PUZZLE_STANDALONE
static int selftest(void)
{
    int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else printf("ok:   %s\n", msg); } while (0)
    solver_init();
    CHECK(g_count == 181440, "BFS reaches exactly 181440 positions (= 9!/2)");
    int maxd = 0;
    for (int i = 0; i < g_count; i++) if (g_dist[g_order[i]] > maxd) maxd = g_dist[g_order[i]];
    CHECK(maxd == 31, "hardest position needs 31 moves");

    Game g;
    memset(&g, 0, sizeof g);
    game_new(&g, 0, 0);
    board_init(&g);
    CHECK(board_is_solved(&g) && board_is_solvable(g.tile), "solved board detected and solvable");
    /* gap is bottom-right: nothing below it and nothing to its right */
    CHECK(board_move(&g, DIR_UP) == 0 && board_move(&g, DIR_LEFT) == 0, "no move when no tile is on that side of the gap");
    CHECK(board_move(&g, DIR_DOWN) == 1 && g.tile[8] == 6 && g.blank == 5, "DOWN slides the tile above the gap downward");
    CHECK(board_move(&g, DIR_UP) == 1 && board_is_solved(&g), "UP undoes it (tile below the gap moves up)");
    CHECK(board_move(&g, DIR_RIGHT) == 1 && g.tile[8] == 8 && board_move(&g, DIR_LEFT) == 1 && board_is_solved(&g),
          "RIGHT / LEFT slide correctly and are reversible");

    int bad = 0, outrange = 0, solved_now = 0, par_ok = 0;
    for (int lv = 0; lv < PUZZLE_LEVELS; lv++) {
        for (int n = 0; n < 2000; n++) {
            Game t;
            memset(&t, 0, sizeof t);
            t.level = lv;
            board_init(&t);
            board_shuffle(&t);
            if (!board_is_solvable(t.tile)) bad++;
            if (t.par < LEVELS[lv].lo || t.par > LEVELS[lv].hi) outrange++;
            if (board_is_solved(&t)) solved_now++;
            /* walking back greedily along decreasing distance must reach solved in exactly par moves */
            Game w = t;
            int steps = 0;
            while (!board_is_solved(&w) && steps < 40) {
                int cur = g_dist[perm_rank(w.tile)], moved = 0;
                for (int d = 0; d < 4 && !moved; d++) {
                    Game c = w;
                    if (board_move(&c, d) && g_dist[perm_rank(c.tile)] == cur - 1) { w = c; moved = 1; }
                }
                if (!moved) break;
                steps++;
            }
            if (board_is_solved(&w) && steps == t.par) par_ok++;
        }
    }
    CHECK(bad == 0, "12000 shuffles are all solvable (parity)");
    CHECK(outrange == 0, "every shuffle's optimal length is inside its level range");
    CHECK(solved_now == 0, "no shuffle starts already solved");
    CHECK(par_ok == 12000, "a par-length solution exists for every shuffle");

    PuzzleImage *pi;
    CHECK(images_scan() >= 7, "7+ images loaded from assets/puzzle");
    int ok = 1;
    for (int i = 0; i < g_nimg; i++) { pi = &g_images[i]; if (pi->img.w < 48 || !pi->img.rgb) ok = 0; }
    CHECK(ok, "all images valid");
    printf("images:");
    for (int i = 0; i < g_nimg; i++) printf(" [%s=%s]", g_images[i].file, g_images[i].label);
    printf("\n");
    return fails;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest() ? 1 : 0;
    puzzle_game_run();
    return 0;
}
#endif
