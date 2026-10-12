#include "snake.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <conio.h>
#include <windows.h>

#define BOARD_W 34
#define BOARD_H 16
#define MAX_SNAKE (BOARD_W * BOARD_H)
#define TOTAL_LEVELS 6
#define MAX_OBSTACLES 18

typedef struct {
    int x;
    int y;
} Point;

typedef struct {
    int target_food;
    int delay_ms;
    int obstacle_count;
} LevelConfig;

static const LevelConfig levels[TOTAL_LEVELS] = {
    {  5, 180,  0 },   /* Level 1 */
    {  8, 150,  0 },   /* Level 2 */
    { 10, 125,  4 },   /* Level 3 */
    { 12, 100,  8 },   /* Level 4 */
    { 15,  80, 12 },   /* Level 5 */
    { 20,  60, 18 }    /* Level 6 */
};

typedef enum {
    UP,
    DOWN,
    LEFT,
    RIGHT
} Direction;

typedef struct {
    Point body[MAX_SNAKE];
    int length;
    Direction direction;

    Point food;
    Point obstacles[MAX_OBSTACLES];
    int obstacle_count;

    int level;
    int food_this_level;
    int score;

    int game_over;
    int won;
    int quit_requested;
} SnakeGame;

/* ---------- Utility ---------- */

static int same_point(Point a, Point b)
{
    return a.x == b.x && a.y == b.y;
}

static int snake_contains(const SnakeGame *game, Point p)
{
    int i;

    for (i = 0; i < game->length; ++i)
    {
        if (same_point(game->body[i], p))
            return 1;
    }

    return 0;
}

static int obstacle_contains(const SnakeGame *game, Point p)
{
    int i;

    for (i = 0; i < game->obstacle_count; ++i)
    {
        if (same_point(game->obstacles[i], p))
            return 1;
    }

    return 0;
}

static int blocked(const SnakeGame *game, Point p)
{
    return snake_contains(game, p) || obstacle_contains(game, p);
}

/* ---------- Board generation ---------- */

static void place_food(SnakeGame *game)
{
    Point p;
    int attempts = 0;

    do
    {
        p.x = 1 + rand() % BOARD_W;
        p.y = 1 + rand() % BOARD_H;
        ++attempts;

        if (attempts > 10000)
        {
            game->game_over = 1;
            return;
        }
    }
    while (blocked(game, p));

    game->food = p;
}

static void build_obstacles(SnakeGame *game)
{
    int wanted = levels[game->level].obstacle_count;
    int attempts = 0;

    game->obstacle_count = 0;

    while (game->obstacle_count < wanted && attempts < 10000)
    {
        Point p;
        ++attempts;

        p.x = 2 + rand() % (BOARD_W - 2);
        p.y = 2 + rand() % (BOARD_H - 2);

        /* Keep the initial area reasonably open. */
        if (p.x < 9 && p.y < 7)
            continue;

        if (snake_contains(game, p))
            continue;

        if (obstacle_contains(game, p))
            continue;

        game->obstacles[game->obstacle_count++] = p;
    }
}

static void initialize_game(SnakeGame *game)
{
    int i;

    game->length = 4;
    game->direction = RIGHT;
    game->level = 0;
    game->food_this_level = 0;
    game->score = 0;
    game->game_over = 0;
    game->won = 0;
    game->quit_requested = 0;
    game->obstacle_count = 0;

    for (i = 0; i < game->length; ++i)
    {
        game->body[i].x = 7 - i;
        game->body[i].y = 5;
    }

    build_obstacles(game);
    place_food(game);
}

/* ---------- Rendering ---------- */

static void clear_screen(void)
{
    system("cls");
}

static void move_cursor_home(void)
{
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD home = {0, 0};

    if (console != INVALID_HANDLE_VALUE)
        SetConsoleCursorPosition(console, home);
}

