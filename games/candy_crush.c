/*
 * candy_crush.c - Candy Crush for the Windows console  ("next level" edition)
 *
 * Public API is unchanged:   int play_candy_crush(void);
 *
 * What's new
 * ----------
 *  UI
 *   - Flicker-free: every frame is composed in memory and pushed to the
 *     console with a single WriteConsoleOutputW call (private screen buffer,
 *     your menu comes back untouched afterwards).
 *   - Real candy tiles (colour + a different symbol per colour, so it is
 *     colour-blind friendly), cursor, side panel with goals and progress bars.
 *   - Smooth animations: swap, flash-and-pop, candies falling one row at a time.
 *   - No more typing "34 35": move the cursor with the arrows/WASD, press
 *     Space to pick a candy, then press an arrow to swap it that way.
 *
 *  Gameplay
 *   - 8 levels with different goals: reach a score, collect a colour,
 *     clear jelly under the candies, or a mix of them.
 *   - Special candies:
 *        match 4        -> striped candy  (clears a row / a column)
 *        match L or T   -> wrapped candy  (explodes 3x3)
 *        match 5        -> colour bomb    (swap it to wipe out one colour)
 *     Specials chain-react, and swapping two specials makes bigger combos.
 *   - Cascades multiply your score, leftover moves turn into bonus points,
 *     1-3 star rating per level.
 *   - Hints (H) plus an automatic hint when you are idle, automatic shuffle
 *     when no move is possible, invalid swaps don't cost a move.
 *   - Persistent high score (candy_highscore.dat).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <conio.h>
#include <windows.h>

/* ------------------------------------------------------------------ */
/*  Configuration                                                      */
/* ------------------------------------------------------------------ */

#define N               8
#define MAX_COLORS      6
#define T_BOMB          6               /* cell.type value of a colour bomb */
#define TOTAL_LEVELS    8
#define HINTS_PER_LEVEL 3
#define BONUS_PER_MOVE  50
#define MAX_MULT        4               /* cascade score multiplier cap */
#define IDLE_HINT_MS    9000.0
#define HISCORE_FILE    "candy_highscore.dat"

/* Screen layout (console characters) */
#define SCR_W           70
#define SCR_H           24
#define BOARD_X         0
#define BOARD_Y         3
#define CELL_W          4
#define CELL_H          2
#define CX(j)           (BOARD_X + 1 + (j) * CELL_W)
#define CY(i)           (BOARD_Y + 1 + (i) * CELL_H)
#define BOARD_RIGHT     (BOARD_X + N * CELL_W + 2)
#define BOARD_BOTTOM    (BOARD_Y + 1 + N * CELL_H)
#define PX              38              /* side panel left edge */

/* Characters */
#define CH_DBL_H        L'\x2550'
#define CH_DBL_V        L'\x2551'
#define CH_DBL_TL       L'\x2554'
#define CH_DBL_TR       L'\x2557'
#define CH_DBL_BL       L'\x255A'
#define CH_DBL_BR       L'\x255D'
#define CH_FULL         L'\x2588'
#define CH_LIGHT        L'\x2591'
#define CH_UPPER        L'\x2580'
#define CH_SUN          L'\x263C'

/* Colours */
#define FG_W            (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COL_TEXT        (FG_W)
#define COL_WHITE       (FG_W | FOREGROUND_INTENSITY)
#define COL_DIM         (FOREGROUND_INTENSITY)
#define COL_BORDER      (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COL_GREEN       (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_RED         (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COL_GOLD        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_MAG         (FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define BG_WHITE        (BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY)
#define BG_JELLY        (BACKGROUND_INTENSITY)

/* Keys returned by read_key() besides plain ASCII */
enum { K_UP = 1001, K_DOWN, K_LEFT, K_RIGHT };

/* ------------------------------------------------------------------ */
/*  Types and tables                                                   */
/* ------------------------------------------------------------------ */

enum { SP_NONE = 0, SP_STRIPE_H, SP_STRIPE_V, SP_WRAPPED, SP_BOMB };
enum { J_NONE = 0, J_CENTER, J_RING, J_CHECKER, J_FULL };

typedef struct {
    signed char type;           /* -1 empty, 0..5 colour, T_BOMB colour bomb */
    unsigned char special;      /* SP_* */
} Cell;

typedef struct {
    const char *name;
    int colors;
    int moves;
    int target;                 /* score needed (0 = none)               */
    int goal_color;             /* colour to collect, -1 = none          */
    int goal_count;
    int jelly;                  /* J_* pattern                           */
} LevelConfig;

static const LevelConfig levels[TOTAL_LEVELS] = {
    /* name            col moves target goal_c goal_n jelly     */
    { "Sweet Start",    4,  20,  3000,   -1,    0,  J_NONE    },
    { "Cherry Pop",     5,  22,  3500,    0,   25,  J_NONE    },
    { "Jelly Jam",      5,  22,  1500,   -1,    0,  J_CENTER  },
    { "Blue Rush",      5,  22,  4500,    2,   28,  J_NONE    },
    { "Frosted Edge",   5,  24,  2000,   -1,    0,  J_RING    },
    { "Rainbow Road",   6,  22,  4000,    1,   30,  J_NONE    },
    { "Checkerboard",   6,  26,  1500,    3,   20,  J_CHECKER },
    { "Sugar Storm",    6,  32,  3000,   -1,    0,  J_FULL    }
};

static const wchar_t glyphs[MAX_COLORS] = {
    L'\x2665', L'\x2663', L'\x2666', L'\x25CF', L'\x25B2', L'\x2660'
};

static const WORD cand_bg[MAX_COLORS] = {
    BACKGROUND_RED | BACKGROUND_INTENSITY,
    BACKGROUND_GREEN | BACKGROUND_INTENSITY,
    BACKGROUND_BLUE | BACKGROUND_INTENSITY,
    BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_INTENSITY,
    BACKGROUND_RED | BACKGROUND_BLUE | BACKGROUND_INTENSITY,
    BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY
};

static const WORD cand_fg[MAX_COLORS] = {
    FOREGROUND_RED | FOREGROUND_INTENSITY,
    FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
    FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY,
    FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
};

typedef struct {
    int i1, j1, i2, j2;
} Move;

typedef struct {
    Cell cell[N][N];
    unsigned char jelly[N][N];
    unsigned char mark[N][N];
    unsigned char spawn[N][N];

    int q[N * N];
    int qh, qt;

    int cur_i, cur_j;
    int sel;

    int level;
    int ncolors;
    int moves;
    int level_score;
    int total_score;
    int hiscore;
    int start_hi;
    int collected;
    int jelly_left;
    int jelly_total;
    int hints;
    int stars[TOTAL_LEVELS];

    int have_swap;
    int swap_i[2], swap_j[2];

    int hint_show;
    Move hint;
    double hint_until;
    double last_action;

    int flash;
    int anim;
    char toast[64];
    double toast_until;
} Game;

