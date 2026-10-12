#include <stdio.h>
#include <stdlib.h>
#include <conio.h>
#include "games/candy_crush.h"
#include "games/snake.h"

static void clear_screen(void)
{
    system("cls");
}

static void show_menu(void)
{
    clear_screen();

    printf("===============================================\n");
    printf("                  C GAME BOX\n");
    printf("===============================================\n");
    printf("                 SELECT GAME\n");
    printf("-----------------------------------------------\n");
    printf("  1. Candy Crush\n");
    printf("  2. Snake\n");
    printf("  0. Exit\n");
    printf("-----------------------------------------------\n");
    printf("  Choose an option: ");
}

int main(void)
{
    int choice;

    while (1)
    {
        show_menu();

        if (scanf("%d", &choice) != 1)
        {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF)
                ;
            continue;
        }

        switch (choice)
        {
            case 1:
                play_candy_crush();
                break;

            case 2:
                play_snake();
                break;

            case 0:
                clear_screen();
                printf("\nThanks for playing C Game Box!\n");
                return 0;

            default:
                printf("\nInvalid choice. Press any key...\n");
                _getch();
        }
    }
}
