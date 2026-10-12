# C Game Box

A terminal-based C Game Box containing:
- Candy Crush
- Snake
- Sliding Image Puzzle

## Build

This project targets Windows/MinGW. The Sliding Image Puzzle uses Windows
console virtual-terminal support, so run it in Windows Terminal or a recent
Windows console host.

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
3. Sliding Puzzle
0. Exit
```

The puzzle's images are loaded from `assets/puzzle/` relative to the current
working directory. Keep that folder beside `gamebox.exe`; it contains the
included `.ppm` pictures. The puzzle menu supports arrows/WASD, Enter, and Q
to return to the Game Box.

## Snake

Eight levels are included:

| Level | Target food | Speed | Layout |
|------:|------------:|------:|--------|
| 1 | 6  | 150 ms | Open field |
| 2 | 8  | 135 ms | Pillars |
| 3 | 10 | 120 ms | Corners |
| 4 | 10 | 110 ms | Bars |
| 5 | 12 | 100 ms | Crossroads |
| 6 | 12 | 90 ms  | Rubble |
| 7 | 14 | 80 ms  | Gauntlet |
| 8 | 15 | 70 ms  | Inferno |

Controls:
- Arrow keys or W/A/S/D: move
- P: pause
- Q: quit Snake
- R: replay after game over
- M: return to GameBox

The Snake module is in:
- `games/snake.c`
- `games/snake.h`

Candy Crush was left unchanged.
