CC = gcc
CFLAGS = -Wall -I.

SRCS = main.c games/candy_crush.c
OBJS = $(SRCS:.c=.o)
TARGET = gamebox

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET)