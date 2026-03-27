#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#include <raylib.h>
#include <raymath.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

//window 
#define SW 1400
#define SH  800

// terrain
#define SEG     100          /* pixels per segment                            */
#define TBUF    900          /* circular-buffer size                          */
static Vector2 TB[TBUF];
static int  T_n   = 0;      /* total points ever generated                   */
static int  T_b   = 0;      /* index of oldest kept point                    */
static float TgX  = 0;
static float TgY  = 0;

//physics 
/* Everything is in pixels / second.
   The car drives RIGHT (+x).  Screen-y is DOWN so "up" is -y.
   We never touch angle in the physics except for body alignment on ground.   */
#define GRAVITY      1200.0f   /* px/s² downward (+y)                        */
#define DRIVE         900.0f   /* wheel x-force when throttle pressed         */
#define BRAKE_F       600.0f
#define MAX_SPD_X     650.0f
#define WHEEL_FRIC    0.88f    /* x-velocity multiplier per 60 Hz tick        */
#define WHEEL_BNC     0.06f    /* y restitution on ground hit                 */
#define SUSP_LEN       80.0f   /* rest length: underside → wheel centre       */
#define SUSP_K         55.0f   /* spring constant — stiffer for snappy bounce */
#define SUSP_D          4.5f   /* low damping = bouncy, springy feel          */
#define AIR_ROT       200.0f   /* deg/s — fast enough for full flips in air   */

// game
#define FUEL_MAX      100.0f
#define FUEL_GAS       14.0f   /* drain/s while throttle — aggressive!        */
#define FUEL_IDLE       0.80f  /* passive drain/s — constant pressure          */
#define MAX_COINS        80
#define MAX_CANISTERS    30
#define MAX_LEVELS        5
#define FLIP_DEAD_DEG   145.0f /* body angle past this = crash                */

//colour helpers
#define RGBA(r,g,b,a) ((Color){(r),(g),(b),(a)})
#define RGB(r,g,b)    RGBA(r,g,b,255)

//ATA TYPES
typedef struct {
    const char *name;
    Color sky0, sky1;          /* gradient top→bot                           */
    Color gnd, dirt;
    float rough;               /* max dy per terrain seg                     */
    float goal_m;              /* distance to win (metres = px/100)          */
    unsigned seed;
    /* decoration colours */
    Color deco;
} Level;

static Level LV[MAX_LEVELS] = {
    /* name          sky0                sky1              gnd               dirt              rough  goal seed  deco */
    {"COUNTRYSIDE", RGB(80,160,230),  RGB(180,220,255), RGB(60,140,50),  RGB(110,75,35),    28, 300, 1111, RGB(30,100,30)  },
    {"DESERT",      RGB(255,185,60),  RGB(255,225,140), RGB(200,155,70), RGB(160,105,40),   50, 500, 2222, RGB(180,120,30) },
    {"ARCTIC",      RGB(160,205,255), RGB(225,242,255), RGB(200,228,255),RGB(150,185,215),  26, 650, 3333, RGB(200,235,255)},
    {"VOLCANO",     RGB(30,6,6),      RGB(70,18,6),     RGB(80,38,18),   RGB(55,18,4),      70, 800, 4444, RGB(200,60,10)  },
    {"MOON",        RGB(4,4,18),      RGB(12,12,40),    RGB(105,105,105),RGB(70,70,70),     55,1000, 5555, RGB(180,180,200)},
};

typedef struct {
    Vector2 pos, vel;
    float   r;          /* radius                                             */
    float   ax;         /* attachment local-x offset from car centre          */
    bool    on;         /* on ground this frame                               */
    float   spin;       /* visual spin angle degrees                          */
} Wheel;

typedef struct {
    Vector2 pos;        /* chassis centre                                     */
    Vector2 vel;
    float   angle;      /* body tilt degrees; 0 = flat/facing right           */
    float   avel;       /* angular velocity deg/s                             */
    float   hw, hh;     /* half width, half height                            */
    Wheel   rear, fwd;  /* rear=left(−x)  fwd=right(+x)                      */
    float   fuel;
    float   dist;       /* metres                                             */
    int     coins;
    int     flips;
    float   flip_acc;
    float   prev_ang;
    bool    prev_gnd;
    bool    dead;
    float   dead_t;
} Car;

typedef struct { Vector2 pos; bool got; float ph; } Coin;
typedef struct { Vector2 pos; bool got; float ph; } Canister;

/* Bridge: a flat plank spanning between two terrain points.
   Represented as a horizontal segment at a fixed Y, with two stone pillars. */
#define MAX_BRIDGES 12
typedef struct {
    float x0, x1;   /* world X of left/right edges                           */
    float y;         /* world Y of bridge deck surface                        */
    bool  active;
} Bridge;

typedef enum { ST_MENU, ST_PLAY, ST_DEAD, ST_WIN } GState;

//GLOBALS
static GState    g_st    = ST_MENU;
static int       g_lv    = 0;
static Car       g_car;
static Camera2D  g_cam;
static Coin      g_coins[MAX_COINS];
static int       g_ncoin = 0;
static Canister  g_cans[MAX_CANISTERS];
static int       g_ncan  = 0;
static Bridge    g_bridges[MAX_BRIDGES];
static int       g_nbridge = 0;
/* menu animation */
static float     g_menu_t = 0;

//TERRAIN
static float rnd_f(float lo, float hi){
    return lo + (hi-lo)*((float)GetRandomValue(0,32767)/32767.0f);
}

static void terrain_add(void){
    float rough = LV[g_lv].rough;
    TgY = Clamp(TgY + rnd_f(-rough, rough), SH*0.15f, SH*0.86f);
    TB[T_n % TBUF] = (Vector2){TgX, TgY};
    T_n++;
    TgX += SEG;
    if(T_n - T_b >= TBUF-1) T_b++;
}

static void terrain_init(void){
    SetRandomSeed(LV[g_lv].seed);
    T_n=0; T_b=0; TgX=0; TgY=SH*0.65f;
    /* flat start */
    for(int i=0;i<14;i++){
        TB[T_n%TBUF]=(Vector2){TgX,TgY};
        T_n++; TgX+=SEG;
    }
    while(T_n<TBUF-2) terrain_add();
}

static void terrain_extend(float cx){
    while(TgX < cx + SEG*110) terrain_add();
}

/* surface Y at world-x (or very large if out of range) */
static float surf_y(float x){
    for(int i=T_b; i<T_n-1; i++){
        Vector2 a=TB[i%TBUF], b=TB[(i+1)%TBUF];
        if(x>=a.x && x<=b.x){
            float t=(x-a.x)/(b.x-a.x);
            return a.y + t*(b.y-a.y);
        }
    }
    return SH*4.0f;
}

