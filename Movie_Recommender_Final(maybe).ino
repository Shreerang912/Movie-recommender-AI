/*
 * ═══════════════════════════════════════════════════════════════
 *  MOVIE RECOMMENDATION AI  —  ESP32 Edition  v3
 *
 *  PRIMARY DISPLAY  : 2.8" ST7789  240×320  (via TFT_eSPI)
 *                     setRotation(1) → landscape 320×240
 *  TOUCH            : XPT2046 on same SPI bus as primary
 *  MATH DISPLAY     : 1.8" ST7735  128×160  (separate SPI bus)
 *
 *  LIBRARIES:
 *    TFT_eSPI            (Bodmer)          ← primary display
 *    XPT2046_Touchscreen (Paul Stoffregen) ← touch
 *    Adafruit_ST7735     (Adafruit)        ← math display
 *    Adafruit_GFX        (Adafruit)        ← math display dependency
 *
 *  ─── PRIMARY DISPLAY WIRING (TFT_eSPI → User_Setup.h) ──────
 *    MOSI=23  MISO=19  SCK=18  CS=15  DC=2  RST=4
 *    Touch CS=5  Touch IRQ=27
 *
 *  ─── MATH DISPLAY WIRING (1.8" ST7735, second SPI bus) ──────
 *    ST7735 pin │ ESP32 GPIO  │ Notes
 *    ───────────┼─────────────┼──────────────────────────
 *    VCC        │ 3.3V        │
 *    GND        │ GND         │
 *    CS         │ GPIO 33     │ MATH_CS
 *    RESET      │ GPIO 32     │ MATH_RST
 *    DC (A0)    │ GPIO 25     │ MATH_DC
 *    SDA (MOSI) │ GPIO 13     │ VSPI MOSI  (separate bus)
 *    SCK        │ GPIO 14     │ VSPI CLK   (separate bus)
 *    LED        │ 3.3V        │ (or PWM for dimming)
 *    SDO (MISO) │ not needed  │ display-only, leave unconnected
 *
 *  ─── ALGORITHM ──────────────────────────────────────────────
 *  WEIGHTED MANHATTAN DISTANCE
 *    • Each genre gets a weight W[i] based on how extreme the
 *      user rated it  (far from 5 = they care more about it).
 *      W[i] = 1 + |userProfile[i] - 5| / 5   → range [1.0 .. 2.0]
 *    • Weighted distance per genre:
 *        wDiff[i] = W[i] × |user[i] - movie[i]|
 *    • Total weighted distance:
 *        D = Σ wDiff[i]
 *    • Max possible weighted distance (W=2, diff=10, 10 genres):
 *        MAX_D = 2 × 10 × 10 = 200
 *    • Similarity:
 *        S% = (1 - D / MAX_D) × 100
 *
 *  SMART SWIPE ORDER
 *    After ratings, all 50 movies are pre-scored. The 15 shown
 *    in the swipe phase are NOT the top 15 — they are selected
 *    to cover the full score range (diverse sampling), so the
 *    user sees a mix of likely good and likely bad movies.
 *    Their like/dislike/idk reactions then fine-tune the profile
 *    before final scoring.
 *
 *  LEARNING
 *    LIKE:    user[i] += 0.35 × (movie[i] − user[i])
 *    DISLIKE: user[i] -= 0.25 × (movie[i] − user[i])
 *    IDK:     no profile change (but movie is excluded from results)
 *
 *  MATH DISPLAY CONTENT (live, per movie scored):
 *    Page 1 — Vector comparison (user vs movie per genre)
 *    Page 2 — Weight calculation  W[i] = 1 + |u-5|/5
 *    Page 3 — Weighted diffs      wDiff[i] = W × |u-m|
 *    Page 4 — Final score         D, MAX_D, S%
 *    Pages auto-cycle every 1.4 s while scoring runs
 * ═══════════════════════════════════════════════════════════════
 */

// ─────────────────────────────────────────────────────────────
//  INCLUDES
// ─────────────────────────────────────────────────────────────
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>



struct Point { int x, y; bool valid; };
// ─────────────────────────────────────────────────────────────
//  TOUCH PINS
// ─────────────────────────────────────────────────────────────
#define TOUCH_CS   5
#define TOUCH_IRQ  27

// ─────────────────────────────────────────────────────────────
//  MATH DISPLAY PINS  (1.8" ST7735, separate SPI)
// ─────────────────────────────────────────────────────────────
#define MATH_CS    33
#define MATH_RST   32
#define MATH_DC    25
#define MATH_MOSI  13
#define MATH_SCK   14
// No MISO needed for display-only

// ─────────────────────────────────────────────────────────────
//  PRIMARY SCREEN DIMENSIONS  (landscape after setRotation 1)
// ─────────────────────────────────────────────────────────────
#define SCR_W  320
#define SCR_H  240

// Math display is portrait native: 128×160
#define MATH_W  128
#define MATH_H  160

// ─────────────────────────────────────────────────────────────
//  TOUCH CALIBRATION
// ─────────────────────────────────────────────────────────────
#define CALIBRATE_TOUCH  0
#define TOUCH_X_MIN   200
#define TOUCH_X_MAX  3800
#define TOUCH_Y_MIN   200
#define TOUCH_Y_MAX  3800

// ─────────────────────────────────────────────────────────────
//  COLOURS — primary display (RGB565)
// ─────────────────────────────────────────────────────────────
#define C_BG      0x0841
#define C_BG2     0x10A2
#define C_ACCENT  0xF800   // red
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_AMBER   0xFD20
#define C_WHITE   0xFFFF
#define C_LGRAY   0xC618
#define C_DGRAY   0x4208
#define C_BLACK   0x0000
#define C_BORDER  0x2965
#define C_YELLOW  0xFFE0

// ─────────────────────────────────────────────────────────────
//  COLOURS — math display (Adafruit RGB565 constants)
// ─────────────────────────────────────────────────────────────
#define M_BLACK   0x0000
#define M_WHITE   0xFFFF
#define M_RED     0xF800
#define M_GREEN   0x07E0
#define M_BLUE    0x001F
#define M_YELLOW  0xFFE0
#define M_CYAN    0x07FF
#define M_MAGENTA 0xF81F
#define M_ORANGE  0xFD20
#define M_PINK    0xFC18
#define M_LIME    0x87E0
#define M_TEAL    0x0410
#define M_PURPLE  0x8010
#define M_DGRAY   0x4208
#define M_LGRAY   0xAD55

// ─────────────────────────────────────────────────────────────
//  GENRES
// ─────────────────────────────────────────────────────────────
#define NUM_GENRES      10
#define GENRES_PER_PAGE  4

const char* GENRE_NAMES[NUM_GENRES] = {
  "Action","Comedy","Sci-Fi","Romance","Horror",
  "Drama","Thriller","Adventure","Fantasy","Crime"
};

