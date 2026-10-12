/*
 * snake.c - Snake for the Windows console
 *
 * What's new compared to the old version
 * --------------------------------------
 *  - No more flicker / shaking: the whole frame is composed in memory and
 *    pushed to the console with ONE WriteConsoleOutputW call, on a private
 *    screen buffer (your menu is restored untouched when the game exits).
 *  - Square cells (2 characters wide), Unicode box drawing, textured snake,
 *    centered layout, proper HUD (score, best, level, lives, food bar).
 *  - Responsive controls: fixed-timestep loop, input is polled every ms and
 *    buffered (you can queue quick turns like Up+Left without losing them).
 *  - 8 hand-built levels with different wall layouts, guaranteed to be fully
 *    connected so food is always reachable.
 *  - 3 lives, combo multiplier, timed bonus food, level-clear bonus,
 *    speed that creeps up as you eat, countdown, pause menu.
 *  - Persistent high score (snake_highscore.dat).
 *
 * Public API is unchanged: int play_snake(void);
 */

#include "snake.h"

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

#define BOARD_W         30
#define BOARD_H         18
#define MAX_SNAKE       (BOARD_W * BOARD_H)

#define TOTAL_LEVELS    8
#define START_LIVES     3
#define START_X         6
#define START_Y         (BOARD_H / 2)
#define START_LENGTH    4
#define RESERVED_X      12      /* walls never block the start corridor   */

#define BONUS_LIFETIME  60      /* in snake steps                         */
#define BONUS_EVERY     4       /* a bonus appears after this many foods  */
#define COMBO_WINDOW    28      /* steps allowed between foods for combo  */
#define COMBO_MAX       5
#define MIN_DELAY_MS    45
#define INPUT_QUEUE     3

#define HISCORE_FILE    "snake_highscore.dat"

/* Screen layout (in console characters) */
#define BOARD_X         1
#define BOARD_Y         3
#define SCR_W           (BOARD_W * 2 + 4)
#define SCR_H           (BOARD_Y + BOARD_H + 4)
#define BOARD_RIGHT     (BOARD_X + BOARD_W * 2 + 1)

/* Characters */
#define CH_DBL_H        L'\x2550'
#define CH_DBL_V        L'\x2551'
#define CH_DBL_TL       L'\x2554'
#define CH_DBL_TR       L'\x2557'
#define CH_DBL_BL       L'\x255A'
#define CH_DBL_BR       L'\x255D'
#define CH_FULL         L'\x2588'
#define CH_DARK         L'\x2593'
#define CH_MED          L'\x2592'
#define CH_LIGHT        L'\x2591'
#define CH_DOT          L'\x00B7'
#define CH_HEART        L'\x2665'
#define CH_EYE          L'\x2022'

/* Colours */
#define FG_W            (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define COL_TEXT        (FG_W)
#define COL_WHITE       (FG_W | FOREGROUND_INTENSITY)
#define COL_DIM         (FOREGROUND_INTENSITY)
#define COL_BORDER      (FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define COL_GREEN       (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_RED         (FOREGROUND_RED | FOREGROUND_INTENSITY)
#define COL_GOLD        (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define COL_WALL        (FG_W | BACKGROUND_INTENSITY)
#define COL_BODY        (FOREGROUND_GREEN | FOREGROUND_INTENSITY | BACKGROUND_GREEN)
#define COL_BODY_HIT    (FOREGROUND_RED | FOREGROUND_INTENSITY | BACKGROUND_RED)
#define COL_HEAD        (BACKGROUND_GREEN | BACKGROUND_INTENSITY)
#define COL_HEAD_HIT    (BACKGROUND_RED | BACKGROUND_INTENSITY)