/* resolve circle; returns true if touching */
static bool resolve(Vector2 *p, Vector2 *v, float r){
    float sy=surf_y(p->x);
    if(p->y+r >= sy){
        p->y = sy-r;
        if(v->y>0) v->y = -v->y*WHEEL_BNC;
        return true;
    }
    return false;
}

//PICKUPS
static void pickups_init(void){
    g_ncoin=0; g_ncan=0; g_nbridge=0;
    /* scatter coins every 5 segs, canisters every 20 segs starting seg 15 */
    for(int i=15; i<T_n-1 && g_ncoin<MAX_COINS; i+=5){
        Vector2 p=TB[i%TBUF];
        g_coins[g_ncoin++]=(Coin){{p.x, p.y-55}, false, (float)i*0.7f};
    }
    for(int i=25; i<T_n-1 && g_ncan<MAX_CANISTERS; i+=20){
        Vector2 p=TB[i%TBUF];
        g_cans[g_ncan++]=(Canister){{p.x, p.y-62}, false, (float)i*1.1f};
    }
    /* Bridges: place every 35 segments starting at seg 30.
       The deck Y is set to be roughly level with the terrain at that point,
       elevated slightly so it floats above the gap.                          */
    for(int i=30; i<T_n-4 && g_nbridge<MAX_BRIDGES; i+=35){
        Vector2 pa=TB[i%TBUF], pb=TB[(i+3)%TBUF];
        float deck_y = (pa.y+pb.y)*0.5f - 12.0f;  /* slightly above terrain */
        g_bridges[g_nbridge++]=(Bridge){pa.x, pb.x, deck_y, true};
    }
}

//CAR INIT

static void car_init(void){
    terrain_init();
    pickups_init();

    /* start on seg 8 */
    Vector2 sp = TB[(T_b+8)%TBUF];
    float sx=sp.x, sy=sp.y-130;

    g_car=(Car){0};
    g_car.pos=(Vector2){sx,sy};
    g_car.hw=100.0f; g_car.hh=36.0f;
    g_car.fuel=FUEL_MAX;

    /* rear wheel: sits at -x, front at +x; car drives RIGHT */
    g_car.rear.r=34; g_car.rear.ax=-68;
    g_car.rear.pos=(Vector2){sx-68, sy+g_car.hh+SUSP_LEN};

    g_car.fwd.r=32; g_car.fwd.ax=+68;
    g_car.fwd.pos=(Vector2){sx+68, sy+g_car.hh+SUSP_LEN};

    g_st=ST_PLAY;
}

