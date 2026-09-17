// iris_widgets.h — native framebuffer widgets drawn around the iris orb.
// Compact 5x7 bitmap font + a stats reader (/proc, vcgencmd) + a text/bar
// renderer that writes directly into the RGB565 back buffer. No dependencies.
#ifndef IRIS_WIDGETS_H
#define IRIS_WIDGETS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

// ---- 5x7 bitmap font: ASCII 32..90 (space..Z) + a few punctuation. ----
// Each glyph is 5 columns x 7 rows, stored as 5 bytes (low 7 bits = rows top->bottom).
// Covers: space ! % . / : 0-9 A-Z  (enough for clock, dates, stats labels).
// Missing chars render as blank.
typedef struct { char c; uint8_t col[5]; } Glyph;

// Column-major 7-row glyphs (bit0=top row ... bit6=bottom row).
static const Glyph FONT[] = {
  {' ',{0,0,0,0,0}},
  {'!',{0x00,0x00,0x5F,0x00,0x00}},
  {'$',{0x24,0x54,0xFF,0x54,0x48}},
  {'%',{0x23,0x13,0x08,0x64,0x62}},
  {'+',{0x08,0x08,0x3E,0x08,0x08}},
  {'.',{0x00,0x60,0x60,0x00,0x00}},
  {'/',{0x20,0x10,0x08,0x04,0x02}},
  {':',{0x00,0x36,0x36,0x00,0x00}},
  {'-',{0x08,0x08,0x08,0x08,0x08}},
  {'_',{0x40,0x40,0x40,0x40,0x40}},
  {'C',{0x3E,0x41,0x41,0x41,0x22}},
  {'0',{0x3E,0x51,0x49,0x45,0x3E}},
  {'1',{0x00,0x42,0x7F,0x40,0x00}},
  {'2',{0x42,0x61,0x51,0x49,0x46}},
  {'3',{0x21,0x41,0x45,0x4B,0x31}},
  {'4',{0x18,0x14,0x12,0x7F,0x10}},
  {'5',{0x27,0x45,0x45,0x45,0x39}},
  {'6',{0x3C,0x4A,0x49,0x49,0x30}},
  {'7',{0x01,0x71,0x09,0x05,0x03}},
  {'8',{0x36,0x49,0x49,0x49,0x36}},
  {'9',{0x06,0x49,0x49,0x29,0x1E}},
  {'A',{0x7E,0x11,0x11,0x11,0x7E}},
  {'B',{0x7F,0x49,0x49,0x49,0x36}},
  {'D',{0x7F,0x41,0x41,0x22,0x1C}},
  {'E',{0x7F,0x49,0x49,0x49,0x41}},
  {'F',{0x7F,0x09,0x09,0x09,0x01}},
  {'G',{0x3E,0x41,0x49,0x49,0x7A}},
  {'H',{0x7F,0x08,0x08,0x08,0x7F}},
  {'I',{0x00,0x41,0x7F,0x41,0x00}},
  {'K',{0x7F,0x08,0x14,0x22,0x41}},
  {'L',{0x7F,0x40,0x40,0x40,0x40}},
  {'M',{0x7F,0x02,0x0C,0x02,0x7F}},
  {'N',{0x7F,0x04,0x08,0x10,0x7F}},
  {'O',{0x3E,0x41,0x41,0x41,0x3E}},
  {'P',{0x7F,0x09,0x09,0x09,0x06}},
  {'Q',{0x3E,0x41,0x51,0x21,0x5E}},
  {'R',{0x7F,0x09,0x19,0x29,0x46}},
  {'S',{0x46,0x49,0x49,0x49,0x31}},
  {'T',{0x01,0x01,0x7F,0x01,0x01}},
  {'U',{0x3F,0x40,0x40,0x40,0x3F}},
  {'V',{0x1F,0x20,0x40,0x20,0x1F}},
  {'W',{0x7F,0x20,0x18,0x20,0x7F}},
  {'Y',{0x07,0x08,0x70,0x08,0x07}},
  {'a',{0x20,0x54,0x54,0x54,0x78}}, // lowercase a-z fall back to uppercase glyphs below
};

static const uint8_t *glyph_for(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;  // uppercase-only font
  for (unsigned i = 0; i < sizeof(FONT)/sizeof(FONT[0]); i++)
    if (FONT[i].c == ch) return FONT[i].col;
  return FONT[0].col; // space
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Draw one RGB565 pixel with additive-ish set (opaque) into back buffer.
static inline void wput(uint16_t *back, int W, int H, int STRIDE, int x, int y,
                        float r, float g, float b) {
  if (x < 0 || x >= W || y < 0 || y >= H) return;
  uint16_t rv = (uint16_t)(r * 31.0f); if (rv > 31) rv = 31;
  uint16_t gv = (uint16_t)(g * 63.0f); if (gv > 63) gv = 63;
  uint16_t bv = (uint16_t)(b * 31.0f); if (bv > 31) bv = 31;
  back[y * (STRIDE / 2) + x] = (rv << 11) | (gv << 5) | bv;
}

// Draw text at (x,y) top-left, scale = pixel size per font cell (1..4),
// color rgb in [0,1]. Returns the x advance (end x).
static int wtext(uint16_t *back, int W, int H, int STRIDE, int x, int y,
                 const char *s, int scale, float r, float g, float b) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    const uint8_t *col = glyph_for(*p);
    for (int c = 0; c < 5; c++) {
      for (int row = 0; row < 7; row++) {
        if (col[c] & (1 << row)) {
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++)
              wput(back, W, H, STRIDE, cx + c*scale + sx, y + row*scale + sy, r, g, b);
        }
      }
    }
    cx += 6 * scale; // 5 cols + 1 space
  }
  return cx;
}

// Simple horizontal bar meter: track + fill by pct (0..100), colored by level.
static void wbar(uint16_t *back, int W, int H, int STRIDE, int x, int y,
                 int w, int h, float pct) {
  float p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  // track
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      wput(back, W, H, STRIDE, x+xx, y+yy, 0.10f, 0.12f, 0.16f);
  // fill color: green < 60 < amber < 85 < red
  float fr, fg, fb;
  if (p >= 85) { fr=0.95f; fg=0.30f; fb=0.30f; }
  else if (p >= 60) { fr=0.95f; fg=0.75f; fb=0.20f; }
  else { fr=0.25f; fg=0.85f; fb=0.45f; }
  int fw = (int)(w * p / 100.0f);
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < fw; xx++)
      wput(back, W, H, STRIDE, x+xx, y+yy, fr, fg, fb);
}

// ---- cron job queue (replaces tool-call feed) ----
#define CRON_MAX 6
typedef struct {
  char clock[16];
  char date[24];
  char names[CRON_MAX][22];   // job name (truncated)
  char when[CRON_MAX][18];    // next-run "in 3h" / "7AM" style
  int  status[CRON_MAX];      // 0 ok/scheduled, 1 error, 2 paused/disabled
  int  njobs;
  // extended fields for the tap-to-inspect detail overlay
  char schedule[CRON_MAX][40];    // human schedule display, e.g. "every day at 7am"
  char laststat[CRON_MAX][16];    // last_status text: ok / error / (none)
  char nextiso[CRON_MAX][40];     // raw next_run_at ISO string
  int  is_ns[CRON_MAX];           // 1 if this is a night-shift task (drawn red in queue)
} FeedStats;

// Grab the string value of "key":"...": into out (bounded). Returns 1 on hit.
static int json_str(const char *start, const char *end, const char *key,
                    char *out, int outsz) {
  char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(start, pat);
  if (!p || p >= end) { out[0] = 0; return 0; }
  p = strchr(p + strlen(pat), ':'); if (!p) { out[0]=0; return 0; }
  p++; while (*p == ' ') p++;
  if (*p != '"') { out[0]=0; return 0; }   // null / non-string
  p++;
  int i = 0;
  while (*p && *p != '"' && i < outsz - 1) out[i++] = *p++;
  out[i] = 0;
  return 1;
}