/* ------------------------------------------------------------------ */
/*  Types                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int x;
    int y;
} Point;

typedef enum {
    UP,
    DOWN,
    LEFT,
    RIGHT
} Direction;

typedef enum {
    LAYOUT_EMPTY,
    LAYOUT_PILLARS,
    LAYOUT_CORNERS,
    LAYOUT_BARS,
    LAYOUT_CROSS,
    LAYOUT_SCATTER,
    LAYOUT_GATES,
    LAYOUT_INFERNO
} Layout;

typedef struct {
    const char *name;
    int target_food;
    int delay_ms;
    Layout layout;
} LevelConfig;

static const LevelConfig levels[TOTAL_LEVELS] = {
    { "Open Field",   6, 150, LAYOUT_EMPTY   },
    { "Pillars",      8, 135, LAYOUT_PILLARS },
    { "Corners",     10, 120, LAYOUT_CORNERS },
    { "The Bars",    10, 110, LAYOUT_BARS    },
    { "Crossroads",  12, 100, LAYOUT_CROSS   },
    { "Rubble",      12,  90, LAYOUT_SCATTER },
    { "Gauntlet",    14,  80, LAYOUT_GATES   },
    { "Inferno",     15,  70, LAYOUT_INFERNO }
};

typedef struct {
    Point body[MAX_SNAKE];
    int length;
    Direction direction;
    Direction queue[INPUT_QUEUE];
    int queue_len;

    Point food;
    int bonus_active;
    Point bonus;
    int bonus_left;

    unsigned char wall[BOARD_H][BOARD_W];

    int level;
    int food_this_level;
    int foods_since_bonus;
    int score;
    int hiscore;
    int start_hi;
    int lives;
    int combo;
    int ticks_since_food;

    int crashed;
    int level_cleared;
    int pause_req;
    int game_over;
    int won;
    int quit_requested;
    int flash;
    int anim;

    char toast[48];
    double toast_until;
} SnakeGame;

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

/* Ask Windows for 1 ms timer resolution so Sleep(1) is really ~1 ms.
 * Loaded dynamically so no extra library has to be linked. */
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
    DWORD written;
    DWORD cells;

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

    /* If the window is too small for the game, try to enlarge it. */
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

/* Push the finished frame to the screen in a single call (no flicker). */
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

    /* Window was resized / moved: wipe leftovers once. */
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

static void put_right(int right_x, int y, WORD attr, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    put_text(right_x - (int)strlen(buf) + 1, y, attr, buf);
}

static void draw_bar(int x, int y, int width, int filled, WORD on, WORD off)
{
    int i;

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

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int same_point(Point a, Point b)
{
    return a.x == b.x && a.y == b.y;
}

static int opposite(Direction a, Direction b)
{
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) ||
           (a == LEFT && b == RIGHT) || (a == RIGHT && b == LEFT);
}