//UPDATE
static void update(float dt){
    Car *c=&g_car;

    //dead fall-through
    if(c->dead){
        c->dead_t+=dt;
        Wheel *ws[2]={&c->rear,&c->fwd};
        for(int i=0;i<2;i++){
            ws[i]->vel.y+=GRAVITY*dt;
            ws[i]->pos.x+=ws[i]->vel.x*dt;
            ws[i]->pos.y+=ws[i]->vel.y*dt;
            resolve(&ws[i]->pos,&ws[i]->vel,ws[i]->r);
        }
        c->vel.y+=GRAVITY*0.3f*dt;
        c->pos.x+=c->vel.x*dt; c->pos.y+=c->vel.y*dt;
        if(c->dead_t>2.4f) g_st=ST_DEAD;
        return;
    }

    bool gas   = IsKeyDown(KEY_RIGHT)||IsKeyDown(KEY_D);
    bool brake = IsKeyDown(KEY_LEFT) ||IsKeyDown(KEY_A);

    /* fuel ─*/
    c->fuel -= (gas ? FUEL_GAS : FUEL_IDLE)*dt;
    c->fuel = Clamp(c->fuel,0,FUEL_MAX);
    bool has_fuel = c->fuel>0.01f;

    /*── drive/brake: applied to body X (wheels are locked to body X anyway) --*/
    bool rear_on = c->rear.on, fwd_on = c->fwd.on;
    bool any_on  = rear_on || fwd_on;
    if(any_on){
        float accel = 0;
        if(gas   && has_fuel) accel += DRIVE;
        if(brake)             accel -= BRAKE_F;
        c->vel.x += accel * dt;
        c->vel.x = Clamp(c->vel.x, -MAX_SPD_X, MAX_SPD_X);
        /* rolling friction when coasting */
        if(!gas && !brake){
            float fric = powf(WHEEL_FRIC, dt*60.0f);
            c->vel.x *= fric;
        }
    }

    /*── wheel Y physics (gravity + ground resolve only) ─*/
    Wheel *ws[2]={&c->rear,&c->fwd};
    for(int i=0;i<2;i++){
        Wheel *w=ws[i];
        w->vel.y += GRAVITY*dt;
        w->pos.y += w->vel.y*dt;
        /* X will be locked in suspension block below, skip x integration    */
        w->on = resolve(&w->pos, &w->vel, w->r);

        /* Bridge collision: treat deck as a flat surface                     */
        if(!w->on){
            for(int b=0;b<g_nbridge;b++){
                Bridge *br=&g_bridges[b];
                if(!br->active) continue;
                if(w->pos.x >= br->x0 && w->pos.x <= br->x1){
                    float deck_surf = br->y;
                    if(w->pos.y+w->r >= deck_surf && w->pos.y < deck_surf){
                        w->pos.y = deck_surf - w->r;
                        if(w->vel.y>0) w->vel.y = -w->vel.y*0.04f;
                        w->on = true;
                    }
                }
            }
        }
        /* visual spin from body x-velocity */
        if(w->on) w->spin += c->vel.x*dt * (float)(180.0/(M_PI*w->r));
    }


    /*── RIGID VERTICAL SUSPENSION 
      Wheels ride on vertical struts — they NEVER swing sideways.
        1. Wheel X is hard-locked to mount X every frame (no lateral drift).
        2. Wheel Y bounces freely: spring + damper in Y only.
        3. Zero lateral force, zero torque from suspension.
        4. Hard stops prevent strut inversion (wheel above mount).           */
    float rad = c->angle*(float)(M_PI/180.0);
    float cr=cosf(rad), sr=sinf(rad);

    for(int i=0;i<2;i++){
        Wheel *w=ws[i];

        /* Underside mount in world space (local ax along body, hh downward) */
        float mount_x = c->pos.x + cr*w->ax + sr*c->hh;
        float mount_y = c->pos.y + sr*w->ax + cr*c->hh;

        /* HARD LOCK: wheel X tracks mount X exactly — strut stays vertical  */
        w->pos.x = mount_x;
        w->vel.x = c->vel.x;

        /* Vertical spring+damper (Y only) */
        float dy     = w->pos.y - mount_y;   /* +ve = below mount (good)     */
        float relvy  = w->vel.y - c->vel.y;
        float force  = (dy - SUSP_LEN)*SUSP_K + relvy*SUSP_D;

        c->vel.y += force * dt / 3.0f;   /* body: lighter push              */
        w->vel.y -= force * dt;           /* wheel: full reaction             */

        /* Stop 1: wheel cannot go above mount (strut cannot invert)         */
        if(w->pos.y < mount_y){ w->pos.y=mount_y; if(w->vel.y<0) w->vel.y=0; }
        /* Stop 2: wheel cannot stretch past 1.85x rest length               */
        if(dy > SUSP_LEN*2.2f){
            w->pos.y = mount_y + SUSP_LEN*2.2f;
            if(w->vel.y > c->vel.y) w->vel.y = c->vel.y;
        }
    }
    /*── body gravity */
    bool gnd = c->rear.on || c->fwd.on;
    c->vel.y += GRAVITY * (gnd ? 0.10f : 0.32f) * dt;

    /*── air rotation ─*/
    if(!gnd){
        /* gas tips nose UP (angle negative = counterclockwise = nose rises) */
        if(gas)   c->avel -= AIR_ROT*dt;
        if(brake) c->avel += AIR_ROT*dt;
    } else {
        /* align body to wheel-axis angle */
        float wa = atan2f(c->fwd.pos.y - c->rear.pos.y,
                          c->fwd.pos.x - c->rear.pos.x) * (float)(180.0/M_PI);
        float diff = wa - c->angle;
        while(diff> 180) diff-=360;
        while(diff<-180) diff+=360;
        c->avel += diff * 15.0f * dt;
    }

    c->avel *= (1.0f - 5.0f*dt);       /* angular damping */
    c->angle += c->avel*dt;

    /* very light air drag on x */
    if(!gnd) c->vel.x *= (1.0f - 0.4f*dt);

    c->pos.x += c->vel.x*dt;
    c->pos.y += c->vel.y*dt;

    /* prevent body centre sinking below ground */
    {
        float sy = surf_y(c->pos.x);
        if(c->pos.y + c->hh*0.5f > sy){
            c->pos.y = sy - c->hh*0.5f;
            if(c->vel.y>0) c->vel.y*=-0.05f;
        }
    }

    /*── distance */
    if(c->pos.x > c->dist*100.0f) c->dist = c->pos.x/100.0f;

    /*── flip counter*/
    {
        float da = c->angle - c->prev_ang;
        while(da> 180) da-=360;
        while(da<-180) da+=360;
        c->flip_acc += da;
        if(!gnd) c->prev_gnd=false;
        else if(!c->prev_gnd){
            if(fabsf(c->flip_acc)>=340.0f) c->flips++;
            c->flip_acc=0; c->prev_gnd=true;
        }
        c->prev_ang = c->angle;
    }

    /*── death: only when stuck upside-down (not mid-flip) ───────────────────
      Allow full flips: only kill if angle is in the "roof on ground" zone
      AND angular velocity is small (car has stopped spinning).               */
    {
        float na = fmodf(c->angle, 360.0f);
        if(na<0) na+=360;
        bool upside = (na>FLIP_DEAD_DEG && na<360-FLIP_DEAD_DEG);
        /* Only kill if nearly stationary angularly — allows spinning flips   */
        if(upside && fabsf(c->avel)<25.0f && gnd) c->dead=true;
    }

    /* collect coins */
    {
        float t=(float)GetTime();
        for(int i=0;i<g_ncoin;i++){
            if(g_coins[i].got) continue;
            Vector2 cp={g_coins[i].pos.x,
                        g_coins[i].pos.y+sinf(t*3+g_coins[i].ph)*5};
            if(Vector2Distance(c->pos,cp)<70||
               Vector2Distance(c->rear.pos,cp)<52||
               Vector2Distance(c->fwd.pos,cp)<52){
                g_coins[i].got=true; c->coins++;
            }
        }
        for(int i=0;i<g_ncan;i++){
            if(g_cans[i].got) continue;
            Vector2 cp={g_cans[i].pos.x,
                        g_cans[i].pos.y+sinf(t*2+g_cans[i].ph)*4};
            if(Vector2Distance(c->pos,cp)<85||
               Vector2Distance(c->rear.pos,cp)<60||
               Vector2Distance(c->fwd.pos,cp)<60){
                g_cans[i].got=true;
                c->fuel=Clamp(c->fuel+40.0f,0,FUEL_MAX);
            }
        }
    }

    terrain_extend(c->pos.x);
    if(c->dist >= LV[g_lv].goal_m) g_st=ST_WIN;
}

/*DRAW – helper world-space point from car-local (fx fraction of hw, fy fraction of hh)  */
static Vector2 cpt(float rad, float fx, float fy){
    float cr=cosf(rad), sr=sinf(rad);
    float lx=g_car.hw*fx, ly=g_car.hh*fy;
    return (Vector2){
        g_car.pos.x + cr*lx - sr*ly,
        g_car.pos.y + sr*lx + cr*ly
    };
}

static void draw_sky(void){
    Color a=LV[g_lv].sky0, b=LV[g_lv].sky1;
    for(int y=0;y<SH;y++){
        float t=(float)y/SH;
        DrawLine(0,y,SW,y,ColorLerp(a,b,t));
    }
}

/* level-specific decoration: trees / cacti / ice spikes / lava pools / craters */
static void draw_deco(void){
    Color dc=LV[g_lv].deco;
    for(int i=T_b+1; i<T_n-1; i++){
        Vector2 p=TB[i%TBUF];
        /* only draw in view */
        Vector2 screen = GetWorldToScreen2D(p, g_cam);
        if(screen.x < -120 || screen.x > SW+120) continue;
        /* every 3rd point gets decoration */
        if(i%3 != 0) continue;
        switch(g_lv){
            case 0: /* tree */
                DrawRectangle((int)p.x-4,(int)p.y-35,8,35, RGB(80,45,15));
                DrawTriangle((Vector2){p.x,p.y-70},(Vector2){p.x-16,p.y-35},(Vector2){p.x+16,p.y-35},dc);
                DrawTriangle((Vector2){p.x,p.y-90},(Vector2){p.x-11,p.y-65},(Vector2){p.x+11,p.y-65},dc);
                break;
            case 1: /* cactus */
                DrawRectangle((int)p.x-5,(int)p.y-45,10,45,dc);
                DrawRectangle((int)p.x-18,(int)p.y-35,13,8, dc);
                DrawRectangle((int)p.x+5, (int)p.y-28,13,8, dc);
                break;
            case 2: /* ice spike */
                DrawTriangle((Vector2){p.x,p.y-38},(Vector2){p.x-7,p.y},(Vector2){p.x+7,p.y},dc);
                break;
            case 3: /* lava rock */
                DrawCircle((int)p.x,(int)p.y-6,9,dc);
                break;
            case 4: /* crater rim */
                DrawEllipse((int)p.x,(int)p.y-3,18,6,dc);
                break;
        }
    }
}