// Short 4-char labels for the narrow math display
const char* GENRE_SHORT[NUM_GENRES] = {
  "ACT","COM","SCI","ROM","HOR","DRM","THR","ADV","FAN","CRM"
};

// ─────────────────────────────────────────────────────────────
//  MOVIE DATABASE
// ─────────────────────────────────────────────────────────────
struct Movie { const char* name; uint8_t g[NUM_GENRES]; };
#define NUM_MOVIES 65

const Movie MOVIES[NUM_MOVIES] PROGMEM = {
  // ── WESTERN CLASSICS ────────────────────────────────────
  {"The Dark Knight",       { 9, 1, 2, 1, 3, 8,10, 7, 2, 9}},
  {"Inception",             { 7, 1, 9, 3, 2, 7, 9, 8, 7, 2}},
  {"The Matrix",            { 9, 1,10, 2, 2, 6, 8, 8, 5, 2}},
  {"Interstellar",          { 5, 1,10, 4, 2, 9, 7, 9, 4, 1}},
  {"The Avengers",          {10, 5, 7, 2, 1, 4, 5, 9, 7, 1}},
  {"Pulp Fiction",          { 5, 7, 1, 2, 3, 8, 9, 2, 1,10}},
  {"The Notebook",          { 1, 2, 1,10, 1, 9, 2, 2, 2, 1}},
  {"Get Out",               { 3, 2, 3, 2,10, 8, 9, 1, 3, 4}},
  {"The Hangover",          { 2,10, 1, 3, 1, 2, 3, 5, 1, 5}},
  {"Forrest Gump",          { 2, 6, 1, 7, 1,10, 2, 5, 2, 1}},
  {"Titanic",               { 4, 1, 1,10, 2,10, 4, 5, 2, 1}},
  {"Gravity",               { 6, 1, 9, 2, 4, 7, 8, 8, 2, 1}},
  {"The Shining",           { 2, 1, 2, 1,10, 8, 9, 1, 4, 3}},
  {"Toy Story",             { 3, 8, 2, 2, 1, 5, 2, 8, 9, 1}},
  {"The Lion King",         { 5, 5, 1, 4, 2, 8, 3, 8, 7, 2}},
  {"Goodfellas",            { 5, 3, 1, 2, 3, 9, 8, 2, 1,10}},
  {"La La Land",            { 1, 5, 1, 9, 1, 9, 2, 2, 4, 1}},
  {"Mad Max: Fury Road",    {10, 1, 5, 2, 4, 5, 7, 9, 4, 3}},
  {"Silence of the Lambs",  { 3, 1, 2, 1, 9, 8,10, 2, 2, 9}},
  {"Avatar",                { 7, 2, 8, 4, 1, 5, 4, 9, 9, 1}},
  {"Lord of the Rings",     { 7, 3, 1, 3, 4, 8, 5,10,10, 1}},
  {"Harry Potter",          { 4, 5, 1, 2, 3, 5, 4, 8,10, 2}},
  {"Jurassic Park",         { 6, 3, 8, 2, 6, 5, 7, 9, 4, 1}},
  {"Die Hard",              {10, 5, 1, 2, 2, 4, 8, 7, 1, 6}},
  {"Parasite",              { 2, 4, 1, 2, 6,10, 9, 1, 1, 7}},
  {"Black Panther",         { 9, 4, 6, 3, 1, 7, 6, 8, 7, 3}},
  {"John Wick",             {10, 2, 1, 1, 2, 4, 8, 6, 2, 7}},
  {"Knives Out",            { 2, 6, 1, 2, 2, 7, 9, 2, 1,10}},
  {"Blade Runner 2049",     { 5, 1,10, 3, 3, 8, 8, 4, 4, 5}},
  {"Spider-Verse",          { 8, 7, 5, 2, 1, 6, 4, 9, 8, 4}},
  {"Top Gun: Maverick",     { 9, 3, 2, 4, 1, 6, 6, 8, 1, 1}},
  {"Dune",                  { 6, 1,10, 2, 3, 7, 6, 8, 8, 1}},
  {"The Batman",            { 7, 1, 2, 2, 4, 8,10, 5, 2, 9}},
  {"Sholay",                { 9, 6, 0, 4, 1, 7, 5, 8, 1, 7}},
  {"DDLJ",                  { 1, 5, 0,10, 0, 8, 1, 4, 2, 0}},
  {"3 Idiots",              { 1, 9, 1, 4, 0, 9, 2, 3, 1, 0}},
  {"Dangal",                { 5, 2, 0, 1, 0,10, 3, 4, 0, 0}},
  {"Lagaan",                { 4, 3, 0, 4, 0, 9, 4, 7, 1, 0}},
  {"Baahubali 2",           {10, 2, 0, 5, 1, 8, 5, 9, 9, 2}},
  {"RRR",                   {10, 3, 0, 3, 1, 7, 6, 9, 5, 2}},
  {"KGF Chapter 2",         {10, 1, 0, 2, 2, 7, 7, 5, 2, 9}},
  {"Pushpa: The Rise",      { 8, 3, 0, 3, 1, 6, 6, 5, 1, 8}},
  {"Uri: Surgical Strike",  { 9, 1, 1, 2, 1, 7, 9, 6, 0, 2}},
  {"Andhadhun",             { 3, 5, 0, 3, 4, 7,10, 1, 1, 8}},
  {"Gangs of Wasseypur",    { 6, 4, 0, 2, 2, 9, 8, 2, 0,10}},
  {"Drishyam",              { 2, 1, 0, 3, 2, 9,10, 1, 0, 7}},
  {"Zindagi Na Milegi Dobara",{ 2, 7, 0, 5, 0, 7, 1, 8, 2, 0}},
  {"Taare Zameen Par",      { 0, 2, 0, 2, 0,10, 1, 1, 2, 0}},
  {"PK",                    { 2, 8, 8, 4, 0, 7, 2, 4, 6, 0}},
  {"Jab We Met",            { 1, 8, 0, 9, 0, 5, 1, 4, 1, 0}},
  {"Rang De Basanti",       { 5, 3, 0, 4, 0,10, 5, 3, 0, 3}},
  {"Munna Bhai MBBS",       { 3, 9, 0, 4, 0, 8, 2, 2, 1, 4}},
  {"Hera Pheri",            { 2,10, 0, 1, 0, 3, 3, 2, 0, 5}},
  {"Stree",                 { 3, 8, 0, 3, 8, 4, 5, 2, 6, 1}},
  {"Animal",                { 9, 1, 0, 4, 2, 8, 7, 2, 0, 7}},
  {"Pathaan",               { 9, 3, 2, 3, 0, 4, 8, 7, 1, 3}},
  {"12th Fail",             { 1, 2, 0, 4, 0,10, 3, 2, 0, 0}},
  {"Queen",                 { 1, 5, 0, 3, 0, 9, 1, 7, 1, 0}},
  {"Kabir Singh",           { 2, 2, 0, 9, 0, 9, 3, 1, 0, 1}},
  {"Sultan",                { 6, 3, 0, 5, 0, 8, 2, 4, 0, 0}},
  {"Swades",                { 1, 2, 1, 4, 0,10, 1, 3, 0, 0}},
  {"Dil Chahta Hai",        { 1, 8, 0, 6, 0, 7, 1, 5, 1, 0}},
  {"Tumbbad",               { 2, 0, 0, 2, 9, 7, 7, 3, 9, 3}},
  {"Dhurandhar",            { 9, 2, 2, 2, 1, 6,10, 6, 0, 5}},
  {"Dhurandhar: The Revenge",{ 9, 1, 2, 2, 1, 7,10, 7, 0, 5}},
};

