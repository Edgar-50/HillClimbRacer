/*
 *  HILL CLIMB RACER  v5.0
 *  ----------------------------------------------------------------------------
 *  A physics-driven 2D hill climbing game written in C with raylib.
 *
 *  What's inside (all from scratch, single file):
 *    - Smooth Catmull-Rom procedural terrain with a deterministic per-stage RNG
 *      and a difficulty curve that ramps up with distance
 *    - 6 stages (incl. ENDLESS) with per-stage gravity, weather and parallax
 *    - 3 vehicles + 4 upgrade tracks, persistent coin bank and save file
 *    - AI RIVAL that races you: terrain look-ahead throttle control, ballistic
 *      landing prediction, PD-controlled air rotation and planned flips
 *    - AUTOPILOT: hand your car to the same AI (TAB)
 *    - AI COACH: fuel-range estimation, terrain warnings, landing guidance
 *    - GHOST replays of your best run per stage
 *    - Nitro boost, stunt system (flips, air time, perfect landings, combos)
 *    - Particles, camera shake, procedural synthesized audio, achievements
 *  ----------------------------------------------------------------------------
 */
#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

#include <raylib.h>
#include <raymath.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

//window
#define SW 1400
#define SH  800
#define D2R ((float)(M_PI/180.0))
#define R2D ((float)(180.0/M_PI))

// terrain
#define SEG     100          /* pixels per segment (1 segment == 1 metre)     */
#define TBUF    900          /* circular-buffer size                          */
static Vector2 TB[TBUF];
static int   T_n   = 0;      /* total points ever generated                   */
static int   T_b   = 0;      /* index of oldest kept point                    */
static float TgX   = 0;
static float TgY   = 0;
static float T_slope = 0;    /* slope momentum -> long, natural hills          */
static unsigned T_rng = 1;   /* private RNG: terrain never depends on effects  */

//physics
/* Everything is in pixels / second.
   The car drives RIGHT (+x).  Screen-y is DOWN so "up" is -y.               */
#define GRAVITY      1200.0f   /* px/s² downward (+y)                        */
#define BRAKE_F       600.0f
#define WHEEL_FRIC    0.88f    /* x-velocity multiplier per 60 Hz tick        */
#define WHEEL_BNC     0.06f    /* y restitution on ground hit                 */
#define AIR_DAMP        1.0f   /* angular damping in the air                  */
#define GND_DAMP        5.0f   /* angular damping on the ground               */
#define FIXED_DT  (1.0f/120.0f)/* deterministic physics step                  */

// game
#define FUEL_GAS       14.0f   /* drain/s while throttle                      */
#define FUEL_IDLE       0.80f  /* passive drain/s                             */
#define NITRO_MAX     100.0f
#define NITRO_ACC    1100.0f
#define NITRO_DRAIN    30.0f
#define MAX_COINS       160
#define MAX_CANISTERS    24
#define MAX_BRIDGES      12
#define MAX_LEVELS        6
#define ENDLESS_LV        5
#define FLIP_DEAD_DEG   145.0f /* body angle past this = crash                */
#ifdef __EMSCRIPTEN__
/* browser build: saves live in IndexedDB, mounted at /save                 */
#  include <emscripten.h>
#  define DATA_DIR "/save/"
static void persist(void){ emscripten_run_script("FS.syncfs(false,function(e){});"); }
static void storage_init(void){
    emscripten_run_script(
        "FS.mkdir('/save'); FS.mount(IDBFS,{},'/save'); Module.hcrReady=0;"
        "FS.syncfs(true,function(e){ Module.hcrReady=1; });");
    for(int i=0;i<200 && !emscripten_run_script_int("Module.hcrReady|0");i++) emscripten_sleep(10);
}
#else
#  define DATA_DIR ""
static void persist(void){}
static void storage_init(void){}
#endif
#define SAVE_FILE DATA_DIR "hcr_save.txt"

//colour helpers
#define RGBA(r,g,b,a) ((Color){(r),(g),(b),(a)})
#define RGB(r,g,b)    RGBA(r,g,b,255)

/*============================================================================
  DATA TYPES
============================================================================*/
enum { WX_NONE, WX_LEAVES, WX_SAND, WX_SNOW, WX_EMBERS, WX_DUST };

typedef struct {
    const char *name;
    Color sky0, sky1;          /* gradient top→bot                           */
    Color gnd, dirt;
    float rough;               /* max dy per terrain seg                     */
    float goal_m;              /* distance to win (0 = endless)              */
    unsigned seed;
    Color deco;                /* decoration colour                          */
    float grav;                /* gravity multiplier                         */
    int   wx;                  /* weather type                               */
} Level;

static Level LV[MAX_LEVELS] = {
    /* name          sky0               sky1              gnd               dirt             rough goal  seed  deco             grav  weather */
    {"COUNTRYSIDE", RGB(80,160,230),  RGB(180,220,255), RGB(60,140,50),  RGB(110,75,35),   28, 300, 1111, RGB(30,100,30),  1.00f, WX_LEAVES},
    {"DESERT",      RGB(255,185,60),  RGB(255,225,140), RGB(200,155,70), RGB(160,105,40),  50, 500, 2222, RGB(180,120,30), 1.00f, WX_SAND  },
    {"ARCTIC",      RGB(160,205,255), RGB(225,242,255), RGB(200,228,255),RGB(150,185,215), 26, 650, 3333, RGB(200,235,255),1.00f, WX_SNOW  },
    {"VOLCANO",     RGB(30,6,6),      RGB(70,18,6),     RGB(80,38,18),   RGB(55,18,4),     70, 800, 4444, RGB(200,60,10),  1.00f, WX_EMBERS},
    {"MOON",        RGB(4,4,18),      RGB(12,12,40),    RGB(105,105,105),RGB(70,70,70),    55,1000, 5555, RGB(180,180,200),0.55f, WX_NONE  },
    {"ENDLESS",     RGB(40,20,80),    RGB(250,140,100), RGB(70,80,130),  RGB(40,40,75),    30,   0, 7777, RGB(160,200,255),1.00f, WX_DUST  },
};

/* ---------------- vehicles ---------------- */
typedef struct {
    const char *name, *desc;
    Color body, cabin, stripe;
    float hw, hh;              /* chassis half extents                       */
    float rr, fr, ax;          /* rear/front wheel radius, axle offset       */
    float drive, max_spd;
    float susp_k, susp_d, susp_len;
    float fuel_cap, air_rot, fuel_use;
    int   price;
} Vehicle;

#define NUM_VEH 3
static const Vehicle VEH[NUM_VEH] = {
  {"JEEP",   "Balanced all-rounder",            RGB(205,38,38), RGB(160,28,28), RGB(245,245,245),
             100,36, 34,32,68,  900,650, 55,4.5f,80, 100,380,1.00f,    0},
  {"BUGGY",  "Light & agile, small fuel tank",  RGB(250,200,30),RGB(205,150,20),RGB(30,30,30),
              86,28, 30,28,60, 1000,760, 48,4.0f,70,  80,450,0.85f,  400},
  {"MONSTER","Huge wheels, big tank, thirsty",  RGB(60,160,70), RGB(40,120,50), RGB(250,250,250),
             112,40, 46,46,80,  980,600, 62,6.0f,92, 135,320,1.15f, 1000},
};

/* ---------------- upgrades ---------------- */
enum { UP_ENGINE, UP_SUSP, UP_FUEL, UP_NITRO, NUM_UP };
#define UP_MAX 5
static const char *UP_NAME[NUM_UP] = {"ENGINE","SUSPENSION","FUEL TANK","NITRO"};
static const char *UP_DESC[NUM_UP] = {
    "+8% power, +4% top speed per level",
    "+12% damping per level: steadier landings",
    "+15% capacity, +5% efficiency per level",
    "+20% nitro capacity & stunt refill per level"};
static int up_cost(int lvl){ return 150<<lvl; }

/* Final, upgrade-adjusted stats of a car */
typedef struct {
    float drive, max_spd, susp_k, susp_d, susp_len;
    float fuel_cap, air_rot, fuel_use, nitro_cap, nitro_gain;
} Spec;

static Spec make_spec(int veh, const int *up){
    const Vehicle *v=&VEH[veh];
    Spec s;
    s.drive     = v->drive   *(1.0f+0.08f*up[UP_ENGINE]);
    s.max_spd   = v->max_spd *(1.0f+0.04f*up[UP_ENGINE]);
    s.susp_k    = v->susp_k;
    s.susp_d    = v->susp_d  *(1.0f+0.12f*up[UP_SUSP]);
    s.susp_len  = v->susp_len;
    s.fuel_cap  = v->fuel_cap*(1.0f+0.15f*up[UP_FUEL]);
    s.fuel_use  = v->fuel_use*(1.0f-0.05f*up[UP_FUEL]);
    s.air_rot   = v->air_rot;
    s.nitro_cap = NITRO_MAX  *(1.0f+0.20f*up[UP_NITRO]);
    s.nitro_gain= 1.0f+0.20f*up[UP_NITRO];
    return s;
}

/* ---------------- car ---------------- */
typedef struct { bool gas, brake, nitro; } Ctl;

typedef struct {
    Vector2 pos, vel;
    float   r;          /* radius                                             */
    float   ax;         /* attachment local-x offset from car centre          */
    bool    on;         /* on ground this frame                               */
    float   spin;       /* visual spin angle degrees                          */
} Wheel;

enum { CAUSE_NONE, CAUSE_FLIP, CAUSE_HEAD, CAUSE_FUEL };

typedef struct {
    Vector2 pos;        /* chassis centre                                     */
    Vector2 vel;
    float   angle;      /* body tilt degrees (unwrapped); 0 = flat            */
    float   avel;       /* angular velocity deg/s                             */
    float   hw, hh;     /* half width, half height                            */
    Wheel   rear, fwd;  /* rear=left(−x)  fwd=right(+x)                      */
    int     veh;
    Spec    sp;
    Ctl     in;         /* last input                                         */
    float   fuel, nitro;
    bool    boosting, infinite_fuel;
    float   dist;       /* metres                                             */
    float   time;
    int     coins, flips, cans, perfects;
    bool    prev_gnd;
    float   air_t, takeoff_ang;
    bool    landed_evt; /* set on the physics step a landing happened        */
    float   land_air, land_rot, land_vy, impact;
    float   still_t;
    bool    dead;
    float   dead_t;
    int     dead_cause;
} Car;

typedef struct { Vector2 pos; bool active; float ph; int value; } Coin;
typedef struct { Vector2 pos; bool active; float ph; } Canister;

/* Bridge: a flat plank spanning between two terrain points.                 */
typedef struct {
    float x0, x1;   /* world X of left/right edges                           */
    float y;        /* world Y of bridge deck surface                        */
    bool  active;
} Bridge;

/* ---------------- AI ---------------- */
typedef struct {
    float skill;        /* fraction of top speed it cruises at               */
    float react;        /* seconds between decisions (human-like lag)        */
    bool  flips, use_nitro;
    float think_t;
    Ctl   held;
    bool  airborne;
    float ref_ang, flip_off;
    float stuck_t, reverse_t;
    unsigned rng;
} AIBrain;

/* ---------------- persistence ---------------- */
enum { ACH_FIRST_WIN, ACH_FLIP, ACH_DOUBLE, ACH_AIR, ACH_HARD, ACH_MARATHON,
       ACH_TYCOON, ACH_TOUR, ACH_SMOOTH, NUM_ACH };
static const char *ACH_NAME[NUM_ACH] = {
    "First Victory","Flip Master","Double Trouble","Frequent Flyer","Giant Slayer",
    "Marathon","Tycoon","World Tour","Smooth Operator"};
static const char *ACH_DESC[NUM_ACH] = {
    "Complete any stage","Land a flip","Land a double flip in one jump",
    "Stay airborne for 3 seconds","Beat the HARD AI rival","Drive 1000 m in Endless",
    "Earn 5000 coins in total","Complete all 5 stages","10 perfect landings in one run"};

typedef struct {
    int   bank, veh;
    int   owned[NUM_VEH];
    int   up[NUM_UP];
    float best_dist[MAX_LEVELS], best_time[MAX_LEVELS];
    int   completed[MAX_LEVELS];
    int   rival;           /* 0 off, 1 easy, 2 normal, 3 hard                */
    int   ghost, hints, sound;
    int   ach[NUM_ACH];
    int   runs, total_flips, total_coins;
    float total_dist;
} Save;

/* ---------------- ghost ---------------- */
#define GHOST_HZ   20
#define GHOST_MAX  12000
#define GHOST_MAGIC 0x48435247
typedef struct { float x, y, a; } GSample;

/* ---------------- effects ---------------- */
#define MAX_PART 900
typedef struct {
    Vector2 p, v;
    float life, max, size, grav, rot;
    Color col;
    int kind;            /* 0 puff (grows), 1 spark (shrinks), 2 debris   */
} Particle;

#define MAX_POP 8
typedef struct { char txt[64]; Color col; float t; int size, serial; } Popup;

typedef struct {
    bool win;
    int  cause, coins, dist_bonus, stage_bonus, rival_bonus, total;
    int  place;          /* 1 = beat rival, 2 = lost to rival, 0 = n/a    */
    bool new_dist, new_time, autopilot;
} RunResult;

typedef enum { ST_MENU, ST_GARAGE, ST_PLAY, ST_PAUSE, ST_DEAD, ST_WIN } GState;

/*============================================================================
  GLOBALS
============================================================================*/
static GState    g_st    = ST_MENU;
static int       g_lv    = 0;
static Car       g_car, g_rival;
static AIBrain   g_rbrain, g_abrain;
static bool      g_rival_on, g_rival_done;
static float     g_rival_time;
static int       g_lead;
static bool      g_autopilot, g_autopilot_used;
static Camera2D  g_cam;
static float     g_shake;
static Coin      g_coins[MAX_COINS];
static Canister  g_cans[MAX_CANISTERS];
static Bridge    g_bridges[MAX_BRIDGES];
static int       g_spawn_i, g_next_can;
static float     g_time, g_menu_t, g_acc;
static int       g_combo;
static float     g_last_stunt;
static bool      g_run_done;
static RunResult g_res;
static Save      g_save;
static int       g_view_veh;
static bool      g_quit;
static float     g_alpha = 1.0f;     /* global draw alpha for ghost/rival    */

static GSample   g_rec[GHOST_MAX];   static int g_nrec;
static GSample   g_ghost[GHOST_MAX]; static int g_nghost, g_ghost_veh;
static float     g_ghost_score = -1;

static Particle  g_part[MAX_PART];
static Popup     g_pop[MAX_POP];     static int g_pop_serial;
static char      g_toast[96];        static float g_toast_t;

/* AI coach */
static struct {
    float rate, last_fuel, last_dist, t;
    char  msg[112];
    Color col;
} g_coach;

/* landing predictor for the player (drawn as a dotted arc) */
#define PRED_MAX 128
static Vector2 g_pred[PRED_MAX]; static int g_npred;
static Vector2 g_pred_land; static float g_pred_t;

/*============================================================================
  SMALL UTILITIES
============================================================================*/
static float rnd_f(float lo, float hi){
    return lo + (hi-lo)*((float)GetRandomValue(0,32767)/32767.0f);
}
static float trnd(void){                       /* terrain RNG: 0..1        */
    T_rng = T_rng*1664525u + 1013904223u;
    return (float)(T_rng>>8)/16777216.0f;
}
static unsigned hash_u(unsigned x){
    x^=x>>16; x*=0x7feb352dU; x^=x>>15; x*=0x846ca68bU; x^=x>>16; return x;
}
static float hash_f(int i, unsigned salt){
    return (float)(hash_u((unsigned)i*2654435761U ^ salt)&0xFFFFFF)/(float)0xFFFFFF;
}
static float wrap180(float a){
    a=fmodf(a+180.0f,360.0f); if(a<0) a+=360.0f; return a-180.0f;
}
static float wrapf(float v, float m){ v=fmodf(v,m); return v<0? v+m : v; }
static Color AC(Color c){ c.a=(unsigned char)(c.a*g_alpha); return c; }

/*============================================================================
  AUDIO  (everything synthesized at start-up, no asset files)
============================================================================*/
enum { SFX_COIN, SFX_FUEL, SFX_FLIP, SFX_CRASH, SFX_LAND, SFX_NITRO,
       SFX_CLICK, SFX_ACH, SFX_WIN, NUM_SFX };
static Sound       g_sfx[NUM_SFX];
static bool        g_audio_ok;
static AudioStream g_eng;
static volatile float g_eng_rpm, g_eng_vol;