static void draw_terrain(void){
    Color gc=LV[g_lv].gnd, dc=LV[g_lv].dirt;
    for(int i=T_b; i<T_n-1; i++){
        Vector2 a=TB[i%TBUF], b=TB[(i+1)%TBUF];
        /* only draw visible */
        Vector2 sa=GetWorldToScreen2D(a,g_cam);
        if(sa.x > SW+200) break;
        if(sa.x < -SEG-200) continue;

        Vector2 ba={a.x,(float)(SH+400)}, bb={b.x,(float)(SH+400)};
        DrawTriangle(a,ba,b,gc); DrawTriangle(b,ba,bb,gc);
        Vector2 a2={a.x,a.y+18},b2={b.x,b.y+18};
        DrawTriangle(a,a2,b,dc); DrawTriangle(b,a2,b2,dc);
        DrawLineEx(a,b,5,dc);
        DrawLineEx(a,b,2.5f,RGBA(0,0,0,180));
    }
}

static void draw_wheel(Wheel *w){
    Vector2 p=w->pos; float r=w->r;
    /* shadow */
    DrawEllipse((int)p.x+3,(int)p.y+5,(int)r,(int)(r*0.5f),RGBA(0,0,0,50));
    /* tyre */
    DrawCircleV(p,r,RGB(22,22,22));
    /* tread bumps */
    for(int i=0;i<10;i++){
        float a=(w->spin+i*36.0f)*(float)(M_PI/180.0);
        DrawCircleV((Vector2){p.x+cosf(a)*(r-4),p.y+sinf(a)*(r-4)},3.5f,RGB(45,45,45));
    }
    /* rim */
    DrawCircleV(p,r*0.54f,RGB(200,200,200));
    DrawCircleV(p,r*0.17f,RGB(35,35,35));
    /* spokes */
    for(int i=0;i<5;i++){
        float a=(w->spin*0.5f+i*72.0f)*(float)(M_PI/180.0);
        Vector2 sp={p.x+cosf(a)*r*0.52f,p.y+sinf(a)*r*0.52f};
        DrawLineEx(p,sp,3.5f,RGB(175,175,175));
    }
}