#define SWIPE_COUNT 15

// ─────────────────────────────────────────────────────────────
//  STATE MACHINE
// ─────────────────────────────────────────────────────────────
enum AppState { STATE_RATING, STATE_SWIPE, STATE_RESULTS };

// ─────────────────────────────────────────────────────────────
//  GLOBAL STATE
// ─────────────────────────────────────────────────────────────
AppState g_state       = STATE_RATING;
float    g_user[NUM_GENRES];          // user preference profile
int      g_ratingPage  = 0;
int      g_swipeIndex  = 0;
int      g_swipeOrder[NUM_MOVIES];    // pre-sorted diverse swipe order
uint8_t  g_reactions[NUM_MOVIES];     // 1=like 2=dislike 3=idk 0=none

// ─────────────────────────────────────────────────────────────
//  CALCULATION SNAPSHOT  (math display + backdoor)
// ─────────────────────────────────────────────────────────────
struct CalcSnapshot {
  char  movieName[40];
  float userG[NUM_GENRES];
  float movieG[NUM_GENRES];
  float weights[NUM_GENRES];    // W[i] = 1 + |user[i]-5|/5
  float wDiffs[NUM_GENRES];     // W[i] * |user[i]-movie[i]|
  float rawDiffs[NUM_GENRES];   // |user[i]-movie[i]|
  float totalDist;              // Σ wDiff
  float maxDist;                // 200.0
  float similarity;             // S%
};
CalcSnapshot g_calc;

// ─────────────────────────────────────────────────────────────
//  HARDWARE
// ─────────────────────────────────────────────────────────────
TFT_eSPI tft;

XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);

// Math display on VSPI (separate from HSPI used by TFT_eSPI)
SPIClass mathSPI(VSPI);
Adafruit_ST7735 mtft = Adafruit_ST7735(&mathSPI, MATH_CS, MATH_DC, MATH_RST);

// ─────────────────────────────────────────────────────────────
//  INTERACTION STATE
// ─────────────────────────────────────────────────────────────
bool g_needsDraw   = true;
bool g_touching    = false;
bool g_wasTouching = false;
int  g_dragGenre   = -1;

// ─────────────────────────────────────────────────────────────
//  RESULTS
// ─────────────────────────────────────────────────────────────
struct Result { int movieIdx; float score; };
Result g_results[NUM_MOVIES];
int    g_resultCount   = 0;
int    g_resultsScroll = 0;

// Math display page cycle
int  g_mathPage     = 0;
unsigned long g_mathPageTime = 0;
#define MATH_PAGE_MS  1800   // ms per page


// ═════════════════════════════════════════════════════════════
//  WEIGHTED ALGORITHM
// ═════════════════════════════════════════════════════════════

float computeWeight(float userVal) {
  // W = 1 + |userVal - 5| / 5   →  range [1.0 .. 2.0]
  // Genres the user rates extreme (0 or 10) count double.
  // Genres rated exactly 5 (neutral) count normally.
  return 1.0f + fabsf(userVal - 5.0f) / 5.0f;
}

// Fill g_calc for movie at movieIdx against current g_user
void scoreMovie(int movieIdx) {
  strncpy(g_calc.movieName, MOVIES[movieIdx].name, 39);
  g_calc.totalDist = 0.0f;
  g_calc.maxDist   = 200.0f;   // W_max(2) × diff_max(10) × genres(10)

  for (int i = 0; i < NUM_GENRES; i++) {
    float mv         = pgm_read_byte(&MOVIES[movieIdx].g[i]);
    float uv         = g_user[i];
    float w          = computeWeight(uv);
    float rawDiff    = fabsf(uv - mv);
    float wDiff      = w * rawDiff;

    g_calc.userG[i]  = uv;
    g_calc.movieG[i] = mv;
    g_calc.weights[i]= w;
    g_calc.rawDiffs[i]= rawDiff;
    g_calc.wDiffs[i] = wDiff;
    g_calc.totalDist += wDiff;
  }
  g_calc.similarity = (1.0f - g_calc.totalDist / g_calc.maxDist) * 100.0f;
  if (g_calc.similarity < 0) g_calc.similarity = 0;
}

void applyLearning(int movieIdx, int reaction) {
  // Asymmetric rates: like pulls harder than dislike pushes
  const float LR_LIKE    = 0.35f;
  const float LR_DISLIKE = 0.25f;
  float lr = (reaction == 1) ? LR_LIKE : LR_DISLIKE;
  int   dir= (reaction == 1) ? +1 : -1;

  for (int i = 0; i < NUM_GENRES; i++) {
    float mv = pgm_read_byte(&MOVIES[movieIdx].g[i]);
    g_user[i] += lr * (mv - g_user[i]) * dir;
    g_user[i]  = constrain(g_user[i], 0.0f, 10.0f);
  }
}

// Build a DIVERSE swipe order:
//   1. Pre-score all movies against initial profile.
//   2. Sort by score descending.
//   3. Pick SWIPE_COUNT movies spread evenly across the ranked list
//      (top, middle, bottom — so the user sees variety, and their
//       reactions meaningfully update the profile both ways).
void buildSwipeOrder() {
  // Score all movies first
  float scores[NUM_MOVIES];
  int   idx[NUM_MOVIES];
  for (int i = 0; i < NUM_MOVIES; i++) {
    scoreMovie(i);
    scores[i] = g_calc.similarity;
    idx[i]    = i;
  }

  // Insertion sort idx by score descending
  for (int i = 1; i < NUM_MOVIES; i++) {
    int   ki = idx[i];
    float ks = scores[i];
    int   j  = i - 1;
    while (j >= 0 && scores[j] < ks) {
      scores[j+1] = scores[j]; idx[j+1] = idx[j]; j--;
    }
    scores[j+1] = ks; idx[j+1] = ki;
  }

  // Evenly sample SWIPE_COUNT entries across the sorted list
  // e.g. 50 movies, 15 slots → step = 50/15 ≈ 3.3
  for (int s = 0; s < SWIPE_COUNT; s++) {
    int pick = (int)((float)s / SWIPE_COUNT * NUM_MOVIES);
    g_swipeOrder[s] = idx[pick];
  }
}

