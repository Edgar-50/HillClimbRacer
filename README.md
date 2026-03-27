🚗 Hill Climb Racer

Hill Climb Racer is a physics-driven 2D side-scrolling game written entirely in C using raylib. Built as a single-file project, it explores how far minimal design can go when combining procedural generation, vehicle dynamics, and real-time rendering — without any external engines or physics libraries.



🌍 Procedural World

At its core, the game simulates a car navigating endlessly generated terrain built from smoothly interpolated height segments.
Each level is driven by a deterministic seed, ensuring reproducibility while still feeling organic and unpredictable.

From 🌿 gentle countryside hills to 🌋 aggressive volcanic terrain, each environment forces the player to adapt their driving style.

---

⚙️ Vehicle & Physics Design

The vehicle system is centred around a carefully tuned suspension model:

* 🛞 Wheels act as vertically constrained masses
* 🔧 Spring-damper system provides bounce and responsiveness
* ⚖️ Horizontal motion is stabilised to eliminate drift and roll instability

Rather than full rigid-body physics, the system focuses on stability and control, resulting in gameplay that feels smooth, responsive, and intuitive.

---

🎮 Gameplay Mechanics

* ⛽ Fuel system — manage consumption and collect canisters
* 💰 Coins & pickups — reward exploration and risk
* 🌉 Bridges & terrain variation — add pacing and structure
* 🔄 Mid-air rotation — perform flips and recover from jumps

Player input directly influences acceleration, braking, and aerial control, allowing for precise handling and satisfying movement.

---

🎥 Camera & HUD

* 🎯 Smooth camera tracking using interpolation
* 🔍 Dynamic zoom based on speed
* 📊 Real-time HUD displaying:

  * Distance
  * Fuel
  * Speed
  * Coins
  * Flip count

Everything is designed to keep the player informed without breaking immersion.



🧠 Engineering Approach

Every system is implemented from scratch:

* Terrain generation
* Collision detection
* Suspension physics
* Rendering pipeline
This makes the project a clear and practical reference for understanding game physics and real-time systems in C.
