CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
LIBS = -lraylib -lgdi32 -lwinmm -lm

TARGET = hill_climb_racer.exe
SRC = hill_climb_racer.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)