void computeResults() {
  g_resultCount = 0;
  for (int i = 0; i < NUM_MOVIES; i++) {
    // Skip movies the user explicitly disliked
    if (g_reactions[i] == 2) continue;
    scoreMovie(i);
    g_results[g_resultCount++] = {i, g_calc.similarity};
  }
  // Insertion sort descending
  for (int i = 1; i < g_resultCount; i++) {
    Result k = g_results[i];
    int j = i - 1;
    while (j >= 0 && g_results[j].score < k.score) {
      g_results[j+1] = g_results[j]; j--;
    }
    g_results[j+1] = k;
  }
}


// ═════════════════════════════════════════════════════════════
//  MATH DISPLAY  (1.8" ST7735  128×160)
// ═════════════════════════════════════════════════════════════
//
//  Four rotating pages, auto-cycle every MATH_PAGE_MS:
//  Page 0 — Header + title + user/movie vectors
//  Page 1 — Weight calculation  W[i] = 1 + |u-5|/5
//  Page 2 — Weighted differences  wDiff[i] = W × |u-m|
//  Page 3 — Final score summary with big percentage
//
//  Called from scoreMovie() when in STATE_SWIPE or
//  STATE_RESULTS scoring pass.

void math_header(uint16_t accentCol, const char* title) {
  mtft.fillScreen(M_BLACK);
  // Top bar
  mtft.fillRect(0, 0, MATH_W, 16, accentCol);
  mtft.setTextColor(M_BLACK);
  mtft.setTextSize(1);
  int tw = strlen(title) * 6;
  mtft.setCursor((MATH_W - tw) / 2, 4);
  mtft.print(title);

  // Movie name (truncate to 18 chars)
  char nb[20]; strncpy(nb, g_calc.movieName, 18); nb[18]=0;
  mtft.setTextColor(M_YELLOW);
  int nw = strlen(nb)*6;
  mtft.setCursor((MATH_W-nw)/2, 20);
  mtft.print(nb);

  // thin divider
  mtft.drawFastHLine(4, 30, MATH_W-8, M_DGRAY);
}

// Page 0: USER vs MOVIE vectors side by side
void math_page_vectors() {
  math_header(M_CYAN, "VECTORS");

  // Column headers
  mtft.setTextSize(1);
  mtft.setTextColor(M_CYAN);   mtft.setCursor(2,  34); mtft.print("Genre");
  mtft.setTextColor(M_GREEN);  mtft.setCursor(62, 34); mtft.print("You");
  mtft.setTextColor(M_ORANGE); mtft.setCursor(92, 34); mtft.print("Film");
  mtft.drawFastHLine(2, 43, MATH_W-4, M_DGRAY);

  uint16_t rowCols[2] = {M_BLACK, 0x1082};
  for (int i = 0; i < NUM_GENRES; i++) {
    int y = 46 + i * 11;
    mtft.fillRect(0, y, MATH_W, 11, rowCols[i%2]);

    // Genre name
    mtft.setTextColor(M_LGRAY);
    mtft.setCursor(2, y+2);
    mtft.print(GENRE_SHORT[i]);

    // User bar (green)
    int ubar = (int)(g_calc.userG[i] / 10.0f * 38);
    mtft.fillRect(38, y+2, ubar, 7, M_GREEN);
    mtft.setTextColor(M_GREEN);
    mtft.setCursor(62, y+2);
    char b[4]; snprintf(b,3,"%2d",(int)g_calc.userG[i]);
    mtft.print(b);

    // Movie bar (orange)
    int mbar = (int)(g_calc.movieG[i] / 10.0f * 38);
    mtft.fillRect(80, y+2, mbar > 38 ? 38 : mbar, 7, M_ORANGE);
    mtft.setTextColor(M_ORANGE);
    mtft.setCursor(92, y+2);
    snprintf(b,3,"%2d",(int)g_calc.movieG[i]);
    mtft.print(b);
  }

  // Legend
  mtft.drawFastHLine(2, 157, MATH_W-4, M_DGRAY);
  mtft.setTextColor(M_DGRAY); mtft.setTextSize(1);
  mtft.setCursor(2, 151);
  mtft.print("U=You  F=Film (0-10)");
}

// Page 1: Weight calculation
void math_page_weights() {
  math_header(M_MAGENTA, "WEIGHTS  W=1+|u-5|/5");

  mtft.setTextSize(1);
  mtft.setTextColor(M_MAGENTA); mtft.setCursor(2,  34); mtft.print("Genre");
  mtft.setTextColor(M_GREEN);   mtft.setCursor(44, 34); mtft.print("U");
  mtft.setTextColor(M_YELLOW);  mtft.setCursor(62, 34); mtft.print("|u-5|");
  mtft.setTextColor(M_MAGENTA); mtft.setCursor(98, 34); mtft.print("W");
  mtft.drawFastHLine(2, 43, MATH_W-4, M_DGRAY);

  uint16_t rowCols[2] = {M_BLACK, 0x1082};
  for (int i = 0; i < NUM_GENRES; i++) {
    int y = 46 + i * 11;
    mtft.fillRect(0, y, MATH_W, 11, rowCols[i%2]);

    mtft.setTextColor(M_LGRAY);
    mtft.setCursor(2, y+2); mtft.print(GENRE_SHORT[i]);

    // U value
    mtft.setTextColor(M_GREEN);
    char b[6];
    snprintf(b,5,"%4.1f", g_calc.userG[i]);
    mtft.setCursor(38, y+2); mtft.print(b);

    // |u-5|
    mtft.setTextColor(M_YELLOW);
    snprintf(b,5,"%3.1f", fabsf(g_calc.userG[i]-5.0f));
    mtft.setCursor(60, y+2); mtft.print(b);

    // W — colour-coded: high weight = bright
    float w = g_calc.weights[i];
    uint16_t wc = w > 1.6f ? M_RED : w > 1.2f ? M_ORANGE : M_LGRAY;
    mtft.setTextColor(wc);
    snprintf(b,5,"%4.2f", w);
    mtft.setCursor(90, y+2); mtft.print(b);
  }

  mtft.drawFastHLine(2, 157, MATH_W-4, M_DGRAY);
  mtft.setTextColor(M_DGRAY); mtft.setCursor(2, 151);
  mtft.print("High W = genre matters more");
}