// Turn an ISO8601-with-offset next_run into a compact "in 2H" / "in 15M" label.
static void when_label(const char *iso, char *out, int outsz) {
  if (!iso[0]) { snprintf(out, outsz, "-"); return; }
  struct tm tm; memset(&tm, 0, sizeof(tm));
  int off_h = 0, off_m = 0; char sign = '+';
  int n = sscanf(iso, "%d-%d-%dT%d:%d:%d%c%d:%d",
                 &tm.tm_year,&tm.tm_mon,&tm.tm_mday,&tm.tm_hour,&tm.tm_min,&tm.tm_sec,
                 &sign,&off_h,&off_m);
  if (n < 6) { snprintf(out, outsz, "-"); return; }
  tm.tm_year -= 1900; tm.tm_mon -= 1;
  time_t local = timegm(&tm);   // treat parsed wall-time as UTC...
  if (n >= 8) { long off = (off_h*3600 + off_m*60) * (sign=='-'?1:-1); local += off; } // ...then correct by offset -> real UTC
  long d = (long)(local - time(NULL));
  if (d < 0) { snprintf(out, outsz, "DUE"); return; }
  if (d < 3600) snprintf(out, outsz, "IN %ldM", d/60);
  else if (d < 86400) snprintf(out, outsz, "IN %ldH", d/3600);
  else snprintf(out, outsz, "IN %ldD", d/86400);
}

// True if a job's FULL name denotes a night-shift task: exactly the recurring
// "Night shift" job, or an overnight one-shot (name contains "overnight"). Matched
// on the full name at parse time (before display-truncation) so "TokenTimes redesign
// ... one-shot overnight" is caught, and matched EXACTLY for "Night shift" so unrelated
// jobs like "LED red midnight" are NOT (no fuzzy "night" substring).
static int ns_is_nightshift(const char *name) {
  if (!name) return 0;
  if (strcmp(name, "Night shift") == 0) return 1;
  for (const char *p = name; *p; p++) {
    if ((p[0]=='o'||p[0]=='O') && (p[1]=='v'||p[1]=='V') && (p[2]=='e'||p[2]=='E') &&
        (p[3]=='r'||p[3]=='R') && (p[4]=='n'||p[4]=='N') && (p[5]=='i'||p[5]=='I') &&
        (p[6]=='g'||p[6]=='G') && (p[7]=='h'||p[7]=='H') && (p[8]=='t'||p[8]=='T'))
      return 1;
  }
  return 0;
}

