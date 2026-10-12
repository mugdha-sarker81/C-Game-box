/*there is a tutorial function on end of the code
    you can adjust your move count or expected target by giving expectd value on the varriable
*/

#include<stdio.h>
#include<stdlib.h>
#include<time.h>
#include<stdbool.h>
#include<windows.h>

//============================USER defined functions=========================
void initial_board(void);
void show_board(void);
void find_Matches(void);
void remove_Matches(void);
void apply_Gravity(void);
void fill_Empty_Cells(void);
void swap(void);
void proccess(void);
void refresh(void);
void input(void);
bool is_valid_move(void);
void tutorial();
void clearScreen();
void start();
void showStatus();
void useMove();
int checkGameStatus();
// =========================== global variables ==============================
char box[8][8]={0};
int matched[8][8] = {0};
char fruits[]={'#','@','+','!','A'};
bool flag;
int score=0;
int a,b,x,y;
int moves= 7;
int targetScore = 50;
//===================================== MAIN FUNCTION ============================
int play_candy_crush(void)
{
    srand((unsigned int)time(NULL));

    score = 0;
    tutorial();
    start();
    initial_board();
    clearScreen();
    show_board();

    while (1){
    input();
    useMove();
    clearScreen();
    show_board();          // redraw with the updated move count
   
    if(checkGameStatus()) break;
}

    return 0;
}
//===============================function logics====================================
void show_board(){
    printf("    ");
    for(int j=0;j<8;j++){
        printf("%d ",j);
    }
    printf("\n");
    printf("   ");
    for(int j=0;j<8;j++){
        printf("--");
    }
    printf("\n");

    for(int i=0;i<8;i++){
        printf("%d | ",i);
        for(int j=0;j<8;j++){
            printf("%c ",box[i][j]);
        }
        printf("\n");
    }

    showStatus();
    return;
}

void initial_board(){

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            box[i][j] = fruits[rand() % 5];
        }
    }
    proccess();
    return;
}

void find_Matches(){

    for(int i = 0; i < 8; i++) {
        for(int j = 0; j < 6; j++) {
            if(box[i][j] == box[i][j+1] && box[i][j] == box[i][j+2]) {
                flag= true;
                matched[i][j] = 1;
                matched[i][j+1] = 1;
                matched[i][j+2] = 1;
            }
        }
    }
    for(int i = 0; i < 6; i++) {
        for(int j = 0; j < 8; j++) {
            if((box[i][j] == box[i+1][j]) && (box[i][j] == box[i+2][j])) {
                flag= true;
                matched[i][j] = 1;
                matched[i+1][j] = 1;
                matched[i+2][j] = 1;
            }
        }
    }
    return;
}

void remove_Matches(){

    for(int i=0;i<8;i++){
        for(int j=0; j<8;j++){
            if(matched[i][j]){
                box[i][j]='.';
            }
        }
    }
    return;
}

void swap(){
    char tmp = box[a][b];
    box[a][b]= box[x][y]; box[x][y]= tmp;
    return;
}

void apply_Gravity(){
    for (int j = 0; j < 8; j++) {

        int pos=7;
        for(int i=7;i>=0;i--){
            if(box[i][j]!='.'){
                box[pos][j]= box[i][j];
                pos--;
            }
        }
        while(pos >= 0) {
            box[pos][j] = '.';
            pos--;
        }
    }
    return;
}

void fill_Empty_Cells(){

    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            if(box[i][j]=='.'){
                score= score+1;
                box[i][j] = fruits[rand() % 5];
            }
        }
    }
    return;
}

void proccess(){

    flag = true;
    bool anyMatchEver = false; 

    while(flag){
        flag=false; refresh();
        find_Matches();

        if(flag){
            anyMatchEver = true;
            remove_Matches();
            clearScreen();
            show_board();
            Sleep(1000);

            apply_Gravity();
            clearScreen();
            show_board();
            Sleep(300);
            fill_Empty_Cells();

            clearScreen();
            show_board();
            Sleep(300);
        }
    }

    if(!anyMatchEver){
        clearScreen();
        show_board();
    }

    printf("\n");
    return;
}