// Page 2: Weighted differences
void math_page_diffs() {
  math_header(M_LIME, "WEIGHTED DIFFS");

  mtft.setTextSize(1);
  mtft.setTextColor(M_LGRAY);   mtft.setCursor(2,  34); mtft.print("Genre");
  mtft.setTextColor(M_CYAN);    mtft.setCursor(38, 34); mtft.print("W");
  mtft.setTextColor(M_YELLOW);  mtft.setCursor(56, 34); mtft.print("|u-m|");
  mtft.setTextColor(M_LIME);    mtft.setCursor(92, 34); mtft.print("W*d");
  mtft.drawFastHLine(2, 43, MATH_W-4, M_DGRAY);

  uint16_t rowCols[2] = {M_BLACK, 0x1082};
  for (int i = 0; i < NUM_GENRES; i++) {
    int y = 46 + i * 11;
    mtft.fillRect(0, y, MATH_W, 11, rowCols[i%2]);

    mtft.setTextColor(M_LGRAY);
    mtft.setCursor(2, y+2); mtft.print(GENRE_SHORT[i]);

    char b[8];
    // W
    mtft.setTextColor(M_CYAN);
    snprintf(b,5,"%4.2f",g_calc.weights[i]);
    mtft.setCursor(30,y+2); mtft.print(b);

    // raw diff
    mtft.setTextColor(M_YELLOW);
    snprintf(b,5,"%4.1f",g_calc.rawDiffs[i]);
    mtft.setCursor(58,y+2); mtft.print(b);

    // weighted diff — colour by magnitude
    float wd = g_calc.wDiffs[i];
    uint16_t dc = wd > 12 ? M_RED : wd > 6 ? M_ORANGE : M_LIME;
    mtft.setTextColor(dc);
    snprintf(b,6,"%5.1f",wd);
    mtft.setCursor(90,y+2); mtft.print(b);
  }

  mtft.drawFastHLine(2, 157, MATH_W-4, M_DGRAY);
  mtft.setTextColor(M_DGRAY); mtft.setCursor(2, 151);
  mtft.print("wDiff = W * |user-movie|");
}

// Page 3: Final score
void math_page_score() {
  mtft.fillScreen(M_BLACK);

  // Header
  mtft.fillRect(0, 0, MATH_W, 16, M_YELLOW);
  mtft.setTextColor(M_BLACK); mtft.setTextSize(1);
  int tw = 5*6; mtft.setCursor((MATH_W-tw)/2, 4);
  mtft.print("SCORE");

  // Movie name
  char nb[20]; strncpy(nb, g_calc.movieName, 18); nb[18]=0;
  mtft.setTextColor(M_WHITE);
  int nw = strlen(nb)*6;
  mtft.setCursor((MATH_W-nw)/2, 20);
  mtft.print(nb);

  mtft.drawFastHLine(4, 30, MATH_W-8, M_DGRAY);

  // Formula lines
  mtft.setTextSize(1);
  mtft.setTextColor(M_CYAN);    mtft.setCursor(4, 36); mtft.print("D = Sum of wDiffs");
  char buf[30];
  snprintf(buf,29,"D = %.1f", g_calc.totalDist);
  mtft.setTextColor(M_YELLOW);  mtft.setCursor(4, 52); mtft.print(buf);

  mtft.setTextColor(M_CYAN);    mtft.setCursor(4, 68); mtft.print("MAX_D = 2*10*10 = 200");
  snprintf(buf,29,"MAX_D = %.0f", g_calc.maxDist);
  mtft.setTextColor(M_LGRAY);   mtft.setCursor(4, 84); mtft.print(buf);

  mtft.setTextColor(M_CYAN);    mtft.setCursor(4, 100); mtft.print("S = (1-D/MAX_D)*100");

  // Big percentage
  float s = g_calc.similarity;
  uint16_t sc = s >= 80 ? M_GREEN : s >= 60 ? M_YELLOW : M_RED;

  // Large text for the score
  mtft.setTextSize(3);
  mtft.setTextColor(sc);
  char perc[10]; snprintf(perc, 9, "%.1f%%", s);
  int pw = strlen(perc)*18;
  mtft.setCursor((MATH_W-pw)/2, 116);
  mtft.print(perc);

  // Score bar
  mtft.fillRect(4, 144, MATH_W-8, 10, M_DGRAY);
  int bw = (int)(s / 100.0f * (MATH_W-8));
  if (bw > 0) mtft.fillRect(4, 144, bw, 10, sc);

  mtft.drawFastHLine(4, 155, MATH_W-8, M_DGRAY);
  mtft.setTextSize(1);
  mtft.setTextColor(M_DGRAY); mtft.setCursor(4, 156);
  mtft.print("S=(1-D/200)*100");
}

// Called after every scoreMovie().
// During non-scoring states, pages cycle on a timer.
void mathDisplay_update() {
  unsigned long now = millis();
  if (now - g_mathPageTime > MATH_PAGE_MS) {
    g_mathPage = (g_mathPage + 1) % 4;
    g_mathPageTime = now;
  }
  switch (g_mathPage) {
    case 0: math_page_vectors(); break;
    case 1: math_page_weights(); break;
    case 2: math_page_diffs();   break;
    case 3: math_page_score();   break;
  }
}

// Force-show the score page immediately when a button is pressed
void mathDisplay_showScore() {
  g_mathPage = 3;
  g_mathPageTime = millis();
  math_page_score();
}


// ═════════════════════════════════════════════════════════════
//  TOUCH HELPERS
// ═════════════════════════════════════════════════════════════


Point getTouch() {
  if (!ts.touched()) return {0,0,false};
  TS_Point p = ts.getPoint();
  int x = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCR_W);
  int y = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCR_H);
  return {constrain(x,0,SCR_W-1), constrain(y,0,SCR_H-1), true};
}

bool inRect(Point t, int rx, int ry, int rw, int rh) {
  return t.valid && t.x>=rx && t.x<rx+rw && t.y>=ry && t.y<ry+rh;
}


// ═════════════════════════════════════════════════════════════
//  PRIMARY DISPLAY — DRAW UTILITIES
// ═════════════════════════════════════════════════════════════

void centreText(const char* s, int y, uint16_t col, uint8_t sz) {
  tft.setTextSize(sz); tft.setTextColor(col, C_BG);
  tft.setCursor((SCR_W - (int)strlen(s)*6*sz)/2, y);
  tft.print(s);
}

void centreTextBg(const char* s, int y, uint16_t col, uint16_t bg, uint8_t sz) {
  tft.setTextSize(sz); tft.setTextColor(col, bg);
  tft.setCursor((SCR_W - (int)strlen(s)*6*sz)/2, y);
  tft.print(s);
}

int drawTitle(const char* title, int yStart, uint16_t col) {
  const int MAXC = 25;
  char buf[80]; strncpy(buf,title,79); buf[79]=0;
  char l1[28]="", l2[28]="";
  int len = strlen(buf);
  if (len <= MAXC) {
    strcpy(l1, buf);
  } else {
    int brk = MAXC;
    while (brk>0 && buf[brk]!=' ') brk--;
    if (!brk) brk=MAXC;
    strncpy(l1,buf,brk); l1[brk]=0;
    strncpy(l2,buf+brk+(buf[brk]==' '?1:0),MAXC); l2[MAXC]=0;
  }
  tft.setTextSize(2); tft.setTextColor(col,C_BG2);
  int y = yStart;
  if (l1[0]) { tft.setCursor((SCR_W-strlen(l1)*12)/2,y); tft.print(l1); y+=24; }
  if (l2[0]) { tft.setCursor((SCR_W-strlen(l2)*12)/2,y); tft.print(l2); y+=24; }
  return y-yStart;
}