typedef enum { PL_WIN, PL_LOSE, PL_QUIT, PL_RESTART } PlayResult;

/* ------------------------------------------------------------------ */
/*  Timing                                                             */
/* ------------------------------------------------------------------ */

typedef UINT (WINAPI *TimePeriodFn)(UINT);
static HMODULE g_winmm = NULL;

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);

    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void timer_begin(void)
{
    TimePeriodFn fn;

    g_winmm = LoadLibraryA("winmm.dll");
    if (g_winmm)
    {
        fn = (TimePeriodFn)GetProcAddress(g_winmm, "timeBeginPeriod");
        if (fn)
            fn(1);
    }
}

static void timer_end(void)
{
    TimePeriodFn fn;

    if (g_winmm)
    {
        fn = (TimePeriodFn)GetProcAddress(g_winmm, "timeEndPeriod");
        if (fn)
            fn(1);
        FreeLibrary(g_winmm);
        g_winmm = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Double-buffered console graphics                                   */
/* ------------------------------------------------------------------ */

static HANDLE g_prev_out = NULL;
static HANDLE g_out = INVALID_HANDLE_VALUE;
static CHAR_INFO g_frame[SCR_H][SCR_W];
static int g_off_x = -1;
static int g_off_y = -1;

static void clear_console(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD origin;
    DWORD written, cells;

    origin.X = 0;
    origin.Y = 0;

    if (!GetConsoleScreenBufferInfo(g_out, &info))
        return;

    cells = (DWORD)info.dwSize.X * (DWORD)info.dwSize.Y;
    FillConsoleOutputCharacterW(g_out, L' ', cells, origin, &written);
    FillConsoleOutputAttribute(g_out, 0, cells, origin, &written);
}

static int gfx_init(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    CONSOLE_CURSOR_INFO cursor;
    COORD size;
    SMALL_RECT rect;
    int ww, wh, nw, nh;

    g_prev_out = GetStdHandle(STD_OUTPUT_HANDLE);

    g_out = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (g_out == INVALID_HANDLE_VALUE)
        return 0;

    SetConsoleActiveScreenBuffer(g_out);

    cursor.dwSize = 1;
    cursor.bVisible = FALSE;
    SetConsoleCursorInfo(g_out, &cursor);

    if (GetConsoleScreenBufferInfo(g_out, &info))
    {
        ww = info.srWindow.Right - info.srWindow.Left + 1;
        wh = info.srWindow.Bottom - info.srWindow.Top + 1;

        if (ww < SCR_W || wh < SCR_H)
        {
            nw = ww < SCR_W ? SCR_W : ww;
            nh = wh < SCR_H ? SCR_H : wh;

            size.X = (SHORT)(nw > info.dwSize.X ? nw : info.dwSize.X);
            size.Y = (SHORT)(nh > info.dwSize.Y ? nh : info.dwSize.Y);
            SetConsoleScreenBufferSize(g_out, size);

            rect.Left = 0;
            rect.Top = 0;
            rect.Right = (SHORT)(nw - 1);
            rect.Bottom = (SHORT)(nh - 1);
            SetConsoleWindowInfo(g_out, TRUE, &rect);
        }
    }

    clear_console();
    return 1;
}

static void gfx_shutdown(void)
{
    if (g_prev_out)
        SetConsoleActiveScreenBuffer(g_prev_out);

    if (g_out != INVALID_HANDLE_VALUE)
        CloseHandle(g_out);

    g_out = INVALID_HANDLE_VALUE;
}

static void gfx_clear(void)
{
    int x, y;

    for (y = 0; y < SCR_H; ++y)
    {
        for (x = 0; x < SCR_W; ++x)
        {
            g_frame[y][x].Char.UnicodeChar = L' ';
            g_frame[y][x].Attributes = 0;
        }
    }
}

static void gfx_present(void)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    COORD size, zero;
    SMALL_RECT dest;
    int ox = 0, oy = 0, ww, wh;

    if (GetConsoleScreenBufferInfo(g_out, &info))
    {
        ww = info.srWindow.Right - info.srWindow.Left + 1;
        wh = info.srWindow.Bottom - info.srWindow.Top + 1;

        ox = (ww - SCR_W) / 2;
        oy = (wh - SCR_H) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;

        ox += info.srWindow.Left;
        oy += info.srWindow.Top;
    }

    if (ox != g_off_x || oy != g_off_y)
    {
        clear_console();
        g_off_x = ox;
        g_off_y = oy;
    }

    size.X = SCR_W;
    size.Y = SCR_H;
    zero.X = 0;
    zero.Y = 0;

    dest.Left = (SHORT)ox;
    dest.Top = (SHORT)oy;
    dest.Right = (SHORT)(ox + SCR_W - 1);
    dest.Bottom = (SHORT)(oy + SCR_H - 1);

    WriteConsoleOutputW(g_out, &g_frame[0][0], size, zero, &dest);
}

static void put_ch(int x, int y, wchar_t ch, WORD attr)
{
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H)
        return;

    g_frame[y][x].Char.UnicodeChar = ch;
    g_frame[y][x].Attributes = attr;
}

static void put_text(int x, int y, WORD attr, const char *s)
{
    while (*s)
    {
        put_ch(x, y, (wchar_t)(unsigned char)*s, attr);
        ++x;
        ++s;
    }
}

static void put_fmt(int x, int y, WORD attr, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    put_text(x, y, attr, buf);
}

static void put_center(int y, WORD attr, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    put_text((SCR_W - (int)strlen(buf)) / 2, y, attr, buf);
}

static void draw_bar(int x, int y, int width, int value, int max, WORD on, WORD off)
{
    int i, filled;

    filled = (max > 0) ? (int)((long long)value * width / max) : width;
    if (filled > width) filled = width;
    if (filled < 0) filled = 0;

    for (i = 0; i < width; ++i)
        put_ch(x + i, y, i < filled ? CH_FULL : CH_LIGHT, i < filled ? on : off);
}

static void draw_box(int w, int h, WORD attr, int *px, int *py)
{
    int x0 = (SCR_W - w) / 2;
    int y0 = (SCR_H - h) / 2;
    int x, y;
    wchar_t ch;

    for (y = 0; y < h; ++y)
    {
        for (x = 0; x < w; ++x)
        {
            ch = L' ';

            if (y == 0)
                ch = (x == 0) ? CH_DBL_TL : (x == w - 1) ? CH_DBL_TR : CH_DBL_H;
            else if (y == h - 1)
                ch = (x == 0) ? CH_DBL_BL : (x == w - 1) ? CH_DBL_BR : CH_DBL_H;
            else if (x == 0 || x == w - 1)
                ch = CH_DBL_V;

            put_ch(x0 + x, y0 + y, ch, attr);
        }
    }

    *px = x0;
    *py = y0;
}