void refresh(){
    for(int i=0;i<8;i++)
        for(int j=0;j<8;j++)
            matched[i][j]=0;

    return;
}

bool is_valid_move(){
    return (a==x && (b==y-1 || b==y+1)) || (b==y && (a==x-1 || a==x+1));
}

void input(){
    bool valid = false;

    while (!valid) {
        printf("which pair you want to swap: ");

        int rc = scanf("%d %d", &a, &x);
        if (rc == EOF) {
            printf("\nno more input available, exiting.\n");
            exit(0);
        }
        if (rc != 2) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF);
            printf("invalid input, please enter two numbers (e.g. 34 45)\n");
            continue;
        }
        printf("\n");

        b = a % 10; a /= 10;
        y = x % 10; x /= 10;

        if (a < 0 || a > 7 || b < 0 || b > 7 ||
            x < 0 || x > 7 || y < 0 || y > 7) {
            printf("invalid pair ! coordinates must be 0-7, try again\n");
            continue;
        }
        if (!is_valid_move()) {
            printf("invalid pair ! try again\n");
            continue;
        }
        valid = true;
    }
    swap();
    proccess();

    return;
}

void tutorial(){
    printf("===================================================\n");
    printf("               WELCOME TO CANDY CRUSH\n");
    printf("===================================================\n");
    printf("The board is an 8x8 Matix. Each cell has a row (0-7)\n");
    printf("and a column (0-7)  shown as labels on the board\n");
    printf("just like chess coordinates:\n\n");
    printf("    0 1 2 3 4 5 6 7 \n");
    printf("   ----------------\n");
    printf("0 | A B C D E A B C \n");
    printf("1 | B A D C B C A D \n");
    printf("  |   ...            \n\n");

    printf("HOW TO SWAP:\n");
    printf("  Enter two cells as two-digit numbers: row then\n");
    printf("  column  with a space between the two cells.\n");
    printf("  Example: 34 35\n");
    printf("    -> swaps row 3, col 4  with  row 3, col 5\n\n");
    printf("RULES:\n");
    printf("  - You can only swap cells that are directly next\n");
    printf("    to each other (up, down, left, or right).\n");
    printf("  - Matching 3 or more of the same letter in a row\n");
    printf("    or column clears them and earns you points.\n");
    printf("    a chain reaction will boost your score.\n");
    printf("  - You got (%d) moves and initial (50) target-score;\n", moves); 
    printf("================= use them wisely ! ================\n\n");
    return;
}

void clearScreen(){
    system("cls");
    fflush(stdout);
}
void start(){
    
    int n;
    while (1) {
        printf("press 5 to get started: ");
        if (scanf("%d", &n) != 1) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF); // clear bad input
            continue;
        }
        if (n == 5) {
            return;
        }
        printf("you haven,t press 5!\n");
    }
}

int checkGameStatus()
{
    if (score >= targetScore)
    {
        system("cls");

        printf("\n\n");
        printf("=================================\n");
        printf("           YOU WON!\n");
        printf("=================================\n");
        printf("Target Score : %d\n", targetScore);
        printf("Your Score   : %d\n", score);
        printf("Moves Left   : %d\n", moves);
        printf("=================================\n");

        return 1;
    }

    if (moves <= 0)
    {
        system("cls");

        printf("\n\n");
        printf("=================================\n");
        printf("          GAME OVER!\n");
        printf("=================================\n");
        printf("Target Score : %d\n", targetScore);
        printf("Your Score   : %d\n", score);
        printf("=================================\n");

        return 1;
    }

    return 0;
}

void useMove()
{
    moves= moves-1; 
}
void showStatus()
{
    printf("\nScore: %d / %d", score, targetScore);
    printf("    Moves Left: %d\n\n", moves);
}