// ═════════════════════════════════════════════════════════════
//  SCREEN: RATING
// ═════════════════════════════════════════════════════════════
#define R_HDR_H   30
#define R_ROW_H   45
#define R_BTN_Y   210
#define R_BTN_H   30
#define R_TRK_L   8
#define R_TRK_R   282
#define R_TRK_W   (R_TRK_R - R_TRK_L)
#define R_TRK_OFF 28

void drawSliderRow(int gi, int rowY) {
  uint16_t bg = ((gi%GENRES_PER_PAGE)%2==0) ? C_BG : C_BG2;
  tft.fillRect(0, rowY, SCR_W, R_ROW_H, bg);
  tft.setTextSize(1); tft.setTextColor(C_LGRAY,bg);
  tft.setCursor(8, rowY+8); tft.print(GENRE_NAMES[gi]);

  int val = (int)roundf(g_user[gi]);
  char vb[4]; snprintf(vb,4,"%2d",val);
  tft.setTextSize(2); tft.setTextColor(C_YELLOW,bg);
  tft.setCursor(292, rowY+6); tft.print(vb);

  int trkY = rowY + R_TRK_OFF;
  tft.fillRoundRect(R_TRK_L, trkY-4, R_TRK_W, 8, 4, C_BORDER);
  int fillW = (int)(val/10.0f*R_TRK_W);
  if (fillW>0) tft.fillRoundRect(R_TRK_L, trkY-4, fillW, 8, 4, C_ACCENT);
  int thumbX = R_TRK_L + fillW;
  tft.fillCircle(thumbX, trkY, 10, C_WHITE);
  tft.fillCircle(thumbX, trkY, 7,  C_ACCENT);
}

void drawRatingScreen(bool full) {
  int startG  = g_ratingPage*GENRES_PER_PAGE;
  int endG    = min(startG+GENRES_PER_PAGE, NUM_GENRES);
  int npages  = (NUM_GENRES+GENRES_PER_PAGE-1)/GENRES_PER_PAGE;
  bool isLast = (g_ratingPage == npages-1);

  if (full) {
    tft.fillScreen(C_BG);
    tft.fillRect(0,0,SCR_W,R_HDR_H,C_BG2);
    tft.fillRect(0,0,SCR_W,3,C_ACCENT);
    centreTextBg("RATE YOUR GENRES",8,C_WHITE,C_BG2,1);
    int dotX = (SCR_W - npages*14)/2;
    for (int i=0;i<npages;i++)
      tft.fillCircle(dotX+i*14+7, R_HDR_H-8, 4,
                     i==g_ratingPage ? C_ACCENT : C_DGRAY);
  }
  for (int gi=startG; gi<endG; gi++)
    drawSliderRow(gi, R_HDR_H+(gi-startG)*R_ROW_H);

  tft.fillRect(0,R_BTN_Y,SCR_W,R_BTN_H,C_BG2);
  tft.fillRoundRect(20,R_BTN_Y+4,SCR_W-40,R_BTN_H-8,6, isLast?C_GREEN:C_ACCENT);
  centreTextBg(isLast?"  START  ":"  NEXT  ", R_BTN_Y+10, C_WHITE,
               isLast?C_GREEN:C_ACCENT, 1);
}

int sliderXtoVal(int tx) {
  return constrain((int)roundf((float)(tx-R_TRK_L)/R_TRK_W*10.0f), 0, 10);
}

void handleRatingTouch(Point t) {
  if (!t.valid) { g_dragGenre=-1; return; }
  int startG = g_ratingPage*GENRES_PER_PAGE;
  int endG   = min(startG+GENRES_PER_PAGE, NUM_GENRES);
  int npages = (NUM_GENRES+GENRES_PER_PAGE-1)/GENRES_PER_PAGE;

  if (!g_wasTouching && t.y >= R_BTN_Y) {
    if (g_ratingPage < npages-1) {
      g_ratingPage++;
    } else {
      buildSwipeOrder();           // ← smart, profile-aware order
      g_swipeIndex = 0;
      memset(g_reactions,0,sizeof(g_reactions));
      g_state = STATE_SWIPE;
    }
    g_needsDraw = true;
    return;
  }
  for (int gi=startG; gi<endG; gi++) {
    int rowY = R_HDR_H + (gi-startG)*R_ROW_H;
    if (t.y>=rowY && t.y<rowY+R_ROW_H) { if(!g_wasTouching) g_dragGenre=gi; break; }
  }
  if (g_dragGenre>=0) {
    float nv = sliderXtoVal(t.x);
    if (fabsf(nv-g_user[g_dragGenre])>=0.9f) {
      g_user[g_dragGenre]=nv;
      drawRatingScreen(false);
    }
  }
}


// ═════════════════════════════════════════════════════════════
//  SCREEN: SWIPE
// ═════════════════════════════════════════════════════════════
#define SW_IDK_X   110
#define SW_IDK_Y     4
#define SW_IDK_W   100
#define SW_IDK_H    44
#define SW_CARD_X    4
#define SW_CARD_Y   52
#define SW_CARD_W  (SCR_W-8)
#define SW_CARD_H  134
#define SW_BTN_Y   192
#define SW_BTN_H    44
#define SW_DIS_X     4
#define SW_DIS_W   150
#define SW_LIK_X   166
#define SW_LIK_W   150
#define SW_PROG_Y  236

void drawSwipeButtons() {
  tft.fillRoundRect(SW_IDK_X,SW_IDK_Y,SW_IDK_W,SW_IDK_H,8,C_AMBER);
  tft.setTextSize(1); tft.setTextColor(C_BLACK,C_AMBER);
  tft.setCursor(SW_IDK_X+(SW_IDK_W-18)/2, SW_IDK_Y+18); tft.print("IDK");

  tft.fillRoundRect(SW_DIS_X,SW_BTN_Y,SW_DIS_W,SW_BTN_H,8,C_RED);
  tft.setTextSize(1); tft.setTextColor(C_WHITE,C_RED);
  tft.setCursor(SW_DIS_X+30,SW_BTN_Y+18); tft.print("DISLIKE");

  tft.fillRoundRect(SW_LIK_X,SW_BTN_Y,SW_LIK_W,SW_BTN_H,8,C_GREEN);
  tft.setTextSize(1); tft.setTextColor(C_WHITE,C_GREEN);
  tft.setCursor(SW_LIK_X+42,SW_BTN_Y+18); tft.print("LIKE");
}

