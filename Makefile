CC = gcc
CFLAGS = -Wall -I.

SRCS = main.c games/candy_crush.c games/snake.c games/puzzle_game.c
OBJS = $(SRCS:.c=.o)
TARGET = gamebox

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET).exe

run: $(TARGET)
	./$(TARGET)
