#ifndef PUZZLE_GAME_H
#define PUZZLE_GAME_H

/*
 * Sliding Image Puzzle (8-puzzle) module for the terminal Game Box.
 *
 * Integration:
 *   1. Add puzzle_game.c to your build.
 *   2. #include "puzzle_game.h" in your menu code.
 *   3. Call  puzzle_game_run();  when the player picks "Puzzle".
 *      It returns when the player chooses "Back to Game Box".
 *
 * Images: every *.ppm (binary P6) file in PUZZLE_ASSET_DIR is listed as a
 * selectable picture, so you can drop your own images there.  Run
 * tools/make_images.py to (re)generate the 7 built-in anime-style scenes.
 */

#define PUZZLE_ASSET_DIR   "assets/puzzle"
#define PUZZLE_MAX_IMAGES  16
#define PUZZLE_LEVELS      6

#ifdef __cplusplus
extern "C" {
#endif

void puzzle_game_run(void);

#ifdef __cplusplus
}
#endif

#endif