void drawMovieCard(int movieIdx) {
  tft.fillRoundRect(SW_CARD_X,SW_CARD_Y,SW_CARD_W,SW_CARD_H,10,C_BG2);
  char badge[12];
  snprintf(badge,11,"%d / %d",g_swipeIndex+1,SWIPE_COUNT);
  tft.setTextSize(1); tft.setTextColor(C_DGRAY,C_BG2);
  tft.setCursor((SCR_W-strlen(badge)*6)/2, SW_CARD_Y+8);
  tft.print(badge);

  drawTitle(MOVIES[movieIdx].name, SW_CARD_Y+28, C_WHITE);

  // Top-3 genre pills
  uint8_t top[3]={0,1,2};
  for (int i=0;i<NUM_GENRES;i++) {
    int v=pgm_read_byte(&MOVIES[movieIdx].g[i]);
    for (int j=0;j<3;j++) {
      if (v>pgm_read_byte(&MOVIES[movieIdx].g[top[j]])) {
        for(int k=2;k>j;k--) top[k]=top[k-1];
        top[j]=i; break;
      }
    }
  }
  int px=12, py=SW_CARD_Y+SW_CARD_H-22;
  for (int j=0;j<3;j++) {
    const char* gn=GENRE_NAMES[top[j]];
    int pw=strlen(gn)*6+10;
    tft.fillRoundRect(px,py,pw,16,4,C_BORDER);
    tft.setTextSize(1); tft.setTextColor(C_LGRAY,C_BORDER);
    tft.setCursor(px+5,py+4); tft.print(gn);
    px+=pw+4;
  }
}

void drawProgressBar() {
  tft.fillRect(0,SW_PROG_Y,SCR_W,4,C_BORDER);
  int w=(int)((float)g_swipeIndex/SWIPE_COUNT*SCR_W);
  if(w>0) tft.fillRect(0,SW_PROG_Y,w,4,C_ACCENT);
}

void drawSwipeScreen(bool full) {
  if (full) { tft.fillScreen(C_BG); drawSwipeButtons(); }
  tft.fillRect(SW_CARD_X,SW_CARD_Y,SW_CARD_W,SW_CARD_H,C_BG);
  if (g_swipeIndex<SWIPE_COUNT) drawMovieCard(g_swipeOrder[g_swipeIndex]);
  drawProgressBar();
}

void flashButton(int reaction) {
  if (reaction==1) {
    tft.fillRoundRect(SW_LIK_X,SW_BTN_Y,SW_LIK_W,SW_BTN_H,8,C_WHITE); delay(55);
    tft.fillRoundRect(SW_LIK_X,SW_BTN_Y,SW_LIK_W,SW_BTN_H,8,C_GREEN);
    tft.setTextSize(1); tft.setTextColor(C_WHITE,C_GREEN);
    tft.setCursor(SW_LIK_X+42,SW_BTN_Y+18); tft.print("LIKE");
  } else if (reaction==2) {
    tft.fillRoundRect(SW_DIS_X,SW_BTN_Y,SW_DIS_W,SW_BTN_H,8,C_WHITE); delay(55);
    tft.fillRoundRect(SW_DIS_X,SW_BTN_Y,SW_DIS_W,SW_BTN_H,8,C_RED);
    tft.setTextSize(1); tft.setTextColor(C_WHITE,C_RED);
    tft.setCursor(SW_DIS_X+30,SW_BTN_Y+18); tft.print("DISLIKE");
  } else {
    tft.fillRoundRect(SW_IDK_X,SW_IDK_Y,SW_IDK_W,SW_IDK_H,8,C_WHITE); delay(55);
    tft.fillRoundRect(SW_IDK_X,SW_IDK_Y,SW_IDK_W,SW_IDK_H,8,C_AMBER);
    tft.setTextSize(1); tft.setTextColor(C_BLACK,C_AMBER);
    tft.setCursor(SW_IDK_X+(SW_IDK_W-18)/2,SW_IDK_Y+18); tft.print("IDK");
  }
}

void handleSwipeTouch(Point t) {
  if (!t.valid || g_wasTouching) return;

  int reaction = 0;
  if      (inRect(t,SW_IDK_X,SW_IDK_Y,SW_IDK_W,SW_IDK_H)) reaction=3;
  else if (inRect(t,SW_DIS_X,SW_BTN_Y,SW_DIS_W,SW_BTN_H)) reaction=2;
  else if (inRect(t,SW_LIK_X,SW_BTN_Y,SW_LIK_W,SW_BTN_H)) reaction=1;
  if (!reaction) return;

  flashButton(reaction);

  int curMovie = g_swipeOrder[g_swipeIndex];

  // Score this movie for the math display (triggers mathDisplay_update)
  scoreMovie(curMovie);
  mathDisplay_showScore();

  if (reaction==1) applyLearning(curMovie, 1);
  if (reaction==2) applyLearning(curMovie, 2);
  g_reactions[curMovie] = reaction;

  g_swipeIndex++;
  if (g_swipeIndex>=SWIPE_COUNT) {
    computeResults();
    g_state=STATE_RESULTS; g_resultsScroll=0; g_needsDraw=true;
  } else {
    tft.fillRect(SW_CARD_X,SW_CARD_Y,SW_CARD_W,SW_CARD_H,C_BG);
    drawMovieCard(g_swipeOrder[g_swipeIndex]);
    drawProgressBar();
  }
}


// ═════════════════════════════════════════════════════════════
//  SCREEN: RESULTS
// ═════════════════════════════════════════════════════════════
#define RS_HDR_H   30
#define RS_ROW_H   24
#define RS_BTN_Y   200
#define RS_BTN_H   40
#define RS_LIST_T  RS_HDR_H
#define RS_LIST_B  RS_BTN_Y
#define RS_VISIBLE ((RS_LIST_B-RS_LIST_T)/RS_ROW_H)
#define RS_SHOW    10