static Sound gen_sfx(int k){
    const int sr=22050;
    static const float dur[NUM_SFX]={0.16f,0.35f,0.36f,0.9f,0.22f,0.55f,0.05f,0.7f,1.1f};
    int n=(int)(sr*dur[k]);
    short *d=(short*)malloc(sizeof(short)*n);
    unsigned seed=1234u+k; float lp=0, ph=0;
    for(int i=0;i<n;i++){
        float t=(float)i/sr, u=t/dur[k], s=0;
        seed=seed*1664525u+1013904223u;
        float noise=(float)(seed>>9)/4194304.0f-1.0f;
        switch(k){
        case SFX_COIN: { float f=t<0.06f?1046.5f:1568.0f;
            s=(fmodf(t*f,1.0f)<0.5f?0.35f:-0.35f)*(1.0f-u); } break;
        case SFX_FUEL: ph+=(300.0f+700.0f*u)/sr; s=sinf(2*PI*ph)*(1.0f-u)*0.7f; break;
        case SFX_FLIP: { static const float nt[4]={523.25f,659.25f,783.99f,1046.5f};
            int idx=(int)(u*4); if(idx>3) idx=3;
            s=(fmodf(t*nt[idx],1.0f)<0.5f?0.3f:-0.3f)*(1.0f-u*0.6f); } break;
        case SFX_CRASH: lp+=(noise-lp)*0.08f;
            s=lp*2.6f*expf(-4.0f*t)+sinf(2*PI*55*t)*expf(-6.0f*t)*0.5f; break;
        case SFX_LAND: ph+=(90.0f-50.0f*u)/sr;
            s=sinf(2*PI*ph)*expf(-14.0f*t)*0.9f+noise*0.2f*expf(-30.0f*t); break;
        case SFX_NITRO: lp+=(noise-lp)*(0.04f+0.25f*u);
            s=lp*1.8f*sinf(PI*u); break;
        case SFX_CLICK: s=sinf(2*PI*900*t)*(1.0f-u)*0.4f; break;
        case SFX_ACH:
            s=(sinf(2*PI*523.25f*t)+(t>0.08f?sinf(2*PI*659.25f*t):0)
              +(t>0.16f?sinf(2*PI*783.99f*t):0))*0.28f*(1.0f-u); break;
        case SFX_WIN: { static const float nt[4]={523.25f,659.25f,783.99f,1046.5f};
            int idx=(int)(t/0.16f); if(idx>3) idx=3;
            float f=nt[idx];
            s=(sinf(2*PI*f*t)*0.6f+(fmodf(t*f,1.0f)<0.5f?0.15f:-0.15f))*(1.0f-u*0.8f); } break;
        }
        d[i]=(short)(Clamp(s,-1.0f,1.0f)*26000.0f);
    }
    Wave w={(unsigned)n,(unsigned)sr,16,1,d};
    Sound snd=LoadSoundFromWave(w);
    UnloadWave(w);
    return snd;
}

/* Engine: two detuned oscillators + noise through a one-pole low-pass,
   pitch follows engine RPM. Runs on the audio thread.                       */
static void engine_cb(void *buf, unsigned int frames){
    static float ph=0, ph2=0, rpm=0, vol=0, lp=0;
    static unsigned ns=99;
    short *d=(short*)buf;
    for(unsigned i=0;i<frames;i++){
        rpm+=(g_eng_rpm-rpm)*0.0006f;
        vol+=(g_eng_vol-vol)*0.0008f;
        float f=42.0f+rpm*110.0f;
        ph +=f/44100.0f;      if(ph >=1.0f) ph -=1.0f;
        ph2+=f*0.5f/44100.0f; if(ph2>=1.0f) ph2-=1.0f;
        ns=ns*1664525u+1013904223u;
        float nz=(float)(ns>>9)/4194304.0f-1.0f;
        float s=(2.0f*ph-1.0f)*0.5f+(ph2<0.5f?0.35f:-0.35f)+nz*0.08f;
        lp+=(s-lp)*(0.10f+rpm*0.12f);
        d[i]=(short)(Clamp(lp*vol,-1.0f,1.0f)*20000.0f);
    }
}

static void audio_init(void){
    InitAudioDevice();
    g_audio_ok=IsAudioDeviceReady();
    if(!g_audio_ok) return;
    for(int i=0;i<NUM_SFX;i++) g_sfx[i]=gen_sfx(i);
    g_eng=LoadAudioStream(44100,16,1);
    SetAudioStreamCallback(g_eng,engine_cb);
    PlayAudioStream(g_eng);
}

static void sfx(int k){
    if(g_audio_ok && g_save.sound) PlaySound(g_sfx[k]);
}

/*============================================================================
  SAVE / LOAD
============================================================================*/
static void save_defaults(void){
    memset(&g_save,0,sizeof g_save);
    g_save.owned[0]=1;
    g_save.rival=2; g_save.ghost=1; g_save.hints=1; g_save.sound=1;
}

static void save_game(void){
    FILE *f=fopen(SAVE_FILE,"w");
    if(!f) return;
    fprintf(f,"hcr_save 1\nbank %d\nveh %d\n",g_save.bank,g_save.veh);
    fprintf(f,"owned");     for(int i=0;i<NUM_VEH;i++)    fprintf(f," %d",g_save.owned[i]);
    fprintf(f,"\nup");      for(int i=0;i<NUM_UP;i++)     fprintf(f," %d",g_save.up[i]);
    fprintf(f,"\nbest_dist");for(int i=0;i<MAX_LEVELS;i++)fprintf(f," %.2f",g_save.best_dist[i]);
    fprintf(f,"\nbest_time");for(int i=0;i<MAX_LEVELS;i++)fprintf(f," %.2f",g_save.best_time[i]);
    fprintf(f,"\ncompleted");for(int i=0;i<MAX_LEVELS;i++)fprintf(f," %d",g_save.completed[i]);
    fprintf(f,"\nach");     for(int i=0;i<NUM_ACH;i++)    fprintf(f," %d",g_save.ach[i]);
    fprintf(f,"\nrival %d\nghost %d\nhints %d\nsound %d\n",
            g_save.rival,g_save.ghost,g_save.hints,g_save.sound);
    fprintf(f,"runs %d\ntotal_flips %d\ntotal_coins %d\ntotal_dist %.1f\n",
            g_save.runs,g_save.total_flips,g_save.total_coins,g_save.total_dist);
    fclose(f);
    persist();
}

static void read_ints(FILE *f, int *a, int n){
    for(int i=0;i<n;i++) if(fscanf(f,"%d",&a[i])!=1) return;
}
static void read_floats(FILE *f, float *a, int n){
    for(int i=0;i<n;i++) if(fscanf(f,"%f",&a[i])!=1) return;
}

static void load_game(void){
    save_defaults();
    FILE *f=fopen(SAVE_FILE,"r");
    if(!f) return;
    char key[32];
    while(fscanf(f,"%31s",key)==1){
        if     (!strcmp(key,"bank"))       read_ints(f,&g_save.bank,1);
        else if(!strcmp(key,"veh"))        read_ints(f,&g_save.veh,1);
        else if(!strcmp(key,"owned"))      read_ints(f,g_save.owned,NUM_VEH);
        else if(!strcmp(key,"up"))         read_ints(f,g_save.up,NUM_UP);
        else if(!strcmp(key,"best_dist"))  read_floats(f,g_save.best_dist,MAX_LEVELS);
        else if(!strcmp(key,"best_time"))  read_floats(f,g_save.best_time,MAX_LEVELS);
        else if(!strcmp(key,"completed"))  read_ints(f,g_save.completed,MAX_LEVELS);
        else if(!strcmp(key,"ach"))        read_ints(f,g_save.ach,NUM_ACH);
        else if(!strcmp(key,"rival"))      read_ints(f,&g_save.rival,1);
        else if(!strcmp(key,"ghost"))      read_ints(f,&g_save.ghost,1);
        else if(!strcmp(key,"hints"))      read_ints(f,&g_save.hints,1);
        else if(!strcmp(key,"sound"))      read_ints(f,&g_save.sound,1);
        else if(!strcmp(key,"runs"))       read_ints(f,&g_save.runs,1);
        else if(!strcmp(key,"total_flips"))read_ints(f,&g_save.total_flips,1);
        else if(!strcmp(key,"total_coins"))read_ints(f,&g_save.total_coins,1);
        else if(!strcmp(key,"total_dist")) read_floats(f,&g_save.total_dist,1);
        else { int ch; while((ch=fgetc(f))!=EOF && ch!='\n'); }
    }
    fclose(f);
    /* sanitise */
    g_save.owned[0]=1;
    if(g_save.veh<0||g_save.veh>=NUM_VEH||!g_save.owned[g_save.veh]) g_save.veh=0;
    for(int i=0;i<NUM_UP;i++) g_save.up[i]=(int)Clamp((float)g_save.up[i],0,UP_MAX);
    g_save.rival=(int)Clamp((float)g_save.rival,0,3);
    if(g_save.bank<0) g_save.bank=0;
}

static bool stage_unlocked(int i){
    return i==0 || i==ENDLESS_LV || g_save.completed[i-1];
}

static void toast(const char *fmt, ...){
    va_list ap; va_start(ap,fmt);
    vsnprintf(g_toast,sizeof g_toast,fmt,ap);
    va_end(ap);
    g_toast_t=3.2f;
}

static void ach_unlock(int i){
    if(g_save.ach[i]) return;
    g_save.ach[i]=1;
    toast("ACHIEVEMENT UNLOCKED: %s",ACH_NAME[i]);
    sfx(SFX_ACH);
}

/*============================================================================
  GHOST REPLAY
============================================================================*/
static void ghost_path(char *buf, int n, int lv){ snprintf(buf,n,DATA_DIR "hcr_ghost_%d.bin",lv); }

static void ghost_load(void){
    g_nghost=0; g_ghost_score=-1; g_ghost_veh=0;
    char p[64]; ghost_path(p,sizeof p,g_lv);
    FILE *f=fopen(p,"rb");
    if(!f) return;
    int hdr[3]; float score;
    if(fread(hdr,sizeof(int),3,f)==3 && hdr[0]==GHOST_MAGIC &&
       hdr[1]>0 && hdr[1]<=GHOST_MAX && fread(&score,sizeof(float),1,f)==1 &&
       fread(g_ghost,sizeof(GSample),(size_t)hdr[1],f)==(size_t)hdr[1]){
        g_nghost=hdr[1]; g_ghost_score=score;
        g_ghost_veh=(hdr[2]>=0&&hdr[2]<NUM_VEH)?hdr[2]:0;
    }
    fclose(f);
}

static void ghost_save(float score){
    if(g_nrec<2) return;
    char p[64]; ghost_path(p,sizeof p,g_lv);
    FILE *f=fopen(p,"wb");
    if(!f) return;
    int hdr[3]={GHOST_MAGIC,g_nrec,g_car.veh};
    fwrite(hdr,sizeof(int),3,f);
    fwrite(&score,sizeof(float),1,f);
    fwrite(g_rec,sizeof(GSample),(size_t)g_nrec,f);
    fclose(f);
    persist();
}

/* ghost pose at time t (interpolated), returns false when no ghost */
static bool ghost_at(float t, GSample *out){
    if(g_nghost<2) return false;
    float fi=t*GHOST_HZ;
    int i=(int)fi;
    if(i>=g_nghost-1){ *out=g_ghost[g_nghost-1]; return true; }
    float u=fi-i;
    GSample a=g_ghost[i], b=g_ghost[i+1];
    out->x=Lerp(a.x,b.x,u); out->y=Lerp(a.y,b.y,u); out->a=Lerp(a.a,b.a,u);
    return true;
}

/*============================================================================
  TERRAIN
============================================================================*/
static void terrain_add(void){
    Level *lv=&LV[g_lv];
    int i=T_n;
    /* difficulty ramps with distance; endless keeps getting harder        */
    float ramp=Clamp((float)i/1500.0f,0.0f,lv->goal_m>0?0.35f:1.2f);
    float rough=lv->rough*(1.0f+ramp);
    T_slope += (trnd()*2.0f-1.0f)*rough*0.55f;
    T_slope *= 0.80f;
    float dy=Clamp(T_slope+(trnd()*2.0f-1.0f)*rough*0.35f,-rough*1.25f,rough*1.25f);
    float ny=TgY+dy;
    const float lo=-SH*0.5f, hi=SH*0.9f;
    if(ny<lo){ ny=lo; T_slope= fabsf(T_slope)*0.5f; }
    if(ny>hi){ ny=hi; T_slope=-fabsf(T_slope)*0.5f; }
    TgY=ny;
    TB[T_n % TBUF] = (Vector2){TgX, TgY};
    T_n++;
    TgX += SEG;
    if(T_n - T_b >= TBUF-1) T_b++;
}

