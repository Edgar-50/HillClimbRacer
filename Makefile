CC     = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11
SRC    = hill_climb_racer.c

ifeq ($(OS),Windows_NT)
    TARGET = hill_climb_racer.exe
    LIBS   = -lraylib -lopengl32 -lgdi32 -lwinmm -lm
else
    UNAME := $(shell uname -s)
    TARGET = hill_climb_racer
    ifeq ($(UNAME),Darwin)
        LIBS = -lraylib -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo
    else
        LIBS = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
    endif
endif

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LIBS)

# Browser build (needs emsdk on PATH and raylib built with PLATFORM=PLATFORM_WEB)
EMCC       ?= emcc
RAYLIB_SRC ?= raylib/src
WEB_OUT    ?= build/web

web:
	mkdir -p $(WEB_OUT)
	$(EMCC) $(CFLAGS) $(SRC) -o $(WEB_OUT)/index.html -I$(RAYLIB_SRC) $(RAYLIB_SRC)/libraylib.a \
	    -DPLATFORM_WEB -sUSE_GLFW=3 -sASYNCIFY -sTOTAL_MEMORY=67108864 \
	    -sEXPORTED_RUNTIME_METHODS=FS -lidbfs.js --shell-file web/shell.html

run: $(TARGET)
	./$(TARGET)

clean:
	rm -rf $(TARGET) build

.PHONY: all web run clean