void drawResultsScreen(bool full) {
  if (full) {
    tft.fillScreen(C_BG);
    tft.fillRect(0,0,SCR_W,RS_HDR_H,C_BG2);
    tft.fillRect(0,0,SCR_W,3,C_ACCENT);
    centreTextBg("YOUR TOP PICKS",9,C_WHITE,C_BG2,1);
    tft.fillRect(0,RS_BTN_Y,SCR_W,RS_BTN_H,C_BG2);
    tft.fillRoundRect(20,RS_BTN_Y+6,SCR_W-40,RS_BTN_H-12,6,C_ACCENT);
    centreTextBg("START OVER", RS_BTN_Y+14, C_WHITE, C_ACCENT, 1);
  }
  tft.fillRect(0,RS_LIST_T,SCR_W,RS_LIST_B-RS_LIST_T,C_BG);

  int show=min(RS_SHOW,g_resultCount);
  for (int i=0; i<RS_VISIBLE && i+g_resultsScroll<show; i++) {
    int ri=i+g_resultsScroll;
    int y=RS_LIST_T+i*RS_ROW_H;
    int idx=g_results[ri].movieIdx;
    uint16_t bg=(i%2==0)?C_BG:C_BG2;
    tft.fillRect(0,y,SCR_W,RS_ROW_H,bg);

    uint16_t rc=C_DGRAY;
    if(ri==0) rc=C_YELLOW;
    else if(ri==1) rc=0xC618;
    else if(ri==2) rc=0xC460;

    char rb[5]; snprintf(rb,4,"#%-2d",ri+1);
    tft.setTextSize(1); tft.setTextColor(rc,bg);
    tft.setCursor(4,y+8); tft.print(rb);

    char nb[28]; strncpy(nb,MOVIES[idx].name,26); nb[26]=0;
    if(strlen(MOVIES[idx].name)>26){nb[24]='.';nb[25]='.';nb[26]=0;}
    tft.setTextColor(C_WHITE,bg);
    tft.setCursor(28,y+8); tft.print(nb);

    float sc=g_results[ri].score;
    char sb[8]; snprintf(sb,7,"%3.0f%%",sc);
    uint16_t sc_col=sc>=80?C_GREEN:sc>=60?C_YELLOW:C_LGRAY;
    tft.setTextColor(sc_col,bg);
    tft.setCursor(SCR_W-34,y+8); tft.print(sb);

    int bw=(int)(sc/100.0f*(SCR_W-4));
    tft.drawFastHLine(2,y+RS_ROW_H-1,bw,sc_col);
  }

  if(g_resultsScroll>0){
    tft.setTextColor(C_DGRAY,C_BG);
    tft.setCursor(SCR_W/2-3,RS_LIST_T+2); tft.print("^");
  }
  if(g_resultsScroll+RS_VISIBLE<show){
    tft.setTextColor(C_DGRAY,C_BG);
    tft.setCursor(SCR_W/2-3,RS_LIST_B-10); tft.print("v");
  }
}

void handleResultsTouch(Point t) {
  if(!t.valid||g_wasTouching) return;

  // Tap result row → show that movie's math on math display
  if (t.y>=RS_LIST_T && t.y<RS_LIST_B) {
    int row = (t.y-RS_LIST_T)/RS_ROW_H + g_resultsScroll;
    if (row<g_resultCount) {
      scoreMovie(g_results[row].movieIdx);
      mathDisplay_update();
    }
    int show=min(RS_SHOW,g_resultCount);
    int mid=RS_LIST_T+(RS_LIST_B-RS_LIST_T)/2;
    if(t.y<mid){
      if(g_resultsScroll>0){g_resultsScroll--;drawResultsScreen(false);}
    } else {
      if(g_resultsScroll+RS_VISIBLE<show){g_resultsScroll++;drawResultsScreen(false);}
    }
    return;
  }

  if(t.y>=RS_BTN_Y){
    g_ratingPage=0; g_swipeIndex=0;
    memset(g_reactions,0,sizeof(g_reactions));
    for(int i=0;i<NUM_GENRES;i++) g_user[i]=5;
    g_state=STATE_RATING; g_needsDraw=true;
  }
}


// ═════════════════════════════════════════════════════════════
//  CALIBRATION
// ═════════════════════════════════════════════════════════════
#if CALIBRATE_TOUCH
void runCalibration() {
  tft.fillScreen(C_BG);
  centreText("CALIBRATION MODE",40,C_WHITE,2);
  centreText("Touch corners",90,C_LGRAY,1);
  centreText("Watch Serial Monitor",108,C_DGRAY,1);
  while(true){
    if(ts.touched()){
      TS_Point p=ts.getPoint();
      Serial.printf("x=%d y=%d z=%d\n",p.x,p.y,p.z);
      delay(120);
    }
  }
}
#endif


// ═════════════════════════════════════════════════════════════
//  SETUP & LOOP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // ── Primary display (TFT_eSPI, pins from User_Setup.h) ──
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  // ── Touch ────────────────────────────────────────────────
  ts.begin();
  ts.setRotation(1);

  // ── Math display (Adafruit ST7735 on VSPI) ───────────────
  mathSPI.begin(MATH_SCK, -1, MATH_MOSI, MATH_CS);
  mtft.initR(INITR_BLACKTAB);   // black tab = most 1.8" 128×160 modules
  mtft.setRotation(0);           // portrait 128×160
  mtft.fillScreen(M_BLACK);

  // Math display splash
  mtft.fillRect(0,0,MATH_W,20,M_PURPLE);
  mtft.setTextColor(M_WHITE); mtft.setTextSize(1);
  mtft.setCursor(8,6); mtft.print("MATH DISPLAY");
  mtft.setTextColor(M_CYAN); mtft.setCursor(4,26); mtft.print("Weighted Manhattan");
  mtft.setCursor(4,38); mtft.print("Distance Algorithm");
  mtft.setTextColor(M_YELLOW); mtft.setCursor(4,54); mtft.print("D = Sum(W*|u-m|)");
  mtft.setTextColor(M_GREEN);  mtft.setCursor(4,66); mtft.print("S=(1-D/200)*100%");
  mtft.setTextColor(M_DGRAY);  mtft.setCursor(4,82); mtft.print("Rate genres on the");
  mtft.setCursor(4,94); mtft.print("main screen first.");
  mtft.setCursor(4,110); mtft.print("Maths shown here");
  mtft.setCursor(4,122); mtft.print("as AI scores each");
  mtft.setCursor(4,134); mtft.print("movie.");

  // Primary display splash
  tft.fillRect(0,0,SCR_W,3,C_ACCENT);
  centreText("MOVIE AI",    60,C_WHITE,3);
  centreText("Recommendation System",110,C_ACCENT,1);
  centreText("Powered by Mathematics",128,C_DGRAY, 1);
  delay(2500);

#if CALIBRATE_TOUCH
  runCalibration();
#endif

  for(int i=0;i<NUM_GENRES;i++) g_user[i]=5;
  g_state    = STATE_RATING;
  g_needsDraw = true;
  g_mathPageTime = millis();
}

void loop() {
  // Cycle math display pages while idle (no scoring happening)
  if (g_state == STATE_RATING) {
    unsigned long now = millis();
    if (now - g_mathPageTime > MATH_PAGE_MS) {
      g_mathPage = (g_mathPage+1) % 4;
      g_mathPageTime = now;
      // Show a static explanation page when idle
      // (g_calc may have stale data — that's fine for demonstration)
      mathDisplay_update();
    }
  }

  Point t    = getTouch();
  g_touching = t.valid;

  if (g_needsDraw) {
    g_needsDraw = false;
    switch(g_state){
      case STATE_RATING:  drawRatingScreen(true);  break;
      case STATE_SWIPE:   drawSwipeScreen(true);   break;
      case STATE_RESULTS: drawResultsScreen(true); break;
    }
  }

  switch(g_state){
    case STATE_RATING:  handleRatingTouch(t);  break;
    case STATE_SWIPE:   handleSwipeTouch(t);   break;
    case STATE_RESULTS: handleResultsTouch(t); break;
  }

  g_wasTouching = g_touching;
  delay(16);
}