static int current_delay(const SnakeGame *g)
{
    int d = levels[g->level].delay_ms - g->food_this_level * 2;
    return d < MIN_DELAY_MS ? MIN_DELAY_MS : d;
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

static void save_hiscore(int v)
{
    FILE *f = fopen(HISCORE_FILE, "w");

    if (f)
    {
        fprintf(f, "%d\n", v);
        fclose(f);
    }
}

static void set_toast(SnakeGame *g, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(g->toast, sizeof g->toast, fmt, ap);
    va_end(ap);

    g->toast_until = now_ms() + 1500.0;
}

static void add_score(SnakeGame *g, int points)
{
    g->score += points;

    if (g->score > g->hiscore)
        g->hiscore = g->score;
}

static void flush_keys(void)
{
    while (_kbhit())
        _getch();
}

/* Blocking key read. Returns 0 for special keys (arrows etc). */
static int wait_key(void)
{
    int k = _getch();

    if (k == 0 || k == 224)
    {
        _getch();
        return 0;
    }

    return k;
}

/* ------------------------------------------------------------------ */
/*  Level generation                                                   */
/* ------------------------------------------------------------------ */

static void wall_set(SnakeGame *g, int x, int y)
{
    if (x < 0 || x >= BOARD_W || y < 0 || y >= BOARD_H)
        return;

    /* Keep the start corridor free. */
    if (x < RESERVED_X && abs(y - START_Y) <= 2)
        return;

    g->wall[y][x] = 1;
}

static void wall_rect(SnakeGame *g, int x, int y, int w, int h)
{
    int i, j;

    for (j = 0; j < h; ++j)
        for (i = 0; i < w; ++i)
            wall_set(g, x + i, y + j);
}

static void wall_scatter(SnakeGame *g, int count)
{
    int i, k, len, horizontal, x, y;

    for (i = 0; i < count; ++i)
    {
        len = 3 + rand() % 3;
        horizontal = rand() % 2;
        x = 2 + rand() % (BOARD_W - 4);
        y = 2 + rand() % (BOARD_H - 4);

        for (k = 0; k < len; ++k)
            wall_set(g, x + (horizontal ? k : 0), y + (horizontal ? 0 : k));
    }
}

static void wall_pillars(SnakeGame *g)
{
    int x, y;

    for (x = 10; x < BOARD_W - 3; x += 6)
        for (y = 3; y < BOARD_H - 3; y += 5)
            wall_rect(g, x, y, 2, 2);
}

static void apply_layout(SnakeGame *g, Layout layout)
{
    int i, x, y, k;

    switch (layout)
    {
        case LAYOUT_EMPTY:
            break;

        case LAYOUT_PILLARS:
            wall_pillars(g);
            break;

        case LAYOUT_CORNERS:
            for (i = 0; i < 6; ++i)
            {
                wall_set(g, 2 + i, 2);
                wall_set(g, 2, 2 + i);
                wall_set(g, BOARD_W - 3 - i, 2);
                wall_set(g, BOARD_W - 3, 2 + i);
                wall_set(g, 2 + i, BOARD_H - 3);
                wall_set(g, 2, BOARD_H - 3 - i);
                wall_set(g, BOARD_W - 3 - i, BOARD_H - 3);
                wall_set(g, BOARD_W - 3, BOARD_H - 3 - i);
            }
            wall_rect(g, 14, 8, 2, 2);
            break;

        case LAYOUT_BARS:
            for (x = 12; x < BOARD_W - 3; ++x)
            {
                if (x == 20 || x == 21)
                    continue;
                wall_set(g, x, 5);
                wall_set(g, x, BOARD_H - 6);
            }
            break;

        case LAYOUT_CROSS:
            for (y = 1; y < BOARD_H - 1; ++y)
            {
                if (y == 4 || y == 5 || y == 12 || y == 13)
                    continue;
                wall_set(g, 15, y);
                wall_set(g, 16, y);
            }
            for (x = 18; x < BOARD_W - 2; ++x)
            {
                if (x == 23 || x == 24)
                    continue;
                wall_set(g, x, 9);
            }
            break;

        case LAYOUT_SCATTER:
            wall_scatter(g, 8);
            break;

        case LAYOUT_GATES:
            for (k = 0; k < 3; ++k)
            {
                x = 12 + k * 6;
                for (y = 0; y < BOARD_H; ++y)
                {
                    if (k % 2 == 0 && y >= 11 && y <= 14)
                        continue;
                    if (k % 2 == 1 && y >= 3 && y <= 6)
                        continue;
                    wall_set(g, x, y);
                }
            }
            break;

        case LAYOUT_INFERNO:
            wall_pillars(g);
            wall_scatter(g, 9);
            break;
    }
}

/* Every free cell must be reachable from the start, otherwise a level
 * could contain unreachable pockets. */
static int layout_connected(const SnakeGame *g)
{
    static const int dx[4] = { 1, -1, 0, 0 };
    static const int dy[4] = { 0, 0, 1, -1 };
    unsigned char seen[BOARD_H][BOARD_W];
    int stack[BOARD_W * BOARD_H];
    int sp = 0, reached = 0, open = 0;
    int x, y, d, cur, cx, cy, nx, ny;

    memset(seen, 0, sizeof seen);

    for (y = 0; y < BOARD_H; ++y)
        for (x = 0; x < BOARD_W; ++x)
            if (!g->wall[y][x])
                ++open;

    stack[sp++] = START_Y * BOARD_W + START_X;
    seen[START_Y][START_X] = 1;

    while (sp > 0)
    {
        cur = stack[--sp];
        cx = cur % BOARD_W;
        cy = cur / BOARD_W;
        ++reached;

        for (d = 0; d < 4; ++d)
        {
            nx = cx + dx[d];
            ny = cy + dy[d];

            if (nx < 0 || nx >= BOARD_W || ny < 0 || ny >= BOARD_H)
                continue;
            if (g->wall[ny][nx] || seen[ny][nx])
                continue;

            seen[ny][nx] = 1;
            stack[sp++] = ny * BOARD_W + nx;
        }
    }

    return reached == open;
}

static void build_layout(SnakeGame *g)
{
    int tries;

    for (tries = 0; tries < 60; ++tries)
    {
        memset(g->wall, 0, sizeof g->wall);
        apply_layout(g, levels[g->level].layout);

        if (layout_connected(g))
            return;
    }

    memset(g->wall, 0, sizeof g->wall);
}

/* Picks a random cell that is not a wall, the snake, food or bonus. */
static int pick_free_cell(const SnakeGame *g, Point *out)
{
    unsigned char occ[BOARD_H][BOARD_W];
    Point cand[MAX_SNAKE];
    int n = 0, i, x, y;

    memcpy(occ, g->wall, sizeof occ);

    for (i = 0; i < g->length; ++i)
        occ[g->body[i].y][g->body[i].x] = 1;

    if (g->food.x >= 0 && g->food.x < BOARD_W && g->food.y >= 0 && g->food.y < BOARD_H)
        occ[g->food.y][g->food.x] = 1;

    if (g->bonus_active)
        occ[g->bonus.y][g->bonus.x] = 1;

    for (y = 0; y < BOARD_H; ++y)
    {
        for (x = 0; x < BOARD_W; ++x)
        {
            if (!occ[y][x])
            {
                cand[n].x = x;
                cand[n].y = y;
                ++n;
            }
        }
    }

    if (n == 0)
        return 0;

    *out = cand[rand() % n];
    return 1;
}

static void place_food(SnakeGame *g)
{
    Point p;

    /* The old food (if any) was just eaten or discarded. */
    g->food.x = -1;
    g->food.y = -1;

    if (pick_free_cell(g, &p))
    {
        g->food = p;
    }
    else
    {
        /* The board is completely full: you win! */
        g->won = 1;
        g->game_over = 1;
    }
}

static void respawn(SnakeGame *g)
{
    int i;

    g->length = START_LENGTH;
    g->direction = RIGHT;
    g->queue_len = 0;
    g->bonus_active = 0;
    g->foods_since_bonus = 0;
    g->combo = 0;
    g->ticks_since_food = 9999;
    g->crashed = 0;
    g->flash = 0;

    for (i = 0; i < g->length; ++i)
    {
        g->body[i].x = START_X - i;
        g->body[i].y = START_Y;
    }

    place_food(g);
}

static void load_level(SnakeGame *g)
{
    g->food_this_level = 0;
    g->level_cleared = 0;
    build_layout(g);
    respawn(g);
}

static void init_game(SnakeGame *g)
{
    memset(g, 0, sizeof *g);

    g->lives = START_LIVES;
    g->hiscore = load_hiscore();
    g->start_hi = g->hiscore;
    g->food.x = -1;
    g->food.y = -1;

    load_level(g);
}

/* ------------------------------------------------------------------ */
/*  Rendering of the game scene                                        */
/* ------------------------------------------------------------------ */

static void draw_scene(SnakeGame *g)
{
    int x, y, i, sx, sy, hx;
    int target = levels[g->level].target_food;
    WORD body_col = g->flash ? COL_BODY_HIT : COL_BODY;
    WORD head_col = g->flash ? COL_HEAD_HIT : COL_HEAD;
    double now = now_ms();
    wchar_t ch;

    gfx_clear();
    ++g->anim;

    /* ---- HUD ---- */
    put_fmt(BOARD_X, 0, COL_WHITE, "SCORE %06d", g->score);
    put_center(0, COL_GREEN, "S  N  A  K  E");
    put_right(BOARD_RIGHT, 0, COL_GOLD, "BEST %06d", g->hiscore);

    put_fmt(BOARD_X, 1, COL_TEXT, "LEVEL %d/%d", g->level + 1, TOTAL_LEVELS);
    put_fmt(BOARD_X + 12, 1, COL_GOLD, "%s", levels[g->level].name);

    hx = BOARD_RIGHT - (START_LIVES * 2 - 2);
    put_text(hx - 6, 1, COL_TEXT, "LIVES");
    for (i = 0; i < START_LIVES; ++i)
        put_ch(hx + i * 2, 1, CH_HEART, i < g->lives ? COL_RED : COL_DIM);

    put_text(BOARD_X, 2, COL_TEXT, "FOOD");
    draw_bar(BOARD_X + 5, 2, 16, g->food_this_level * 16 / target, COL_GREEN, COL_DIM);
    put_fmt(BOARD_X + 22, 2, COL_TEXT, "%d/%d", g->food_this_level, target);

    if (g->bonus_active)
    {
        put_text(BOARD_RIGHT - 17, 2, COL_GOLD, "BONUS");
        draw_bar(BOARD_RIGHT - 11, 2, 12, g->bonus_left * 12 / BONUS_LIFETIME + 1, COL_GOLD, COL_DIM);
    }
    else if (g->combo > 1 && g->ticks_since_food <= COMBO_WINDOW)
    {
        put_right(BOARD_RIGHT, 2, COL_GOLD, "COMBO x%d",
                  g->combo < COMBO_MAX ? g->combo : COMBO_MAX);
    }
    else
    {
        put_right(BOARD_RIGHT, 2, COL_DIM, "SPEED %.1f", 1000.0 / current_delay(g));
    }

    /* ---- Board frame ---- */
    put_ch(BOARD_X, BOARD_Y, CH_DBL_TL, COL_BORDER);
    put_ch(BOARD_RIGHT, BOARD_Y, CH_DBL_TR, COL_BORDER);
    put_ch(BOARD_X, BOARD_Y + BOARD_H + 1, CH_DBL_BL, COL_BORDER);
    put_ch(BOARD_RIGHT, BOARD_Y + BOARD_H + 1, CH_DBL_BR, COL_BORDER);

    for (x = 0; x < BOARD_W * 2; ++x)
    {
        put_ch(BOARD_X + 1 + x, BOARD_Y, CH_DBL_H, COL_BORDER);
        put_ch(BOARD_X + 1 + x, BOARD_Y + BOARD_H + 1, CH_DBL_H, COL_BORDER);
    }

    for (y = 0; y < BOARD_H; ++y)
    {
        put_ch(BOARD_X, BOARD_Y + 1 + y, CH_DBL_V, COL_BORDER);
        put_ch(BOARD_RIGHT, BOARD_Y + 1 + y, CH_DBL_V, COL_BORDER);
    }

    /* ---- Floor and walls ---- */
    for (y = 0; y < BOARD_H; ++y)
    {
        for (x = 0; x < BOARD_W; ++x)
        {
            sx = BOARD_X + 1 + x * 2;
            sy = BOARD_Y + 1 + y;

            if (g->wall[y][x])
            {
                put_ch(sx, sy, CH_MED, COL_WALL);
                put_ch(sx + 1, sy, CH_MED, COL_WALL);
            }
            else
            {
                put_ch(sx, sy, CH_DOT, COL_DIM);
            }
        }
    }

    /* ---- Food ---- */
    if (g->food.x >= 0)
    {
        sx = BOARD_X + 1 + g->food.x * 2;
        sy = BOARD_Y + 1 + g->food.y;
        i = (g->anim / 6) % 2 ? COL_RED : FOREGROUND_RED;
        put_ch(sx, sy, L'(', (WORD)i);
        put_ch(sx + 1, sy, L')', (WORD)i);
    }

    /* ---- Bonus food (blinks when about to vanish) ---- */
    if (g->bonus_active && (g->bonus_left > 18 || (g->anim / 3) % 2 == 0))
    {
        sx = BOARD_X + 1 + g->bonus.x * 2;
        sy = BOARD_Y + 1 + g->bonus.y;
        put_ch(sx, sy, L'*', COL_GOLD);
        put_ch(sx + 1, sy, L'*', COL_GOLD);
    }

    /* ---- Snake (tail first so the head is always on top) ---- */
    for (i = g->length - 1; i >= 0; --i)
    {
        sx = BOARD_X + 1 + g->body[i].x * 2;
        sy = BOARD_Y + 1 + g->body[i].y;

        if (i == 0)
        {
            put_ch(sx, sy, CH_EYE, head_col);
            put_ch(sx + 1, sy, CH_EYE, head_col);
        }
        else
        {
            if (i == g->length - 1)
                ch = CH_LIGHT;
            else
                ch = (i % 2) ? CH_MED : CH_DARK;

            put_ch(sx, sy, ch, body_col);
            put_ch(sx + 1, sy, ch, body_col);
        }
    }

    /* ---- Footer ---- */
    put_center(BOARD_Y + BOARD_H + 2, COL_DIM,
               "Arrows / WASD  Move     P  Pause     Q  Menu");

    if (now < g->toast_until)
        put_center(BOARD_Y + BOARD_H + 3, COL_GOLD, "%s", g->toast);
    else
        put_center(BOARD_Y + BOARD_H + 3, COL_DIM,
                   "() food    ** bonus    eat fast for combos!");
}

/* ------------------------------------------------------------------ */
/*  Overlays / screens                                                 */
/* ------------------------------------------------------------------ */

static void countdown(SnakeGame *g)
{
    int n, x0, y0;

    for (n = 3; n >= 1; --n)
    {
        draw_scene(g);
        draw_box(34, 10, COL_BORDER, &x0, &y0);
        put_center(y0 + 2, COL_GREEN, "LEVEL %d", g->level + 1);
        put_center(y0 + 3, COL_GOLD, "%s", levels[g->level].name);
        put_center(y0 + 5, COL_WHITE, "%d", n);
        put_center(y0 + 7, COL_DIM, "Get ready...");
        gfx_present();
        Sleep(450);
    }

    flush_keys();
}

static void pause_screen(SnakeGame *g)
{
    int x0, y0, k;

    draw_scene(g);
    draw_box(34, 10, COL_GOLD, &x0, &y0);
    put_center(y0 + 2, COL_GOLD, "P A U S E D");
    put_center(y0 + 5, COL_TEXT, "P / Esc   Resume");
    put_center(y0 + 6, COL_TEXT, "Q         Quit to menu");
    gfx_present();

    for (;;)
    {
        k = wait_key();

        if (k == 'p' || k == 'P' || k == 27)
            break;

        if (k == 'q' || k == 'Q')
        {
            g->quit_requested = 1;
            break;
        }
    }

    flush_keys();
}

static void handle_crash(SnakeGame *g)
{
    int i, x0, y0;

    for (i = 0; i < 6; ++i)
    {
        g->flash = (i % 2 == 0);
        draw_scene(g);
        gfx_present();
        Sleep(110);
    }

    g->flash = 0;
    g->crashed = 0;
    g->combo = 0;
    --g->lives;

    if (g->lives <= 0)
    {
        g->game_over = 1;
        return;
    }

    draw_scene(g);
    draw_box(36, 9, COL_RED, &x0, &y0);
    put_center(y0 + 2, COL_RED, "C R A S H !");
    put_center(y0 + 4, COL_TEXT, "%d %s left", g->lives,
               g->lives == 1 ? "life" : "lives");
    put_center(y0 + 6, COL_DIM, "Press any key");
    gfx_present();

    Sleep(700);
    flush_keys();
    wait_key();

    respawn(g);
    countdown(g);
}

static void handle_level_clear(SnakeGame *g)
{
    int bonus = 50 * (g->level + 1) + 25 * g->lives;
    int x0, y0;

    g->level_cleared = 0;
    add_score(g, bonus);

    if (g->level == TOTAL_LEVELS - 1)
    {
        g->won = 1;
        g->game_over = 1;
        return;
    }

    draw_scene(g);
    draw_box(38, 11, COL_GOLD, &x0, &y0);
    put_center(y0 + 2, COL_GOLD, "LEVEL %d CLEARED!", g->level + 1);
    put_center(y0 + 4, COL_WHITE, "Level bonus  +%d", bonus);
    put_center(y0 + 6, COL_TEXT, "Next: %s", levels[g->level + 1].name);
    put_center(y0 + 8, COL_DIM, "Press any key");
    gfx_present();

    Sleep(900);
    flush_keys();
    wait_key();

    ++g->level;
    load_level(g);
    countdown(g);
}

static int end_screen(SnakeGame *g)
{
    int x0, y0, k;
    int record = g->score > g->start_hi;
    WORD col = g->won ? COL_GOLD : COL_RED;

    draw_scene(g);
    draw_box(40, 13, col, &x0, &y0);

    if (g->won)
    {
        put_center(y0 + 2, COL_GOLD, "YOU BEAT SNAKE!");
        put_center(y0 + 3, COL_TEXT, "All %d levels cleared", TOTAL_LEVELS);
    }
    else
    {
        put_center(y0 + 2, COL_RED, "G A M E   O V E R");
    }

    put_center(y0 + 5, COL_WHITE, "Score  %06d", g->score);
    put_center(y0 + 6, COL_TEXT, "Level  %d / %d", g->level + 1, TOTAL_LEVELS);

    if (record)
        put_center(y0 + 8, COL_GOLD, "*** NEW HIGH SCORE ***");
    else
        put_center(y0 + 8, COL_DIM, "Best   %06d", g->hiscore);

    put_center(y0 + 10, COL_GREEN, "R  Play again       M  Menu");
    gfx_present();

    Sleep(500);
    flush_keys();

    for (;;)
    {
        k = wait_key();

        if (k == 'r' || k == 'R')
            return 1;

        if (k == 'm' || k == 'M' || k == 'q' || k == 'Q' || k == 27)
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Input                                                              */
/* ------------------------------------------------------------------ */

/* Turns are buffered so fast key combos are never lost. */
static void queue_dir(SnakeGame *g, Direction d)
{
    Direction last = g->queue_len > 0 ? g->queue[g->queue_len - 1] : g->direction;

    if (d == last || opposite(d, last))
        return;

    if (g->queue_len < INPUT_QUEUE)
        g->queue[g->queue_len++] = d;
}

static void poll_input(SnakeGame *g)
{
    int key;

    while (_kbhit())
    {
        key = _getch();

        /* Arrow keys arrive as 0/224 followed by a second code. */
        if (key == 0 || key == 224)
        {
            key = _getch();

            switch (key)
            {
                case 72: queue_dir(g, UP);    break;
                case 80: queue_dir(g, DOWN);  break;
                case 75: queue_dir(g, LEFT);  break;
                case 77: queue_dir(g, RIGHT); break;
            }

            continue;
        }

        switch (key)
        {
            case 'w': case 'W': queue_dir(g, UP);    break;
            case 's': case 'S': queue_dir(g, DOWN);  break;
            case 'a': case 'A': queue_dir(g, LEFT);  break;
            case 'd': case 'D': queue_dir(g, RIGHT); break;

            case 'p': case 'P':
            case 'q': case 'Q':
            case 27:
                g->pause_req = 1;
                break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Game logic                                                         */
/* ------------------------------------------------------------------ */

static void step_snake(SnakeGame *g)
{
    Point next = g->body[0];
    int ate = 0, ate_bonus = 0;
    int i, limit, mult, points;

    if (g->queue_len > 0)
    {
        g->direction = g->queue[0];

        for (i = 1; i < g->queue_len; ++i)
            g->queue[i - 1] = g->queue[i];

        --g->queue_len;
    }

    switch (g->direction)
    {
        case UP:    --next.y; break;
        case DOWN:  ++next.y; break;
        case LEFT:  --next.x; break;
        case RIGHT: ++next.x; break;
    }

    if (next.x < 0 || next.x >= BOARD_W ||
        next.y < 0 || next.y >= BOARD_H ||
        g->wall[next.y][next.x])
    {
        g->crashed = 1;
        return;
    }

    ate = same_point(next, g->food);
    ate_bonus = g->bonus_active && same_point(next, g->bonus);

    /* The tail moves away unless we grow, so it is a legal target. */
    limit = g->length - ((ate || ate_bonus) ? 0 : 1);

    for (i = 0; i < limit; ++i)
    {
        if (same_point(g->body[i], next))
        {
            g->crashed = 1;
            return;
        }
    }

    if ((ate || ate_bonus) && g->length < MAX_SNAKE)
        ++g->length;

    for (i = g->length - 1; i > 0; --i)
        g->body[i] = g->body[i - 1];

    g->body[0] = next;
    ++g->ticks_since_food;

    if (ate_bonus)
    {
        points = 25 + g->bonus_left;
        g->bonus_active = 0;
        add_score(g, points);
        set_toast(g, "BONUS!  +%d", points);
    }

    if (ate)
    {
        if (g->combo > 0 && g->ticks_since_food <= COMBO_WINDOW)
            ++g->combo;
        else
            g->combo = 1;

        g->ticks_since_food = 0;

        mult = g->combo < COMBO_MAX ? g->combo : COMBO_MAX;
        points = 10 * mult;
        add_score(g, points);

        if (!ate_bonus)
        {
            if (mult > 1)
                set_toast(g, "+%d   COMBO x%d", points, mult);
            else
                set_toast(g, "+%d", points);
        }

        ++g->food_this_level;
        ++g->foods_since_bonus;

        if (g->food_this_level >= levels[g->level].target_food)
        {
            g->level_cleared = 1;
            return;
        }

        place_food(g);

        if (!g->game_over && g->foods_since_bonus >= BONUS_EVERY && !g->bonus_active)
        {
            Point p;

            if (pick_free_cell(g, &p))
            {
                g->bonus = p;
                g->bonus_active = 1;
                g->bonus_left = BONUS_LIFETIME;
                g->foods_since_bonus = 0;
                set_toast(g, "BONUS FOOD appeared!");
            }
        }
    }

    if (g->bonus_active)
    {
        --g->bonus_left;

        if (g->bonus_left <= 0)
            g->bonus_active = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Main loop                                                          */
/* ------------------------------------------------------------------ */

static void run_game(SnakeGame *g)
{
    double last_step, last_draw = 0.0, now, delay;

    init_game(g);
    flush_keys();
    countdown(g);
    last_step = now_ms();

    while (!g->game_over && !g->quit_requested)
    {
        now = now_ms();
        poll_input(g);

        if (g->pause_req)
        {
            g->pause_req = 0;
            pause_screen(g);
            last_step = now_ms();
            last_draw = 0.0;
            continue;
        }

        delay = (double)current_delay(g);

        if (now - last_step >= delay)
        {
            last_step += delay;

            /* Don't try to "catch up" after a long stall. */
            if (now - last_step > delay * 2.0)
                last_step = now;

            step_snake(g);

            if (g->crashed)
                handle_crash(g);
            else if (g->level_cleared)
                handle_level_clear(g);

            if (g->crashed || g->level_cleared || g->game_over)
            {
                last_step = now_ms();
                last_draw = 0.0;
            }

            now = now_ms();
        }

        if (now - last_draw >= 16.0 && !g->game_over)
        {
            draw_scene(g);
            gfx_present();
            last_draw = now;
        }

        Sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int play_snake(void)
{
    SnakeGame game;
    int again = 1;

    srand((unsigned int)time(NULL));

    if (!gfx_init())
        return 0;

    timer_begin();

    while (again)
    {
        run_game(&game);

        if (game.score > game.start_hi)
            save_hiscore(game.score);

        if (game.quit_requested)
            break;

        again = end_screen(&game);
    }

    timer_end();
    gfx_shutdown();
    return 0;
}