/* A 3x(1.5) tile: one coloured row with the symbol, and a half-block row. */
static void put_tile(int x, int y, wchar_t l, wchar_t m, wchar_t r,
                     WORD bg, WORD fg, WORD base)
{
    put_ch(x, y, l, bg);
    put_ch(x + 1, y, m, bg);
    put_ch(x + 2, y, r, bg);
    put_ch(x, y + 1, CH_UPPER, (WORD)(fg | base));
    put_ch(x + 1, y + 1, CH_UPPER, (WORD)(fg | base));
    put_ch(x + 2, y + 1, CH_UPPER, (WORD)(fg | base));
}

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void flush_keys(void)
{
    while (_kbhit())
        _getch();
}

static int read_key(void)
{
    int k = _getch();

    if (k == 0 || k == 224)
    {
        k = _getch();

        switch (k)
        {
            case 72: return K_UP;
            case 80: return K_DOWN;
            case 75: return K_LEFT;
            case 77: return K_RIGHT;
            default: return 0;
        }
    }

    return k;
}

static int load_hiscore(void)
{
    FILE *f = fopen(HISCORE_FILE, "r");
    int v = 0;

    if (f)
    {
        if (fscanf(f, "%d", &v) != 1)
            v = 0;
        fclose(f);
    }

    return v < 0 ? 0 : v;
}

static void save_if_best(Game *g)
{
    FILE *f;

    if (g->hiscore <= g->start_hi)
        return;

    f = fopen(HISCORE_FILE, "w");
    if (f)
    {
        fprintf(f, "%d\n", g->hiscore);
        fclose(f);
        g->start_hi = g->hiscore;
    }
}

static void set_toast(Game *g, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g->toast, sizeof g->toast, fmt, ap);
    va_end(ap);

    g->toast_until = now_ms() + 1800.0;
}

static void add_score(Game *g, int points)
{
    g->level_score += points;

    if (g->total_score + g->level_score > g->hiscore)
        g->hiscore = g->total_score + g->level_score;
}

static int objectives_met(const Game *g)
{
    const LevelConfig *cfg = &levels[g->level];

    if (g->level_score < cfg->target)
        return 0;
    if (cfg->goal_color >= 0 && g->collected < cfg->goal_count)
        return 0;
    if (g->jelly_left > 0)
        return 0;

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Board logic                                                        */
/* ------------------------------------------------------------------ */

static int color_at(const Game *g, int i, int j)
{
    int t = g->cell[i][j].type;

    return (t >= 0 && t < MAX_COLORS) ? t : -1;
}

static void swap_cells(Game *g, int i1, int j1, int i2, int j2)
{
    Cell tmp = g->cell[i1][j1];

    g->cell[i1][j1] = g->cell[i2][j2];
    g->cell[i2][j2] = tmp;
}

static int has_any_match(const Game *g)
{
    int i, j, t;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            t = color_at(g, i, j);
            if (t < 0)
                continue;

            if (j + 2 < N && color_at(g, i, j + 1) == t && color_at(g, i, j + 2) == t)
                return 1;
            if (i + 2 < N && color_at(g, i + 1, j) == t && color_at(g, i + 2, j) == t)
                return 1;
        }
    }

    return 0;
}

/* Would swapping these two (adjacent) cells be a legal move? */
static int move_ok(Game *g, int i1, int j1, int i2, int j2)
{
    Cell a = g->cell[i1][j1];
    Cell b = g->cell[i2][j2];
    int ok;

    if (a.type < 0 || b.type < 0)
        return 0;
    if (a.type == T_BOMB || b.type == T_BOMB)
        return 1;
    if (a.special && b.special)
        return 1;

    swap_cells(g, i1, j1, i2, j2);
    ok = has_any_match(g);
    swap_cells(g, i1, j1, i2, j2);

    return ok;
}

static int list_moves(Game *g, Move *out, int max)
{
    int i, j, n = 0;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            if (j + 1 < N && n < max && move_ok(g, i, j, i, j + 1))
            {
                out[n].i1 = i; out[n].j1 = j; out[n].i2 = i; out[n].j2 = j + 1;
                ++n;
            }
            if (i + 1 < N && n < max && move_ok(g, i, j, i + 1, j))
            {
                out[n].i1 = i; out[n].j1 = j; out[n].i2 = i + 1; out[n].j2 = j;
                ++n;
            }
        }
    }

    return n;
}

static int board_has_move(Game *g)
{
    int i, j;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            if (j + 1 < N && move_ok(g, i, j, i, j + 1))
                return 1;
            if (i + 1 < N && move_ok(g, i, j, i + 1, j))
                return 1;
        }
    }

    return 0;
}

static void init_board(Game *g)
{
    int i, j, t;

    do
    {
        for (i = 0; i < N; ++i)
        {
            for (j = 0; j < N; ++j)
            {
                do
                {
                    t = rand() % g->ncolors;
                }
                while ((j >= 2 && g->cell[i][j - 1].type == t && g->cell[i][j - 2].type == t) ||
                       (i >= 2 && g->cell[i - 1][j].type == t && g->cell[i - 2][j].type == t));

                g->cell[i][j].type = (signed char)t;
                g->cell[i][j].special = SP_NONE;
            }
        }
    }
    while (!board_has_move(g));
}

static void reshuffle(Game *g)
{
    Cell tmp[N * N], t;
    int n = 0, i, j, k, tries;

    for (i = 0; i < N; ++i)
        for (j = 0; j < N; ++j)
            tmp[n++] = g->cell[i][j];

    for (tries = 0; tries < 300; ++tries)
    {
        for (i = n - 1; i > 0; --i)
        {
            k = rand() % (i + 1);
            t = tmp[i];
            tmp[i] = tmp[k];
            tmp[k] = t;
        }

        for (i = 0; i < N; ++i)
            for (j = 0; j < N; ++j)
                g->cell[i][j] = tmp[i * N + j];

        if (!has_any_match(g) && board_has_move(g))
            return;
    }

    init_board(g);
}

static void setup_jelly(Game *g)
{
    int i, j, on, count = 0;

    memset(g->jelly, 0, sizeof g->jelly);

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            switch (levels[g->level].jelly)
            {
                case J_CENTER:  on = (i >= 2 && i <= 5 && j >= 2 && j <= 5); break;
                case J_RING:    on = (i == 0 || i == N - 1 || j == 0 || j == N - 1); break;
                case J_CHECKER: on = ((i + j) % 2 == 0); break;
                case J_FULL:    on = 1; break;
                default:        on = 0; break;
            }

            g->jelly[i][j] = (unsigned char)on;
            count += on;
        }
    }

    g->jelly_left = count;
    g->jelly_total = count;
}

