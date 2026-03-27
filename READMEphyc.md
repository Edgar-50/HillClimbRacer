 Hill Climb Racer
A full Hill Climb Racing clone built with raylib (C).

 Features
- **5 unique stages**: Countryside, Desert, Arctic, Volcano, Moon
- **Fuel system** – throttle drains fuel; run out and you roll to a stop
- **Distance tracker** with progress bar toward each stage's goal
- **Coin collection** – bobbing gold coins scattered across terrain
- **Flip counter** – land full flips for bragging rights
- **Crash detection** – flip upside-down = game over
- **Speed indicator** (km/h)
- **Procedural terrain** that streams infinitely ahead
- **Physics**: suspension springs, slope forces, wheel spin, air rotation
- **Smooth camera** that zooms out at speed

 Controls
| Key | Action |
|-----|--------|
| RIGHT / D | Gas (accelerate forward) |
| LEFT / A  | Brake / reverse |
| RIGHT / D (in air) | Rotate car clockwise |
| LEFT / A (in air)  | Rotate counter-clockwise |
| 1–5 | Quick-start a stage from menu |
| R   | Retry current stage |
| N   | Next stage (win screen) |
| M   | Return to menu |

 Build (Linux)

 Prerequisites
```bash
 Ubuntu / Debian
sudo apt install libraylib-dev

 Arch
sudo pacman -S raylib

 Fedora
sudo dnf install raylib-devel
```

 Compile & Run
```bash
gcc -Wall -O2 -std=c11 hill_climb_racer.c -o hill_climb_racer \
    -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
./hill_climb_racer
```

Or use the included Makefile:
```bash
make && ./hill_climb_racer
```

 Build (macOS)
```bash
brew install raylib
gcc -std=c11 hill_climb_racer.c -o hill_climb_racer \
    -lraylib -framework OpenGL -framework Cocoa -framework IOKit
./hill_climb_racer
```

 Build (Windows — MinGW)
Download raylib from https://github.com/raysan5/raylib/releases and:
```cmd
gcc hill_climb_racer.c -o hill_climb_racer.exe ^
    -I raylib/include -L raylib/lib ^
    -lraylib -lopengl32 -lgdi32 -lwinmm
```

 Stage Goals
| Stage | Terrain | Target |
|-------|---------|--------|
| 1 – Countryside | Gentle rolling hills | 300 m |
| 2 – Desert      | Sandy dunes          | 400 m |
| 3 – Arctic      | Icy slopes           | 500 m |
| 4 – Volcano     | Extreme jagged rock  | 600 m |
| 5 – Moon        | Low-gravity craters  | 700 m |