static void set_color(WORD color)
{
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

    if (console != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(console, color);
}

static void set_cursor_visible(BOOL visible)
{
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO cursor_info;

    if (console != INVALID_HANDLE_VALUE &&
        GetConsoleCursorInfo(console, &cursor_info))
    {
        cursor_info.bVisible = visible;
        SetConsoleCursorInfo(console, &cursor_info);
    }
}

static void draw_game(const SnakeGame *game)
{
    int x, y, i;
    char cell;

    move_cursor_home();
    set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);

    printf("===============================================\n");
    printf("                 S N A K E\n");
    printf("===============================================\n");
    printf(" LEVEL %d/%d   SCORE %04d   FOOD %02d/%02d\n",
           game->level + 1, TOTAL_LEVELS, game->score,
           game->food_this_level,
           levels[game->level].target_food);
    printf(" Speed: %d ms   Controls: Arrows / WASD | P Pause | Q Menu\n",
           levels[game->level].delay_ms);
    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("+");

    for (x = 0; x < BOARD_W + 2; ++x)
        putchar('-');

    printf("+\n");

    for (y = 1; y <= BOARD_H; ++y)
    {
        set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        printf("|");

        for (x = 1; x <= BOARD_W; ++x)
        {
            Point p = {x, y};
            cell = ' ';
            set_color(FOREGROUND_GREEN | FOREGROUND_BLUE);

            if (same_point(p, game->body[0]))
            {
                cell = 'O';
                set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            }
            else if (same_point(p, game->food))
            {
                cell = '$';
                set_color(FOREGROUND_RED | FOREGROUND_INTENSITY);
            }
            else
            {
                for (i = 1; i < game->length; ++i)
                {
                    if (same_point(p, game->body[i]))
                    {
                        cell = 'o';
                        set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
                        break;
                    }
                }

                if (cell == ' ')
                {
                    for (i = 0; i < game->obstacle_count; ++i)
                    {
                        if (same_point(p, game->obstacles[i]))
                        {
                            cell = '#';
                            set_color(FOREGROUND_RED | FOREGROUND_GREEN |
                                      FOREGROUND_INTENSITY);
                            break;
                        }
                    }
                }
            }

            putchar(cell);
        }

        set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        printf("|\n");
    }

    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("+");

    for (x = 0; x < BOARD_W + 2; ++x)
        putchar('-');

    printf("+\n");
    set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf(" O Snake   $ Food   # Obstacle | Eat food to grow and speed up!\n");

    fflush(stdout);
}

/* ---------- Input ---------- */

static int opposite(Direction a, Direction b)
{
    return
        (a == UP && b == DOWN) ||
        (a == DOWN && b == UP) ||
        (a == LEFT && b == RIGHT) ||
        (a == RIGHT && b == LEFT);
}

static void pause_game(SnakeGame *game)
{
    int key;

    clear_screen();

    printf("\n\n");
    printf("===============================================\n");
    printf("                    PAUSED\n");
    printf("===============================================\n");
    printf("\n");
    printf(" Press P to resume.\n");
    printf(" Press Q to quit.\n");

    while (1)
    {
        key = _getch();

        if (key == 'p' || key == 'P')
            return;

        if (key == 'q' || key == 'Q')
        {
            game->quit_requested = 1;
            game->game_over = 1;
            return;
        }
    }
}

static void read_input(SnakeGame *game)
{
    int key;

    if (!_kbhit())
        return;

    key = _getch();

    /* Windows arrow keys send 0/224 followed by a second code. */
    if (key == 0 || key == 224)
    {
        key = _getch();

        switch (key)
        {
            case 72:
                if (!opposite(game->direction, UP))
                    game->direction = UP;
                break;

            case 80:
                if (!opposite(game->direction, DOWN))
                    game->direction = DOWN;
                break;

            case 75:
                if (!opposite(game->direction, LEFT))
                    game->direction = LEFT;
                break;

            case 77:
                if (!opposite(game->direction, RIGHT))
                    game->direction = RIGHT;
                break;
        }

        return;
    }

    switch (key)
    {
        case 'w':
        case 'W':
            if (!opposite(game->direction, UP))
                game->direction = UP;
            break;

        case 's':
        case 'S':
            if (!opposite(game->direction, DOWN))
                game->direction = DOWN;
            break;

        case 'a':
        case 'A':
            if (!opposite(game->direction, LEFT))
                game->direction = LEFT;
            break;

        case 'd':
        case 'D':
            if (!opposite(game->direction, RIGHT))
                game->direction = RIGHT;
            break;

        case 'p':
        case 'P':
            pause_game(game);
            break;

        case 'q':
        case 'Q':
            game->quit_requested = 1;
            game->game_over = 1;
            break;
    }
}