static void load_level(Game *g)
{
    const LevelConfig *cfg = &levels[g->level];

    g->ncolors = cfg->colors;
    g->moves = cfg->moves;
    g->level_score = 0;
    g->collected = 0;
    g->hints = HINTS_PER_LEVEL;
    g->cur_i = N / 2;
    g->cur_j = N / 2;
    g->sel = 0;
    g->hint_show = 0;
    g->flash = 0;
    g->have_swap = 0;
    g->toast[0] = '\0';
    g->toast_until = 0.0;
    g->last_action = now_ms();

    setup_jelly(g);
    init_board(g);
}

/* ---------- match detection with special-candy spawning ---------- */

static void spawn_set(Game *g, int i, int j, int kind)
{
    if (kind > g->spawn[i][j])
        g->spawn[i][j] = (unsigned char)kind;
}

/* Pick which cell of a 4+ run becomes the special candy. */
static int choose_spawn(const Game *g, int i0, int j0, int di, int dj, int len,
                        int *pi, int *pj)
{
    int k, s, m, ci, cj;

    if (g->have_swap)
    {
        for (s = 0; s < 2; ++s)
        {
            for (k = 0; k < len; ++k)
            {
                ci = i0 + di * k;
                cj = j0 + dj * k;

                if (ci == g->swap_i[s] && cj == g->swap_j[s] && !g->cell[ci][cj].special)
                {
                    *pi = ci;
                    *pj = cj;
                    return 1;
                }
            }
        }
    }

    for (m = 0; m < len; ++m)
    {
        k = (len / 2 + m) % len;
        ci = i0 + di * k;
        cj = j0 + dj * k;

        if (!g->cell[ci][cj].special)
        {
            *pi = ci;
            *pj = cj;
            return 1;
        }
    }

    return 0;
}

static void mark_plain(Game *g, int i, int j)
{
    if (i >= 0 && i < N && j >= 0 && j < N)
        g->mark[i][j] = 1;
}

/* Mark a cell for clearing; specials get queued so they explode too. */
static void mark_cell(Game *g, int i, int j)
{
    if (i < 0 || i >= N || j < 0 || j >= N || g->mark[i][j])
        return;

    g->mark[i][j] = 1;

    if (g->cell[i][j].special)
        g->q[g->qt++] = i * N + j;
}

static void process_queue(Game *g)
{
    int idx, i, j, k, di, dj, c, n;
    int present[MAX_COLORS];
    Cell cl;

    while (g->qh < g->qt)
    {
        idx = g->q[g->qh++];
        i = idx / N;
        j = idx % N;
        cl = g->cell[i][j];

        switch (cl.special)
        {
            case SP_STRIPE_H:
                for (k = 0; k < N; ++k)
                    mark_cell(g, i, k);
                break;

            case SP_STRIPE_V:
                for (k = 0; k < N; ++k)
                    mark_cell(g, k, j);
                break;

            case SP_WRAPPED:
                for (di = -1; di <= 1; ++di)
                    for (dj = -1; dj <= 1; ++dj)
                        mark_cell(g, i + di, j + dj);
                break;

            case SP_BOMB:
                memset(present, 0, sizeof present);
                n = 0;

                for (di = 0; di < N; ++di)
                {
                    for (dj = 0; dj < N; ++dj)
                    {
                        c = color_at(g, di, dj);
                        if (c >= 0 && !g->mark[di][dj] && !present[c])
                        {
                            present[c] = 1;
                            ++n;
                        }
                    }
                }

                if (n > 0)
                {
                    k = rand() % n;

                    for (c = 0; c < MAX_COLORS; ++c)
                    {
                        if (present[c] && k-- == 0)
                            break;
                    }

                    for (di = 0; di < N; ++di)
                        for (dj = 0; dj < N; ++dj)
                            if (g->cell[di][dj].type == c)
                                mark_cell(g, di, dj);
                }
                break;

            default:
                break;
        }
    }

    g->qh = 0;
    g->qt = 0;
}

/* Finds all runs of 3+, decides which specials are created and marks the
 * cells to clear. Returns the number of matched cells (0 = stable board). */
static int find_matches(Game *g)
{
    int hr[N][N], vr[N][N];
    int i, j, k, c, len, t, pi, pj, total = 0;

    memset(hr, 0, sizeof hr);
    memset(vr, 0, sizeof vr);
    memset(g->mark, 0, sizeof g->mark);
    memset(g->spawn, 0, sizeof g->spawn);
    g->qh = 0;
    g->qt = 0;

    /* horizontal runs */
    for (i = 0; i < N; ++i)
    {
        j = 0;
        while (j < N)
        {
            t = color_at(g, i, j);
            if (t < 0)
            {
                ++j;
                continue;
            }

            k = j;
            while (k + 1 < N && color_at(g, i, k + 1) == t)
                ++k;

            len = k - j + 1;

            if (len >= 3)
            {
                for (c = j; c <= k; ++c)
                    hr[i][c] = len;

                if (len >= 4 && choose_spawn(g, i, j, 0, 1, len, &pi, &pj))
                    spawn_set(g, pi, pj, len >= 5 ? SP_BOMB : SP_STRIPE_V);
            }

            j = k + 1;
        }
    }

    /* vertical runs */
    for (j = 0; j < N; ++j)
    {
        i = 0;
        while (i < N)
        {
            t = color_at(g, i, j);
            if (t < 0)
            {
                ++i;
                continue;
            }

            k = i;
            while (k + 1 < N && color_at(g, k + 1, j) == t)
                ++k;

            len = k - i + 1;

            if (len >= 3)
            {
                for (c = i; c <= k; ++c)
                    vr[c][j] = len;

                if (len >= 4 && choose_spawn(g, i, j, 1, 0, len, &pi, &pj))
                    spawn_set(g, pi, pj, len >= 5 ? SP_BOMB : SP_STRIPE_H);
            }

            i = k + 1;
        }
    }

    /* L / T shapes: a cell that belongs to a horizontal AND a vertical run */
    for (i = 0; i < N; ++i)
        for (j = 0; j < N; ++j)
            if (hr[i][j] >= 3 && vr[i][j] >= 3 && !g->cell[i][j].special)
                spawn_set(g, i, j, SP_WRAPPED);

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            if (hr[i][j] >= 3 || vr[i][j] >= 3)
            {
                ++total;

                if (!g->spawn[i][j])
                    mark_cell(g, i, j);
            }
        }
    }

    process_queue(g);
    return total;
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