static void read_stats(FeedStats *st) {
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  strftime(st->clock, sizeof(st->clock), "%H:%M:%S", tm);
  strftime(st->date, sizeof(st->date), "%a %b %d", tm);
  for (char *p = st->date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;

  st->njobs = 0;
  FILE *f = fopen("/home/humdan/.hermes/cron/jobs.json", "r");
  if (!f) return;
  static char buf[16384];
  size_t nlen = fread(buf, 1, sizeof(buf) - 1, f); buf[nlen] = 0; fclose(f);

  // iterate job objects by locating each "id" then bounding to the next "id".
  const char *p = buf;
  while (st->njobs < CRON_MAX) {
    const char *idp = strstr(p, "\"id\"");
    if (!idp) break;
    const char *nextid = strstr(idp + 4, "\"id\"");
    const char *end = nextid ? nextid : buf + nlen;

    char name[128], when_iso[40], status[16], enabled[8], state[20], sched[40];
    json_str(idp, end, "name", name, sizeof(name));
    json_str(idp, end, "next_run_at", when_iso, sizeof(when_iso));
    json_str(idp, end, "last_status", status, sizeof(status));
    json_str(idp, end, "state", state, sizeof(state));
    json_str(idp, end, "schedule_display", sched, sizeof(sched));
    // enabled is a bareword true/false — scan manually
    int is_enabled = 1;
    const char *ep = strstr(idp, "\"enabled\"");
    if (ep && ep < end) { const char *c = strchr(ep, ':'); if (c && strstr(c, "false") && strstr(c, "false") < c + 8) is_enabled = 0; }

    strncpy(st->names[st->njobs], name, 21); st->names[st->njobs][21] = 0;
    when_label(when_iso, st->when[st->njobs], sizeof(st->when[st->njobs]));
    strncpy(st->schedule[st->njobs], sched[0] ? sched : "-", 39); st->schedule[st->njobs][39] = 0;
    strncpy(st->laststat[st->njobs], status[0] ? status : "NONE", 15); st->laststat[st->njobs][15] = 0;
    strncpy(st->nextiso[st->njobs], when_iso, 39); st->nextiso[st->njobs][39] = 0;
    int s = 0;
    if (!is_enabled || (state[0] && strncmp(state, "paused", 6) == 0)) s = 2;
    else if (strncmp(status, "error", 5) == 0) s = 1;
    st->status[st->njobs] = s;
    st->is_ns[st->njobs] = ns_is_nightshift(name);

    st->njobs++;
    p = end;
  }
}

// --- queue panel layout constants (shared with touch hit-testing in iris_fb.c) ---
#define QUEUE_LX      12     // left column x
#define QUEUE_LY      116    // first row baseline y
#define QUEUE_ROW_H   44     // pixels per job row
#define QUEUE_PANEL_W 260    // touch-active width of the left column
#define QUEUE_VISIBLE 6      // max rows drawn at once

// Render clock (top) + cron job queue (left column).
// scroll: number of jobs scrolled off the top (already clamped by caller).
// sel:    selected job index, or -1 if none (drawn highlighted).
static void draw_widgets(uint16_t *back, int W, int H, int STRIDE, const FeedStats *st,
                         int scroll, int sel) {
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.40f, dim_g = 0.45f, dim_b = 0.52f;
  const float ok_r = 0.25f, ok_g = 0.85f, ok_b = 0.45f;
  const float err_r = 0.95f, err_g = 0.30f, err_b = 0.30f;

  // --- top: big clock centered; date under it ---
  int tw = (int)strlen(st->clock) * 6 * 4;
  wtext(back, W, H, STRIDE, (W - tw) / 2, 10, st->clock, 4, cyan_r, cyan_g, cyan_b);
  int dw = (int)strlen(st->date) * 6 * 2;
  wtext(back, W, H, STRIDE, (W - dw) / 2, 44, st->date, 2, dim_r, dim_g, dim_b);

  // --- left column: cron QUEUE ---
  int lx = QUEUE_LX, ly = QUEUE_LY;
  char hdr[24]; snprintf(hdr, sizeof(hdr), "QUEUE %d", st->njobs);
  wtext(back, W, H, STRIDE, lx, ly - 28, hdr, 2, dim_r, dim_g, dim_b);

  if (st->njobs == 0) {
    wtext(back, W, H, STRIDE, lx, ly, "NO JOBS", 2, dim_r, dim_g, dim_b);
    return;
  }
  if (scroll < 0) scroll = 0;
  if (scroll > st->njobs - 1) scroll = st->njobs - 1;
  int shown = 0;
  for (int i = scroll; i < st->njobs && shown < QUEUE_VISIBLE; i++, shown++) {
    int yy = ly + shown * QUEUE_ROW_H;
    // selected-row highlight band
    if (i == sel) {
      for (int by = -6; by < QUEUE_ROW_H - 8; by++)
        for (int bx = -4; bx < QUEUE_PANEL_W - QUEUE_LX; bx++)
          wput(back, W, H, STRIDE, lx + bx, yy + by, 0.10f, 0.16f, 0.22f);
    }
    // status dot: green ok, red error, amber paused
    float dr, dg, db;
    if (st->status[i] == 1) { dr=err_r; dg=err_g; db=err_b; }
    else if (st->status[i] == 2) { dr=0.95f; dg=0.75f; db=0.20f; }
    else { dr=ok_r; dg=ok_g; db=ok_b; }
    for (int a = 0; a < 8; a++) for (int b = 0; b < 8; b++)
      if ((a-4)*(a-4)+(b-4)*(b-4) <= 16) wput(back, W, H, STRIDE, lx+a, yy+2+b, dr, dg, db);
    // name: RED for night-shift tasks (the recurring "Night shift" job + overnight one-shots),
    // otherwise the normal cyan. Lets the queue signal at a glance which jobs run overnight.
    int is_ns = ns_is_nightshift(st->names[i]);
    float nr = is_ns ? err_r : cyan_r;
    float ng = is_ns ? err_g : cyan_g;
    float nb = is_ns ? err_b : cyan_b;
    wtext(back, W, H, STRIDE, lx + 14, yy, st->names[i], 2, nr, ng, nb);
    wtext(back, W, H, STRIDE, lx + 14, yy + 18, st->when[i], 1, dim_r, dim_g, dim_b);
  }
  // scroll affordance: little up/down chevrons if more jobs exist off-screen
  if (scroll > 0)
    wtext(back, W, H, STRIDE, QUEUE_PANEL_W - 20, ly - 12, "-", 2, dim_r, dim_g, dim_b);
  if (scroll + QUEUE_VISIBLE < st->njobs)
    wtext(back, W, H, STRIDE, QUEUE_PANEL_W - 20, ly + (QUEUE_VISIBLE - 1) * QUEUE_ROW_H + 20, "_", 2, dim_r, dim_g, dim_b);
}

// Solid filled rectangle (opaque set) into the back buffer.
static void wfill(uint16_t *back, int W, int H, int STRIDE, int x, int y, int w, int h,
                  float r, float g, float b) {
  for (int yy = 0; yy < h; yy++)
    for (int xx = 0; xx < w; xx++)
      wput(back, W, H, STRIDE, x + xx, y + yy, r, g, b);
}

// Detail overlay for a tapped job, drawn over the orb area. Returns nothing;
// caller decides when to show it (sel >= 0). Panel bounds are fixed so the
// touch handler can dismiss on taps outside them.
#define OVL_X 300
#define OVL_Y 130
#define OVL_W 470
#define OVL_H 210

// ---- Night shift CONSOLE mode ----
// The "Night shift" job (id b7a26b64f480) gets a special overlay: a live-tailing
// transcript of its newest run output plus a RUN NOW tap-zone button.
#define NS_JOB_NAME  "Night shift"
#define NS_JOB_ID    "b7a26b64f480"
#define NS_OUT_DIR   "/home/humdan/.hermes/cron/output/" NS_JOB_ID
// RUN NOW button: bottom-right inside the SMALL (tap-to-inspect) panel. Bounds
// derived from OVL_* so the tap handler in iris_fb.c can hit-test the same rect.
#define NS_BTN_W 120
#define NS_BTN_H 34
#define NS_BTN_X (OVL_X + OVL_W - NS_BTN_W - 12)
#define NS_BTN_Y (OVL_Y + OVL_H - NS_BTN_H - 12)

// LARGE night-shift console: dominates the screen so the live step log is the
// focus while the shift runs. Spans the area right of the queue column, below
// the clock, above the agent panel. 800x480 screen.
#define NSC_X 288
#define NSC_Y 66
#define NSC_W 504
#define NSC_H 366
// RUN NOW button inside the big console (bottom-right).
#define NSC_BTN_W 130
#define NSC_BTN_H 38
#define NSC_BTN_X (NSC_X + NSC_W - NSC_BTN_W - 14)
#define NSC_BTN_Y (NSC_Y + NSC_H - NSC_BTN_H - 14)

// Transcript cache: filled by ns_read_transcript() at ~1Hz, drawn every frame.
// Sized to fill the large night-shift console panel (majority of the screen).
#define NS_MAX_LINES 34
#define NS_LINE_LEN  76
typedef struct {
  char   lines[NS_MAX_LINES][NS_LINE_LEN];
  int    nlines;
  int    have;          // 1 if a run file was found
  time_t mtime;         // newest output file mtime
  int    running;       // fresh output (< ~20s) => job appears to be running
} NightConsole;

// Return the newest *.md file path in the Night shift output dir into `out`.
// Returns 1 on success, 0 if dir missing / empty.
static int ns_newest_file(char *out, int outsz) {
  DIR *d = opendir(NS_OUT_DIR);
  if (!d) return 0;
  struct dirent *de;
  char best[512] = {0};
  time_t best_m = 0;
  while ((de = readdir(d)) != NULL) {
    const char *nm = de->d_name;
    size_t l = strlen(nm);
    if (l < 4 || strcmp(nm + l - 3, ".md") != 0) continue;
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", NS_OUT_DIR, nm);
    struct stat sb;
    if (stat(full, &sb) != 0) continue;
    if (sb.st_mtime >= best_m) { best_m = sb.st_mtime; snprintf(best, sizeof(best), "%s", full); }
  }
  closedir(d);
  if (!best[0]) return 0;
  snprintf(out, outsz, "%s", best);
  return 1;
}

// Live step log written by the iris plugin during a night-shift run. Its mtime
// is the truest "is it running now?" signal: it updates at session start and on
// every tool call, whereas the final *.md report is only written when the run
// ENDS. So both the running-state probe and the transcript reader key off it.
#define NS_LIVE_LOG "/tmp/iris_ns_live.log"

// Lightweight running-state probe: a night-shift run is "live" when the live
// step log was touched within the last ~25s (session start + each tool call
// bump it). Falls back to the newest *.md mtime so a run that wrote its report
// but no live log (e.g. plugin disabled) still briefly registers. Meant to be
// called ~1Hz off the render cadence, never per frame.
#define NS_RUNNING_FRESH_S 25
static int ns_running_check(void) {
  struct stat lsb;
  if (stat(NS_LIVE_LOG, &lsb) == 0 && lsb.st_size > 0)
    if ((time(NULL) - lsb.st_mtime) < NS_RUNNING_FRESH_S) return 1;
  char path[512];
  if (!ns_newest_file(path, sizeof(path))) return 0;
  struct stat sb;
  if (stat(path, &sb) != 0) return 0;
  return (time(NULL) - sb.st_mtime) < NS_RUNNING_FRESH_S;
}

// Cheap, bounded, non-blocking-ish read: prefer the LIVE step log written by
// the iris plugin during a night-shift run (tool-by-tool progress); fall back to
// the newest run's final report if no live log exists. Meant to be called ~1Hz
// off the render cadence, not per frame.
static void ns_read_transcript(NightConsole *nc) {
  nc->have = 0; nc->nlines = 0; nc->running = 0; nc->mtime = 0;

  // 1) Live step log: if present, it's the real-time trace — always prefer it.
  struct stat lsb;
  if (stat(NS_LIVE_LOG, &lsb) == 0 && lsb.st_size > 0) {
    FILE *lf = fopen(NS_LIVE_LOG, "r");
    if (lf) {
      nc->have = 1;
      nc->mtime = lsb.st_mtime;
      nc->running = (time(NULL) - lsb.st_mtime) < NS_RUNNING_FRESH_S;
      char ring[NS_MAX_LINES][NS_LINE_LEN];
      int rn = 0, rhead = 0;
      char raw[1024];
      while (fgets(raw, sizeof(raw), lf)) {
        size_t l = strlen(raw);
        while (l > 0 && (raw[l-1] == '\n' || raw[l-1] == '\r')) raw[--l] = 0;
        if (raw[0] == 0) continue;
        char *slot = ring[rhead];
        int i = 0;
        for (const char *p = raw; *p && i < NS_LINE_LEN - 1; p++) {
          char c = *p; if (c == '\t') c = ' '; slot[i++] = c;
        }
        slot[i] = 0;
        rhead = (rhead + 1) % NS_MAX_LINES;
        if (rn < NS_MAX_LINES) rn++;
      }
      fclose(lf);
      int start = (rhead - rn + NS_MAX_LINES) % NS_MAX_LINES;
      for (int k = 0; k < rn; k++)
        memcpy(nc->lines[k], ring[(start + k) % NS_MAX_LINES], NS_LINE_LEN);
      nc->nlines = rn;
      return;
    }
  }

  // 2) Fall back to the newest final report's "## Response" tail.
  char path[512];
  if (!ns_newest_file(path, sizeof(path))) return;
  struct stat sb;
  if (stat(path, &sb) == 0) {
    nc->mtime = sb.st_mtime;
    nc->running = (time(NULL) - sb.st_mtime) < 20;
  }
  FILE *f = fopen(path, "r");
  if (!f) return;
  nc->have = 1;

  // ring buffer of the last NS_MAX_LINES lines that appear AFTER '## Response'
  char ring[NS_MAX_LINES][NS_LINE_LEN];
  int  rn = 0, rhead = 0;
  int  after = 0;
  char raw[1024];
  while (fgets(raw, sizeof(raw), f)) {
    // strip trailing newline / CR
    size_t l = strlen(raw);
    while (l > 0 && (raw[l-1] == '\n' || raw[l-1] == '\r')) raw[--l] = 0;
    if (!after) {
      if (strncmp(raw, "## Response", 11) == 0) after = 1;
      continue;
    }
    if (raw[0] == 0) continue;   // skip blank lines to pack the panel
    // store truncated line into the ring
    char *slot = ring[rhead];
    int i = 0;
    for (const char *p = raw; *p && i < NS_LINE_LEN - 1; p++) {
      char c = *p;
      if (c == '\t') c = ' ';
      slot[i++] = c;
    }
    slot[i] = 0;
    rhead = (rhead + 1) % NS_MAX_LINES;
    if (rn < NS_MAX_LINES) rn++;
  }
  fclose(f);

  // emit ring in chronological order into nc->lines
  int start = (rhead - rn + NS_MAX_LINES) % NS_MAX_LINES;
  for (int k = 0; k < rn; k++) {
    int idx = (start + k) % NS_MAX_LINES;
    memcpy(nc->lines[k], ring[idx], NS_LINE_LEN);
  }
  nc->nlines = rn;
}

// Draw the Night shift CONSOLE overlay: a LARGE panel dominating the screen with
// the live step log as the focus, plus a status line and RUN NOW button.
// `firing_active` = 1 while showing the brief FIRING feedback after a tap.
static void draw_night_console(uint16_t *back, int W, int H, int STRIDE,
                               const FeedStats *st, int sel,
                               const NightConsole *nc, int firing_active) {
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.55f, dim_g = 0.60f, dim_b = 0.68f;
  const float log_r = 0.55f, log_g = 0.85f, log_b = 0.70f;   // live-log green tint
  const float ok_r = 0.25f, ok_g = 0.85f, ok_b = 0.45f;

  // large panel background + border
  wfill(back, W, H, STRIDE, NSC_X, NSC_Y, NSC_W, NSC_H, 0.03f, 0.06f, 0.09f);
  for (int x = 0; x < NSC_W; x++) {
    if (x < 6 || x > NSC_W - 7) continue;
    wput(back, W, H, STRIDE, NSC_X + x, NSC_Y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, NSC_X + x, NSC_Y + NSC_H - 1, cyan_r, cyan_g, cyan_b);
  }
  for (int y = 0; y < NSC_H; y++) {
    if (y < 6 || y > NSC_H - 7) continue;
    wput(back, W, H, STRIDE, NSC_X, NSC_Y + y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, NSC_X + NSC_W - 1, NSC_Y + y, cyan_r, cyan_g, cyan_b);
  }

  int px = NSC_X + 16, py = NSC_Y + 12;
  // title
  wtext(back, W, H, STRIDE, px, py, st->names[sel], 2, cyan_r, cyan_g, cyan_b);

  // status line: running vs idle+last-run time
  {
    char sbuf[48];
    float sr, sg, sb;
    if (firing_active) { snprintf(sbuf, sizeof(sbuf), "FIRING..."); sr=0.95f; sg=0.75f; sb=0.20f; }
    else if (!nc->have) { snprintf(sbuf, sizeof(sbuf), "NO RUNS YET"); sr=dim_r; sg=dim_g; sb=dim_b; }
    else if (nc->running) { snprintf(sbuf, sizeof(sbuf), "RUNNING..."); sr=ok_r; sg=ok_g; sb=ok_b; }
    else {
      time_t d = time(NULL) - nc->mtime;
      if (d < 60) snprintf(sbuf, sizeof(sbuf), "IDLE - %lldS AGO", (long long)d);
      else if (d < 3600) snprintf(sbuf, sizeof(sbuf), "IDLE - %lldM AGO", (long long)(d/60));
      else if (d < 86400) snprintf(sbuf, sizeof(sbuf), "IDLE - %lldH AGO", (long long)(d/3600));
      else snprintf(sbuf, sizeof(sbuf), "IDLE - %lldD AGO", (long long)(d/86400));
      sr=dim_r; sg=dim_g; sb=dim_b;
    }
    wtext(back, W, H, STRIDE, px + 12 * 6 * 2, py + 2, sbuf, 1, sr, sg, sb);
  }

  // "LIVE LOG" header + separator; the log itself gets the rest of the panel.
  int cy = py + 26;
  wtext(back, W, H, STRIDE, px, cy, "LIVE LOG", 1, cyan_r, cyan_g, cyan_b);
  cy += 14;
  for (int x = px; x < NSC_X + NSC_W - 16; x++) wput(back, W, H, STRIDE, x, cy, 0.14f, 0.18f, 0.22f);
  cy += 6;

  // transcript area — the live step log tail; fills down to the button row.
  int ty = cy;
  int line_h = 11;   // scale-1 glyph is 7px tall + gap
  if (!nc->have || nc->nlines == 0) {
    wtext(back, W, H, STRIDE, px, ty, nc->have ? "(NO STEPS YET)" : "NO RUNS YET",
          1, dim_r, dim_g, dim_b);
  } else {
    // how many lines fit above the button row
    int avail = (NSC_BTN_Y - 8 - ty) / line_h;
    if (avail > nc->nlines) avail = nc->nlines;
    if (avail > NS_MAX_LINES) avail = NS_MAX_LINES;
    int first = nc->nlines - avail; if (first < 0) first = 0;
    for (int k = 0; k < avail; k++) {
      wtext(back, W, H, STRIDE, px, ty + k * line_h, nc->lines[first + k], 1,
            log_r, log_g, log_b);
    }
  }

  // RUN NOW button (bottom-right of the big console), bordered rectangle
  {
    float br = firing_active ? 0.95f : cyan_r;
    float bg = firing_active ? 0.75f : cyan_g;
    float bb = firing_active ? 0.20f : cyan_b;
    // fill
    wfill(back, W, H, STRIDE, NSC_BTN_X, NSC_BTN_Y, NSC_BTN_W, NSC_BTN_H,
          0.08f, 0.12f, 0.16f);
    // border
    for (int x = 0; x < NSC_BTN_W; x++) {
      wput(back, W, H, STRIDE, NSC_BTN_X + x, NSC_BTN_Y, br, bg, bb);
      wput(back, W, H, STRIDE, NSC_BTN_X + x, NSC_BTN_Y + NSC_BTN_H - 1, br, bg, bb);
    }
    for (int y = 0; y < NSC_BTN_H; y++) {
      wput(back, W, H, STRIDE, NSC_BTN_X, NSC_BTN_Y + y, br, bg, bb);
      wput(back, W, H, STRIDE, NSC_BTN_X + NSC_BTN_W - 1, NSC_BTN_Y + y, br, bg, bb);
    }
    const char *label = firing_active ? "FIRING" : "RUN NOW";
    int lw = (int)strlen(label) * 6 * 2;
    wtext(back, W, H, STRIDE, NSC_BTN_X + (NSC_BTN_W - lw) / 2, NSC_BTN_Y + 12,
          label, 2, br, bg, bb);
  }
}

// --- Night shift RUNNING ambient indicator (additive; drawn only while running) ---
// Red-tint the ENTIRE back buffer in one pass. RGB565 decode -> tint -> re-encode.
// `pulse` is 0..1 (gentle sine) so the wash breathes; kept subtle so the sphere,
// widgets and overlays stay readable. Called at most once per frame, only while
// the night shift is running (skipped entirely when idle => zero cost).
static void ns_tint_red(uint16_t *back, int W, int H, int STRIDE, float pulse) {
  // wash strength: base + small pulse. R lifted, G/B slightly damped.
  const float rk = 0.10f + 0.05f * pulse;   // add to red
  const float dk = 0.88f - 0.04f * pulse;   // multiply g/b (dim toward red mood)
  const int spx = STRIDE / 2;
  for (int y = 0; y < H; y++) {
    uint16_t *row = back + y * spx;
    for (int x = 0; x < W; x++) {
      uint16_t px = row[x];
      int r5 = (px >> 11) & 0x1F;
      int g6 = (px >> 5) & 0x3F;
      int b5 = px & 0x1F;
      int nr = (int)(r5 + rk * 31.0f);      if (nr > 31) nr = 31;
      int ng = (int)(g6 * dk);              if (ng > 63) ng = 63;
      int nb = (int)(b5 * dk);              if (nb > 31) nb = 31;
      row[x] = (uint16_t)((nr << 11) | (ng << 5) | nb);
    }
  }
}

// Full-width top banner: a stronger red bar with centered white label. Sits at the
// very top strip; `pulse` (0..1) gently modulates the bar brightness so it reads live.
#define NS_BANNER_H 32
static void draw_night_banner(uint16_t *back, int W, int H, int STRIDE, float pulse, const char *label) {
  float br = 0.55f + 0.20f * pulse;   // bar red
  wfill(back, W, H, STRIDE, 0, 0, W, NS_BANNER_H, br, 0.05f, 0.06f);
  // thin bright underline for definition
  wfill(back, W, H, STRIDE, 0, NS_BANNER_H - 2, W, 2, 0.95f, 0.35f, 0.35f);
  if (!label) label = "NIGHT SHIFT RUNNING";
  int scale = 3;
  int tw = (int)strlen(label) * 6 * scale;   // wtext advance = 6px/char * scale
  int tx = (W - tw) / 2;
  int ty = (NS_BANNER_H - 7 * scale) / 2;
  float lw = 0.95f + 0.05f * pulse;
  wtext(back, W, H, STRIDE, tx, ty, label, scale, lw, lw, lw);
}

static void draw_job_detail(uint16_t *back, int W, int H, int STRIDE, const FeedStats *st, int sel) {
  if (sel < 0 || sel >= st->njobs) return;
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.55f, dim_g = 0.60f, dim_b = 0.68f;
  const float lbl_r = 0.40f, lbl_g = 0.45f, lbl_b = 0.52f;

  // panel background (dark) with a 1px cyan border and rounded-ish corners
  wfill(back, W, H, STRIDE, OVL_X, OVL_Y, OVL_W, OVL_H, 0.04f, 0.07f, 0.10f);
  for (int x = 0; x < OVL_W; x++) {
    if (x < 6 || x > OVL_W - 7) continue; // corner nick for rounded look
    wput(back, W, H, STRIDE, OVL_X + x, OVL_Y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, OVL_X + x, OVL_Y + OVL_H - 1, cyan_r, cyan_g, cyan_b);
  }
  for (int y = 0; y < OVL_H; y++) {
    if (y < 6 || y > OVL_H - 7) continue;
    wput(back, W, H, STRIDE, OVL_X, OVL_Y + y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, OVL_X + OVL_W - 1, OVL_Y + y, cyan_r, cyan_g, cyan_b);
  }

  int px = OVL_X + 18, py = OVL_Y + 16;
  // status dot mirrors the row color
  float dr, dg, db;
  if (st->status[sel] == 1) { dr=0.95f; dg=0.30f; db=0.30f; }
  else if (st->status[sel] == 2) { dr=0.95f; dg=0.75f; db=0.20f; }
  else { dr=0.25f; dg=0.85f; db=0.45f; }
  for (int a = 0; a < 12; a++) for (int b = 0; b < 12; b++)
    if ((a-6)*(a-6)+(b-6)*(b-6) <= 36) wput(back, W, H, STRIDE, px+a, py+2+b, dr, dg, db);

  wtext(back, W, H, STRIDE, px + 20, py, st->names[sel], 2, cyan_r, cyan_g, cyan_b);

  py += 44;
  wtext(back, W, H, STRIDE, px, py, "SCHEDULE", 1, lbl_r, lbl_g, lbl_b);
  wtext(back, W, H, STRIDE, px, py + 12, st->schedule[sel], 2, dim_r, dim_g, dim_b);

  py += 44;
  wtext(back, W, H, STRIDE, px, py, "LAST STATUS", 1, lbl_r, lbl_g, lbl_b);
  {
    float sr = dim_r, sg = dim_g, sb = dim_b;
    if (strncmp(st->laststat[sel], "ok", 2) == 0) { sr=0.25f; sg=0.85f; sb=0.45f; }
    else if (strncmp(st->laststat[sel], "error", 5) == 0) { sr=0.95f; sg=0.30f; sb=0.30f; }
    wtext(back, W, H, STRIDE, px, py + 12, st->laststat[sel], 2, sr, sg, sb);
  }

  py += 44;
  wtext(back, W, H, STRIDE, px, py, "NEXT RUN", 1, lbl_r, lbl_g, lbl_b);
  wtext(back, W, H, STRIDE, px, py + 12, st->when[sel], 2, cyan_r, cyan_g, cyan_b);

  wtext(back, W, H, STRIDE, OVL_X + OVL_W - 96, OVL_Y + OVL_H - 16, "TAP OUT", 1, lbl_r, lbl_g, lbl_b);
}

// ---- agent / Hermes stats ----
typedef struct {
  float activity;        // 0..1 from /tmp/iris_state
  int thinking;          // activity high / state == thinking
  int gateway_up;        // gateway_state == running
  int telegram_ok;       // telegram platform connected
  int active_agents;     // subagents currently working
  int last_active_s;     // seconds since gateway updated_at
  char version[16];      // hermes code_version
  char task_label[24];   // current tool/task from /tmp/iris_task (e.g., SHELL, SEARCH, BROWSER, CODE, EDIT)
} AgentStats;

// Extract a "key":value from a small JSON blob without a parser.
// Returns pointer just after the ':' for `key`, or NULL.
// If `end` is non-NULL, stops searching at that position.
static const char *json_find(const char *buf, const char *key, const char *end) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return NULL;
  if (end && p >= end) return NULL;
  p = strchr(p + strlen(pat), ':');
  return p ? p + 1 : NULL;
}

