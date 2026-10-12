ongoing..........................

A terminal-based C Game Box containing Candy Crush and Snake.

## Build

This project targets Windows/MinGW because the existing Candy Crush game uses
`<windows.h>` and `Sleep()`.

```bash
mingw32-make
```

Run:

```bash
gamebox.exe
```

## Game menu

```text
1. Candy Crush
2. Snake
0. Exit
```

## Snake

Six levels are included:

| Level | Food | Speed | Obstacles |
|------:|-----:|------:|----------:|
| 1 | 5  | 180 ms | 0 |
| 2 | 8  | 150 ms | 0 |
| 3 | 10 | 125 ms | 4 |
| 4 | 12 | 100 ms | 8 |
| 5 | 15 | 80 ms  | 12 |
| 6 | 20 | 60 ms  | 18 |

Controls:
- Arrow keys or W/A/S/D: move
- P: pause
- Q: quit Snake
- R: replay after game over
- M: return to GameBox

The Snake module is in:
- `games/snake.c`
- `games/snake.h`