static void draw_game(Game *g)
{
    const LevelConfig *cfg = &levels[g->level];
    double now = now_ms();
    int i, j, k, x, y, row;
    WORD base, bgc, fgc, ca;
    wchar_t gl, l, r;
    Cell cl;

    gfx_clear();
    ++g->anim;

    /* ---- title / toast ---- */
    put_center(0, COL_MAG, "C A N D Y   C R U S H");

    if (now < g->toast_until)
        put_center(1, COL_GOLD, "%s", g->toast);

    /* ---- board frame ---- */
    put_ch(BOARD_X, BOARD_Y, CH_DBL_TL, COL_BORDER);
    put_ch(BOARD_RIGHT, BOARD_Y, CH_DBL_TR, COL_BORDER);
    put_ch(BOARD_X, BOARD_BOTTOM, CH_DBL_BL, COL_BORDER);
    put_ch(BOARD_RIGHT, BOARD_BOTTOM, CH_DBL_BR, COL_BORDER);

    for (x = BOARD_X + 1; x < BOARD_RIGHT; ++x)
    {
        put_ch(x, BOARD_Y, CH_DBL_H, COL_BORDER);
        put_ch(x, BOARD_BOTTOM, CH_DBL_H, COL_BORDER);
    }

    for (y = BOARD_Y + 1; y < BOARD_BOTTOM; ++y)
    {
        put_ch(BOARD_X, y, CH_DBL_V, COL_BORDER);
        put_ch(BOARD_RIGHT, y, CH_DBL_V, COL_BORDER);
    }

    /* ---- cells ---- */
    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            x = CX(j);
            y = CY(i);
            base = g->jelly[i][j] ? BG_JELLY : 0;

            for (row = 0; row < CELL_H; ++row)
                for (k = 0; k < CELL_W; ++k)
                    put_ch(x + k, y + row, L' ', base);

            cl = g->cell[i][j];
            if (cl.type < 0)
                continue;

            if (cl.type == T_BOMB)
            {
                k = (g->anim / 3) % MAX_COLORS;
                bgc = cand_bg[k];
                fgc = cand_fg[k];
                gl = CH_SUN;
            }
            else
            {
                bgc = cand_bg[(int)cl.type];
                fgc = cand_fg[(int)cl.type];
                gl = glyphs[(int)cl.type];
            }

            if (g->flash && g->mark[i][j])
            {
                bgc = BG_WHITE;
                fgc = COL_WHITE;
                gl = L'*';
            }

            if (g->hint_show &&
                ((i == g->hint.i1 && j == g->hint.j1) || (i == g->hint.i2 && j == g->hint.j2)) &&
                (g->anim / 6) % 2)
            {
                bgc = BG_WHITE;
                fgc = COL_WHITE;
            }

            l = L' ';
            r = L' ';

            switch (cl.special)
            {
                case SP_STRIPE_H: l = CH_DBL_H; r = CH_DBL_H; break;
                case SP_STRIPE_V: l = CH_DBL_V; r = CH_DBL_V; break;
                case SP_WRAPPED:  l = L'(';     r = L')';     break;
                default: break;
            }

            put_tile(x + 1, y, l, gl, r, bgc, fgc, base);
        }
    }

    /* ---- cursor ---- */
    x = CX(g->cur_j);
    y = CY(g->cur_i);
    ca = g->sel ? (((g->anim / 5) % 2) ? COL_GOLD : COL_WHITE) : COL_WHITE;

    put_ch(x, y, L'[', ca);
    put_ch(x + 4, y, L']', ca);
    put_ch(x + 1, y + 1, CH_UPPER, ca);
    put_ch(x + 2, y + 1, CH_UPPER, ca);
    put_ch(x + 3, y + 1, CH_UPPER, ca);

    /* ---- side panel ---- */
    put_fmt(PX, 3, COL_GOLD, "LEVEL %d / %d", g->level + 1, TOTAL_LEVELS);
    put_fmt(PX, 4, COL_WHITE, "%s", cfg->name);

    put_text(PX, 6, COL_TEXT, "MOVES LEFT");
    put_fmt(PX + 12, 6, g->moves <= 5 ? COL_RED : COL_WHITE, "%2d", g->moves);
    draw_bar(PX, 7, 20, g->moves, cfg->moves,
             g->moves <= 5 ? COL_RED : COL_GOLD, COL_DIM);

    put_text(PX, 9, COL_GOLD, "GOALS");

    if (cfg->target > 0)
    {
        put_text(PX, 10, COL_TEXT, "Score");
        draw_bar(PX + 8, 10, 12, g->level_score, cfg->target, COL_GREEN, COL_DIM);
        put_fmt(PX + 21, 10, g->level_score >= cfg->target ? COL_GREEN : COL_TEXT,
                "%d/%d", g->level_score, cfg->target);
    }

    if (cfg->goal_color >= 0)
    {
        put_ch(PX, 11, glyphs[cfg->goal_color], cand_fg[cfg->goal_color]);
        put_text(PX + 2, 11, COL_TEXT, "Get");
        draw_bar(PX + 8, 11, 12, g->collected, cfg->goal_count, COL_GREEN, COL_DIM);
        put_fmt(PX + 21, 11, g->collected >= cfg->goal_count ? COL_GREEN : COL_TEXT,
                "%d/%d", g->collected > cfg->goal_count ? cfg->goal_count : g->collected,
                cfg->goal_count);
    }

    if (g->jelly_total > 0)
    {
        put_text(PX, 12, COL_TEXT, "Jelly");
        draw_bar(PX + 8, 12, 12, g->jelly_total - g->jelly_left, g->jelly_total,
                 COL_GREEN, COL_DIM);
        put_fmt(PX + 21, 12, g->jelly_left == 0 ? COL_GREEN : COL_TEXT,
                "%d/%d", g->jelly_total - g->jelly_left, g->jelly_total);
    }

    put_fmt(PX, 14, COL_WHITE, "TOTAL %07d", g->total_score + g->level_score);
    put_fmt(PX, 15, COL_GOLD, "BEST  %07d", g->hiscore);
    put_fmt(PX, 17, COL_TEXT, "HINTS %d  (H)", g->hints);

    put_text(PX, 19, COL_DIM, "Match 4  = line blaster");
    put_text(PX, 20, COL_DIM, "L or T   = (x) 3x3 blast");
    put_text(PX, 21, COL_DIM, "Match 5  =   rainbow bomb");
    put_ch(PX + 11, 21, CH_SUN, COL_GOLD);

    /* ---- footer ---- */
    put_center(22, COL_DIM,
               "Arrows/WASD move   Space pick, then arrow to swap   H hint   P pause");
}

static void render(Game *g)
{
    draw_game(g);
    gfx_present();
}

/* ------------------------------------------------------------------ */
/*  Animated game actions                                              */
/* ------------------------------------------------------------------ */