static void draw_car(void){
    Car *c=&g_car;
    float rad=c->angle*(float)(M_PI/180.0);

    /* ── LAYER 0: suspension arms (drawn first, behind everything) ─
       Mounts are on the UNDERSIDE of chassis: fy=+1.0 (bottom edge).
       Each arm is a double line for an A-arm look.                            */
    Vector2 mnt_r = cpt(rad,-0.68f,+1.0f);
    Vector2 mnt_f = cpt(rad,+0.68f,+1.0f);
    float arm_off=5.5f;
    Vector2 perp={-sinf(rad)*arm_off, cosf(rad)*arm_off};
    DrawLineEx((Vector2){mnt_r.x+perp.x,mnt_r.y+perp.y}, c->rear.pos, 7, RGB(35,35,35));
    DrawLineEx((Vector2){mnt_r.x-perp.x,mnt_r.y-perp.y}, c->rear.pos, 7, RGB(35,35,35));
    DrawLineEx((Vector2){mnt_f.x+perp.x,mnt_f.y+perp.y}, c->fwd.pos,  7, RGB(35,35,35));
    DrawLineEx((Vector2){mnt_f.x-perp.x,mnt_f.y-perp.y}, c->fwd.pos,  7, RGB(35,35,35));
    /* highlight line down centre of each arm */
    DrawLineEx(mnt_r, c->rear.pos, 3, RGB(70,70,70));
    DrawLineEx(mnt_f, c->fwd.pos,  3, RGB(70,70,70));
    DrawCircleV(mnt_r,7,RGB(50,50,50)); DrawCircleV(mnt_r,3,RGB(155,155,155));
    DrawCircleV(mnt_f,7,RGB(50,50,50)); DrawCircleV(mnt_f,3,RGB(155,155,155));

    /* ── LAYER 1: main chassis block 
       fy=-1 = top edge,  fy=+1 = bottom edge.  Car faces RIGHT (+x).         */
    DrawRectanglePro(
        (Rectangle){c->pos.x,c->pos.y,c->hw*2,c->hh*2},
        (Vector2){c->hw,c->hh}, c->angle, RGB(205,38,38));

    /* racing stripe across lower third of body */
    Vector2 st0=cpt(rad,-1.0f,+0.30f), st1=cpt(rad,+1.0f,+0.30f);
    Vector2 st2=cpt(rad,-1.0f,+0.80f), st3=cpt(rad,+1.0f,+0.80f);
    DrawTriangle(st0,st2,st1,RGBA(245,245,245,220));
    DrawTriangle(st1,st2,st3,RGBA(245,245,245,220));

    /* body outline */
    DrawRectanglePro(
        (Rectangle){c->pos.x,c->pos.y,c->hw*2,c->hh*2},
        (Vector2){c->hw,c->hh}, c->angle, RGBA(0,0,0,55));

    /* ── LAYER 2: cabin — sits ABOVE chassis, base at fy=-1.0 (top of body) ──
       Cabin corners in local coords (fraction of hw/hh):
         bottom-rear  (-0.55, -1.0)   bottom-front (+0.45, -1.0)
         top-rear     (-0.42, -2.80)  top-front    (+0.32, -2.80)
       Note fy=-1 is already the top edge of the chassis, so cabin starts
       flush at the top and projects upward (more negative fy).               */
    Vector2 cab_br_base = cpt(rad,-0.55f,-1.0f);  /* cabin bottom-rear         */
    Vector2 cab_fr_base = cpt(rad,+0.45f,-1.0f);  /* cabin bottom-front        */
    Vector2 cab_br_top  = cpt(rad,-0.42f,-2.80f); /* cabin top-rear (tapered)  */
    Vector2 cab_fr_top  = cpt(rad,+0.30f,-2.80f); /* cabin top-front (tapered) */

    /* Fill cabin with two triangles */
    DrawTriangle(cab_br_base, cab_br_top, cab_fr_base, RGB(160,28,28));
    DrawTriangle(cab_fr_base, cab_br_top, cab_fr_top,  RGB(160,28,28));

    /* Cabin outline */
    DrawLineEx(cab_br_base, cab_br_top,  2.5f, RGBA(0,0,0,170));
    DrawLineEx(cab_br_top,  cab_fr_top,  2.5f, RGBA(0,0,0,170));
    DrawLineEx(cab_fr_top,  cab_fr_base, 2.5f, RGBA(0,0,0,170));
    DrawLineEx(cab_fr_base, cab_br_base, 2.0f, RGBA(0,0,0,100)); /* bottom seam */

    /* ── LAYER 3: windscreen (front face of cabin) 
       Goes from cabin-bottom-front up to cabin-top-front, slanted.           */
    Vector2 ws_bl = cab_fr_base;
    Vector2 ws_br = cpt(rad,+0.95f,-1.0f);  /* windscreen base right edge     */
    Vector2 ws_tl = cab_fr_top;
    Vector2 ws_tr = cpt(rad,+0.80f,-2.70f); /* windscreen top right (angled)  */
    DrawTriangle(ws_bl, ws_tl, ws_br, RGBA(120,200,255,210));
    DrawTriangle(ws_br, ws_tl, ws_tr, RGBA(120,200,255,210));
    /* frame lines */
    DrawLineEx(ws_bl, ws_tl, 3.0f, RGBA(0,0,0,180));
    DrawLineEx(ws_tl, ws_tr, 3.0f, RGBA(0,0,0,180));
    DrawLineEx(ws_tr, ws_br, 3.0f, RGBA(0,0,0,180));
    /* glint */
    DrawCircleV(cpt(rad,+0.62f,-1.90f), 4, RGBA(255,255,255,160));
    DrawCircleV(cpt(rad,+0.52f,-2.20f), 2, RGBA(255,255,255,120));

    /* ── LAYER 4: rear window ─*/
    Vector2 rw_bl = cpt(rad,-0.96f,-1.0f);
    Vector2 rw_br = cab_br_base;
    Vector2 rw_tl = cpt(rad,-0.78f,-2.68f);
    Vector2 rw_tr = cab_br_top;
    DrawTriangle(rw_bl, rw_tl, rw_br, RGBA(90,165,215,180));
    DrawTriangle(rw_br, rw_tl, rw_tr, RGBA(90,165,215,180));
    DrawLineEx(rw_bl, rw_tl, 3.0f, RGBA(0,0,0,170));
    DrawLineEx(rw_tl, rw_tr, 3.0f, RGBA(0,0,0,170));
    DrawLineEx(rw_tr, rw_br, 3.0f, RGBA(0,0,0,170));

    /* ── LAYER 5: driver — head and helmet sit inside cabin 
       Head is at fy=-1.65 (inside cabin, above chassis top), centred x=-0.05
       Helmet cap sits above head: fy=-2.15                                   */
    Vector2 hd = cpt(rad,-0.05f,-1.65f);
    DrawCircleV(hd, 13, RGB(255,210,170));              /* face/skin           */
    /* helmet — shifted upward in local space (fy more negative) */
    Vector2 helm = cpt(rad,-0.05f,-2.10f);
    DrawCircleV(helm, 14, RGB(20,20,190));              /* helmet body         */
    DrawCircleV(helm, 14, RGBA(20,20,190,180));
    /* visor strip across middle of helmet */
    Vector2 vis_l = cpt(rad,-0.18f,-1.92f);
    Vector2 vis_r = cpt(rad,+0.08f,-1.92f);
    DrawLineEx(vis_l, vis_r, 6, RGBA(50,150,255,200));

    /* ── LAYER 6: front headlight ─*/
    Vector2 hl=cpt(rad,+1.01f,-0.18f);
    DrawCircleV(hl,11,RGB(255,255,175));
    DrawCircleV(hl, 7,RGB(255,255,255));
    DrawCircleV(hl, 3,RGB(255,235,80));

    /* ── LAYER 7: rear exhaust pipe ─*/
    Vector2 ex0=cpt(rad,-1.01f,-0.38f);
    Vector2 ex1={ex0.x-cosf(rad)*26, ex0.y-sinf(rad)*26};
    DrawLineEx(ex0,ex1,10,RGB(45,45,45));
    DrawLineEx(ex0,ex1, 6,RGB(72,72,72));
    DrawCircleV(ex1,5,RGB(38,38,38));

    /* ── LAYER 8: fuel can strapped to rear of roof 
       Sits at the rear-top of cabin: fy≈-2.5 (above chassis, behind cabin)   */
    Vector2 fc=cpt(rad,-0.58f,-2.55f);
    DrawRectanglePro((Rectangle){fc.x,fc.y,28,19},(Vector2){14,19},c->angle,RGB(210,58,28));
    DrawRectanglePro((Rectangle){fc.x,fc.y,14,19},(Vector2){7, 19},c->angle,RGB(238,75,38));
    /* strap */
    Vector2 sp0=cpt(rad,-0.72f,-2.28f), sp1=cpt(rad,-0.40f,-2.28f);
    DrawLineEx(sp0,sp1,4,RGB(68,38,12));

    /* ── LAYER 9: front bumper bar ─*/
    Vector2 bp0=cpt(rad,+1.02f,+0.60f), bp1=cpt(rad,+1.02f,-0.68f);
    DrawLineEx(bp0,bp1,9,RGB(42,42,42));
    DrawLineEx(bp0,bp1,5,RGB(165,165,165));

    /* ── LAYER 10: number plate (front lower) ─*/
    Vector2 plt=cpt(rad,+0.60f,+0.55f);
    DrawRectanglePro((Rectangle){plt.x,plt.y,30,15},(Vector2){15,7},c->angle,RGB(248,248,248));
    DrawRectanglePro((Rectangle){plt.x,plt.y,30,15},(Vector2){15,7},c->angle,RGBA(0,0,0,40));
}

/* bridges ─*/
static void draw_bridges(void){
    for(int i=0;i<g_nbridge;i++){
        Bridge *b=&g_bridges[i];
        if(!b->active) continue;
        Vector2 sc =GetWorldToScreen2D((Vector2){b->x0,b->y},g_cam);
        Vector2 sc2=GetWorldToScreen2D((Vector2){b->x1,b->y},g_cam);
        if(sc2.x < -20 || sc.x > SW+20) continue;

        float x0=b->x0, x1=b->x1, y=b->y, span=x1-x0;
        float ph=70.0f;  /* pillar height */
        Color stone     =RGB(115,104,92);
        Color stone_dark=RGB(82,74,65);

        /* left pillar */
        DrawRectangle((int)x0,    (int)y,    22,(int)ph,stone);
        DrawRectangle((int)x0,    (int)y,    11,(int)ph,stone_dark);
        DrawRectangle((int)x0-9,  (int)y-12, 40,13,stone);
        /* right pillar */
        DrawRectangle((int)x1-22, (int)y,    22,(int)ph,stone);
        DrawRectangle((int)x1-22, (int)y,    11,(int)ph,stone_dark);
        DrawRectangle((int)x1-31, (int)y-12, 40,13,stone);

        /* wooden planks */
        int np=(int)(span/20)+1;
        float pw=span/np;
        for(int p=0;p<np;p++){
            float px=x0+p*pw;
            Color plk=(p%2==0)?RGB(145,92,42):RGB(118,74,30);
            DrawRectangle((int)px,(int)(y-12),(int)pw-2,14,plk);
            DrawRectangle((int)px,(int)(y-12),2,14,RGB(78,48,18));
        }
        /* deck top surface */
        DrawLineEx((Vector2){x0,(float)(y-12)},(Vector2){x1,(float)(y-12)},3,RGB(98,62,24));

        /* suspension cables — parabolic arcs */
        Color cable=RGB(55,55,55);
        float sag=22.0f;
        float top_y=y-ph;
        int segs=14;
        for(int s=0;s<segs;s++){
            float t0=(float)s/segs, t1=(float)(s+1)/segs;
            float px0=x0+t0*span, px1=x0+t1*span;
            float cy0=top_y + sag*(4*t0*(1-t0));
            float cy1=top_y + sag*(4*t1*(1-t1));
            DrawLineEx((Vector2){px0,cy0},(Vector2){px1,cy1},3,cable);
        }

        /* vertical hangers from cable to deck */
        for(int h=1;h<=5;h++){
            float t=(float)h/6.0f;
            float hx=x0+t*span;
            float cy=top_y + sag*(4*t*(1-t));
            DrawLineEx((Vector2){hx,cy},(Vector2){hx,(float)(y-12)},2,cable);
        }

        /* pillar top caps */
        DrawCircle((int)x0+11, (int)y-12, 5, stone);
        DrawCircle((int)x1-11, (int)y-12, 5, stone);
    }
}