// Overload for simple case without end bound
static const char *json_find2(const char *buf, const char *key) {
  return json_find(buf, key, NULL);
}

// Parse an ISO8601 UTC timestamp ("2026-09-08T01:14:41...") to epoch seconds.
static time_t parse_iso_utc(const char *s) {
  struct tm tm; memset(&tm, 0, sizeof(tm));
  if (sscanf(s, "%d-%d-%dT%d:%d:%d",
             &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
  tm.tm_year -= 1900; tm.tm_mon -= 1;
  return timegm(&tm);
}

static void read_agent_stats(AgentStats *a) {
  memset(a, 0, sizeof(*a));
  strcpy(a->version, "?");

  // activity level from iris state file
  FILE *f = fopen("/tmp/iris_state", "r");
  if (f) {
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf) - 1, f)) {
      if (strncmp(buf, "thinking", 8) == 0) { a->activity = 1.0f; a->thinking = 1; }
      else if (strncmp(buf, "idle", 4) == 0) { a->activity = 0.0f; }
      else { a->activity = strtof(buf, NULL); a->thinking = a->activity > 0.5f; }
    }
    fclose(f);
  }

  // current task label from /tmp/iris_task (refreshed ~1Hz from Hermes plugin)
  f = fopen("/tmp/iris_task", "r");
  if (f) {
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf) - 1, f)) {
      // strip newline
      char *n = strchr(buf, '\n');
      if (n) *n = 0;
      strncpy(a->task_label, buf, sizeof(a->task_label) - 1);
      a->task_label[sizeof(a->task_label) - 1] = 0;
    }
    fclose(f);
  } else {
    a->task_label[0] = 0;
  }

  // gateway_state.json — read whole small file
  f = fopen("/home/humdan/.hermes/gateway_state.json", "r");
  if (f) {
    char buf[4096]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
    fclose(f);
    const char *p;
    if ((p = json_find2(buf, "gateway_state"))) a->gateway_up = (strstr(p, "running") && strstr(p, "running") < p + 20);
    // telegram connected?
    const char *tg = strstr(buf, "\"telegram\"");
    if (tg && (p = json_find2(tg, "state"))) a->telegram_ok = (strstr(p, "connected") && strstr(p, "connected") < p + 20);
    if ((p = json_find2(buf, "active_agents"))) a->active_agents = atoi(p);
    if ((p = json_find2(buf, "code_version"))) {
      const char *q = strchr(p, '"');
      if (q) { q++; int i = 0; while (*q && *q != '"' && i < 15) a->version[i++] = *q++; a->version[i] = 0; }
    }
    if ((p = json_find2(buf, "updated_at"))) {
      const char *q = strchr(p, '"');
      if (q) { time_t up = parse_iso_utc(q + 1); if (up) { time_t d = time(NULL) - up; a->last_active_s = d < 0 ? 0 : (int)d; } }
    }
  }
}