/* Candies fall one row per frame and new ones drop in from the top. */
static void settle(Game *g)
{
    int i, j, moved;

    for (;;)
    {
        moved = 0;

        for (j = 0; j < N; ++j)
        {
            for (i = N - 2; i >= 0; --i)
            {
                if (g->cell[i][j].type >= 0 && g->cell[i + 1][j].type < 0)
                {
                    g->cell[i + 1][j] = g->cell[i][j];
                    g->cell[i][j].type = -1;
                    g->cell[i][j].special = SP_NONE;
                    moved = 1;
                }
            }

            if (g->cell[0][j].type < 0)
            {
                g->cell[0][j].type = (signed char)(rand() % g->ncolors);
                g->cell[0][j].special = SP_NONE;
                moved = 1;
            }
        }

        if (!moved)
            break;

        render(g);
        Sleep(45);
    }
}

/* Clears everything currently marked, scores it and lets candies fall. */
static void clear_step(Game *g, int mult)
{
    const LevelConfig *cfg = &levels[g->level];
    int i, j, n = 0, jel = 0, gain;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            if (!g->mark[i][j])
                continue;

            ++n;

            if (cfg->goal_color >= 0 && g->cell[i][j].type == cfg->goal_color)
                ++g->collected;

            if (g->jelly[i][j])
            {
                g->jelly[i][j] = 0;
                --g->jelly_left;
                ++jel;
            }
        }
    }

    gain = n * 10 * (mult > MAX_MULT ? MAX_MULT : mult) + jel * 15;
    add_score(g, gain);

    if (mult > 1)
        set_toast(g, "Cascade x%d!   +%d", mult, gain);
    else
        set_toast(g, "+%d", gain);

    g->flash = 1;
    render(g);
    Sleep(150);
    g->flash = 0;

    for (i = 0; i < N; ++i)
    {
        for (j = 0; j < N; ++j)
        {
            if (g->mark[i][j])
            {
                g->cell[i][j].type = -1;
                g->cell[i][j].special = SP_NONE;
            }
            else if (g->spawn[i][j])
            {
                if (g->spawn[i][j] == SP_BOMB)
                    g->cell[i][j].type = T_BOMB;

                g->cell[i][j].special = g->spawn[i][j];
            }

            g->mark[i][j] = 0;
            g->spawn[i][j] = 0;
        }
    }

    render(g);
    Sleep(60);
    settle(g);
}

static void run_cascade(Game *g, int first_mult)
{
    int mult = first_mult;

    while (find_matches(g) > 0)
    {
        clear_step(g, mult);
        ++mult;
    }
}

/* Triggered when a colour bomb or two specials are swapped together. */
static void activate_pair(Game *g, int i1, int j1, int i2, int j2)
{
    Cell a = g->cell[i1][j1];
    Cell b = g->cell[i2][j2];
    Cell o;
    int i, j, bi, bj, c;

    memset(g->mark, 0, sizeof g->mark);
    memset(g->spawn, 0, sizeof g->spawn);
    g->qh = 0;
    g->qt = 0;

    if (a.type == T_BOMB && b.type == T_BOMB)
    {
        for (i = 0; i < N; ++i)
            for (j = 0; j < N; ++j)
                mark_cell(g, i, j);
    }
    else if (a.type == T_BOMB || b.type == T_BOMB)
    {
        if (a.type == T_BOMB)
        {
            bi = i1; bj = j1; o = b;
        }
        else
        {
            bi = i2; bj = j2; o = a;
        }

        mark_plain(g, bi, bj);
        c = o.type;

        for (i = 0; i < N; ++i)
        {
            for (j = 0; j < N; ++j)
            {
                if (g->cell[i][j].type != c)
                    continue;

                /* Bomb + special: every candy of that colour becomes it. */
                if (o.special && !g->cell[i][j].special)
                {
                    if (o.special == SP_STRIPE_H || o.special == SP_STRIPE_V)
                        g->cell[i][j].special = (unsigned char)((rand() % 2) ? SP_STRIPE_H : SP_STRIPE_V);
                    else
                        g->cell[i][j].special = o.special;
                }

                mark_cell(g, i, j);
            }
        }
    }
    else
    {
        mark_cell(g, i1, j1);
        mark_cell(g, i2, j2);
    }

    process_queue(g);
}

/* Returns 1 if the swap used up a move, 0 if it was rejected. */
static int try_swap(Game *g, int i1, int j1, int i2, int j2)
{
    Cell a = g->cell[i1][j1];
    Cell b = g->cell[i2][j2];
    int combo;

    g->hint_show = 0;
    g->last_action = now_ms();

    if (!move_ok(g, i1, j1, i2, j2))
    {
        swap_cells(g, i1, j1, i2, j2);
        render(g);
        Sleep(130);
        swap_cells(g, i1, j1, i2, j2);
        set_toast(g, "That swap makes no match");
        render(g);
        return 0;
    }

    swap_cells(g, i1, j1, i2, j2);
    render(g);
    Sleep(110);

    --g->moves;

    g->have_swap = 1;
    g->swap_i[0] = i1; g->swap_j[0] = j1;
    g->swap_i[1] = i2; g->swap_j[1] = j2;

    combo = (a.type == T_BOMB || b.type == T_BOMB || (a.special && b.special));

    if (combo)
    {
        activate_pair(g, i1, j1, i2, j2);
        clear_step(g, 1);
        g->have_swap = 0;
        run_cascade(g, 2);
    }
    else
    {
        run_cascade(g, 1);
    }

    g->have_swap = 0;
    g->last_action = now_ms();
    return 1;
}

static void show_hint(Game *g, double duration)
{
    Move list[2 * N * N];
    int n = list_moves(g, list, 2 * N * N);

    if (n > 0)
    {
        g->hint = list[rand() % n];
        g->hint_show = 1;
        g->hint_until = now_ms() + duration;
    }
}

/* ------------------------------------------------------------------ */
/*  Screens and overlays                                               */
/* ------------------------------------------------------------------ */

static void goal_lines(Game *g, int x, int y)
{
    const LevelConfig *cfg = &levels[g->level];
    int row = y;

    if (cfg->target > 0)
        put_fmt(x, row++, COL_TEXT, "- Score at least %d", cfg->target);

    if (cfg->goal_color >= 0)
    {
        put_fmt(x, row, COL_TEXT, "- Collect %d", cfg->goal_count);
        put_ch(x + 14 + (cfg->goal_count >= 10 ? 1 : 0), row, glyphs[cfg->goal_color],
               cand_fg[cfg->goal_color]);
        ++row;
    }

    if (g->jelly_total > 0)
        put_fmt(x, row++, COL_TEXT, "- Clear all jelly (%d tiles)", g->jelly_total);
}