/* coins*/
static void draw_coins(void){
    float t=(float)GetTime();
    for(int i=0;i<g_ncoin;i++){
        if(g_coins[i].got) continue;
        float bob=sinf(t*3+g_coins[i].ph)*6;
        Vector2 p={g_coins[i].pos.x, g_coins[i].pos.y+bob};
        /* shadow */
        DrawEllipse((int)p.x+2,(int)p.y+18,13,5,RGBA(0,0,0,50));
        /* outer glow ring */
        DrawCircleV(p,20,RGBA(255,230,0,45));
        /* coin body – three layers for depth */
        DrawCircleV(p,17,RGB(255,205,0));
        DrawCircleV(p,13,RGB(220,170,0));
        /* star/sparkle top */
        for(int j=0;j<4;j++){
            float a=(t*120+j*90)*(float)(M_PI/180.0);
            float r1=4,r2=9;
            Vector2 inner={p.x+cosf(a)*r1,p.y+sinf(a)*r1};
            Vector2 outer={p.x+cosf(a+(float)(M_PI/4))*r2,p.y+sinf(a+(float)(M_PI/4))*r2};
            DrawLineEx(inner,outer,2,RGBA(255,255,180,180));
        }
        /* "$" centre */
        DrawText("$",(int)p.x-5,(int)p.y-9,15,RGBA(255,248,120,230));
    }
}

/*─ fuel canisters ─*/
static void draw_canisters(void){
    float t=(float)GetTime();
    for(int i=0;i<g_ncan;i++){
        if(g_cans[i].got) continue;
        float bob=sinf(t*2.2f+g_cans[i].ph)*5;
        Vector2 p={g_cans[i].pos.x, g_cans[i].pos.y+bob};
        /* glow pulse */
        float glow=0.5f+0.5f*sinf(t*3+g_cans[i].ph);
        DrawCircleV(p,26+glow*6,RGBA(80,230,80,(unsigned char)(40+glow*40)));
        /* shadow */
        DrawEllipse((int)p.x+2,(int)p.y+24,14,5,RGBA(0,0,0,50));
        /* canister body */
        DrawRectangle((int)p.x-12,(int)p.y-20,24,38,RGB(40,180,40));
        DrawRectangle((int)p.x-12,(int)p.y-20,11,38,RGB(55,205,55));
        /* label band */
        DrawRectangle((int)p.x-12,(int)p.y-4,24,12,RGBA(255,255,255,200));
        DrawText("GAS",(int)p.x-10,(int)p.y-3,10,RGB(20,120,20));
        /* cap */
        DrawRectangle((int)p.x-6,(int)p.y-26,12,8,RGB(30,130,30));
        DrawRectangle((int)p.x-3,(int)p.y-30,6,6,RGB(25,110,25));
        /* outline */
        DrawRectangleLines((int)p.x-12,(int)p.y-20,24,38,RGBA(0,0,0,120));
    }
}

/*═
  HUD
═*/
static void hud_panel(Rectangle r, Color bg){
    DrawRectangleRounded(r,0.38f,8,bg);
    DrawRectangleRoundedLines(r,0.38f,8,RGBA(255,255,255,30));
}

/* Analogue speedometer dial */
static void draw_speedo(float spd_kmh){
    int cx=SW-80, cy=SH-80, R=62;
    float max_spd=160.0f;
    /* background disc */
    DrawCircle(cx,cy,R+4,RGBA(0,0,0,180));
    DrawCircle(cx,cy,R,  RGBA(20,20,35,230));
    /* tick marks */
    for(int i=0;i<=16;i++){
        float a=((float)i/16.0f)*240.0f - 210.0f;  /* -210..+30 deg */
        float ar=a*(float)(M_PI/180.0);
        float cr2=cosf(ar), sr2=sinf(ar);
        int len=(i%4==0)?10:5;
        Color tc=(i%4==0)?WHITE:RGBA(160,160,160,200);
        DrawLine(cx+(int)(cr2*(R-len-2)),cy+(int)(sr2*(R-len-2)),
                 cx+(int)(cr2*(R-2)),    cy+(int)(sr2*(R-2)), tc);
        if(i%4==0 && i<=16){
            int spd_label=(int)((float)i/16.0f*max_spd);
            char lb[8]; snprintf(lb,sizeof(lb),"%d",spd_label);
            DrawText(lb, cx+(int)(cr2*(R-18))-MeasureText(lb,10)/2,
                        cy+(int)(sr2*(R-18))-5, 10, RGBA(200,200,200,200));
        }
    }
    /* speed zones: green/yellow/red arc */
    /* needle */
    float needle_ang = -210.0f + Clamp(spd_kmh/max_spd,0,1)*240.0f;
    float nr=needle_ang*(float)(M_PI/180.0);
    DrawLineEx((Vector2){(float)cx,(float)cy},
               (Vector2){cx+cosf(nr)*(R-8), cy+sinf(nr)*(R-8)},
               3, RGB(255,60,60));
    DrawCircle(cx,cy,6,RGB(200,200,200));
    DrawCircle(cx,cy,3,RGB(100,100,100));
    /* km/h label */
    char sp[16]; snprintf(sp,sizeof(sp),"%.0f",spd_kmh);
    DrawText(sp, cx-MeasureText(sp,18)/2, cy+18, 18, WHITE);
    DrawText("km/h", cx-MeasureText("km/h",10)/2, cy+38, 10, RGBA(180,180,180,200));
}