// ---- Paper-portfolio snapshot, refreshed via a tiny cache file the C loop reads
// cheaply (~1Hz, same cadence as AgentStats). The cache is written by a small
// wrapper script (see scripts/portfolio-cache.sh) that shells ledger.py status
// -- keeps the render loop free of subprocess spawns. Format (one line,
// pipe-delimited, all it needs): "<total_value>|<pnl_pct>".
typedef struct {
  int have;         // 1 if cache file present and parsed
  float total;       // total portfolio value in USD
  float pnl_pct;      // % change since $1000 start
} PortfolioStats;


// ---- Session orbs: one orb per open Hermes session ----
#define SESSION_MAX 32
#define SESSION_FILE "/tmp/iris_sessions.json"

typedef struct {
  char label[24];
  char short_label[8];   // abbreviated for display (up to 7 chars + null)
  char source[16];       // "cli", "telegram", "cron"
  char color[16];        // "cyan", "magenta", "amber"
  int is_primary;        // 1 for most recent session
  float idle_seconds;
  float orbit_angle;     // current orbital angle for smooth motion
  float orbit_radius;    // distance from center
  float pulse_phase;     // individual pulse phase
} SessionOrb;

typedef struct {
  int count;
  SessionOrb orbs[SESSION_MAX];
  time_t last_read;
} SessionStats;