static void help_screen(void)
{
    int x0, y0;

    gfx_clear();
    draw_box(64, 22, COL_BORDER, &x0, &y0);

    put_center(y0 + 1, COL_MAG, "H O W   T O   P L A Y");
    put_text(x0 + 3, y0 + 3, COL_TEXT, "Swap two neighbouring candies to line up 3 or more of");
    put_text(x0 + 3, y0 + 4, COL_TEXT, "the same kind. Reach every goal before moves run out.");

    put_text(x0 + 3, y0 + 6, COL_GOLD, "CONTROLS");
    put_text(x0 + 5, y0 + 7, COL_TEXT, "Arrows / WASD   move the cursor");
    put_text(x0 + 5, y0 + 8, COL_TEXT, "Space / Enter   pick a candy up, then press an arrow");
    put_text(x0 + 5, y0 + 9, COL_TEXT, "                to swap it in that direction");
    put_text(x0 + 5, y0 + 10, COL_TEXT, "H hint     P pause     Q menu");

    put_text(x0 + 3, y0 + 12, COL_GOLD, "SPECIAL CANDIES");
    put_tile(x0 + 5, y0 + 13, CH_DBL_H, glyphs[0], CH_DBL_H, cand_bg[0], cand_fg[0], 0);
    put_text(x0 + 10, y0 + 13, COL_TEXT, "Match 4: clears a whole row or column");
    put_tile(x0 + 5, y0 + 15, L'(', glyphs[1], L')', cand_bg[1], cand_fg[1], 0);
    put_text(x0 + 10, y0 + 15, COL_TEXT, "Match in an L or T shape: explodes 3x3");
    put_tile(x0 + 5, y0 + 17, L' ', CH_SUN, L' ', cand_bg[3], cand_fg[3], 0);
    put_text(x0 + 10, y0 + 17, COL_TEXT, "Match 5: rainbow bomb. Swap it with a candy");
    put_text(x0 + 10, y0 + 18, COL_TEXT, "to wipe out every candy of that colour.");
    put_text(x0 + 3, y0 + 19, COL_DIM, "Two specials swapped together = big combos. Grey tiles are jelly.");
    put_center(y0 + 20, COL_GREEN, "Press any key");

    gfx_present();
    flush_keys();
    read_key();
}

static int title_screen(Game *g)
{
    int x0, y0, k, i, tx;

    for (;;)
    {
        gfx_clear();
        draw_box(52, 17, COL_BORDER, &x0, &y0);

        put_center(y0 + 2, COL_MAG, "C A N D Y   C R U S H");
        put_center(y0 + 3, COL_DIM, "- terminal edition -");

        tx = (SCR_W - (MAX_COLORS * 4 - 1)) / 2;
        for (i = 0; i < MAX_COLORS; ++i)
            put_tile(tx + i * 4, y0 + 5, L' ', glyphs[i], L' ', cand_bg[i], cand_fg[i], 0);

        put_center(y0 + 8, COL_WHITE, "ENTER    Start game");
        put_center(y0 + 9, COL_TEXT, "H        How to play");
        put_center(y0 + 10, COL_TEXT, "Q        Back to menu");
        put_center(y0 + 12, COL_GOLD, "Best score   %07d", g->hiscore);
        put_center(y0 + 14, COL_DIM, "8 levels - jelly - specials - combos");

        gfx_present();
        flush_keys();
        k = read_key();

        if (k == 13 || k == ' ')
            return 1;

        if (k == 'q' || k == 'Q' || k == 27)
            return 0;

        if (k == 'h' || k == 'H')
            help_screen();
    }
}

static int level_intro(Game *g)
{
    int x0, y0, k;

    draw_game(g);
    draw_box(46, 15, COL_BORDER, &x0, &y0);

    put_center(y0 + 2, COL_GOLD, "LEVEL %d / %d", g->level + 1, TOTAL_LEVELS);
    put_center(y0 + 3, COL_WHITE, "%s", levels[g->level].name);
    put_text(x0 + 6, y0 + 5, COL_GOLD, "GOALS");
    goal_lines(g, x0 + 8, y0 + 6);
    put_fmt(x0 + 6, y0 + 10, COL_TEXT, "You have %d moves", g->moves);
    put_center(y0 + 12, COL_GREEN, "ENTER  Start      Q  Menu");

    gfx_present();
    flush_keys();

    for (;;)
    {
        k = read_key();

        if (k == 13 || k == ' ')
            return 1;

        if (k == 'q' || k == 'Q' || k == 27)
            return 0;
    }
}

/* Returns 0 resume, 1 restart level, 2 quit. */
static int pause_screen(Game *g)
{
    int x0, y0, k;

    draw_game(g);
    draw_box(38, 11, COL_GOLD, &x0, &y0);

    put_center(y0 + 2, COL_GOLD, "P A U S E D");
    put_center(y0 + 5, COL_TEXT, "P / Esc   Resume");
    put_center(y0 + 6, COL_TEXT, "R         Restart level");
    put_center(y0 + 7, COL_TEXT, "Q         Quit to menu");

    gfx_present();
    flush_keys();

    for (;;)
    {
        k = read_key();

        if (k == 'p' || k == 'P' || k == 27)
            return 0;
        if (k == 'r' || k == 'R')
            return 1;
        if (k == 'q' || k == 'Q')
            return 2;
    }
}

static void win_sequence(Game *g)
{
    int init = levels[g->level].moves;
    int ratio = g->moves * 100 / init;

    g->stars[g->level] = ratio >= 50 ? 3 : (ratio >= 25 ? 2 : 1);

    set_toast(g, "LEVEL CLEARED!");
    render(g);
    Sleep(600);

    while (g->moves > 0)
    {
        --g->moves;
        add_score(g, BONUS_PER_MOVE);
        set_toast(g, "Moves bonus  +%d", BONUS_PER_MOVE);
        render(g);
        Sleep(80);
    }

    Sleep(300);
}

static void level_complete_screen(Game *g)
{
    int x0, y0, i, st = g->stars[g->level];

    draw_game(g);
    draw_box(42, 13, COL_GOLD, &x0, &y0);

    put_center(y0 + 2, COL_GOLD, "LEVEL %d CLEARED!", g->level + 1);

    for (i = 0; i < 3; ++i)
        put_text(SCR_W / 2 - 5 + i * 4, y0 + 4, i < st ? COL_GOLD : COL_DIM, "*");

    put_center(y0 + 6, COL_WHITE, "Level score   %06d", g->level_score);
    put_center(y0 + 7, COL_TEXT, "Total score   %07d", g->total_score + g->level_score);
    put_center(y0 + 10, COL_GREEN, "Press ENTER");

    gfx_present();
    Sleep(600);
    flush_keys();

    for (;;)
    {
        i = read_key();
        if (i == 13 || i == ' ')
            break;
    }
}