static void draw_hud(void){
    Car *c=&g_car;
    Level *lv=&LV[g_lv];
    Color bg=RGBA(0,0,0,165);

    /*── top-left: stage + distance ─*/
    hud_panel((Rectangle){10,10,305,110},bg);
    DrawText(lv->name,22,16,18,RGBA(255,220,60,235));
    DrawText("DISTANCE",22,40,12,RGBA(150,150,150,200));
    char db[64]; snprintf(db,sizeof(db),"%.0f m / %.0f m",c->dist,(float)lv->goal_m);
    DrawText(db,22,54,20,WHITE);
    float prog=Clamp(c->dist/lv->goal_m,0,1);
    DrawRectangle(22,84,268,10,RGBA(45,45,45,200));
    DrawRectangle(22,84,(int)(268*prog),10,RGB(70,210,90));
    DrawRectangleLines(22,84,268,10,RGBA(255,255,255,50));
    /* flag */
    DrawText("🏁",284,78,18,WHITE);

    /*── top-right: fuel bar ──*/
    hud_panel((Rectangle){SW-175,10,162,110},bg);
    DrawText("FUEL",SW-163,16,13,RGBA(150,150,150,200));
    float fr=c->fuel/FUEL_MAX;
    Color fc=fr>0.3f?RGB(70,215,70):RGB(220,50,50);
    /* vertical bar */
    DrawRectangle(SW-163,34,140,64,RGBA(38,38,38,200));
    DrawRectangle(SW-163,34+(int)(64*(1-fr)),140,(int)(64*fr),fc);
    DrawRectangleLines(SW-163,34,140,64,RGBA(255,255,255,50));
    char fb[16]; snprintf(fb,sizeof(fb),"%d%%",(int)(fr*100));
    DrawText(fb,SW-118,102,15,fc);
    if(fr<0.25f && (int)(GetTime()*2)%2==0)
        DrawText("!! LOW !!",SW-158,104,13,RGB(255,60,60));

    /* top-centre: coins counter ─*/
    hud_panel((Rectangle){SW/2-80,10,160,50},bg);
    /* mini coin */
    DrawCircle(SW/2-52,35,15,RGB(255,210,0));
    DrawCircle(SW/2-52,35,10,RGB(215,165,0));
    DrawText("$",SW/2-57,26,14,RGBA(255,248,120,230));
    char cb[16]; snprintf(cb,sizeof(cb),"x %d",c->coins);
    DrawText(cb,SW/2-30,22,26,RGB(255,218,55));

    /*─ flips ─*/
    if(c->flips>0){
        hud_panel((Rectangle){SW/2-90,68,180,34},RGBA(200,115,0,175));
        char fl[32]; snprintf(fl,sizeof(fl),"%d FLIP%s!",c->flips,c->flips!=1?"S":"");
        DrawText(fl,SW/2-MeasureText(fl,19)/2,76,19,WHITE);
    }

    /*── analogue speedometer (bottom-right) ─*/
    draw_speedo(Vector2Length(c->vel)*0.036f);

    /*── fuel canister count (next to fuel bar) */
    int cans_got=0;
    for(int i=0;i<g_ncan;i++) if(g_cans[i].got) cans_got++;
    if(cans_got>0){
        hud_panel((Rectangle){SW-175,128,162,36},RGBA(20,100,20,175));
        char canbuf[32]; snprintf(canbuf,sizeof(canbuf),"Fuel x%d",cans_got);
        DrawText(canbuf,SW-162,136,18,RGB(80,230,80));
    }

    /* controls hint */
    DrawText("RIGHT/D=Gas   LEFT/A=Brake",12,SH-22,13,RGBA(170,170,170,130));
}

// MENU

static void draw_menu(void){
    g_menu_t += GetFrameTime();

    /* animated sky */
    for(int y=0;y<SH;y++){
        float t=(float)y/SH;
        DrawLine(0,y,SW,y,ColorLerp(RGB(8,15,55),RGB(40,80,160),t));
    }

    /* animated stars */
    for(int i=0;i<80;i++){
        float sx2=(float)((i*137+11)%SW);
        float sy2=(float)((i*251+17)%(SH/2));
        float twinkle=0.5f+0.5f*sinf(g_menu_t*2+i*0.7f);
        unsigned char br=(unsigned char)(180+70*twinkle);
        DrawCircle((int)sx2,(int)sy2,i%3==0?2:1,RGBA(br,br,br,255));
    }

    /* rolling hills silhouette */
    for(int x=0;x<SW;x+=2){
        float h=SH*0.62f + sinf(x*0.008f+g_menu_t*0.4f)*60
                         + sinf(x*0.003f-g_menu_t*0.2f)*90;
        DrawLine(x,(int)h,x,SH,RGB(20,60,20));
    }

    /* title glow */
    const char *title="HILL CLIMB RACER";
    int tw=MeasureText(title,72);
    float glow=0.7f+0.3f*sinf(g_menu_t*2);
    DrawText(title,SW/2-tw/2+3,72+3,72,RGBA(0,0,0,130));
    DrawText(title,SW/2-tw/2,  72,  72,RGBA(255,(unsigned char)(200+55*glow),30,255));

    const char *sub="THE ULTIMATE HILL CLIMBING CHALLENGE";
    int sw2=MeasureText(sub,18);
    DrawText(sub,SW/2-sw2/2,152,18,RGBA(200,200,255,200));

    /* START button */
    Rectangle start_btn={(float)(SW/2-140),(float)(205),280,58};
    bool sh=CheckCollisionPointRec(GetMousePosition(),start_btn);
    Color sc=sh?RGB(60,220,60):RGB(30,150,30);
    DrawRectangleRounded(start_btn,0.5f,10,sc);
    DrawRectangleRoundedLines(start_btn,0.5f,10,RGBA(255,255,255,80));
    const char *stxt="▶  START GAME";
    DrawText(stxt,SW/2-MeasureText(stxt,26)/2,218,26,WHITE);
    if(sh && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
        g_lv=0; car_init();
    }

    /* stage selector */
    DrawText("Select Stage:",SW/2-MeasureText("Select Stage:",20)/2,275,20,RGBA(200,200,200,210));
    for(int i=0;i<MAX_LEVELS;i++){
        Rectangle btn={(float)(SW/2-310+i*128),(float)(305),120,52};
        bool hov=CheckCollisionPointRec(GetMousePosition(),btn);
        bool sel=(g_lv==i);
        Color bc= sel?RGB(255,195,30): hov?RGB(90,120,210):RGB(45,60,145);
        DrawRectangleRounded(btn,0.4f,8,bc);
        DrawRectangleRoundedLines(btn,0.4f,8,RGBA(255,255,255,50));
        char nm[32]; snprintf(nm,sizeof(nm),"%d.%s",i+1,LV[i].name);
        int nw=MeasureText(nm,13);
        DrawText(nm,btn.x+(120-nw)/2,btn.y+8,13, sel?BLACK:WHITE);
        char gm[20]; snprintf(gm,sizeof(gm),"%.0fm",LV[i].goal_m);
        int gw=MeasureText(gm,12);
        DrawText(gm,btn.x+(120-gw)/2,btn.y+28,12,sel?RGBA(0,0,0,180):RGBA(200,200,200,180));
        if(hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) g_lv=i;
    }

    /* instructions */
    const char *inst="RIGHT / D = Gas       LEFT / A = Brake & Rotate       Keys 1-5 = Quick Start";
    DrawText(inst,SW/2-MeasureText(inst,14)/2,370,14,RGBA(170,170,200,190));

    /* quick-start hint */
    DrawText("Press 1-5 to jump straight into a stage",
             SW/2-MeasureText("Press 1-5 to jump straight into a stage",14)/2,
             395,14,RGBA(140,140,180,160));

    /* mini car preview */
    {
        float t=g_menu_t;
        float cx2=SW/2+0.0f, cy2=470;
        float bounce=sinf(t*3)*5;
        /* wheels */
        DrawCircle((int)cx2-60,(int)(cy2+22+bounce),22,RGB(22,22,22));
        DrawCircle((int)cx2-60,(int)(cy2+22+bounce),13,RGB(180,180,180));
        DrawCircle((int)cx2+60,(int)(cy2+22+bounce),20,RGB(22,22,22));
        DrawCircle((int)cx2+60,(int)(cy2+22+bounce),12,RGB(180,180,180));
        /* body */
        DrawRectangle((int)cx2-90,(int)(cy2-10+bounce),180,35,RGB(200,40,40));
        DrawRectangle((int)cx2-90,(int)(cy2+2+bounce),180,12,RGB(230,230,230));
        /* cabin */
        DrawRectangle((int)cx2-55,(int)(cy2-38+bounce),100,32,RGB(155,28,28));
        DrawRectangle((int)cx2-10,(int)(cy2-38+bounce),55,32,RGBA(130,205,255,200));
        /* headlight */
        DrawCircle((int)cx2+88,(int)(cy2+2+bounce),8,RGB(255,255,180));
        /* exhaust */
        DrawLine((int)cx2-90,(int)(cy2+8+bounce),(int)cx2-108,(int)(cy2+8+bounce), RGB(70,70,70));
        /* wheels spin */
        for(int j=0;j<5;j++){
            float a=(t*200+j*72)*(float)(M_PI/180.0);
            DrawLine((int)cx2-60,(int)(cy2+22+bounce),
                     (int)(cx2-60+cosf(a)*13),(int)(cy2+22+bounce+sinf(a)*13), RGB(160,160,160));
            DrawLine((int)cx2+60,(int)(cy2+22+bounce),
                     (int)(cx2+60+cosf(a)*11),(int)(cy2+22+bounce+sinf(a)*11), RGB(160,160,160));
        }
    }

    /* version */
    DrawText("v4.0  |  Hill Climb Racer",10,SH-20,13,RGBA(100,100,100,150));
}