static void read_session_stats(SessionStats *s) {
  // Only re-read every ~2 seconds
  time_t now = time(NULL);
  if (s->count > 0 && now - s->last_read < 2) return;

  FILE *f = fopen(SESSION_FILE, "r");
  if (!f) { s->count = 0; return; }

  static char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);

  // Simple JSON parsing for the sessions array
  const char *p = strstr(buf, "\"sessions\"");
  if (!p) { s->count = 0; return; }
  p = strchr(p, '[');
  if (!p) { s->count = 0; return; }

  s->count = 0;
  p++; // skip '['
  while (*p && s->count < SESSION_MAX) {
    // Find next object
    while (*p && *p != '{') p++;
    if (!*p) break;
    const char *obj_start = p;
    int brace = 0;
    while (*p) {
      if (*p == '{') brace++;
      else if (*p == '}') {
        brace--;
        if (brace == 0) { p++; break; }
      }
      p++;
    }
    const char *obj_end = p;

    // Parse this object
    SessionOrb *orb = &s->orbs[s->count];
    memset(orb, 0, sizeof(*orb));

    const char *v;
    if ((v = json_find(obj_start, "label", obj_end))) {
      const char *q = strchr(v, '"');
      if (q) { q++; int i = 0; while (*q && *q != '"' && i < 23) orb->label[i++] = *q++; orb->label[i] = 0; }
    }
    // Create short label (max 7 chars) for display
    orb->short_label[0] = 0;
    if (orb->label[0]) {
      int src = 0;
      // Skip emoji prefix (📁 = F0 9F 93 81, 🌿 = F0 9F 8C BF)
      if ((unsigned char)orb->label[0] == 0xF0) {
        src = 4;
        while (orb->label[src] == ' ') src++;
      }
      for (int k = 0; k < 7 && orb->label[src + k]; k++) orb->short_label[k] = orb->label[src + k];
      orb->short_label[7] = 0;
    }
    if (!orb->short_label[0]) {
      // Fallback to source initial
      orb->short_label[0] = orb->source[0] ? orb->source[0] : '?';
      orb->short_label[1] = 0;
    }
    if ((v = json_find(obj_start, "source", obj_end))) {
      const char *q = strchr(v, '"');
      if (q) { q++; int i = 0; while (*q && *q != '"' && i < 15) orb->source[i++] = *q++; orb->source[i] = 0; }
    }
    if ((v = json_find(obj_start, "color", obj_end))) {
      const char *q = strchr(v, '"');
      if (q) { q++; int i = 0; while (*q && *q != '"' && i < 15) orb->color[i++] = *q++; orb->color[i] = 0; }
    }
    if ((v = json_find(obj_start, "idle_seconds", obj_end))) {
      orb->idle_seconds = strtof(v, NULL);
    }
    // is_primary is implied by position (first = primary)
    orb->is_primary = (s->count == 0);
    
    // Initialize orbital parameters for smooth animation
    // Distribute orbs in a ring with varying radii
    float base_angle = (s->count * 2.0f * 3.14159f / (s->count > 0 ? s->count : 1)) - 3.14159f / 2.0f;
    orb->orbit_angle = base_angle;
    orb->orbit_radius = 140.0f + (s->count % 3) * 20.0f;  // 140, 160, 180 px rings
    orb->pulse_phase = s->count * 0.7f;

    // Skip to next object
    while (*p && *p != '{') p++;
    s->count++;
  }
  s->last_read = now;
}