static void terrain_init(void){
    T_rng=LV[g_lv].seed*2654435761u+12345u;
    T_n=0; T_b=0; TgX=0; TgY=SH*0.65f; T_slope=0;
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

/* surface Y at world-x: O(1) lookup, Catmull-Rom smoothed between points */
static float surf_y(float x){
    int i=(int)floorf(x/SEG);
    if(i<T_b||i>=T_n-1) return SH*4.0f;
    float t=(x-(float)i*SEG)/SEG;
    float p1=TB[i%TBUF].y, p2=TB[(i+1)%TBUF].y;
    if(i<T_b+1||i>=T_n-2) return p1+t*(p2-p1);
    float p0=TB[(i-1)%TBUF].y, p3=TB[(i+2)%TBUF].y;
    return 0.5f*((2*p1)+(-p0+p2)*t+(2*p0-5*p1+4*p2-p3)*t*t+(-p0+3*p1-3*p2+p3)*t*t*t);
}

static float slope_deg(float x){
    return atan2f(surf_y(x+40)-surf_y(x-40),80.0f)*R2D;
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

/*============================================================================
  PICKUPS  (streamed in just ahead of the player, pooled & recycled)
============================================================================*/
static void pickups_reset(void){
    memset(g_coins,0,sizeof g_coins);
    memset(g_cans,0,sizeof g_cans);
    memset(g_bridges,0,sizeof g_bridges);
    g_spawn_i=15; g_next_can=25;
}

static float recycle_x(void){ return g_car.pos.x-2500.0f; }

static void spawn_at(int i){
    float x=(float)i*SEG;
    float gy=surf_y(x);
    unsigned salt=LV[g_lv].seed;
    /* coin clusters: 4 coins every 12 m, arcing over the ground */
    if((i%12)<4){
        int k=i/12;
        bool gold=hash_f(k,salt^0xA5u)<0.15f;
        int j=i%12;
        for(int s=0;s<MAX_COINS;s++){
            if(g_coins[s].active && g_coins[s].pos.x>=recycle_x()) continue;
            float arc=sinf((float)j/3.0f*PI)*35.0f;
            g_coins[s]=(Coin){{x,gy-55.0f-arc},true,(float)i*0.7f,gold?5:1};
            break;
        }
    }
    /* fuel canisters: get sparser the further you go */
    if(i==g_next_can){
        for(int s=0;s<MAX_CANISTERS;s++){
            if(g_cans[s].active && g_cans[s].pos.x>=recycle_x()) continue;
            g_cans[s]=(Canister){{x,gy-62.0f},true,(float)i*1.1f};
            break;
        }
        int gap=20+i/50; if(gap>45) gap=45;
        g_next_can+=gap;
    }
    /* bridges every 35 segments starting at seg 30 */
    if(i>=30 && (i-30)%35==0){
        for(int s=0;s<MAX_BRIDGES;s++){
            if(g_bridges[s].active && g_bridges[s].x1>=recycle_x()) continue;
            Vector2 pa=TB[i%TBUF], pb=TB[(i+3)%TBUF];
            float deck_y=(pa.y+pb.y)*0.5f-12.0f;
            g_bridges[s]=(Bridge){pa.x,pb.x,deck_y,true};
            break;
        }
    }
}

static void pickups_stream(void){
    while(g_spawn_i<T_n-5 && (float)g_spawn_i*SEG < g_car.pos.x+7000.0f)
        spawn_at(g_spawn_i++);
}

static float next_can_dist(float x){
    float best=1e9f;
    for(int i=0;i<MAX_CANISTERS;i++)
        if(g_cans[i].active && g_cans[i].pos.x>x-40.0f)
            best=fminf(best,(g_cans[i].pos.x-x)/100.0f);
    return best;
}

/*============================================================================
  CAR
============================================================================*/
static Vector2 car_pt(const Car *c, float fx, float fy){
    float rad=c->angle*D2R, cr=cosf(rad), sr=sinf(rad);
    float lx=c->hw*fx, ly=c->hh*fy;
    return (Vector2){ c->pos.x + cr*lx - sr*ly, c->pos.y + sr*lx + cr*ly };
}

static void car_spawn(Car *c, float x, int veh, Spec sp){
    memset(c,0,sizeof *c);
    const Vehicle *v=&VEH[veh];
    c->veh=veh; c->sp=sp; c->hw=v->hw; c->hh=v->hh;
    c->rear.r=v->rr; c->rear.ax=-v->ax;
    c->fwd.r =v->fr; c->fwd.ax = v->ax;
    c->rear.pos=(Vector2){x-v->ax, surf_y(x-v->ax)-v->rr};
    c->fwd.pos =(Vector2){x+v->ax, surf_y(x+v->ax)-v->fr};
    float wy=fminf(c->rear.pos.y,c->fwd.pos.y);
    c->pos=(Vector2){x, wy-sp.susp_len-c->hh-6.0f};
    c->angle=slope_deg(x);
    c->fuel=sp.fuel_cap;
    c->nitro=sp.nitro_cap*0.5f;
    c->prev_gnd=true;
    c->dist=x/100.0f;
}

static void car_kill(Car *c, int cause){
    if(c->dead) return;
    c->dead=true; c->dead_t=0; c->dead_cause=cause;
    if(cause==CAUSE_FUEL) return;
    /* wheels fly off */
    c->rear.vel=(Vector2){c->vel.x-160.0f,c->vel.y-320.0f};
    c->fwd.vel =(Vector2){c->vel.x+160.0f,c->vel.y-360.0f};
    c->vel.y-=220.0f;
    c->avel+=(c->avel>=0?1.0f:-1.0f)*120.0f;
}

static void car_physics(Car *c, Ctl in, float dt){
    float g=GRAVITY*LV[g_lv].grav;
    Wheel *ws[2]={&c->rear,&c->fwd};
    c->landed_evt=false;
    c->in=in;

    //dead fall-through
    if(c->dead){
        c->dead_t+=dt;
        c->boosting=false;
        if(c->dead_cause==CAUSE_FUEL) return;       /* just sits there    */
        for(int i=0;i<2;i++){
            ws[i]->vel.y+=g*dt;
            ws[i]->vel.x*=(1.0f-1.2f*dt);
            ws[i]->pos.x+=ws[i]->vel.x*dt;
            ws[i]->pos.y+=ws[i]->vel.y*dt;
            ws[i]->on=resolve(&ws[i]->pos,&ws[i]->vel,ws[i]->r);
            ws[i]->spin+=ws[i]->vel.x*dt*(R2D/ws[i]->r);
        }
        c->vel.y+=g*0.6f*dt;
        c->vel.x*=(1.0f-1.2f*dt);
        c->pos.x+=c->vel.x*dt; c->pos.y+=c->vel.y*dt;
        c->angle+=c->avel*dt; c->avel*=(1.0f-1.5f*dt);
        float sy=surf_y(c->pos.x);
        if(c->pos.y+c->hh*0.5f>sy){
            c->pos.y=sy-c->hh*0.5f;
            if(c->vel.y>0) c->vel.y*=-0.2f;
            c->avel*=0.9f;
        }
        return;
    }

    bool gas=in.gas, brake=in.brake;

    /* fuel */
    if(!c->infinite_fuel){
        c->fuel-=(gas?FUEL_GAS:FUEL_IDLE)*c->sp.fuel_use*dt;
        if(c->fuel<0) c->fuel=0;
    }
    bool has_fuel=c->fuel>0.01f;

    /* nitro */
    c->boosting=in.nitro && c->nitro>0.5f && has_fuel;
    if(c->boosting){ c->nitro-=NITRO_DRAIN*dt; if(c->nitro<0) c->nitro=0; }

    /*── drive/brake: applied to body X (wheels are locked to body X) ──*/
    float rad=c->angle*D2R;
    bool any_on=c->rear.on||c->fwd.on;
    float vmax=c->sp.max_spd*(c->boosting?1.35f:1.0f);
    if(any_on){
        float accel=0;
        if(gas&&has_fuel) accel+=c->sp.drive;
        if(brake)         accel-=BRAKE_F;
        if(c->boosting)   accel+=NITRO_ACC;
        float prev=c->vel.x;
        c->vel.x+=accel*dt;
        /* top speed: hard cap while accelerating, gentle decay after boost */
        if(c->vel.x>vmax) c->vel.x=fmaxf(vmax,fminf(prev,c->vel.x)-700.0f*dt);
        if(c->vel.x<-c->sp.max_spd) c->vel.x=-c->sp.max_spd;
        /* rolling friction when coasting */
        if(!gas&&!brake&&!c->boosting) c->vel.x*=powf(WHEEL_FRIC,dt*60.0f);
    } else if(c->boosting){
        /* rocket push along the body axis while airborne */
        c->vel.x+=cosf(rad)*NITRO_ACC*0.45f*dt;
        c->vel.y+=sinf(rad)*NITRO_ACC*0.45f*dt;
    }

    /*── wheel Y physics (gravity + ground resolve only) ─*/
    float impact=0;
    for(int i=0;i<2;i++){
        Wheel *w=ws[i];
        bool was_on=w->on;
        w->vel.y+=g*dt;
        w->pos.y+=w->vel.y*dt;
        float vy=w->vel.y;
        w->on=resolve(&w->pos,&w->vel,w->r);

        /* Bridge collision: treat deck as a flat surface                     */
        if(!w->on){
            for(int b=0;b<MAX_BRIDGES;b++){
                Bridge *br=&g_bridges[b];
                if(!br->active) continue;
                if(w->pos.x>=br->x0 && w->pos.x<=br->x1){
                    if(w->pos.y+w->r>=br->y && w->pos.y<br->y){
                        w->pos.y=br->y-w->r;
                        if(w->vel.y>0) w->vel.y=-w->vel.y*0.04f;
                        w->on=true;
                    }
                }
            }
        }
        if(w->on && !was_on) impact=fmaxf(impact,vy);
        /* visual spin */
        if(w->on) w->spin+=c->vel.x*dt*(R2D/w->r);
        else if(gas&&has_fuel) w->spin+=900.0f*dt;
    }

    /*── RIGID VERTICAL SUSPENSION
      Wheels ride on vertical struts — they NEVER swing sideways.
        1. Wheel X is hard-locked to mount X every frame (no lateral drift).
        2. Wheel Y bounces freely: spring + damper in Y only.
        3. Hard stops prevent strut inversion (wheel above mount).           */
    float cr=cosf(rad), sr=sinf(rad);
    for(int i=0;i<2;i++){
        Wheel *w=ws[i];
        float mount_x=c->pos.x+cr*w->ax-sr*c->hh;
        float mount_y=c->pos.y+sr*w->ax+cr*c->hh;
        w->pos.x=mount_x;
        w->vel.x=c->vel.x;

        float dy=w->pos.y-mount_y;
        float relvy=w->vel.y-c->vel.y;
        float force=(dy-c->sp.susp_len)*c->sp.susp_k+relvy*c->sp.susp_d;
        c->vel.y+=force*dt/3.0f;
        w->vel.y-=force*dt;

        if(w->pos.y<mount_y){ w->pos.y=mount_y; if(w->vel.y<0) w->vel.y=0; }
        if(dy>c->sp.susp_len*2.2f){
            w->pos.y=mount_y+c->sp.susp_len*2.2f;
            if(w->vel.y>c->vel.y) w->vel.y=c->vel.y;
        }
    }

    /*── body gravity */
    bool gnd=c->rear.on||c->fwd.on;
    c->vel.y+=g*(gnd?0.10f:0.32f)*dt;

    /*── rotation ─*/
    if(!gnd){
        /* gas tips nose UP (angle negative), brake tips nose DOWN          */
        if(gas)   c->avel-=c->sp.air_rot*dt;
        if(brake) c->avel+=c->sp.air_rot*dt;
        c->avel*=(1.0f-AIR_DAMP*dt);
    } else {
        float wa=atan2f(c->fwd.pos.y-c->rear.pos.y,c->fwd.pos.x-c->rear.pos.x)*R2D;
        float diff=wrap180(wa-c->angle);
        c->avel+=diff*15.0f*dt;
        c->avel*=(1.0f-GND_DAMP*dt);
    }
    c->angle+=c->avel*dt;

    /* very light air drag on x */
    if(!gnd) c->vel.x*=(1.0f-0.4f*dt);

    c->pos.x+=c->vel.x*dt;
    c->pos.y+=c->vel.y*dt;

    /* prevent body centre sinking below ground */
    {
        float sy=surf_y(c->pos.x);
        if(c->pos.y+c->hh*0.5f>sy){
            c->pos.y=sy-c->hh*0.5f;
            if(c->vel.y>0) c->vel.y*=-0.05f;
        }
    }

    if(c->pos.x>c->dist*100.0f) c->dist=c->pos.x/100.0f;

    /*── air time / landing events (used by stunts, AI and effects) */
    if(!gnd){
        if(c->prev_gnd){ c->takeoff_ang=c->angle; c->air_t=0; }
        c->air_t+=dt;
    } else if(!c->prev_gnd){
        c->landed_evt=true;
        c->land_air=c->air_t;
        c->land_rot=c->angle-c->takeoff_ang;
        c->land_vy=impact;
        c->air_t=0;
    }
    c->prev_gnd=gnd;

    /*── death: stuck upside-down, or the driver's head hits the ground */
    {
        float na=fmodf(c->angle,360.0f);
        if(na<0) na+=360.0f;
        bool upside=(na>FLIP_DEAD_DEG && na<360.0f-FLIP_DEAD_DEG);
        if(upside && fabsf(c->avel)<25.0f && gnd) car_kill(c,CAUSE_FLIP);
        Vector2 head=car_pt(c,-0.05f,-2.10f);
        if(head.y+12.0f>surf_y(head.x)) car_kill(c,CAUSE_HEAD);
    }

    /*── out of fuel and rolled to a stop */
    if(!c->infinite_fuel && !has_fuel && gnd && fabsf(c->vel.x)<25.0f){
        c->still_t+=dt;
        if(c->still_t>2.5f) car_kill(c,CAUSE_FUEL);
    } else c->still_t=0;
}

/*============================================================================
  AI DRIVER
  - On the ground: looks ahead along the terrain (distance scales with speed),
    measures upcoming slope and curvature, picks a target speed (full power on
    climbs, back off before sharp crests to avoid launching), uses nitro on
    climbs and backs up when stuck.
  - In the air: integrates a ballistic trajectory to predict where/when it
    lands, reads the ground angle there and steers body rotation with a PD
    controller so it touches down parallel to the slope. With enough hang
    time it plans a backflip first.
============================================================================*/
static void ai_reset(AIBrain *b, int level, unsigned seed){
    memset(b,0,sizeof *b);
    b->rng=seed;
    switch(level){
    case 1:  b->skill=0.74f; b->react=0.20f; b->flips=false; b->use_nitro=false; break;
    case 2:  b->skill=0.87f; b->react=0.08f; b->flips=true;  b->use_nitro=true;  break;
    default: b->skill=0.98f; b->react=0.0f;  b->flips=true;  b->use_nitro=true;  break;
    }
}

static float ai_rand(AIBrain *b){
    b->rng=b->rng*1664525u+1013904223u;
    return (float)(b->rng>>8)/16777216.0f;
}

/* Ballistic landing prediction; optionally records the arc. */
static int predict_flight(const Car *c, Vector2 *pts, int maxp, Vector2 *land, float *land_t){
    float g=GRAVITY*LV[g_lv].grav*0.62f;
    float clr=c->hh+c->sp.susp_len*1.05f+c->rear.r;
    Vector2 p=c->pos, v=c->vel;
    const float st=1.0f/30.0f;
    float t=0; int n=0;
    while(t<4.0f){
        v.y+=g*st; v.x*=(1.0f-0.4f*st);
        p.x+=v.x*st; p.y+=v.y*st; t+=st;
        if(pts && n<maxp) pts[n++]=(Vector2){p.x,p.y+clr};
        if(p.y+clr>=surf_y(p.x)) break;
    }
    *land=(Vector2){p.x,p.y+clr};
    *land_t=t;
    return n;
}

/* how long a full rotation takes with the stick held, from this car's
   actual air-rotation model (torque + damping)                            */
static float flip_time(float air_rot){
    float av=0, ang=0, t=0;
    const float st=1.0f/120.0f;
    while(ang<360.0f && t<5.0f){
        av+=air_rot*st; av*=(1.0f-AIR_DAMP*st); ang+=av*st; t+=st;
    }
    return t;
}

static Ctl ai_drive(Car *c, AIBrain *b, float dt){
    b->think_t-=dt;
    if(b->think_t>0) return b->held;
    float step=fmaxf(dt,b->react);
    b->think_t=b->react;

    Ctl o={0};
    bool gnd=c->rear.on||c->fwd.on;
    float spd=c->vel.x, x=c->pos.x;

    if(gnd){
        b->airborne=false;
        float look=140.0f+fmaxf(spd,0)*0.5f;
        float y0=surf_y(x), y1=surf_y(x+look), y2=surf_y(x+look*2.0f);
        float s1=(y1-y0)/look;           /* <0 climbing, >0 descending        */
        float s2=(y2-y1)/look;
        float crest=s2-s1;               /* >0 : ground falls away ahead      */
        float vmax=c->sp.max_spd;
        float target=vmax*b->skill;
        if(s1<-0.30f) target=vmax;                                /* climb   */
        if(crest>0.40f) target*=Clamp(1.0f-(crest-0.40f)*0.9f,0.45f,1.0f);
        if(s1>0.45f) target*=0.8f;                                /* descent */

        /* fuel awareness (autopilot only - the rival has unlimited fuel)    */
        if(!c->infinite_fuel){
            float frac=c->fuel/c->sp.fuel_cap;
            float nc=next_can_dist(x);
            float rate=g_coach.rate>0?g_coach.rate:0.5f;
            if(frac<0.4f && c->fuel/rate<nc*1.2f) target*=0.85f;  /* feather */
            if(frac<0.4f && s1>0.12f && spd>220.0f) target=0;     /* coast   */
        }

        o.gas  =spd<target;
        o.brake=spd>target+160.0f;

        /* stuck? back up and take another run at it */
        if(o.gas && spd<15.0f) b->stuck_t+=step; else b->stuck_t=0;
        if(b->stuck_t>1.5f){ b->reverse_t=0.7f; b->stuck_t=0; }
        if(b->reverse_t>0){ b->reverse_t-=step; o.gas=false; o.brake=true; }

        o.nitro=b->use_nitro && c->nitro>c->sp.nitro_cap*0.25f && crest<0.3f &&
                ((s1<-0.25f && spd<target*0.85f) || (b->skill>0.95f && s1<-0.1f));
    } else {
        Vector2 land; float lt;
        predict_flight(c,NULL,0,&land,&lt);
        float base=slope_deg(land.x);
        if(!b->airborne){
            b->airborne=true; b->ref_ang=c->angle; b->flip_off=0;
            if(b->flips && lt>flip_time(c->sp.air_rot)*1.1f+0.15f && ai_rand(b)<0.7f)
                b->flip_off=-360.0f;
        }
        float tgt=base+360.0f*roundf((b->ref_ang-base)/360.0f)+b->flip_off;
        float err=tgt-c->angle;
        float want=Clamp(err*2.5f,-260.0f,260.0f);
        if(lt<0.25f) want=Clamp(err*4.0f,-120.0f,120.0f);
        if(want<c->avel-12.0f)      o.gas=true;
        else if(want>c->avel+12.0f) o.brake=true;
    }
    b->held=o;
    return o;
}

/*============================================================================
  EFFECTS: particles, popups
============================================================================*/
static void emit(Vector2 p, Vector2 v, float life, float size, Color col, float grav, int kind){
    for(int i=0;i<MAX_PART;i++){
        if(g_part[i].life>0) continue;
        g_part[i]=(Particle){p,v,life,life,size,grav,rnd_f(0,360),col,kind};
        return;
    }
}

static void particles_update(float dt){
    for(int i=0;i<MAX_PART;i++){
        Particle *q=&g_part[i];
        if(q->life<=0) continue;
        q->life-=dt;
        q->v.y+=q->grav*dt;
        q->v.x*=(1.0f-1.5f*dt);
        q->p.x+=q->v.x*dt; q->p.y+=q->v.y*dt;
        q->rot+=q->v.x*dt*2.0f;
    }
}

static void draw_particles(void){
    for(int i=0;i<MAX_PART;i++){
        Particle *q=&g_part[i];
        if(q->life<=0) continue;
        float u=q->life/q->max;                 /* 1 → 0 */
        Color c=q->col; c.a=(unsigned char)(c.a*u);
        switch(q->kind){
        case 0: DrawCircleV(q->p,q->size*(1.6f-u*0.6f),c); break;
        case 1: DrawCircleV(q->p,q->size*u+0.5f,c); break;
        case 2: DrawRectanglePro((Rectangle){q->p.x,q->p.y,q->size,q->size*0.6f},
                                 (Vector2){q->size*0.5f,q->size*0.3f},q->rot,c); break;
        }
    }
}

static void popup(Color col, int size, const char *fmt, ...){
    int slot=0; float best=1e9f;
    for(int i=0;i<MAX_POP;i++){
        if(g_pop[i].t<=0){ slot=i; break; }
        if(g_pop[i].t<best){ best=g_pop[i].t; slot=i; }
    }
    Popup *p=&g_pop[slot];
    va_list ap; va_start(ap,fmt);
    vsnprintf(p->txt,sizeof p->txt,fmt,ap);
    va_end(ap);
    p->col=col; p->size=size; p->t=1.8f; p->serial=++g_pop_serial;
}

static void popups_update(float dt){
    for(int i=0;i<MAX_POP;i++) if(g_pop[i].t>0) g_pop[i].t-=dt;
}

static void car_fx(const Car *c, float dt){
    if(c->dead) return;
    Color dust=LV[g_lv].dirt; dust.a=170;
    const Wheel *ws[2]={&c->rear,&c->fwd};
    float spd=fabsf(c->vel.x);
    for(int i=0;i<2;i++){
        if(!ws[i]->on || spd<60.0f) continue;
        float rate=spd/650.0f*(c->in.gas?45.0f:15.0f);
        if(rnd_f(0,1)<rate*dt){
            Vector2 p={ws[i]->pos.x-ws[i]->r*0.4f,ws[i]->pos.y+ws[i]->r*0.8f};
            emit(p,(Vector2){-c->vel.x*0.15f+rnd_f(-30,30),-rnd_f(40,150)},
                 rnd_f(0.5f,0.9f),rnd_f(4,9),dust,220.0f,0);
        }
    }
    float rad=c->angle*D2R;
    Vector2 ex0=car_pt(c,-1.01f,-0.38f);
    Vector2 ex={ex0.x-cosf(rad)*26.0f,ex0.y-sinf(rad)*26.0f};
    Vector2 back={-cosf(rad),-sinf(rad)};
    if(c->in.gas && c->fuel>0.01f && rnd_f(0,1)<18.0f*dt)
        emit(ex,(Vector2){back.x*60+c->vel.x*0.3f,-40+back.y*60},0.8f,5,RGBA(90,90,90,120),-30.0f,0);
    if(c->boosting){
        for(int k=0;k<3;k++){
            Color fc=k==0?RGBA(255,240,120,230):RGBA(255,(unsigned char)rnd_f(90,170),30,220);
            emit(ex,(Vector2){back.x*rnd_f(300,520)+c->vel.x*0.5f,back.y*rnd_f(300,520)+rnd_f(-40,40)},
                 rnd_f(0.15f,0.3f),rnd_f(4,8),fc,0,1);
        }
    }
}

static void burst(Vector2 p, int n, Color col, float spd, int kind, float grav){
    for(int i=0;i<n;i++){
        float a=rnd_f(0,2*PI), s=rnd_f(spd*0.3f,spd);
        emit(p,(Vector2){cosf(a)*s,sinf(a)*s},rnd_f(0.4f,0.9f),rnd_f(3,7),col,grav,kind);
    }
}

/*============================================================================
  RUN FLOW
============================================================================*/
static void run_start(void){
    terrain_init();
    pickups_reset();
    Spec sp=make_spec(g_save.veh,g_save.up);
    float sx=8.0f*SEG;
    car_spawn(&g_car,sx,g_save.veh,sp);
    pickups_stream();

    g_rival_on=g_save.rival>0;
    g_rival_done=false; g_rival_time=0; g_lead=0;
    if(g_rival_on){
        Spec rs=sp;
        car_spawn(&g_rival,sx,g_save.veh,rs);
        g_rival.infinite_fuel=true;
        ai_reset(&g_rbrain,g_save.rival,LV[g_lv].seed);
    }
    ai_reset(&g_abrain,3,99u);
    g_autopilot=false; g_autopilot_used=false;

    g_nrec=0;
    ghost_load();

    memset(g_part,0,sizeof g_part);
    memset(g_pop,0,sizeof g_pop);
    memset(&g_coach,0,sizeof g_coach);
    g_coach.last_fuel=g_car.fuel; g_coach.last_dist=g_car.dist;
    g_combo=0; g_last_stunt=-100; g_shake=0; g_acc=0; g_npred=0;
    g_run_done=false;

    g_cam.target=g_car.pos; g_cam.zoom=1.05f;
    g_st=ST_PLAY;
    popup(RGB(255,220,60),40,"%s",LV[g_lv].name);
    if(g_rival_on) popup(RGB(120,170,255),22,"Race the AI rival!");
}

static void end_run(bool win){
    if(g_run_done) return;
    g_run_done=true;
    Car *c=&g_car;
    RunResult *r=&g_res;
    memset(r,0,sizeof *r);
    r->win=win; r->cause=c->dead_cause;
    r->coins=c->coins;
    r->dist_bonus=(int)(c->dist/10.0f);
    if(win) r->stage_bonus=(int)(LV[g_lv].goal_m*0.5f);
    /* head-to-head only counts on stages with a finish line: in ENDLESS the
       rival never stops, so it is shown for reference without a payout      */
    if(g_rival_on && LV[g_lv].goal_m>0){
        bool beat=win && !(g_rival_done && g_rival_time<c->time);
        r->place=beat?1:2;
        if(beat) r->rival_bonus=100*g_save.rival;
        if(beat && g_save.rival==3) ach_unlock(ACH_HARD);
    }
    r->total=r->coins+r->dist_bonus+r->stage_bonus+r->rival_bonus;
    r->autopilot=g_autopilot_used;
    if(r->autopilot) r->total/=2;

    g_save.bank+=r->total;
    g_save.total_coins+=r->total;
    g_save.runs++;
    g_save.total_dist+=c->dist;
    g_save.total_flips+=c->flips;

    if(!r->autopilot){
        if(c->dist>g_save.best_dist[g_lv]){ g_save.best_dist[g_lv]=c->dist; r->new_dist=true; }
        if(win && (g_save.best_time[g_lv]<=0 || c->time<g_save.best_time[g_lv])){
            g_save.best_time[g_lv]=c->time; r->new_time=true;
        }
        if(win) g_save.completed[g_lv]=1;
        float score=win?100000.0f-c->time:c->dist;
        if(score>g_ghost_score){ ghost_save(score); g_ghost_score=score; }
    }

    if(win) ach_unlock(ACH_FIRST_WIN);
    if(g_lv==ENDLESS_LV && c->dist>=1000.0f) ach_unlock(ACH_MARATHON);
    if(g_save.total_coins>=5000) ach_unlock(ACH_TYCOON);
    if(c->perfects>=10) ach_unlock(ACH_SMOOTH);
    bool all=true; for(int i=0;i<ENDLESS_LV;i++) if(!g_save.completed[i]) all=false;
    if(all) ach_unlock(ACH_TOUR);

    save_game();
    g_st=win?ST_WIN:ST_DEAD;
    if(win) sfx(SFX_WIN);
}

static void handle_landing(Car *c){
    if(c->land_vy>500.0f){
        g_shake=fmaxf(g_shake,Clamp((c->land_vy-500.0f)/60.0f,0,14));
        sfx(SFX_LAND);
        Color d=LV[g_lv].dirt; d.a=190;
        burst((Vector2){c->rear.pos.x,c->rear.pos.y+c->rear.r},8,d,180,0,300);
        burst((Vector2){c->fwd.pos.x, c->fwd.pos.y+c->fwd.r}, 8,d,180,0,300);
    }
    if(c->dead || c->land_air<0.35f) return;

    float diff=fabsf(wrap180(c->angle-slope_deg(c->pos.x)));
    int nflips=(int)((fabsf(c->land_rot)+40.0f)/360.0f);
    int pts=0;
    if(nflips>0){
        const char *nm=c->land_rot<0?"BACKFLIP":"FRONTFLIP";
        if(nflips>1) popup(RGB(255,120,255),34,"%dx %s!",nflips,nm);
        else         popup(RGB(255,165,45),34,"%s!",nm);
        pts+=15*nflips*nflips;
        c->flips+=nflips;
        c->nitro=fminf(c->sp.nitro_cap,c->nitro+35.0f*nflips*c->sp.nitro_gain);
        sfx(SFX_FLIP);
        ach_unlock(ACH_FLIP);
        if(nflips>=2) ach_unlock(ACH_DOUBLE);
    }
    if(c->land_air>1.0f){
        popup(RGB(120,210,255),26,"AIR TIME %.1fs",c->land_air);
        pts+=(int)(c->land_air*4.0f);
        c->nitro=fminf(c->sp.nitro_cap,c->nitro+c->land_air*8.0f*c->sp.nitro_gain);
        if(c->land_air>=3.0f) ach_unlock(ACH_AIR);
    }
    if(diff<10.0f){
        popup(RGB(120,255,140),26,"PERFECT LANDING");
        pts+=5; c->perfects++;
        c->nitro=fminf(c->sp.nitro_cap,c->nitro+12.0f*c->sp.nitro_gain);
    }
    if(pts>0){
        if(g_time-g_last_stunt<6.0f) g_combo++; else g_combo=1;
        g_last_stunt=g_time;
        int total=pts*g_combo;
        c->coins+=total;
        if(g_combo>1) popup(RGB(255,230,80),24,"+%d coins  (COMBO x%d)",total,g_combo);
        else          popup(RGB(255,230,80),22,"+%d coins",total);
    }
}

static Ctl read_input(void){
    Ctl o={0};
    o.gas  =IsKeyDown(KEY_RIGHT)||IsKeyDown(KEY_D);
    o.brake=IsKeyDown(KEY_LEFT) ||IsKeyDown(KEY_A);
    o.nitro=IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT)||IsKeyDown(KEY_SPACE);
    if(IsGamepadAvailable(0)){
        o.gas  |=IsGamepadButtonDown(0,GAMEPAD_BUTTON_RIGHT_TRIGGER_2)||
                 IsGamepadButtonDown(0,GAMEPAD_BUTTON_RIGHT_FACE_DOWN)||
                 GetGamepadAxisMovement(0,GAMEPAD_AXIS_RIGHT_TRIGGER)>0.3f;
        o.brake|=IsGamepadButtonDown(0,GAMEPAD_BUTTON_LEFT_TRIGGER_2)||
                 IsGamepadButtonDown(0,GAMEPAD_BUTTON_RIGHT_FACE_LEFT)||
                 GetGamepadAxisMovement(0,GAMEPAD_AXIS_LEFT_TRIGGER)>0.3f;
        o.nitro|=IsGamepadButtonDown(0,GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
    }
    return o;
}

static void rival_step(float dt){
    Car *r=&g_rival;
    float goal=LV[g_lv].goal_m;
    Ctl in=g_rival_done?(Ctl){false,r->vel.x>30.0f,false}:ai_drive(r,&g_rbrain,dt);
    car_physics(r,in,dt);
    /* rival never gets stuck for good: respawn after a crash / falling behind */
    bool lost=r->pos.x<(float)(T_b+3)*SEG;
    if((r->dead && r->dead_t>2.0f) || lost){
        float x=lost?g_car.pos.x-600.0f:r->pos.x-150.0f;
        float d=r->dist, t=r->time;
        car_spawn(r,fmaxf(x,(float)(T_b+4)*SEG),r->veh,r->sp);
        r->infinite_fuel=true; r->dist=fmaxf(d,r->dist); r->time=t;
        g_rbrain.airborne=false;
    }
    if(!r->dead) r->time+=dt;
    if(goal>0 && !g_rival_done && r->dist>=goal){
        g_rival_done=true; g_rival_time=r->time;
        if(g_st==ST_PLAY) popup(RGB(120,170,255),26,"RIVAL FINISHED (%.1fs)",g_rival_time);
    }
}

static void game_step(float dt){
    Car *c=&g_car;
    Ctl in=g_autopilot?ai_drive(c,&g_abrain,dt):read_input();
    if(g_autopilot) g_autopilot_used=true;
    bool was_dead=c->dead, was_boost=c->boosting;

    car_physics(c,in,dt);
    if(c->boosting && !was_boost) sfx(SFX_NITRO);
    if(!c->dead) c->time+=dt;

    if(c->dead && !was_dead){
        if(c->dead_cause!=CAUSE_FUEL){
            g_shake=16;
            sfx(SFX_CRASH);
            burst(c->pos,26,RGBA(60,60,60,230),320,2,600);
            burst(c->pos,18,RGBA(255,170,40,230),260,1,200);
            popup(RGB(255,80,80),40,c->dead_cause==CAUSE_HEAD?"HEAD HIT!":"CRASH!");
        } else popup(RGB(255,170,60),36,"OUT OF FUEL");
    }
    if(c->landed_evt) handle_landing(c);
    if(c->dead && c->dead_t>(c->dead_cause==CAUSE_FUEL?0.6f:2.0f)){ end_run(false); return; }

    if(g_rival_on){
        rival_step(dt);
        float d=c->pos.x-g_rival.pos.x;
        if(d>150.0f && g_lead!=1){
            if(g_lead==-1) popup(RGB(120,255,140),28,"OVERTAKE!");
            g_lead=1;
        } else if(d<-150.0f && g_lead!=-1){
            if(g_lead==1) popup(RGB(120,170,255),24,"Rival takes the lead");
            g_lead=-1;
        }
    }

    /* pickups */
    if(!c->dead){
        for(int i=0;i<MAX_COINS;i++){
            Coin *k=&g_coins[i];
            if(!k->active) continue;
            Vector2 cp={k->pos.x,k->pos.y+sinf(g_time*3+k->ph)*5};
            if(Vector2Distance(c->pos,cp)<70||
               Vector2Distance(c->rear.pos,cp)<52||
               Vector2Distance(c->fwd.pos,cp)<52){
                k->active=false; c->coins+=k->value;
                sfx(SFX_COIN);
                burst(cp,k->value>1?14:7,RGBA(255,225,60,255),160,1,0);
            }
        }
        for(int i=0;i<MAX_CANISTERS;i++){
            Canister *k=&g_cans[i];
            if(!k->active) continue;
            Vector2 cp={k->pos.x,k->pos.y+sinf(g_time*2+k->ph)*4};
            if(Vector2Distance(c->pos,cp)<85||
               Vector2Distance(c->rear.pos,cp)<60||
               Vector2Distance(c->fwd.pos,cp)<60){
                k->active=false; c->cans++;
                c->fuel=fminf(c->sp.fuel_cap,c->fuel+c->sp.fuel_cap*0.45f);
                sfx(SFX_FUEL);
                burst(cp,16,RGBA(90,240,90,255),180,1,0);
                popup(RGB(90,240,90),22,"+FUEL");
            }
        }
    }

    /* ghost recording */
    while(g_nrec<GHOST_MAX && (float)g_nrec/GHOST_HZ<=c->time)
        g_rec[g_nrec++]=(GSample){c->pos.x,c->pos.y,c->angle};

    float far=fmaxf(c->pos.x,g_rival_on?g_rival.pos.x:0);
    terrain_extend(far);
    pickups_stream();

    float goal=LV[g_lv].goal_m;
    if(goal>0 && c->dist>=goal && !c->dead){
        popup(RGB(90,245,110),40,"FINISH!");
        end_run(true);
    }
}

/* after the run: keep the world alive (coasting car, falling wreck) */
static void idle_step(float dt){
    car_physics(&g_car,(Ctl){0},dt);
    if(g_rival_on) rival_step(dt);
    terrain_extend(g_car.pos.x);
}

/*============================================================================
  AI COACH
============================================================================*/
static void coach_update(float dt){
    Car *c=&g_car;
    g_coach.t+=dt;
    if(g_coach.t>=0.5f){
        float df=g_coach.last_fuel-c->fuel, dd=c->dist-g_coach.last_dist;
        if(df>0 && dd>0.3f){
            float s=df/dd;
            g_coach.rate=g_coach.rate<=0?s:Lerp(g_coach.rate,s,0.15f);
        }
        g_coach.last_fuel=c->fuel; g_coach.last_dist=c->dist; g_coach.t=0;
    }

    g_coach.msg[0]=0;
    g_npred=0;
    if(c->dead) return;
    bool gnd=c->rear.on||c->fwd.on;

    if(!gnd && c->air_t>0.12f){
        g_npred=predict_flight(c,g_pred,PRED_MAX,&g_pred_land,&g_pred_t);
        float target=slope_deg(g_pred_land.x);
        float err=wrap180(target-c->angle);
        if(g_pred_t>0.25f && fabsf(err)>20.0f){
            snprintf(g_coach.msg,sizeof g_coach.msg,"LANDING: tilt nose %s  (hold %s)",
                     err>0?"DOWN":"UP",err>0?"BRAKE":"GAS");
            g_coach.col=RGB(255,200,80);
            return;
        }
    }

    float range=g_coach.rate>0?c->fuel/g_coach.rate:9999.0f;
    float nc=next_can_dist(c->pos.x);
    float frac=c->fuel/c->sp.fuel_cap;
    if(frac<0.35f && range<nc && nc<1e8f){
        snprintf(g_coach.msg,sizeof g_coach.msg,
                 "FUEL CRITICAL: ~%.0f m range, next can in %.0f m - feather the throttle",range,nc);
        g_coach.col=RGB(255,90,90);
        return;
    }
    if(gnd){
        float x=c->pos.x;
        float worst=0, xs=0;
        for(float a=300;a<=2500;a+=100){
            float s=(surf_y(x+a+100)-surf_y(x+a))/100.0f;
            if(s<worst){ worst=s; xs=a; }
        }
        if(worst<-0.75f){
            snprintf(g_coach.msg,sizeof g_coach.msg,"Steep climb in %.0f m - build speed%s",
                     xs/100.0f,c->nitro>30?", save NITRO for it":"");
            g_coach.col=RGB(255,230,120);
            return;
        }
        float look=200.0f+fmaxf(c->vel.x,0)*0.6f;
        float s1=(surf_y(x+look)-surf_y(x))/look;
        float s2=(surf_y(x+look*2)-surf_y(x+look))/look;
        if(s2-s1>0.6f && c->vel.x>420.0f){
            snprintf(g_coach.msg,sizeof g_coach.msg,"Sharp crest ahead - ease off to stay grounded");
            g_coach.col=RGB(255,230,120);
            return;
        }
    }
}

/*============================================================================
  DRAWING - world
============================================================================*/
typedef struct { Color body, cabin, stripe, helmet; } Paint;
static Paint veh_paint(int v){
    return (Paint){VEH[v].body,VEH[v].cabin,VEH[v].stripe,RGB(20,20,190)};
}
static const Paint RIVAL_PAINT={{40,90,220,255},{25,60,170,255},{255,210,40,255},{230,230,230,255}};
static const Paint GHOST_PAINT={{235,235,255,255},{200,200,230,255},{255,255,255,255},{200,200,230,255}};

static void draw_backdrop(void){
    Level *lv=&LV[g_lv];
    DrawRectangleGradientV(0,0,SW,SH,lv->sky0,lv->sky1);
    float cx=g_cam.target.x, cy=g_cam.target.y;

    /* stars */
    if(g_lv>=3){
        for(int i=0;i<110;i++){
            float sx=wrapf(hash_f(i,11)*SW*1.3f-cx*0.02f,(float)SW);
            float sy=hash_f(i,23)*SH*0.6f;
            float tw=0.5f+0.5f*sinf(g_time*2+i);
            DrawCircle((int)sx,(int)sy,i%5==0?2:1,RGBA(255,255,255,(unsigned char)(120+100*tw)));
        }
    }
    /* sun / earth / glow */
    switch(g_lv){
    case 0: case 2: DrawCircle(SW-260,130,70,RGBA(255,250,210,90)); DrawCircle(SW-260,130,48,RGB(255,248,200)); break;
    case 1: DrawCircle(SW-300,150,90,RGBA(255,240,150,90)); DrawCircle(SW-300,150,60,RGB(255,250,200)); break;
    case 3: DrawCircleGradient(SW/2,SH,(float)SH*0.9f,RGBA(255,80,10,90),RGBA(255,80,10,0)); break;
    case 4: DrawCircle(SW-280,140,56,RGB(40,90,200));
            DrawCircle(SW-295,128,22,RGB(60,150,70)); DrawCircle(SW-262,158,16,RGB(60,150,70));
            DrawCircle(SW-280,140,58,RGBA(150,200,255,60)); break;
    case 5: DrawCircle(SW/2+200,SH/2+40,140,RGBA(255,170,90,80));
            DrawCircle(SW/2+200,SH/2+40,105,RGB(255,190,110));
            for(int k=0;k<5;k++) DrawRectangle(SW/2+80,SH/2+40+k*16,240,6,ColorLerp(lv->sky0,lv->sky1,0.85f));
            break;
    }
    /* clouds */
    if(g_lv<=2){
        for(int i=0;i<7;i++){
            float x=wrapf(hash_f(i,5)*SW*1.6f-cx*0.06f+g_time*12.0f,(float)SW+400)-200;
            float y=60+hash_f(i,9)*200;
            Color c=RGBA(255,255,255,170);
            DrawEllipse((int)x,(int)y,70,24,c);
            DrawEllipse((int)x+45,(int)y-12,50,26,c);
            DrawEllipse((int)x-40,(int)y-6,44,20,c);
        }
    }
    /* two parallax mountain layers */
    float vy=Clamp((cy-SH*0.5f)*-0.08f,-80,80);
    Color far=ColorLerp(lv->sky1,lv->gnd,0.30f), near=ColorLerp(lv->sky1,lv->gnd,0.55f);
    for(int x=0;x<SW;x+=6){
        float wx=x+cx*0.12f;
        float h=SH*0.50f+sinf(wx*0.004f)*70+sinf(wx*0.0013f+1.0f)*90+vy;
        DrawRectangle(x,(int)h,6,SH-(int)h,far);
    }
    for(int x=0;x<SW;x+=6){
        float wx=x+cx*0.3f;
        float h=SH*0.66f+sinf(wx*0.006f+2.0f)*45+sinf(wx*0.0021f)*70+vy*1.6f;
        DrawRectangle(x,(int)h,6,SH-(int)h,near);
    }
}

static void draw_weather(void){
    Level *lv=&LV[g_lv];
    float cx=g_cam.target.x;
    switch(lv->wx){
    case WX_SNOW:
        for(int i=0;i<160;i++){
            float sp=40+hash_f(i,3)*60;
            float x=wrapf(hash_f(i,1)*SW+sinf(g_time*0.8f+i)*30-cx*0.5f*(0.5f+hash_f(i,7)),(float)SW);
            float y=wrapf(hash_f(i,2)*SH+g_time*sp,(float)SH);
            DrawCircleV((Vector2){x,y},1.5f+hash_f(i,4)*2.5f,RGBA(255,255,255,210));
        }
        break;
    case WX_SAND:
        for(int i=0;i<70;i++){
            float x=wrapf(hash_f(i,1)*SW-g_time*(500+hash_f(i,3)*300)-cx*0.6f,(float)SW);
            float y=hash_f(i,2)*SH;
            DrawLineEx((Vector2){x,y},(Vector2){x+18+hash_f(i,5)*20,y+2},1.5f,RGBA(240,210,150,90));
        }
        break;
    case WX_EMBERS:
        for(int i=0;i<80;i++){
            float x=wrapf(hash_f(i,1)*SW+sinf(g_time+i)*20-cx*0.4f,(float)SW);
            float y=wrapf(hash_f(i,2)*SH-g_time*(40+hash_f(i,3)*70),(float)SH);
            float fl=0.5f+0.5f*sinf(g_time*8+i*3);
            DrawCircleV((Vector2){x,y},1.5f+fl*1.5f,RGBA(255,(unsigned char)(100+fl*100),30,(unsigned char)(150+fl*90)));
        }
        break;
    case WX_LEAVES:
        for(int i=0;i<22;i++){
            float x=wrapf(hash_f(i,1)*SW-g_time*(60+hash_f(i,3)*40)-cx*0.5f,(float)SW);
            float y=wrapf(hash_f(i,2)*SH+g_time*(30+hash_f(i,4)*30)+sinf(g_time*2+i)*15,(float)SH);
            Color c=i%3==0?RGBA(220,140,40,200):RGBA(90,170,60,200);
            DrawEllipse((int)x,(int)y,5,3,c);
        }
        break;
    case WX_DUST:
        for(int i=0;i<60;i++){
            float x=wrapf(hash_f(i,1)*SW-cx*0.3f+sinf(g_time*0.5f+i)*40,(float)SW);
            float y=wrapf(hash_f(i,2)*SH-g_time*(10+hash_f(i,3)*15),(float)SH);
            float tw=0.5f+0.5f*sinf(g_time*3+i);
            DrawCircleV((Vector2){x,y},1+tw*1.5f,RGBA(200,220,255,(unsigned char)(60+tw*120)));
        }
        break;
    default: break;
    }
}

static void view_range(float *wl, float *wr, float *wb){
    *wl=g_cam.target.x-g_cam.offset.x/g_cam.zoom-60.0f;
    *wr=g_cam.target.x+(SW-g_cam.offset.x)/g_cam.zoom+60.0f;
    *wb=g_cam.target.y+(SH-g_cam.offset.y)/g_cam.zoom+80.0f;
}

static void draw_terrain(void){
    Color gc=LV[g_lv].gnd, dc=LV[g_lv].dirt, deep=ColorLerp(dc,BLACK,0.35f);
    float wl,wr,wb; view_range(&wl,&wr,&wb);
    const float st=25.0f;
    float x=floorf(wl/st)*st;
    float ya=surf_y(x);
    for(;x<wr;x+=st){
        float xb=x+st, yb=surf_y(xb);
        if(ya<SH*3.0f && yb<SH*3.0f){
            float bot=fmaxf(wb,fmaxf(ya,yb)+50.0f);
            Vector2 a={x,ya}, b={xb,yb};
            Vector2 ba={x,bot}, bb={xb,bot};
            DrawTriangle(a,ba,b,dc); DrawTriangle(b,ba,bb,dc);
            Vector2 a3={x,ya+90}, b3={xb,yb+90};
            DrawTriangle(a3,ba,b3,deep); DrawTriangle(b3,ba,bb,deep);
            Vector2 a2={x,ya+22}, b2={xb,yb+22};
            DrawTriangle(a,a2,b,gc); DrawTriangle(b,a2,b2,gc);
            DrawLineEx(a,b,3.0f,ColorLerp(gc,BLACK,0.35f));
        }
        ya=yb;
    }
}

/* level-specific decoration */
static void draw_deco(void){
    Color dc=LV[g_lv].deco;
    float wl,wr,wb; view_range(&wl,&wr,&wb);
    int i0=(int)(wl/SEG)-2, i1=(int)(wr/SEG)+2;
    for(int i=i0;i<=i1;i++){
        if(i<T_b+1||i>=T_n-2||i<14) continue;
        if(i%3!=0) continue;
        float h=hash_f(i,LV[g_lv].seed);
        float px=(float)i*SEG+(h-0.5f)*40.0f;
        Vector2 p={px,surf_y(px)+4};
        float s=0.75f+h*0.6f;
        switch(g_lv){
        case 0: /* tree */
            DrawRectangle((int)p.x-4,(int)(p.y-35*s),8,(int)(35*s),RGB(80,45,15));
            DrawTriangle((Vector2){p.x,p.y-70*s},(Vector2){p.x-16*s,p.y-35*s},(Vector2){p.x+16*s,p.y-35*s},dc);
            DrawTriangle((Vector2){p.x,p.y-90*s},(Vector2){p.x-11*s,p.y-62*s},(Vector2){p.x+11*s,p.y-62*s},dc);
            break;
        case 1: /* cactus */
            DrawRectangle((int)p.x-5,(int)(p.y-45*s),10,(int)(45*s),dc);
            DrawRectangle((int)p.x-18,(int)(p.y-35*s),13,8,dc);
            DrawRectangle((int)p.x+5,(int)(p.y-28*s),13,8,dc);
            break;
        case 2: /* ice spike */
            DrawTriangle((Vector2){p.x,p.y-38*s},(Vector2){p.x-7,p.y},(Vector2){p.x+7,p.y},dc);
            DrawTriangle((Vector2){p.x+9,p.y-22*s},(Vector2){p.x+4,p.y},(Vector2){p.x+14,p.y},dc);
            break;
        case 3: /* lava rock with glow */
            DrawCircle((int)p.x,(int)p.y-6,(int)(9*s),RGB(40,20,15));
            DrawCircle((int)p.x,(int)p.y-6,(int)(5*s),RGBA(255,90,20,(unsigned char)(140+100*sinf(g_time*3+i))));
            break;
        case 4: /* crater rim */
            DrawEllipse((int)p.x,(int)p.y-3,(int)(18*s),6,dc);
            DrawEllipse((int)p.x,(int)p.y-2,(int)(11*s),3,RGB(70,70,80));
            break;
        case 5: /* glowing crystals */
            DrawTriangle((Vector2){p.x,p.y-40*s},(Vector2){p.x-8,p.y},(Vector2){p.x+8,p.y},dc);
            DrawTriangle((Vector2){p.x-10,p.y-24*s},(Vector2){p.x-16,p.y},(Vector2){p.x-5,p.y},ColorLerp(dc,WHITE,0.4f));
            DrawCircleV((Vector2){p.x,p.y-20*s},14*s,RGBA(160,200,255,(unsigned char)(30+20*sinf(g_time*2+i))));
            break;
        }
    }
}

static void draw_wheel(const Wheel *w){
    Vector2 p=w->pos; float r=w->r;
    DrawEllipse((int)p.x+3,(int)p.y+5,(int)r,(int)(r*0.5f),AC(RGBA(0,0,0,50)));
    DrawCircleV(p,r,AC(RGB(22,22,22)));
    for(int i=0;i<10;i++){
        float a=(w->spin+i*36.0f)*D2R;
        DrawCircleV((Vector2){p.x+cosf(a)*(r-4),p.y+sinf(a)*(r-4)},r*0.1f,AC(RGB(45,45,45)));
    }
    DrawCircleV(p,r*0.54f,AC(RGB(200,200,200)));
    DrawCircleV(p,r*0.17f,AC(RGB(35,35,35)));
    for(int i=0;i<5;i++){
        float a=(w->spin*0.5f+i*72.0f)*D2R;
        Vector2 sp={p.x+cosf(a)*r*0.52f,p.y+sinf(a)*r*0.52f};
        DrawLineEx(p,sp,3.5f,AC(RGB(175,175,175)));
    }
}

static void draw_car(const Car *c, Paint pt){
    float rad=c->angle*D2R;
    float mx=c->rear.ax/c->hw;    /* arm mount as fraction of half width */

    /* ── suspension arms ─*/
    Vector2 mnt_r=car_pt(c, mx,+1.0f);
    Vector2 mnt_f=car_pt(c,-mx,+1.0f);
    float arm_off=5.5f;
    Vector2 perp={-sinf(rad)*arm_off,cosf(rad)*arm_off};
    DrawLineEx((Vector2){mnt_r.x+perp.x,mnt_r.y+perp.y},c->rear.pos,7,AC(RGB(35,35,35)));
    DrawLineEx((Vector2){mnt_r.x-perp.x,mnt_r.y-perp.y},c->rear.pos,7,AC(RGB(35,35,35)));
    DrawLineEx((Vector2){mnt_f.x+perp.x,mnt_f.y+perp.y},c->fwd.pos, 7,AC(RGB(35,35,35)));
    DrawLineEx((Vector2){mnt_f.x-perp.x,mnt_f.y-perp.y},c->fwd.pos, 7,AC(RGB(35,35,35)));
    DrawLineEx(mnt_r,c->rear.pos,3,AC(RGB(70,70,70)));
    DrawLineEx(mnt_f,c->fwd.pos, 3,AC(RGB(70,70,70)));
    DrawCircleV(mnt_r,7,AC(RGB(50,50,50))); DrawCircleV(mnt_r,3,AC(RGB(155,155,155)));
    DrawCircleV(mnt_f,7,AC(RGB(50,50,50))); DrawCircleV(mnt_f,3,AC(RGB(155,155,155)));

    /* ── chassis */
    DrawRectanglePro((Rectangle){c->pos.x,c->pos.y,c->hw*2,c->hh*2},
                     (Vector2){c->hw,c->hh},c->angle,AC(pt.body));
    Vector2 st0=car_pt(c,-1.0f,+0.30f), st1=car_pt(c,+1.0f,+0.30f);
    Vector2 st2=car_pt(c,-1.0f,+0.80f), st3=car_pt(c,+1.0f,+0.80f);
    Color sc=pt.stripe; sc.a=220;
    DrawTriangle(st0,st2,st1,AC(sc));
    DrawTriangle(st1,st2,st3,AC(sc));
    DrawRectanglePro((Rectangle){c->pos.x,c->pos.y,c->hw*2,c->hh*2},
                     (Vector2){c->hw,c->hh},c->angle,AC(RGBA(0,0,0,55)));

    /* ── cabin */
    Vector2 cab_br_base=car_pt(c,-0.55f,-1.0f);
    Vector2 cab_fr_base=car_pt(c,+0.45f,-1.0f);
    Vector2 cab_br_top =car_pt(c,-0.42f,-2.80f);
    Vector2 cab_fr_top =car_pt(c,+0.30f,-2.80f);
    DrawTriangle(cab_br_base,cab_br_top,cab_fr_base,AC(pt.cabin));
    DrawTriangle(cab_fr_base,cab_br_top,cab_fr_top, AC(pt.cabin));
    DrawLineEx(cab_br_base,cab_br_top, 2.5f,AC(RGBA(0,0,0,170)));
    DrawLineEx(cab_br_top, cab_fr_top, 2.5f,AC(RGBA(0,0,0,170)));
    DrawLineEx(cab_fr_top, cab_fr_base,2.5f,AC(RGBA(0,0,0,170)));
    DrawLineEx(cab_fr_base,cab_br_base,2.0f,AC(RGBA(0,0,0,100)));

    /* ── windscreen */
    Vector2 ws_bl=cab_fr_base, ws_br=car_pt(c,+0.95f,-1.0f);
    Vector2 ws_tl=cab_fr_top,  ws_tr=car_pt(c,+0.80f,-2.70f);
    DrawTriangle(ws_bl,ws_tl,ws_br,AC(RGBA(120,200,255,210)));
    DrawTriangle(ws_br,ws_tl,ws_tr,AC(RGBA(120,200,255,210)));
    DrawLineEx(ws_bl,ws_tl,3.0f,AC(RGBA(0,0,0,180)));
    DrawLineEx(ws_tl,ws_tr,3.0f,AC(RGBA(0,0,0,180)));
    DrawLineEx(ws_tr,ws_br,3.0f,AC(RGBA(0,0,0,180)));
    DrawCircleV(car_pt(c,+0.62f,-1.90f),4,AC(RGBA(255,255,255,160)));
    DrawCircleV(car_pt(c,+0.52f,-2.20f),2,AC(RGBA(255,255,255,120)));

    /* ── rear window */
    Vector2 rw_bl=car_pt(c,-0.96f,-1.0f), rw_br=cab_br_base;
    Vector2 rw_tl=car_pt(c,-0.78f,-2.68f), rw_tr=cab_br_top;
    DrawTriangle(rw_bl,rw_tl,rw_br,AC(RGBA(90,165,215,180)));
    DrawTriangle(rw_br,rw_tl,rw_tr,AC(RGBA(90,165,215,180)));
    DrawLineEx(rw_bl,rw_tl,3.0f,AC(RGBA(0,0,0,170)));
    DrawLineEx(rw_tl,rw_tr,3.0f,AC(RGBA(0,0,0,170)));
    DrawLineEx(rw_tr,rw_br,3.0f,AC(RGBA(0,0,0,170)));

    /* ── driver */
    Vector2 hd=car_pt(c,-0.05f,-1.65f);
    DrawCircleV(hd,13,AC(RGB(255,210,170)));
    Vector2 helm=car_pt(c,-0.05f,-2.10f);
    DrawCircleV(helm,14,AC(pt.helmet));
    DrawLineEx(car_pt(c,-0.18f,-1.92f),car_pt(c,+0.08f,-1.92f),6,AC(RGBA(50,150,255,200)));

    /* ── headlight */
    Vector2 hl=car_pt(c,+1.01f,-0.18f);
    DrawCircleV(hl,11,AC(RGB(255,255,175)));
    DrawCircleV(hl, 7,AC(RGB(255,255,255)));
    DrawCircleV(hl, 3,AC(RGB(255,235,80)));

    /* ── exhaust + nitro flame */
    Vector2 ex0=car_pt(c,-1.01f,-0.38f);
    Vector2 ex1={ex0.x-cosf(rad)*26,ex0.y-sinf(rad)*26};
    DrawLineEx(ex0,ex1,10,AC(RGB(45,45,45)));
    DrawLineEx(ex0,ex1, 6,AC(RGB(72,72,72)));
    DrawCircleV(ex1,5,AC(RGB(38,38,38)));
    if(c->boosting){
        float fl=34.0f+sinf(g_time*60.0f)*8.0f;
        Vector2 tip={ex1.x-cosf(rad)*fl,ex1.y-sinf(rad)*fl};
        DrawLineEx(ex1,tip,11,AC(RGBA(255,120,30,200)));
        DrawLineEx(ex1,(Vector2){(ex1.x+tip.x)/2,(ex1.y+tip.y)/2},6,AC(RGBA(255,245,170,240)));
    }

    /* ── fuel can on roof */
    Vector2 fc=car_pt(c,-0.58f,-2.55f);
    DrawRectanglePro((Rectangle){fc.x,fc.y,28,19},(Vector2){14,19},c->angle,AC(RGB(210,58,28)));
    DrawRectanglePro((Rectangle){fc.x,fc.y,14,19},(Vector2){7,19},c->angle,AC(RGB(238,75,38)));
    DrawLineEx(car_pt(c,-0.72f,-2.28f),car_pt(c,-0.40f,-2.28f),4,AC(RGB(68,38,12)));

    /* ── bumper + plate */
    Vector2 bp0=car_pt(c,+1.02f,+0.60f), bp1=car_pt(c,+1.02f,-0.68f);
    DrawLineEx(bp0,bp1,9,AC(RGB(42,42,42)));
    DrawLineEx(bp0,bp1,5,AC(RGB(165,165,165)));
    Vector2 plt=car_pt(c,+0.60f,+0.55f);
    DrawRectanglePro((Rectangle){plt.x,plt.y,30,15},(Vector2){15,7},c->angle,AC(RGB(248,248,248)));
    DrawRectanglePro((Rectangle){plt.x,plt.y,30,15},(Vector2){15,7},c->angle,AC(RGBA(0,0,0,40)));
}

static void draw_vehicle(const Car *c, Paint p, float alpha){
    g_alpha=alpha;
    draw_wheel(&c->rear);
    draw_wheel(&c->fwd);
    draw_car(c,p);
    g_alpha=1.0f;
}

static void draw_ghost(void){
    GSample s;
    if(!g_save.ghost || !ghost_at(g_car.time,&s)) return;
    const Vehicle *v=&VEH[g_ghost_veh];
    Car t; memset(&t,0,sizeof t);
    t.hw=v->hw; t.hh=v->hh; t.pos=(Vector2){s.x,s.y}; t.angle=s.a;
    t.rear.r=v->rr; t.rear.ax=-v->ax; t.fwd.r=v->fr; t.fwd.ax=v->ax;
    Vector2 mr=car_pt(&t,-v->ax/v->hw,1.0f), mf=car_pt(&t,v->ax/v->hw,1.0f);
    t.rear.pos=(Vector2){mr.x,mr.y+v->susp_len}; t.fwd.pos=(Vector2){mf.x,mf.y+v->susp_len};
    t.rear.spin=s.x/v->rr*R2D; t.fwd.spin=s.x/v->fr*R2D;
    draw_vehicle(&t,GHOST_PAINT,0.30f);
    Vector2 tag=car_pt(&t,0,-3.5f);
    DrawText("GHOST",(int)tag.x-MeasureText("GHOST",16)/2,(int)tag.y,16,RGBA(255,255,255,140));
}

static void draw_bridges(void){
    for(int i=0;i<MAX_BRIDGES;i++){
        Bridge *b=&g_bridges[i];
        if(!b->active) continue;
        Vector2 sc =GetWorldToScreen2D((Vector2){b->x0,b->y},g_cam);
        Vector2 sc2=GetWorldToScreen2D((Vector2){b->x1,b->y},g_cam);
        if(sc2.x<-20||sc.x>SW+20) continue;

        float x0=b->x0, x1=b->x1, y=b->y, span=x1-x0;
        float ph=70.0f;
        Color stone=RGB(115,104,92), stone_dark=RGB(82,74,65);
        DrawRectangle((int)x0,   (int)y,   22,(int)ph,stone);
        DrawRectangle((int)x0,   (int)y,   11,(int)ph,stone_dark);
        DrawRectangle((int)x0-9, (int)y-12,40,13,stone);
        DrawRectangle((int)x1-22,(int)y,   22,(int)ph,stone);
        DrawRectangle((int)x1-22,(int)y,   11,(int)ph,stone_dark);
        DrawRectangle((int)x1-31,(int)y-12,40,13,stone);

        int np=(int)(span/20)+1;
        float pw=span/np;
        for(int p=0;p<np;p++){
            float px=x0+p*pw;
            Color plk=(p%2==0)?RGB(145,92,42):RGB(118,74,30);
            DrawRectangle((int)px,(int)(y-12),(int)pw-2,14,plk);
            DrawRectangle((int)px,(int)(y-12),2,14,RGB(78,48,18));
        }
        DrawLineEx((Vector2){x0,y-12},(Vector2){x1,y-12},3,RGB(98,62,24));

        Color cable=RGB(55,55,55);
        float sag=22.0f, top_y=y-ph;
        int segs=14;
        for(int s=0;s<segs;s++){
            float t0=(float)s/segs, t1=(float)(s+1)/segs;
            DrawLineEx((Vector2){x0+t0*span,top_y+sag*(4*t0*(1-t0))},
                       (Vector2){x0+t1*span,top_y+sag*(4*t1*(1-t1))},3,cable);
        }
        for(int h=1;h<=5;h++){
            float t=(float)h/6.0f, hx=x0+t*span;
            DrawLineEx((Vector2){hx,top_y+sag*(4*t*(1-t))},(Vector2){hx,y-12},2,cable);
        }
        DrawCircle((int)x0+11,(int)y-12,5,stone);
        DrawCircle((int)x1-11,(int)y-12,5,stone);
    }
}

static void draw_coins(void){
    for(int i=0;i<MAX_COINS;i++){
        Coin *k=&g_coins[i];
        if(!k->active) continue;
        float bob=sinf(g_time*3+k->ph)*5;
        Vector2 p={k->pos.x,k->pos.y+bob};
        bool big=k->value>1;
        float R=big?20.0f:17.0f;
        Color outer=big?RGB(255,150,30):RGB(255,205,0);
        Color inner=big?RGB(225,110,10):RGB(220,170,0);
        DrawEllipse((int)p.x+2,(int)p.y+18,13,5,RGBA(0,0,0,50));
        DrawCircleV(p,R+3,big?RGBA(255,140,0,60):RGBA(255,230,0,45));
        DrawCircleV(p,R,outer);
        DrawCircleV(p,R-4,inner);
        for(int j=0;j<4;j++){
            float a=(g_time*120+j*90)*D2R;
            Vector2 in={p.x+cosf(a)*4,p.y+sinf(a)*4};
            Vector2 out={p.x+cosf(a+PI/4)*9,p.y+sinf(a+PI/4)*9};
            DrawLineEx(in,out,2,RGBA(255,255,180,180));
        }
        DrawText(big?"5":"$",(int)p.x-5,(int)p.y-9,15,RGBA(255,248,120,230));
    }
}

static void draw_canisters(void){
    for(int i=0;i<MAX_CANISTERS;i++){
        Canister *k=&g_cans[i];
        if(!k->active) continue;
        float bob=sinf(g_time*2.2f+k->ph)*5;
        Vector2 p={k->pos.x,k->pos.y+bob};
        float glow=0.5f+0.5f*sinf(g_time*3+k->ph);
        DrawCircleV(p,26+glow*6,RGBA(80,230,80,(unsigned char)(40+glow*40)));
        DrawEllipse((int)p.x+2,(int)p.y+24,14,5,RGBA(0,0,0,50));
        DrawRectangle((int)p.x-12,(int)p.y-20,24,38,RGB(40,180,40));
        DrawRectangle((int)p.x-12,(int)p.y-20,11,38,RGB(55,205,55));
        DrawRectangle((int)p.x-12,(int)p.y-4,24,12,RGBA(255,255,255,200));
        DrawText("GAS",(int)p.x-10,(int)p.y-3,10,RGB(20,120,20));
        DrawRectangle((int)p.x-6,(int)p.y-26,12,8,RGB(30,130,30));
        DrawRectangle((int)p.x-3,(int)p.y-30,6,6,RGB(25,110,25));
        DrawRectangleLines((int)p.x-12,(int)p.y-20,24,38,RGBA(0,0,0,120));
    }
}

static void draw_finish(void){
    float goal=LV[g_lv].goal_m;
    if(goal<=0) return;
    float x=goal*100.0f, y=surf_y(x);
    if(y>SH*3.0f) return;
    DrawRectangle((int)x-3,(int)y-170,6,170,RGB(230,230,230));
    for(int r=0;r<4;r++) for(int q=0;q<6;q++){
        float wave=sinf(g_time*4+q*0.6f)*4;
        DrawRectangle((int)x+3+q*12,(int)(y-170+r*12+wave),12,12,((r+q)%2)?BLACK:WHITE);
    }
}

static void draw_prediction(void){
    if(!g_save.hints || g_npred<2) return;
    for(int i=1;i<g_npred;i+=2)
        DrawCircleV(g_pred[i],3.0f,RGBA(255,255,255,(unsigned char)(160-120*(float)i/g_npred)));
    /* target landing attitude */
    float a=slope_deg(g_pred_land.x)*D2R;
    Vector2 d={cosf(a)*70,sinf(a)*70};
    Vector2 l=g_pred_land;
    DrawLineEx((Vector2){l.x-d.x,l.y-d.y-6},(Vector2){l.x+d.x,l.y+d.y-6},4,RGBA(120,255,140,200));
    DrawCircleLinesV(l,14,RGBA(120,255,140,220));
}

/*============================================================================
  HUD
============================================================================*/
static void hud_panel(Rectangle r, Color bg){
    DrawRectangleRounded(r,0.30f,8,bg);
    DrawRectangleRoundedLines(r,0.30f,8,RGBA(255,255,255,30));
}

static void draw_speedo(float spd_kmh){
    int cx=SW-80, cy=SH-80, R=62;
    float max_spd=200.0f;
    DrawCircle(cx,cy,R+4,RGBA(0,0,0,180));
    DrawCircle(cx,cy,R,  RGBA(20,20,35,230));
    DrawRing((Vector2){(float)cx,(float)cy},R-6,R-2,-210.0f+240.0f*0.75f,30.0f,24,RGBA(220,60,60,160));
    for(int i=0;i<=16;i++){
        float a=((float)i/16.0f)*240.0f-210.0f;
        float ar=a*D2R, cr2=cosf(ar), sr2=sinf(ar);
        int len=(i%4==0)?10:5;
        Color tc=(i%4==0)?WHITE:RGBA(160,160,160,200);
        DrawLine(cx+(int)(cr2*(R-len-2)),cy+(int)(sr2*(R-len-2)),
                 cx+(int)(cr2*(R-2)),    cy+(int)(sr2*(R-2)),tc);
        if(i%4==0){
            char lb[8]; snprintf(lb,sizeof(lb),"%d",(int)((float)i/16.0f*max_spd));
            DrawText(lb,cx+(int)(cr2*(R-18))-MeasureText(lb,10)/2,cy+(int)(sr2*(R-18))-5,10,RGBA(200,200,200,200));
        }
    }
    float nr=(-210.0f+Clamp(spd_kmh/max_spd,0,1)*240.0f)*D2R;
    DrawLineEx((Vector2){(float)cx,(float)cy},(Vector2){cx+cosf(nr)*(R-8),cy+sinf(nr)*(R-8)},3,RGB(255,60,60));
    DrawCircle(cx,cy,6,RGB(200,200,200));
    DrawCircle(cx,cy,3,RGB(100,100,100));
    char sp[16]; snprintf(sp,sizeof(sp),"%.0f",spd_kmh);
    DrawText(sp,cx-MeasureText(sp,18)/2,cy+18,18,WHITE);
    DrawText("km/h",cx-MeasureText("km/h",10)/2,cy+38,10,RGBA(180,180,180,200));
}

/* elevation profile of the next ~80 m with pickups, rival & ghost */
static void draw_profile(void){
    Rectangle r={14,SH-122,400,92};
    hud_panel(r,RGBA(0,0,0,150));
    float x0=g_car.pos.x-2000.0f, x1=g_car.pos.x+8000.0f;
    const int N=100;
    float ys[101], lo=1e9f, hi=-1e9f;
    for(int i=0;i<=N;i++){
        ys[i]=surf_y(x0+(x1-x0)*i/N);
        if(ys[i]<SH*3.0f){ lo=fminf(lo,ys[i]); hi=fmaxf(hi,ys[i]); }
    }
    if(hi<lo) return;
    float span=fmaxf(hi-lo,200.0f);
    float ix=r.x+8, iw=r.width-16, iy=r.y+18, ih=r.height-26;
    #define PX(wx) (ix+((wx)-x0)/(x1-x0)*iw)
    #define PY(wy) (iy+((wy)-lo)/span*ih)
    Color gc=LV[g_lv].gnd; gc.a=200;
    for(int i=0;i<N;i++){
        if(ys[i]>SH*3.0f) continue;
        float sx=ix+iw*i/N, y=PY(ys[i]);
        DrawRectangle((int)sx,(int)y,(int)(iw/N)+1,(int)(iy+ih-y)+1,gc);
    }
    for(int i=0;i<MAX_CANISTERS;i++) if(g_cans[i].active && g_cans[i].pos.x>x0 && g_cans[i].pos.x<x1)
        DrawCircleV((Vector2){PX(g_cans[i].pos.x),PY(surf_y(g_cans[i].pos.x))-5},4,RGB(80,240,80));
    float goal=LV[g_lv].goal_m*100.0f;
    if(goal>x0 && goal<x1) DrawLineEx((Vector2){PX(goal),iy},(Vector2){PX(goal),iy+ih},2,WHITE);
    GSample gs;
    if(g_save.ghost && ghost_at(g_car.time,&gs) && gs.x>x0 && gs.x<x1)
        DrawCircleV((Vector2){PX(gs.x),PY(surf_y(gs.x))-6},4,RGBA(255,255,255,170));
    if(g_rival_on && g_rival.pos.x>x0 && g_rival.pos.x<x1)
        DrawCircleV((Vector2){PX(g_rival.pos.x),PY(surf_y(g_rival.pos.x))-6},5,RGB(70,130,255));
    DrawCircleV((Vector2){PX(g_car.pos.x),PY(surf_y(g_car.pos.x))-6},5,RGB(255,60,60));
    #undef PX
    #undef PY
    DrawText("TERRAIN AHEAD",(int)r.x+10,(int)r.y+4,10,RGBA(180,180,180,200));
    float nc=next_can_dist(g_car.pos.x);
    if(nc<1e8f){
        char b[32]; snprintf(b,sizeof b,"next fuel %.0f m",nc);
        DrawText(b,(int)(r.x+r.width-MeasureText(b,10)-10),(int)r.y+4,10,RGB(80,230,80));
    }
}

static void draw_popups(void){
    int idx[MAX_POP], n=0;
    for(int i=0;i<MAX_POP;i++) if(g_pop[i].t>0) idx[n++]=i;
    for(int a=1;a<n;a++){ int k=idx[a],b=a-1;
        while(b>=0 && g_pop[idx[b]].serial>g_pop[k].serial){ idx[b+1]=idx[b]; b--; }
        idx[b+1]=k; }
    float y=SH*0.22f;
    for(int j=0;j<n;j++){
        Popup *p=&g_pop[idx[j]];
        float age=1.8f-p->t;
        float a=Clamp(p->t/0.4f,0,1);
        float sc=age<0.12f?0.6f+age/0.12f*0.4f:1.0f;
        int fs=(int)(p->size*sc);
        int tw=MeasureText(p->txt,fs);
        Color col=p->col; col.a=(unsigned char)(255*a);
        DrawText(p->txt,SW/2-tw/2+2,(int)y+2,fs,RGBA(0,0,0,(unsigned char)(140*a)));
        DrawText(p->txt,SW/2-tw/2,(int)y,fs,col);
        y+=p->size+8;
    }
}

static void draw_toast(void){
    if(g_toast_t<=0) return;
    float a=Clamp(g_toast_t/0.5f,0,1);
    int fs=20, tw=MeasureText(g_toast,fs);
    Rectangle r={(float)(SW/2-tw/2-20),(float)(SH-170),(float)(tw+40),40};
    DrawRectangleRounded(r,0.4f,8,RGBA(40,30,0,(unsigned char)(220*a)));
    DrawRectangleRoundedLines(r,0.4f,8,RGBA(255,210,60,(unsigned char)(220*a)));
    DrawText(g_toast,(int)r.x+20,(int)r.y+10,fs,RGBA(255,220,80,(unsigned char)(255*a)));
}

static void draw_hud(void){
    Car *c=&g_car;
    Level *lv=&LV[g_lv];
    Color bg=RGBA(0,0,0,165);
    bool endless=lv->goal_m<=0;

    /*── top-left: stage + distance + time ─*/
    hud_panel((Rectangle){10,10,320,118},bg);
    DrawText(lv->name,22,16,18,RGBA(255,220,60,235));
    char tb[32]; snprintf(tb,sizeof tb,"%d:%05.2f",(int)(c->time/60),fmodf(c->time,60));
    DrawText(tb,318-MeasureText(tb,18),16,18,RGBA(220,220,220,230));
    DrawText("DISTANCE",22,40,12,RGBA(150,150,150,200));
    char db[64];
    if(endless && g_save.best_dist[g_lv]>0)
        snprintf(db,sizeof(db),"%.0f m   (best %.0f m)",c->dist,g_save.best_dist[g_lv]);
    else if(endless) snprintf(db,sizeof(db),"%.0f m",c->dist);
    else        snprintf(db,sizeof(db),"%.0f m / %.0f m",c->dist,lv->goal_m);
    DrawText(db,22,54,20,WHITE);
    float scale=endless?fmaxf(g_save.best_dist[g_lv],fmaxf(c->dist,100.0f))*1.1f:lv->goal_m;
    float prog=Clamp(c->dist/scale,0,1);
    DrawRectangle(22,84,284,10,RGBA(45,45,45,200));
    DrawRectangle(22,84,(int)(284*prog),10,RGB(70,210,90));
    DrawRectangleLines(22,84,284,10,RGBA(255,255,255,50));
    GSample gs;
    if(g_save.ghost && ghost_at(c->time,&gs)){
        float gp=Clamp(gs.x/100.0f/scale,0,1);
        DrawRectangle(22+(int)(284*gp)-1,80,3,18,RGBA(255,255,255,200));
    }
    if(g_rival_on){
        float rp=Clamp(g_rival.dist/scale,0,1);
        DrawRectangle(22+(int)(284*rp)-2,80,4,18,RGB(70,130,255));
    }
    /* rival / ghost deltas */
    int ly=102;
    if(g_rival_on){
        float d=(g_rival.pos.x-c->pos.x)/100.0f;
        char rb[64];
        if(g_rival_done) snprintf(rb,sizeof rb,"RIVAL finished in %.1fs",g_rival_time);
        else if(d>0)     snprintf(rb,sizeof rb,"RIVAL %.0f m ahead",d);
        else             snprintf(rb,sizeof rb,"RIVAL %.0f m behind",-d);
        DrawText(rb,22,ly,13,d>0||g_rival_done?RGB(255,120,120):RGB(120,255,140));
    }
    if(g_save.ghost && ghost_at(c->time,&gs)){
        float d=(c->pos.x-gs.x)/100.0f;
        char gb[48]; snprintf(gb,sizeof gb,"GHOST %+.0f m",d);
        DrawText(gb,318-MeasureText(gb,13),ly,13,d>=0?RGB(120,255,140):RGB(255,170,170));
    }

    /*── top-right: fuel + nitro ──*/
    hud_panel((Rectangle){SW-185,10,172,130},bg);
    DrawText("FUEL",SW-173,16,13,RGBA(150,150,150,200));
    float fr=c->fuel/c->sp.fuel_cap;
    Color fc=fr>0.3f?RGB(70,215,70):RGB(220,50,50);
    DrawRectangle(SW-173,34,148,40,RGBA(38,38,38,200));
    DrawRectangle(SW-173,34,(int)(148*fr),40,fc);
    DrawRectangleLines(SW-173,34,148,40,RGBA(255,255,255,50));
    char fb[16]; snprintf(fb,sizeof(fb),"%d%%",(int)(fr*100));
    DrawText(fb,SW-99-MeasureText(fb,18)/2,45,18,WHITE);
    if(fr<0.25f && (int)(g_time*2)%2==0) DrawText("LOW FUEL",SW-125,16,13,RGB(255,60,60));
    DrawText("NITRO  [SHIFT]",SW-173,82,12,RGBA(150,150,150,200));
    float nr=c->nitro/c->sp.nitro_cap;
    DrawRectangle(SW-173,98,148,14,RGBA(38,38,38,200));
    DrawRectangle(SW-173,98,(int)(148*nr),14,c->boosting?RGB(255,240,120):RGB(60,170,255));
    DrawRectangleLines(SW-173,98,148,14,RGBA(255,255,255,50));
    if(g_coach.rate>0){
        char rb[40]; snprintf(rb,sizeof rb,"range ~%.0f m",c->fuel/g_coach.rate);
        DrawText(rb,SW-173,118,12,RGBA(190,190,190,220));
    }

    /* top-centre: coins + combo */
    hud_panel((Rectangle){SW/2-90,10,180,50},bg);
    DrawCircle(SW/2-62,35,15,RGB(255,210,0));
    DrawCircle(SW/2-62,35,10,RGB(215,165,0));
    DrawText("$",SW/2-67,26,14,RGBA(255,248,120,230));
    char cb[16]; snprintf(cb,sizeof(cb),"x %d",c->coins);
    DrawText(cb,SW/2-40,22,26,RGB(255,218,55));
    if(g_combo>1 && g_time-g_last_stunt<6.0f){
        hud_panel((Rectangle){SW/2-80,66,160,30},RGBA(200,115,0,175));
        char cm[32]; snprintf(cm,sizeof cm,"COMBO x%d",g_combo);
        DrawText(cm,SW/2-MeasureText(cm,18)/2,72,18,WHITE);
        float left=1.0f-(g_time-g_last_stunt)/6.0f;
        DrawRectangle(SW/2-70,92,(int)(140*left),3,RGB(255,230,120));
    } else if(c->flips>0){
        char fl[32]; snprintf(fl,sizeof(fl),"%d FLIP%s",c->flips,c->flips!=1?"S":"");
        DrawText(fl,SW/2-MeasureText(fl,16)/2,66,16,RGBA(255,190,90,220));
    }

    if(g_autopilot){
        Rectangle ap={SW/2-110,104,220,30};
        hud_panel(ap,RGBA(30,80,200,200));
        const char *t=(int)(g_time*2)%2?"AUTOPILOT  [TAB]":"AI DRIVING  [TAB]";
        DrawText(t,SW/2-MeasureText(t,16)/2,111,16,WHITE);
    }

    draw_speedo(fabsf(c->vel.x)*0.18f);
    draw_profile();

    /* off-screen rival indicator */
    if(g_rival_on){
        Vector2 s=GetWorldToScreen2D(g_rival.pos,g_cam);
        float d=(g_rival.pos.x-c->pos.x)/100.0f;
        char b[24]; snprintf(b,sizeof b,"%.0f m",fabsf(d));
        if(s.x>SW-10){
            float y=Clamp(s.y,160,SH-200);
            DrawTriangle((Vector2){SW-8,y},(Vector2){SW-30,y-14},(Vector2){SW-30,y+14},RGB(70,130,255));
            DrawText(b,SW-36-MeasureText(b,14),(int)y-7,14,RGB(150,190,255));
        } else if(s.x<10){
            float y=Clamp(s.y,160,SH-200);
            DrawTriangle((Vector2){8,y},(Vector2){30,y+14},(Vector2){30,y-14},RGB(70,130,255));
            DrawText(b,36,(int)y-7,14,RGB(150,190,255));
        }
    }

    /* AI coach line */
    if(g_save.hints && g_coach.msg[0]){
        int fs=18, tw=MeasureText(g_coach.msg,fs);
        Rectangle r={(float)(SW/2-tw/2-16),(float)(SH-62),(float)(tw+32),34};
        hud_panel(r,RGBA(0,0,0,170));
        DrawText("COACH",(int)r.x+10,(int)r.y-14,11,RGBA(180,180,180,200));
        DrawText(g_coach.msg,(int)r.x+16,(int)r.y+8,fs,g_coach.col);
    }

    DrawText("D/RIGHT gas  A/LEFT brake  SHIFT nitro  TAB autopilot  H hints  P pause",
             430,SH-22,12,RGBA(200,200,200,120));
}

/*============================================================================
  UI helpers + MENU / GARAGE
============================================================================*/
static bool ui_button(Rectangle r, const char *txt, int fs, Color base, bool enabled){
    bool hov=enabled && CheckCollisionPointRec(GetMousePosition(),r);
    Color c=enabled?(hov?ColorBrightness(base,0.25f):base):RGB(70,70,80);
    DrawRectangleRounded(r,0.35f,8,c);
    DrawRectangleRoundedLines(r,0.35f,8,RGBA(255,255,255,hov?150:60));
    int tw=MeasureText(txt,fs);
    DrawText(txt,(int)(r.x+(r.width-tw)/2),(int)(r.y+(r.height-fs)/2),fs,enabled?WHITE:RGB(150,150,150));
    bool click=hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if(click) sfx(SFX_CLICK);
    return click;
}

static void draw_menu_bg(void){
    DrawRectangleGradientV(0,0,SW,SH,RGB(8,15,55),RGB(40,80,160));
    for(int i=0;i<80;i++){
        float sx2=(float)((i*137+11)%SW);
        float sy2=(float)((i*251+17)%(SH/2));
        float tw=0.5f+0.5f*sinf(g_menu_t*2+i*0.7f);
        unsigned char br=(unsigned char)(180+70*tw);
        DrawCircle((int)sx2,(int)sy2,i%3==0?2:1,RGBA(br,br,br,255));
    }
    for(int x=0;x<SW;x+=2){
        float h=SH*0.70f+sinf(x*0.008f+g_menu_t*0.4f)*50+sinf(x*0.003f-g_menu_t*0.2f)*80;
        DrawLine(x,(int)h,x,SH,RGB(20,60,20));
    }
}

static void draw_bank(void){
    char b[32]; snprintf(b,sizeof b,"%d",g_save.bank);
    int w=MeasureText(b,26)+70;
    Rectangle r={(float)(SW-w-20),18,(float)w,46};
    hud_panel(r,RGBA(0,0,0,170));
    DrawCircle((int)r.x+24,(int)r.y+23,14,RGB(255,210,0));
    DrawCircle((int)r.x+24,(int)r.y+23,9,RGB(215,165,0));
    DrawText(b,(int)r.x+50,(int)r.y+11,26,RGB(255,218,55));
}

static void draw_vehicle_preview(int veh, Rectangle area){
    const Vehicle *v=&VEH[veh];
    Spec sp=make_spec(veh,g_save.up);
    Car t; memset(&t,0,sizeof t);
    t.veh=veh; t.sp=sp; t.hw=v->hw; t.hh=v->hh;
    float bounce=sinf(g_menu_t*3)*4;
    t.pos=(Vector2){0,bounce};
    t.rear=(Wheel){.r=v->rr,.ax=-v->ax}; t.fwd=(Wheel){.r=v->fr,.ax=v->ax};
    t.rear.pos=(Vector2){-v->ax,v->hh+sp.susp_len*0.85f};
    t.fwd.pos =(Vector2){ v->ax,v->hh+sp.susp_len*0.85f};
    t.rear.spin=t.fwd.spin=g_menu_t*200.0f;
    Camera2D cam={.offset={area.x+area.width/2,area.y+area.height*0.52f},.target={0,20},.zoom=0.55f};
    BeginMode2D(cam);
    draw_vehicle(&t,veh_paint(veh),1.0f);
    EndMode2D();
}

static void stat_bar(int x, int y, const char *name, float v){
    DrawText(name,x,y,13,RGBA(200,200,220,220));
    DrawRectangle(x+110,y+2,170,10,RGBA(40,40,60,220));
    DrawRectangle(x+110,y+2,(int)(170*Clamp(v,0,1)),10,RGB(90,200,255));
}

static void draw_menu(void){
    draw_menu_bg();

    const char *title="HILL CLIMB RACER";
    int tw=MeasureText(title,64);
    float glow=0.7f+0.3f*sinf(g_menu_t*2);
    DrawText(title,SW/2-tw/2+3,40+3,64,RGBA(0,0,0,130));
    DrawText(title,SW/2-tw/2,40,64,RGBA(255,(unsigned char)(200+55*glow),30,255));
    const char *sub="AI RIVALS  -  GHOSTS  -  UPGRADES  -  ENDLESS MODE";
    DrawText(sub,SW/2-MeasureText(sub,16)/2,110,16,RGBA(200,200,255,200));
    draw_bank();

    /* ---- stage cards ---- */
    DrawText("SELECT STAGE",70,140,16,RGBA(200,200,220,220));
    for(int i=0;i<MAX_LEVELS;i++){
        Rectangle r={70.0f+i*212.0f,162,200,118};
        bool un=stage_unlocked(i);
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        bool sel=g_lv==i;
        Color bc=sel?RGB(255,195,30):hov&&un?RGB(90,120,210):RGB(45,60,145);
        DrawRectangleRounded(r,0.18f,8,bc);
        DrawRectangleGradientV((int)r.x+8,(int)r.y+8,(int)r.width-16,40,LV[i].sky0,LV[i].sky1);
        DrawRectangle((int)r.x+8,(int)r.y+38,(int)r.width-16,10,LV[i].gnd);
        if(!un){
            DrawRectangleRounded(r,0.18f,8,RGBA(20,22,40,235));
            DrawText("LOCKED",(int)(r.x+r.width/2-MeasureText("LOCKED",20)/2),(int)r.y+60,20,RGB(220,220,220));
            const char *lk="finish the previous stage";
            DrawText(lk,(int)(r.x+r.width/2-MeasureText(lk,11)/2),(int)r.y+86,11,RGB(170,170,190));
            continue;
        }
        char nm[32]; snprintf(nm,sizeof nm,"%d. %s",i+1,LV[i].name);
        Color tc=sel?BLACK:WHITE;
        DrawText(nm,(int)r.x+10,(int)r.y+54,16,tc);
        char gl[32];
        if(LV[i].goal_m>0) snprintf(gl,sizeof gl,"Goal %.0f m%s",LV[i].goal_m,LV[i].grav<1?"  low-g":"");
        else snprintf(gl,sizeof gl,"No finish line");
        DrawText(gl,(int)r.x+10,(int)r.y+74,12,sel?RGBA(0,0,0,190):RGBA(210,210,230,200));
        char bs[40]="";
        if(g_save.completed[i]&&LV[i].goal_m>0&&g_save.best_time[i]>0) snprintf(bs,sizeof bs,"Best %.1fs",g_save.best_time[i]);
        else if(g_save.best_dist[i]>0) snprintf(bs,sizeof bs,"Best %.0f m",g_save.best_dist[i]);
        DrawText(bs,(int)r.x+10,(int)r.y+92,13,sel?RGBA(0,0,0,220):RGB(120,255,140));
        if(g_save.completed[i]&&LV[i].goal_m>0)
            DrawText("CLEAR",(int)(r.x+r.width-MeasureText("CLEAR",12)-10),(int)r.y+92,12,sel?RGB(20,110,30):RGB(120,255,140));
        if(hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)){ g_lv=i; sfx(SFX_CLICK); }
    }

    /* ---- vehicle panel ---- */
    Rectangle vp={70,300,620,270};
    hud_panel(vp,RGBA(0,0,0,150));
    DrawText("VEHICLE",(int)vp.x+16,(int)vp.y+10,16,RGBA(200,200,220,220));
    int v=g_view_veh;
    draw_vehicle_preview(v,(Rectangle){vp.x+10,vp.y+30,280,200});
    if(ui_button((Rectangle){vp.x+16,vp.y+222,40,36},"<",22,RGB(45,60,145),true)) g_view_veh=(v+NUM_VEH-1)%NUM_VEH;
    if(ui_button((Rectangle){vp.x+244,vp.y+222,40,36},">",22,RGB(45,60,145),true)) g_view_veh=(v+1)%NUM_VEH;
    char vn[48]; snprintf(vn,sizeof vn,"%s  (%d/%d)",VEH[v].name,v+1,NUM_VEH);
    DrawText(vn,(int)vp.x+150-MeasureText(vn,18)/2,(int)vp.y+231,18,WHITE);

    Spec sp=make_spec(v,g_save.up);
    int sx=(int)vp.x+310, sy=(int)vp.y+40;
    DrawText(VEH[v].desc,sx,sy,15,RGB(255,220,120));
    stat_bar(sx,sy+30, "POWER",      sp.drive/1300.0f);
    stat_bar(sx,sy+52, "TOP SPEED",  sp.max_spd/900.0f);
    stat_bar(sx,sy+74, "FUEL",       sp.fuel_cap/200.0f/sp.fuel_use);
    stat_bar(sx,sy+96, "AIR CONTROL",sp.air_rot/360.0f);
    stat_bar(sx,sy+118,"SUSPENSION", sp.susp_d/10.0f);
    Rectangle ab={(float)sx,(float)sy+150,280,44};
    if(g_save.owned[v]){
        if(g_save.veh==v){
            DrawRectangleRounded(ab,0.35f,8,RGBA(40,140,60,200));
            DrawText("SELECTED",(int)(ab.x+ab.width/2-MeasureText("SELECTED",20)/2),(int)ab.y+12,20,WHITE);
        } else if(ui_button(ab,"SELECT",20,RGB(40,120,200),true)){ g_save.veh=v; save_game(); }
    } else {
        char bb[32]; snprintf(bb,sizeof bb,"BUY  %d coins",VEH[v].price);
        if(ui_button(ab,bb,20,RGB(200,130,20),g_save.bank>=VEH[v].price)){
            g_save.bank-=VEH[v].price; g_save.owned[v]=1; g_save.veh=v; save_game();
        }
    }

    /* ---- options panel ---- */
    Rectangle op={710,300,620,270};
    hud_panel(op,RGBA(0,0,0,150));
    DrawText("RACE OPTIONS",(int)op.x+16,(int)op.y+10,16,RGBA(200,200,220,220));
    static const char *RIV[4]={"OFF","EASY","NORMAL","HARD"};
    char ob[64];
    int ox=(int)op.x+20, oy=(int)op.y+40;
    snprintf(ob,sizeof ob,"AI RIVAL:  %s",RIV[g_save.rival]);
    if(ui_button((Rectangle){(float)ox,(float)oy,280,44},ob,18,RGB(45,60,145),true)){ g_save.rival=(g_save.rival+1)%4; save_game(); }
    DrawText("A computer driver races you on the",ox+296,oy+6,13,RGBA(190,190,210,200));
    DrawText("same track (unlimited fuel).",ox+296,oy+24,13,RGBA(190,190,210,200));
    oy+=56;
    snprintf(ob,sizeof ob,"GHOST REPLAY:  %s",g_save.ghost?"ON":"OFF");
    if(ui_button((Rectangle){(float)ox,(float)oy,280,44},ob,18,RGB(45,60,145),true)){ g_save.ghost^=1; save_game(); }
    DrawText("Race against your best run.",ox+296,oy+14,13,RGBA(190,190,210,200));
    oy+=56;
    snprintf(ob,sizeof ob,"AI COACH:  %s",g_save.hints?"ON":"OFF");
    if(ui_button((Rectangle){(float)ox,(float)oy,280,44},ob,18,RGB(45,60,145),true)){ g_save.hints^=1; save_game(); }
    DrawText("Fuel range, terrain & landing tips.",ox+296,oy+14,13,RGBA(190,190,210,200));
    oy+=56;
    snprintf(ob,sizeof ob,"SOUND:  %s",g_save.sound?"ON":"OFF");
    if(ui_button((Rectangle){(float)ox,(float)oy,280,44},ob,18,RGB(45,60,145),true)){ g_save.sound^=1; save_game(); }
    DrawText("Synthesized engine & effects.",ox+296,oy+14,13,RGBA(190,190,210,200));

    /* ---- main buttons ---- */
    bool can_start=stage_unlocked(g_lv);
    if(ui_button((Rectangle){SW/2-330,592,320,64},"START RACE  [ENTER]",24,RGB(30,150,30),can_start)) run_start();
    if(ui_button((Rectangle){SW/2+10,592,320,64},"GARAGE  [G]",24,RGB(170,100,20),true)) g_st=ST_GARAGE;

#ifdef __EMSCRIPTEN__
    const char *inst="1-6 quick start    In race: D/RIGHT gas, A/LEFT brake, SHIFT/SPACE nitro, TAB autopilot";
#else
    const char *inst="1-6 quick start    ESC quit    In race: D/RIGHT gas, A/LEFT brake, SHIFT/SPACE nitro, TAB autopilot";
#endif
    DrawText(inst,SW/2-MeasureText(inst,14)/2,676,14,RGBA(170,170,200,190));
    DrawText("v5.0  |  Hill Climb Racer",10,SH-20,13,RGBA(140,140,160,150));
}

static void draw_garage(void){
    draw_menu_bg();
    const char *title="GARAGE";
    DrawText(title,SW/2-MeasureText(title,56)/2,30,56,RGB(255,200,40));
    draw_bank();

    Rectangle up={70,120,800,420};
    hud_panel(up,RGBA(0,0,0,160));
    DrawText("UPGRADES  (apply to every vehicle)",(int)up.x+16,(int)up.y+12,16,RGBA(200,200,220,220));
    for(int i=0;i<NUM_UP;i++){
        int y=(int)up.y+50+i*92;
        DrawText(UP_NAME[i],(int)up.x+20,y,24,WHITE);
        DrawText(UP_DESC[i],(int)up.x+20,y+30,14,RGBA(190,190,210,210));
        for(int p=0;p<UP_MAX;p++){
            Rectangle pr={up.x+20+p*42,(float)y+54,36,12};
            DrawRectangleRec(pr,p<g_save.up[i]?RGB(90,220,110):RGBA(60,60,80,220));
        }
        Rectangle b={up.x+560,(float)y+8,220,52};
        if(g_save.up[i]>=UP_MAX){
            DrawRectangleRounded(b,0.35f,8,RGBA(40,140,60,200));
            DrawText("MAXED",(int)(b.x+b.width/2-MeasureText("MAXED",20)/2),(int)b.y+16,20,WHITE);
        } else {
            int cost=up_cost(g_save.up[i]);
            char t[32]; snprintf(t,sizeof t,"UPGRADE  %d",cost);
            if(ui_button(b,t,20,RGB(200,130,20),g_save.bank>=cost)){
                g_save.bank-=cost; g_save.up[i]++; save_game();
                toast("%s upgraded to level %d",UP_NAME[i],g_save.up[i]);
            }
        }
    }

    Rectangle st={70,556,800,120};
    hud_panel(st,RGBA(0,0,0,160));
    DrawText("CAREER",(int)st.x+16,(int)st.y+10,16,RGBA(200,200,220,220));
    char l1[96], l2[96];
    snprintf(l1,sizeof l1,"Runs: %d      Total distance: %.0f m",g_save.runs,g_save.total_dist);
    snprintf(l2,sizeof l2,"Flips landed: %d      Coins earned: %d",g_save.total_flips,g_save.total_coins);
    DrawText(l1,(int)st.x+20,(int)st.y+42,20,WHITE);
    DrawText(l2,(int)st.x+20,(int)st.y+74,20,WHITE);

    Rectangle ac={890,120,440,556};
    hud_panel(ac,RGBA(0,0,0,160));
    int got=0; for(int i=0;i<NUM_ACH;i++) got+=g_save.ach[i];
    char ah[48]; snprintf(ah,sizeof ah,"ACHIEVEMENTS  %d/%d",got,NUM_ACH);
    DrawText(ah,(int)ac.x+16,(int)ac.y+12,16,RGBA(200,200,220,220));
    for(int i=0;i<NUM_ACH;i++){
        int y=(int)ac.y+46+i*56;
        bool ok=g_save.ach[i];
        DrawCircle((int)ac.x+30,y+16,13,ok?RGB(255,200,40):RGBA(70,70,90,255));
        if(ok) DrawText("*",(int)ac.x+25,y+6,24,RGB(120,60,0));
        DrawText(ACH_NAME[i],(int)ac.x+54,y,18,ok?WHITE:RGB(150,150,160));
        DrawText(ACH_DESC[i],(int)ac.x+54,y+22,13,ok?RGB(200,230,200):RGB(120,120,140));
    }

    if(ui_button((Rectangle){70,700,220,56},"BACK  [ESC]",20,RGB(45,60,145),true)) g_st=ST_MENU;
}

/*============================================================================
  OVERLAYS
============================================================================*/
static void draw_pause(void){
    DrawRectangle(0,0,SW,SH,RGBA(0,0,0,150));
    const char *t="PAUSED";
    DrawText(t,SW/2-MeasureText(t,70)/2,180,70,WHITE);
    if(ui_button((Rectangle){SW/2-150,300,300,56},"RESUME  [P]",22,RGB(30,150,30),true)) g_st=ST_PLAY;
    if(ui_button((Rectangle){SW/2-150,370,300,56},"RESTART  [R]",22,RGB(170,100,20),true)){ end_run(false); run_start(); }
    if(ui_button((Rectangle){SW/2-150,440,300,56},"QUIT TO MENU  [M]",22,RGB(45,60,145),true)){ end_run(false); g_st=ST_MENU; }
    const char *n="Quitting or restarting still banks the coins you collected.";
    DrawText(n,SW/2-MeasureText(n,16)/2,520,16,RGBA(200,200,200,200));
}

static void draw_end(void){
    RunResult *r=&g_res;
    Car *c=&g_car;
    DrawRectangle(0,0,SW,SH,RGBA(0,0,0,165));
    const char *title; Color tc;
    if(r->win){ title="STAGE COMPLETE!"; tc=RGB(70,238,100); }
    else if(r->cause==CAUSE_FUEL){ title="OUT OF FUEL"; tc=RGB(255,170,60); }
    else if(r->cause==CAUSE_NONE){ title="RUN ENDED"; tc=RGB(200,200,200); }
    else { title="CRASHED!"; tc=RGB(218,45,45); }
    int tw=MeasureText(title,64);
    DrawText(title,SW/2-tw/2+3,80+3,64,RGBA(0,0,0,120));
    DrawText(title,SW/2-tw/2,80,64,tc);
    const char *why=r->cause==CAUSE_HEAD?"The driver's head hit the ground"
                   :r->cause==CAUSE_FLIP?"Landed on the roof"
                   :r->cause==CAUSE_FUEL?"The tank ran dry - grab more canisters":"";
    if(!r->win) DrawText(why,SW/2-MeasureText(why,18)/2,150,18,RGBA(220,220,220,200));

    if(r->place){
        const char *pl=r->place==1?"YOU BEAT THE AI RIVAL!":"THE AI RIVAL BEAT YOU";
        DrawText(pl,SW/2-MeasureText(pl,26)/2,180,26,r->place==1?RGB(120,255,140):RGB(130,170,255));
    } else if(g_rival_on){
        char pl[64]; snprintf(pl,sizeof pl,"AI rival distance: %.0f m",g_rival.dist);
        DrawText(pl,SW/2-MeasureText(pl,22)/2,182,22,RGB(130,170,255));
    }

    /* left: run stats */
    Rectangle L={(float)SW/2-470,230,440,300};
    hud_panel(L,RGBA(0,0,0,140));
    char b[96]; int y=(int)L.y+20, x=(int)L.x+24;
    DrawText("THIS RUN",x,y,16,RGBA(180,180,200,220)); y+=34;
    snprintf(b,sizeof b,"Distance      %.0f m%s",c->dist,r->new_dist?"   NEW BEST!":"");
    DrawText(b,x,y,24,r->new_dist?RGB(120,255,140):WHITE); y+=38;
    snprintf(b,sizeof b,"Time          %d:%05.2f%s",(int)(c->time/60),fmodf(c->time,60),r->new_time?"   RECORD!":"");
    DrawText(b,x,y,24,r->new_time?RGB(120,255,140):WHITE); y+=38;
    snprintf(b,sizeof b,"Flips         %d",c->flips);       DrawText(b,x,y,24,RGB(255,165,45)); y+=38;
    snprintf(b,sizeof b,"Perfect lands %d",c->perfects);    DrawText(b,x,y,24,RGB(120,210,255)); y+=38;
    snprintf(b,sizeof b,"Fuel cans     %d",c->cans);        DrawText(b,x,y,24,RGB(70,215,70));

    /* right: earnings */
    Rectangle R={(float)SW/2+30,230,440,300};
    hud_panel(R,RGBA(0,0,0,140));
    y=(int)R.y+20; x=(int)R.x+24;
    DrawText("EARNINGS",x,y,16,RGBA(180,180,200,220)); y+=34;
    snprintf(b,sizeof b,"Coins & stunts   %d",r->coins);       DrawText(b,x,y,22,RGB(255,215,0)); y+=34;
    snprintf(b,sizeof b,"Distance bonus   %d",r->dist_bonus);  DrawText(b,x,y,22,WHITE); y+=34;
    if(r->stage_bonus){ snprintf(b,sizeof b,"Stage bonus      %d",r->stage_bonus); DrawText(b,x,y,22,WHITE); y+=34; }
    if(r->rival_bonus){ snprintf(b,sizeof b,"Rival bonus      %d",r->rival_bonus); DrawText(b,x,y,22,WHITE); y+=34; }
    if(r->autopilot){ DrawText("Autopilot used   x0.5",x,y,22,RGB(255,140,140)); y+=34; }
    DrawLine(x,y,x+390,y,RGBA(255,255,255,80)); y+=10;
    snprintf(b,sizeof b,"TOTAL           +%d",r->total);    DrawText(b,x,y,28,RGB(255,218,55)); y+=40;
    snprintf(b,sizeof b,"Bank: %d coins",g_save.bank);      DrawText(b,x,y,18,RGBA(220,220,220,220));

    int bx=SW/2-330;
    bool next=r->win && g_lv<ENDLESS_LV-1;
    if(ui_button((Rectangle){(float)bx,560,200,56},"RETRY  [R]",20,RGB(170,100,20),true)) run_start();
    if(ui_button((Rectangle){(float)bx+230,560,200,56},"MENU  [M]",20,RGB(45,60,145),true)) g_st=ST_MENU;
    if(ui_button((Rectangle){(float)bx+460,560,200,56},"NEXT  [N]",20,RGB(30,150,30),next)){ g_lv++; run_start(); }
    if(r->autopilot) DrawText("Records and ghosts are not saved for autopilot runs.",
                              SW/2-MeasureText("Records and ghosts are not saved for autopilot runs.",15)/2,640,15,RGBA(220,180,180,200));
}

/*============================================================================
  CAMERA + AUDIO PARAMS
============================================================================*/
static const Vector2 CAM_OFF={SW*0.38f,SH*0.55f};

static void camera_update(float dt){
    Car *c=&g_car;
    Vector2 look={c->pos.x+Clamp(c->vel.x*0.35f,-150,260),c->pos.y+Clamp(c->vel.y*0.12f,-80,120)};
    g_cam.target=Vector2Lerp(g_cam.target,look,fminf(1.0f,6.0f*dt));
    float tz=Clamp(1.05f-Vector2Length(c->vel)/2500.0f,0.72f,1.15f);
    if(!c->rear.on && !c->fwd.on && !c->dead) tz-=0.06f;
    g_cam.zoom+=(tz-g_cam.zoom)*fminf(1.0f,3.0f*dt);
    g_shake=fmaxf(0,g_shake-30.0f*dt);
    g_cam.offset=(Vector2){CAM_OFF.x+rnd_f(-1,1)*g_shake,CAM_OFF.y+rnd_f(-1,1)*g_shake};
}

static void audio_update(void){
    if(!g_audio_ok) return;
    Car *c=&g_car;
    bool on=g_save.sound && g_st==ST_PLAY && !c->dead && c->fuel>0.01f;
    g_eng_vol=on?(0.30f+(c->in.gas?0.35f:0)+(c->boosting?0.15f:0)):0.0f;
    g_eng_rpm=Clamp(fabsf(c->vel.x)/c->sp.max_spd,0,1.3f)*0.75f+(c->in.gas?0.25f:0)+(c->boosting?0.2f:0);
}

/*============================================================================
  MAIN
============================================================================*/
int main(void){
    SetConfigFlags(FLAG_MSAA_4X_HINT|FLAG_VSYNC_HINT);
    InitWindow(SW,SH,"Hill Climb Racer");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    storage_init();
    load_game();
    audio_init();
    g_view_veh=g_save.veh;
    g_cam=(Camera2D){.offset=CAM_OFF,.zoom=1.05f};

    while(!g_quit && !WindowShouldClose()){
        float dt=GetFrameTime();
        if(dt>0.05f) dt=0.05f;
        g_time+=dt;
        g_menu_t+=dt;
        if(g_toast_t>0) g_toast_t-=dt;

        /* a key that changed the screen must not also act on the new one   */
        static GState last_st=ST_MENU;
        static int    st_age=0;
        if(g_st!=last_st){ last_st=g_st; st_age=0; } else st_age++;

        //input
        if(st_age>1) switch(g_st){
        case ST_MENU:
            for(int i=0;i<MAX_LEVELS;i++)
                if(IsKeyPressed(KEY_ONE+i) && stage_unlocked(i)){ g_lv=i; run_start(); }
            if(g_st==ST_MENU && IsKeyPressed(KEY_ENTER) && stage_unlocked(g_lv)) run_start();
            if(IsKeyPressed(KEY_G)) g_st=ST_GARAGE;
#ifndef __EMSCRIPTEN__
            if(IsKeyPressed(KEY_ESCAPE)) g_quit=true;
#endif
            break;
        case ST_GARAGE:
            if(IsKeyPressed(KEY_ESCAPE)||IsKeyPressed(KEY_BACKSPACE)) g_st=ST_MENU;
            break;
        case ST_PLAY:
            if(IsKeyPressed(KEY_P)||IsKeyPressed(KEY_ESCAPE)) g_st=ST_PAUSE;
            if(IsKeyPressed(KEY_TAB)){
                g_autopilot=!g_autopilot; g_abrain.airborne=false;
                popup(RGB(120,170,255),26,g_autopilot?"AUTOPILOT ENGAGED":"YOU HAVE CONTROL");
            }
            if(IsKeyPressed(KEY_H)){ g_save.hints^=1; save_game(); }
            break;
        case ST_PAUSE:
            if(IsKeyPressed(KEY_P)||IsKeyPressed(KEY_ESCAPE)) g_st=ST_PLAY;
            else if(IsKeyPressed(KEY_R)){ end_run(false); run_start(); }
            else if(IsKeyPressed(KEY_M)){ end_run(false); g_st=ST_MENU; }
            break;
        case ST_DEAD: case ST_WIN:
            if(IsKeyPressed(KEY_M)||IsKeyPressed(KEY_ESCAPE)) g_st=ST_MENU;
            else if(IsKeyPressed(KEY_R)) run_start();
            else if(g_st==ST_WIN && IsKeyPressed(KEY_N) && g_lv<ENDLESS_LV-1){ g_lv++; run_start(); }
            break;
        }

        //update
        bool in_game=g_st==ST_PLAY||g_st==ST_DEAD||g_st==ST_WIN;
        if(in_game){
            g_acc+=dt;
            int steps=0;
            while(g_acc>=FIXED_DT && steps<8){
                if(g_st==ST_PLAY) game_step(FIXED_DT); else idle_step(FIXED_DT);
                g_acc-=FIXED_DT; steps++;
            }
            if(steps>=8) g_acc=0;
            if(g_st==ST_PLAY) coach_update(dt); else g_npred=0;
            car_fx(&g_car,dt);
            if(g_rival_on) car_fx(&g_rival,dt);
            particles_update(dt);
            popups_update(dt);
            camera_update(dt);
        }
        audio_update();

        //draw
        BeginDrawing();
        ClearBackground(BLACK);
        if(g_st==ST_MENU) draw_menu();
        else if(g_st==ST_GARAGE) draw_garage();
        else {
            draw_backdrop();
            BeginMode2D(g_cam);
                draw_terrain();
                draw_deco();
                draw_bridges();
                draw_finish();
                draw_canisters();
                draw_coins();
                draw_ghost();
                if(g_rival_on){
                    draw_vehicle(&g_rival,RIVAL_PAINT,0.85f);
                    if(!g_rival.dead){
                        Vector2 tag=car_pt(&g_rival,0,-3.6f);
                        const char *nm=g_save.rival==3?"ACE (AI)":g_save.rival==2?"RIVAL (AI)":"ROOKIE (AI)";
                        int w=MeasureText(nm,18);
                        DrawRectangleRounded((Rectangle){tag.x-w/2-8,tag.y-3,(float)w+16,24},0.5f,6,RGBA(20,40,110,190));
                        DrawText(nm,(int)tag.x-w/2,(int)tag.y,18,RGB(190,215,255));
                    }
                }
                draw_particles();
                draw_prediction();
                draw_vehicle(&g_car,veh_paint(g_car.veh),1.0f);
            EndMode2D();
            draw_weather();
            draw_hud();
            draw_popups();
            if(g_st==ST_PAUSE) draw_pause();
            if(g_st==ST_DEAD||g_st==ST_WIN) draw_end();
        }
        draw_toast();
        EndDrawing();
    }

    if(g_audio_ok){
        UnloadAudioStream(g_eng);
        for(int i=0;i<NUM_SFX;i++) UnloadSound(g_sfx[i]);
        CloseAudioDevice();
    }
    CloseWindow();
    return 0;
}