//END SCREENS

static void draw_overlay(const char *title, Color tc){
    DrawRectangle(0,0,SW,SH,RGBA(0,0,0,155));
    int tw=MeasureText(title,70);
    DrawText(title,SW/2-tw/2+3,145+3,70,RGBA(0,0,0,120));
    DrawText(title,SW/2-tw/2,  145,  70,tc);
    Car *c=&g_car;
    char d[64],co[64],fl[64],fu[64];
    snprintf(d, sizeof(d),"Distance:  %.0f m",c->dist);
    snprintf(co,sizeof(co),"Coins:     %d",   c->coins);
    snprintf(fl,sizeof(fl),"Flips:     %d",   c->flips);
    int cans_got=0; for(int i=0;i<g_ncan;i++) if(g_cans[i].got) cans_got++;
    snprintf(fu,sizeof(fu),"Fuel cans: %d",   cans_got);
    int sx=SW/2-160,sy=260;
    DrawText(d, sx,sy,     32,WHITE);
    DrawText(co,sx,sy+46,  32,RGB(255,215,0));
    DrawText(fl,sx,sy+92,  32,RGB(255,165,45));
    DrawText(fu,sx,sy+138, 32,RGB(70,215,70));
    DrawText("R=Retry   M=Menu",SW/2-MeasureText("R=Retry   M=Menu",24)/2,sy+195,24,RGBA(195,195,195,215));
    if(g_st==ST_WIN && g_lv<MAX_LEVELS-1)
        DrawText("N=Next Stage",SW/2-MeasureText("N=Next Stage",24)/2,sy+230,24,RGB(90,245,110));
}

//AIN
int main(void){
    InitWindow(SW,SH,"Hill Climb Racer");
    SetTargetFPS(60);

    g_cam=(Camera2D){
        .offset={SW*0.38f, SH*0.55f},
        .zoom=1.05f,
    };

    while(!WindowShouldClose()){
        float dt=GetFrameTime();
        if(dt>0.05f) dt=0.05f;

        //input
        if(g_st==ST_MENU){
            for(int i=0;i<MAX_LEVELS;i++)
                if(IsKeyPressed(KEY_ONE+i)){ g_lv=i; car_init(); }
        }
        if(g_st==ST_DEAD||g_st==ST_WIN){
            if(IsKeyPressed(KEY_M)) g_st=ST_MENU;
            if(IsKeyPressed(KEY_R)) car_init();
            if(g_st==ST_WIN && IsKeyPressed(KEY_N) && g_lv<MAX_LEVELS-1){
                g_lv++; car_init();
            }
        }

        //update 
        if(g_st==ST_PLAY||(g_st==ST_DEAD&&g_car.dead)) update(dt);

        //camera 
        if(g_st==ST_PLAY||g_st==ST_DEAD){
            g_cam.target=Vector2Lerp(g_cam.target,g_car.pos,7.0f*dt);
            float tz=1.05f - Vector2Length(g_car.vel)/2500.0f;
            tz=Clamp(tz,0.75f,1.18f);
            g_cam.zoom+=(tz-g_cam.zoom)*5.0f*dt;
        }

        //draw
        BeginDrawing();
        ClearBackground(BLACK);

        if(g_st==ST_MENU){
            draw_menu();
        } else {
            draw_sky();
            BeginMode2D(g_cam);
                draw_terrain();
                draw_deco();
                draw_bridges();
                draw_canisters();
                draw_coins();
                draw_wheel(&g_car.rear);
                draw_wheel(&g_car.fwd);
                draw_car();
            EndMode2D();
            draw_hud();
            if(g_st==ST_DEAD) draw_overlay("CRASHED!",      RGB(218,45,45));
            if(g_st==ST_WIN)  draw_overlay("STAGE COMPLETE!",RGB(70,238,100));
        }

        EndDrawing();
    }
    CloseWindow();
    return 0;
}