/* Returns 1 to retry, 0 for menu. */
static int fail_screen(Game *g)
{
    int x0, y0, k;

    draw_game(g);
    draw_box(44, 14, COL_RED, &x0, &y0);

    put_center(y0 + 2, COL_RED, "O U T   O F   M O V E S");
    put_text(x0 + 6, y0 + 4, COL_GOLD, "GOALS");
    goal_lines(g, x0 + 8, y0 + 5);
    put_center(y0 + 10, COL_GREEN, "R  Try again       M  Menu");

    gfx_present();
    Sleep(600);
    flush_keys();

    for (;;)
    {
        k = read_key();

        if (k == 'r' || k == 'R' || k == 13)
            return 1;

        if (k == 'm' || k == 'M' || k == 'q' || k == 'Q' || k == 27)
            return 0;
    }
}

/* Returns 1 to play again, 0 for menu. */
static int victory_screen(Game *g)
{
    int x0, y0, k, i, stars = 0;
    int record = g->hiscore > g->start_hi;

    for (i = 0; i < TOTAL_LEVELS; ++i)
        stars += g->stars[i];

    gfx_clear();
    draw_box(46, 14, COL_GOLD, &x0, &y0);

    put_center(y0 + 2, COL_GOLD, "S W E E T   V I C T O R Y !");
    put_center(y0 + 3, COL_TEXT, "You cleared all %d levels", TOTAL_LEVELS);
    put_center(y0 + 5, COL_WHITE, "Total score   %07d", g->total_score);
    put_center(y0 + 6, COL_GOLD, "Stars         %d / %d", stars, TOTAL_LEVELS * 3);

    if (record)
        put_center(y0 + 8, COL_GOLD, "*** NEW HIGH SCORE ***");
    else
        put_center(y0 + 8, COL_DIM, "Best          %07d", g->hiscore);

    put_center(y0 + 11, COL_GREEN, "R  Play again       M  Menu");

    save_if_best(g);
    gfx_present();
    Sleep(600);
    flush_keys();

    for (;;)
    {
        k = read_key();

        if (k == 'r' || k == 'R')
            return 1;

        if (k == 'm' || k == 'M' || k == 'q' || k == 'Q' || k == 27)
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Level loop and campaign                                            */
/* ------------------------------------------------------------------ */

static PlayResult after_move_checks(Game *g, int *finished)
{
    *finished = 1;

    if (objectives_met(g))
        return PL_WIN;

    if (g->moves <= 0)
        return PL_LOSE;

    if (!board_has_move(g))
    {
        set_toast(g, "No moves left - shuffling!");
        render(g);
        Sleep(800);
        reshuffle(g);
        render(g);
    }

    *finished = 0;
    return PL_WIN;
}

static PlayResult play_level(Game *g)
{
    double now, last_draw = 0.0;
    int key, di, dj, ti, tj, done;
    PlayResult res;

    flush_keys();
    g->last_action = now_ms();

    for (;;)
    {
        now = now_ms();

        while (_kbhit())
        {
            key = read_key();
            di = 0;
            dj = 0;

            switch (key)
            {
                case K_UP:    case 'w': case 'W': di = -1; break;
                case K_DOWN:  case 's': case 'S': di = 1;  break;
                case K_LEFT:  case 'a': case 'A': dj = -1; break;
                case K_RIGHT: case 'd': case 'D': dj = 1;  break;

                case ' ':
                case 13:
                    g->sel = !g->sel;
                    break;

                case 27:
                    if (g->sel)
                    {
                        g->sel = 0;
                        break;
                    }
                    /* fall through */
                case 'p': case 'P':
                case 'q': case 'Q':
                    switch (pause_screen(g))
                    {
                        case 1:  return PL_RESTART;
                        case 2:  return PL_QUIT;
                        default: break;
                    }
                    flush_keys();
                    g->last_action = now_ms();
                    break;

                case 'h': case 'H':
                    if (g->hints > 0)
                    {
                        --g->hints;
                        show_hint(g, 4000.0);
                        set_toast(g, "Hint!  (%d left)", g->hints);
                    }
                    else
                    {
                        set_toast(g, "No hints left on this level");
                    }
                    g->last_action = now_ms();
                    break;

                default:
                    break;
            }

            if (di != 0 || dj != 0)
            {
                g->hint_show = 0;
                g->last_action = now_ms();

                if (g->sel)
                {
                    ti = g->cur_i + di;
                    tj = g->cur_j + dj;

                    if (ti >= 0 && ti < N && tj >= 0 && tj < N)
                    {
                        g->sel = 0;

                        if (try_swap(g, g->cur_i, g->cur_j, ti, tj))
                        {
                            res = after_move_checks(g, &done);
                            if (done)
                                return res;
                        }

                        flush_keys();
                    }
                }
                else
                {
                    ti = g->cur_i + di;
                    tj = g->cur_j + dj;

                    if (ti >= 0 && ti < N)
                        g->cur_i = ti;
                    if (tj >= 0 && tj < N)
                        g->cur_j = tj;
                }
            }
        }

        now = now_ms();

        if (g->hint_show && now > g->hint_until)
            g->hint_show = 0;

        if (!g->hint_show && now - g->last_action > IDLE_HINT_MS)
        {
            show_hint(g, 3500.0);
            g->last_action = now;
        }

        if (now - last_draw >= 33.0)
        {
            render(g);
            last_draw = now;
        }

        Sleep(2);
    }
}

/* Returns 1 when all levels were cleared, 0 if the player quit. */
static int run_campaign(Game *g)
{
    PlayResult r;

    g->level = 0;
    g->total_score = 0;
    memset(g->stars, 0, sizeof g->stars);

    while (g->level < TOTAL_LEVELS)
    {
        load_level(g);

        if (!level_intro(g))
            return 0;

        r = play_level(g);

        if (r == PL_QUIT)
            return 0;

        if (r == PL_RESTART)
            continue;

        if (r == PL_WIN)
        {
            win_sequence(g);
            level_complete_screen(g);
            g->total_score += g->level_score;
            g->level_score = 0;
            ++g->level;
        }
        else
        {
            if (!fail_screen(g))
                return 0;
        }
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int play_candy_crush(void)
{
    Game game;
    int finished;

    srand((unsigned int)time(NULL));
    memset(&game, 0, sizeof game);

    game.hiscore = load_hiscore();
    game.start_hi = game.hiscore;

    if (!gfx_init())
        return 0;

    timer_begin();

    if (title_screen(&game))
    {
        for (;;)
        {
            finished = run_campaign(&game);
            save_if_best(&game);

            if (!finished)
                break;

            if (!victory_screen(&game))
                break;
        }
    }

    timer_end();
    gfx_shutdown();
    return 0;
}