static void draw_session_orbs(uint16_t *back, int W, int H, int STRIDE,
                              const SessionStats *s, float global_time) {
  if (s->count == 0) return;

  // Draw orbs around the main sphere area
  // Position them in a ring around the sphere center
  float cx = 560.0f, cy = 240.0f;  // matches orb center in iris_fb.c

  // First pass: draw connections between orbs of the same source
  // This creates a subtle "neural network" effect linking related sessions
  for (int i = 0; i < s->count; i++) {
    for (int j = i + 1; j < s->count; j++) {
      const SessionOrb *orb1 = &s->orbs[i];
      const SessionOrb *orb2 = &s->orbs[j];
      
      // Only connect orbs of the same source type (cli, telegram, cron)
      if (strcmp(orb1->source, orb2->source) != 0) continue;
      
      float orbit_speed1 = orb1->is_primary ? 0.12f : 0.06f;
      float cur_angle1 = orb1->orbit_angle + global_time * orbit_speed1;
      float px1 = cx + cosf(cur_angle1) * orb1->orbit_radius;
      float py1 = cy + sinf(cur_angle1) * orb1->orbit_radius;
      
      float orbit_speed2 = orb2->is_primary ? 0.12f : 0.06f;
      float cur_angle2 = orb2->orbit_angle + global_time * orbit_speed2;
      float px2 = cx + cosf(cur_angle2) * orb2->orbit_radius;
      float py2 = cy + sinf(cur_angle2) * orb2->orbit_radius;
      
      float dx = px2 - px1;
      float dy = py2 - py1;
      float dist = sqrtf(dx*dx + dy*dy);
      
      // Only draw connections if orbs are reasonably close
      if (dist > 120.0f) continue;
      
      // Connection color matches the source
      float r = 0.15f, g = 0.5f, b = 0.7f; // cyan default
      if (strcmp(orb1->color, "magenta") == 0) { r = 0.7f; g = 0.2f; b = 0.5f; }
      else if (strcmp(orb1->color, "amber") == 0) { r = 0.7f; g = 0.5f; b = 0.1f; }
      
      // Pulsing connection strength
      float conn_pulse = 0.3f + 0.4f * sinf(global_time * 1.5f + (i + j) * 0.5f);
      float alpha = conn_pulse * (1.0f - dist / 120.0f);
      
      // Draw line using Bresenham-like steps
      int steps = (int)(dist / 2.0f);
      for (int step = 0; step <= steps; step++) {
        float t = step / (float)steps;
        float lx = px1 + dx * t;
        float ly = py1 + dy * t;
        int ix = (int)lx, iy = (int)ly;
        if (ix >= 0 && ix < W && iy >= 0 && iy < H) {
          uint16_t *row = back + iy * (STRIDE / 2);
          uint16_t px_val = row[ix];
          uint16_t cr = (px_val >> 11) & 0x1F;
          uint16_t cg = (px_val >> 5) & 0x3F;
          uint16_t cb = px_val & 0x1F;
          uint16_t rv = (uint16_t)(clampf(r * alpha * 0.5f, 0, 1) * 31.0f);
          uint16_t gv = (uint16_t)(clampf(g * alpha * 0.5f, 0, 1) * 63.0f);
          uint16_t bv = (uint16_t)(clampf(b * alpha * 0.5f, 0, 1) * 31.0f);
          cr = (cr + rv) > 0x1F ? 0x1F : cr + rv;
          cg = (cg + gv) > 0x3F ? 0x3F : cg + gv;
          cb = (cb + bv) > 0x1F ? 0x1F : cb + bv;
          row[ix] = (cr << 11) | (cg << 5) | cb;
        }
      }
    }
  }

  // Second pass: draw the orbs themselves
  for (int i = 0; i < s->count; i++) {
    const SessionOrb *orb = &s->orbs[i];

    // Orbital motion - primary moves faster, others slower
    float orbit_speed = orb->is_primary ? 0.12f : 0.06f;
    float cur_angle = orb->orbit_angle + global_time * orbit_speed;

    float px = cx + cosf(cur_angle) * orb->orbit_radius;
    float py = cy + sinf(cur_angle) * orb->orbit_radius;

    // Pulsing size - smoother with individual phase
    float pulse = 1.0f + 0.3f * sinf(global_time * 2.0f + orb->pulse_phase);
    if (orb->is_primary) pulse *= 1.3f;
    float rad = 5.0f * pulse;  // Larger base radius

    // Color based on source
    float r = 0.2f, g = 0.8f, b = 1.0f; // default cyan
    if (strcmp(orb->color, "magenta") == 0) { r = 1.0f; g = 0.4f; b = 0.8f; }
    else if (strcmp(orb->color, "amber") == 0) { r = 1.0f; g = 0.7f; b = 0.2f; }

    // Brightness - primary is brighter, idle sessions dimmer
    float idle_factor = orb->idle_seconds > 300 ? 0.4f : (orb->idle_seconds > 60 ? 0.7f : 1.0f);
    float bright = orb->is_primary ? 1.0f : 0.65f;
    bright *= idle_factor;
    r *= bright; g *= bright; b *= bright;

    // Draw filled circle with additive blending (like particles)
    int x0 = (int)(px - rad), x1 = (int)(px + rad + 1);
    int y0 = (int)(py - rad), y1 = (int)(py + rad + 1);
    float inv = 1.0f / (rad * rad + 0.1f);
    for (int y = y0; y <= y1; y++) {
      if (y < 0 || y >= H) continue;
      uint16_t *row = back + y * (STRIDE / 2);
      for (int x = x0; x <= x1; x++) {
        if (x < 0 || x >= W) continue;
        float dx = x - px, dy = y - py, d2 = (dx * dx + dy * dy) * inv;
        if (d2 > 1.0f) continue;
        float k = (1.0f - d2); k *= k;
        // Additive blend onto existing pixel
        uint16_t px_val = row[x];
        uint16_t cr = (px_val >> 11) & 0x1F;
        uint16_t cg = (px_val >> 5) & 0x3F;
        uint16_t cb = px_val & 0x1F;
        uint16_t rv = (uint16_t)(clampf(r * k, 0, 1) * 31.0f);
        uint16_t gv = (uint16_t)(clampf(g * k, 0, 1) * 63.0f);
        uint16_t bv = (uint16_t)(clampf(b * k, 0, 1) * 31.0f);
        cr = (cr + rv) > 0x1F ? 0x1F : cr + rv;
        cg = (cg + gv) > 0x3F ? 0x3F : cg + gv;
        cb = (cb + bv) > 0x1F ? 0x1F : cb + bv;
        row[x] = (cr << 11) | (cg << 5) | cb;
      }
    }

    // Draw activity particles for recently active orbs (idle < 60s)
    if (orb->idle_seconds < 60 && (rand() % 10) < 3) {
      // Emit a few particles from the orb
      for (int p = 0; p < 2; p++) {
        float angle = (rand() / (float)RAND_MAX) * 6.2831853f;
        float speed = 10.0f + (rand() / (float)RAND_MAX) * 20.0f;
        float part_x = px + cosf(angle) * (rad + speed * 0.1f);
        float part_y = py + sinf(angle) * (rad + speed * 0.1f);
        int ipx = (int)part_x, ipy = (int)part_y;
        if (ipx >= 0 && ipx < W && ipy >= 0 && ipy < H) {
          wput(back, W, H, STRIDE, ipx, ipy, r * 0.8f, g * 0.8f, b * 0.8f);
        }
      }
    }

    // Primary orb gets a subtle glow ring
    if (orb->is_primary) {
      float glow_rad = rad + 3.0f;
      int gx0 = (int)(px - glow_rad), gx1 = (int)(px + glow_rad + 1);
      int gy0 = (int)(py - glow_rad), gy1 = (int)(py + glow_rad + 1);
      float glow_inv = 1.0f / (glow_rad * glow_rad + 0.1f);
      for (int y = gy0; y <= gy1; y++) {
        if (y < 0 || y >= H) continue;
        uint16_t *row = back + y * (STRIDE / 2);
        for (int x = gx0; x <= gx1; x++) {
          if (x < 0 || x >= W) continue;
          float dx = x - px, dy = y - py, d2 = (dx * dx + dy * dy) * glow_inv;
          if (d2 > 1.0f || d2 < 0.6f) continue;  // Ring only
          float k = (1.0f - d2) * 0.3f;  // Subtle glow
          uint16_t px_val = row[x];
          uint16_t cr = (px_val >> 11) & 0x1F;
          uint16_t cg = (px_val >> 5) & 0x3F;
          uint16_t cb = px_val & 0x1F;
          uint16_t rv = (uint16_t)(clampf(r * k, 0, 1) * 31.0f);
          uint16_t gv = (uint16_t)(clampf(g * k, 0, 1) * 63.0f);
          uint16_t bv = (uint16_t)(clampf(b * k, 0, 1) * 31.0f);
          cr = (cr + rv) > 0x1F ? 0x1F : cr + rv;
          cg = (cg + gv) > 0x3F ? 0x3F : cg + gv;
          cb = (cb + bv) > 0x1F ? 0x1F : cb + bv;
          row[x] = (cr << 11) | (cg << 5) | cb;
        }
      }
    }

    // Draw label for ALL orbs - use float position for smooth text
    if (orb->short_label[0]) {
      float label_x = px + rad + 6.0f;
      float label_y = py - 4.0f;
      
      // Ensure label stays on screen
      int label_w = strlen(orb->short_label) * 6;  // 6px per char at scale 1
      if (label_x + label_w > W - 10) label_x = px - rad - label_w - 6.0f;
      if (label_y < 10) label_y = 10;
      if (label_y > H - 20) label_y = H - 20;

      // Draw label with a subtle background for readability
      int lbl_x = (int)label_x, lbl_y = (int)label_y;
      // Small background rect
      for (int yy = -2; yy < 9; yy++) {
        for (int xx = -2; xx < label_w + 2; xx++) {
          int x = lbl_x + xx, y = lbl_y + yy;
          if (x >= 0 && x < W && y >= 0 && y < H) {
            uint16_t *row = back + y * (STRIDE / 2);
            uint16_t px_val = row[x];
            // Only darken if there's something there
            if (px_val != 0) {
              uint16_t cr = (px_val >> 11) & 0x1F;
              uint16_t cg = (px_val >> 5) & 0x3F;
              uint16_t cb = px_val & 0x1F;
              cr = cr / 3; cg = cg / 3; cb = cb / 3;
              row[x] = (cr << 11) | (cg << 5) | cb;
            }
          }
        }
      }
      
      wtext(back, W, H, STRIDE, lbl_x, lbl_y,
            orb->short_label, 1, r, g, b);
      
      // Draw idle indicator for non-primary orbs (small dot)
      if (!orb->is_primary && orb->idle_seconds > 60) {
        int dot_x = lbl_x + label_w + 4;
        int dot_y = lbl_y + 3;
        if (dot_x < W && dot_y < H) {
          float idle_r = orb->idle_seconds > 300 ? 0.9f : 0.9f;
          float idle_g = orb->idle_seconds > 300 ? 0.3f : 0.7f;
          float idle_b = 0.2f;
          for (int dy = -2; dy <= 2; dy++) {
            for (int dx = -2; dx <= 2; dx++) {
              if (dx*dx + dy*dy <= 4) {
                int x = dot_x + dx, y = dot_y + dy;
                if (x >= 0 && x < W && y >= 0 && y < H) {
                  wput(back, W, H, STRIDE, x, y, idle_r, idle_g, idle_b);
                }
              }
            }
          }
        }
      }
    }
  }
}