/* ---------- Game logic ---------- */

static void move_snake(SnakeGame *game)
{
    Point next = game->body[0];
    int ate_food;
    int i;
    int body_limit;

    switch (game->direction)
    {
        case UP:
            --next.y;
            break;

        case DOWN:
            ++next.y;
            break;

        case LEFT:
            --next.x;
            break;

        case RIGHT:
            ++next.x;
            break;
    }

    /* Wall collision */
    if (next.x < 1 || next.x > BOARD_W ||
        next.y < 1 || next.y > BOARD_H)
    {
        game->game_over = 1;
        return;
    }

    /* Obstacle collision */
    if (obstacle_contains(game, next))
    {
        game->game_over = 1;
        return;
    }

    ate_food = same_point(next, game->food);

    /*
     * When the snake is not eating, the tail will move away,
     * so moving into the current tail position is allowed.
     */
    body_limit = game->length - (ate_food ? 0 : 1);

    for (i = 0; i < body_limit; ++i)
    {
        if (same_point(game->body[i], next))
        {
            game->game_over = 1;
            return;
        }
    }

    if (ate_food && game->length < MAX_SNAKE)
        ++game->length;

    for (i = game->length - 1; i > 0; --i)
        game->body[i] = game->body[i - 1];

    game->body[0] = next;

    if (!ate_food)
        return;

    game->score += 10;
    ++game->food_this_level;

    /* Current level completed. */
    if (game->food_this_level >= levels[game->level].target_food)
    {
        if (game->level == TOTAL_LEVELS - 1)
        {
            game->won = 1;
            game->game_over = 1;
            return;
        }

        ++game->level;
        game->food_this_level = 0;

        build_obstacles(game);
        place_food(game);

        clear_screen();

        printf("\n\n");
        printf("===============================================\n");
        printf("              LEVEL %d UNLOCKED!\n", game->level + 1);
        printf("===============================================\n");
        printf("\n");
        printf(" Speed increased!");
        if (levels[game->level].obstacle_count > 0)
            printf(" New obstacles added!");
        printf("\n\n");
        printf(" Press any key to continue...");

        _getch();
    }
    else
    {
        place_food(game);
    }
}

/* ---------- End screens ---------- */

static int end_screen(const SnakeGame *game)
{
    int key;

    clear_screen();

    printf("\n\n");
    printf("===============================================\n");

    if (game->won)
    {
        printf("             YOU BEAT SNAKE!\n");
        printf("          ALL 6 LEVELS CLEARED\n");
    }
    else
    {
        printf("                GAME OVER\n");
    }

    printf("===============================================\n");
    printf("\n");
    printf(" Final Score   : %d\n", game->score);
    printf(" Level Reached : %d/%d\n",
           game->level + 1, TOTAL_LEVELS);
    printf("\n");
    printf(" R - Play Again\n");
    printf(" M - Main Menu\n");
    printf("\n");

    while (1)
    {
        key = _getch();

        if (key == 'r' || key == 'R')
            return 1;

        if (key == 'm' || key == 'M' ||
            key == 'q' || key == 'Q')
            return 0;
    }
}

/* ---------- Public API ---------- */

int play_snake(void)
{
    SnakeGame game;

    srand((unsigned int)time(NULL));
    clear_screen();
    set_cursor_visible(FALSE);

    while (1)
    {
        initialize_game(&game);

        while (!game.game_over)
        {
            draw_game(&game);
            read_input(&game);

            if (!game.game_over)
            {
                move_snake(&game);
                Sleep((DWORD)levels[game.level].delay_ms);
            }
        }

        if (game.quit_requested)
            break;

        if (!end_screen(&game))
            break;
    }

    clear_screen();
    set_cursor_visible(TRUE);
    return 0;
}