static void read_portfolio_stats(PortfolioStats *p) {
  memset(p, 0, sizeof(*p));
  FILE *f = fopen("/tmp/iris_portfolio", "r");
  if (!f) return;
  char buf[64] = {0};
  if (fgets(buf, sizeof(buf) - 1, f)) {
    float total = 0, pct = 0;
    if (sscanf(buf, "%f|%f", &total, &pct) == 2) {
      p->have = 1; p->total = total; p->pnl_pct = pct;
    }
  }
  fclose(f);
}

// Bottom full-width agent panel.
static void draw_agent_panel(uint16_t *back, int W, int H, int STRIDE, const AgentStats *a, int ns_manual, const PortfolioStats *pf) {
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.40f, dim_g = 0.45f, dim_b = 0.52f;
  const float ok_r = 0.25f, ok_g = 0.85f, ok_b = 0.45f;
  const float bad_r = 0.95f, bad_g = 0.30f, bad_b = 0.30f;
  char buf[48];
  int y = H - 58;   // strip top

  // separator line
  for (int x = 12; x < W - 12; x++) wput(back, W, H, STRIDE, x, y - 8, 0.14f, 0.16f, 0.20f);

  // HERMES label + version
  int x = 12;
  x = wtext(back, W, H, STRIDE, x, y, "HERMES", 2, dim_r, dim_g, dim_b);
  snprintf(buf, sizeof(buf), "V%s", a->version);
  wtext(back, W, H, STRIDE, x + 6, y + 2, buf, 1, dim_r, dim_g, dim_b);

  // STATE: THINKING / IDLE + activity bar
  const char *stxt = a->thinking ? "THINKING" : "IDLE";
  float sr = a->thinking ? cyan_r : dim_r, sg = a->thinking ? cyan_g : dim_g, sb = a->thinking ? cyan_b : dim_b;
  wtext(back, W, H, STRIDE, 12, y + 24, stxt, 2, sr, sg, sb);
  wbar(back, W, H, STRIDE, 120, y + 26, 120, 8, a->activity * 100.0f);

  // GATEWAY dot
  int gx = 300;
  float gr = a->gateway_up ? ok_r : bad_r, gg = a->gateway_up ? ok_g : bad_g, gb = a->gateway_up ? ok_b : bad_b;
  for (int yy = 0; yy < 10; yy++) for (int xx = 0; xx < 10; xx++)
    if ((xx-5)*(xx-5)+(yy-5)*(yy-5) <= 25) wput(back, W, H, STRIDE, gx+xx, y+2+yy, gr, gg, gb);
  wtext(back, W, H, STRIDE, gx + 16, y, "GATEWAY", 2, dim_r, dim_g, dim_b);
  wtext(back, W, H, STRIDE, gx + 16, y + 22, a->telegram_ok ? "TELEGRAM OK" : "TG DOWN", 1,
        a->telegram_ok ? ok_r : bad_r, a->telegram_ok ? ok_g : bad_g, a->telegram_ok ? ok_b : bad_b);

  // AGENTS / TASK TAG (right column): task label (cyan) when active; else agents or IDLE
  int ax = 500;
  if (a->task_label[0]) {
    snprintf(buf, sizeof(buf), "%s", a->task_label);
    wtext(back, W, H, STRIDE, ax, y, buf, 2, cyan_r, cyan_g, cyan_b);
    wtext(back, W, H, STRIDE, ax, y + 24, "WORKING", 1, dim_r, dim_g, dim_b);
  } else {
    snprintf(buf, sizeof(buf), "AGENTS %d", a->active_agents);
    wtext(back, W, H, STRIDE, ax, y, buf, 2,
          a->active_agents > 0 ? cyan_r : dim_r, a->active_agents > 0 ? cyan_g : dim_g, a->active_agents > 0 ? cyan_b : dim_b);
    // If truly idle (no agents, no thinking), show IDLE as sub-label in dim
    const char *sub = (a->active_agents == 0 && !a->thinking) ? "IDLE" : "";
    wtext(back, W, H, STRIDE, ax, y + 24, sub, 1, dim_r, dim_g, dim_b);
  }

  // PORTFOLIO (paper-trading fund) — far right column, own space so it never
  // collides with AGENTS/TASK. Green above $1000 start, red below, dim if
  // the cache hasn't been written yet (script/ledger not running).
  int px = 650;
  if (pf->have) {
    char pfbuf[24];
    snprintf(pfbuf, sizeof(pfbuf), "$%d", (int)(pf->total + 0.5f));
    float pr = pf->pnl_pct > 0 ? ok_r : pf->pnl_pct < 0 ? bad_r : dim_r;
    float pg = pf->pnl_pct > 0 ? ok_g : pf->pnl_pct < 0 ? bad_g : dim_g;
    float pb = pf->pnl_pct > 0 ? ok_b : pf->pnl_pct < 0 ? bad_b : dim_b;
    wtext(back, W, H, STRIDE, px, y, pfbuf, 2, pr, pg, pb);
    char pctbuf[16];
    snprintf(pctbuf, sizeof(pctbuf), "%s%d.%d%%", pf->pnl_pct >= 0 ? "+" : "-",
             abs((int)pf->pnl_pct), abs((int)(pf->pnl_pct * 10)) % 10);
    wtext(back, W, H, STRIDE, px, y + 24, pctbuf, 1, pr, pg, pb);
  } else {
    wtext(back, W, H, STRIDE, px, y, "PORTFOLIO", 1, dim_r, dim_g, dim_b);
    wtext(back, W, H, STRIDE, px, y + 24, "NO DATA", 1, dim_r, dim_g, dim_b);
  }

  // NIGHT MODE pill (manual toggle) - visible pill in agent panel, tappable zone below it
  int npill_x = 460, npill_y = H - 38;
  int npill_w = 72, npill_h = 18;
  float np_r = ns_manual ? 0.95f : 0.3f, np_g = 0.15f, np_b = 0.15f;
  wfill(back, W, H, STRIDE, npill_x, npill_y, npill_w, npill_h, np_r, np_g, np_b);
  wtext(back, W, H, STRIDE, npill_x + 6, npill_y + 3, ns_manual ? "NIGHT ON" : "NIGHT OFF", 1, 0.95f, 0.95f, 0.95f);

  // LAST ACTIVE
  int ls = a->last_active_s;
  if (ls < 60) snprintf(buf, sizeof(buf), "SEEN %dS", ls);
  else if (ls < 3600) snprintf(buf, sizeof(buf), "SEEN %dM", ls / 60);
  else snprintf(buf, sizeof(buf), "SEEN %dH", ls / 3600);
  wtext(back, W, H, STRIDE, ax, y + 24, buf, 1, dim_r, dim_g, dim_b);
}

#endif
