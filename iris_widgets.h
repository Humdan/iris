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
  {'%',{0x23,0x13,0x08,0x64,0x62}},
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

    char name[22], when_iso[40], status[16], enabled[8], state[20], sched[40];
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
    // name (bright) + next-run (dim) under it
    wtext(back, W, H, STRIDE, lx + 14, yy, st->names[i], 2, cyan_r, cyan_g, cyan_b);
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
// RUN NOW button: bottom-right inside the panel. Bounds derived from OVL_* so the
// tap handler in iris_fb.c can hit-test the exact same rectangle.
#define NS_BTN_W 120
#define NS_BTN_H 34
#define NS_BTN_X (OVL_X + OVL_W - NS_BTN_W - 12)
#define NS_BTN_Y (OVL_Y + OVL_H - NS_BTN_H - 12)

// Transcript cache: filled by ns_read_transcript() at ~1Hz, drawn every frame.
#define NS_MAX_LINES 12
#define NS_LINE_LEN  60
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

// Cheap, bounded, non-blocking-ish read: open newest run file, seek to the last
// '## Response' section, keep the LAST NS_MAX_LINES non-empty lines (truncated to
// NS_LINE_LEN). Meant to be called ~1Hz off the render cadence, not per frame.
static void ns_read_transcript(NightConsole *nc) {
  nc->have = 0; nc->nlines = 0; nc->running = 0; nc->mtime = 0;
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

// Draw the Night shift CONSOLE overlay: transcript tail + status line + RUN NOW.
// `firing_active` = 1 while showing the brief FIRING feedback after a tap.
static void draw_night_console(uint16_t *back, int W, int H, int STRIDE,
                               const FeedStats *st, int sel,
                               const NightConsole *nc, int firing_active) {
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.55f, dim_g = 0.60f, dim_b = 0.68f;
  const float ok_r = 0.25f, ok_g = 0.85f, ok_b = 0.45f;

  // panel background + border (same style as the plain detail overlay)
  wfill(back, W, H, STRIDE, OVL_X, OVL_Y, OVL_W, OVL_H, 0.04f, 0.07f, 0.10f);
  for (int x = 0; x < OVL_W; x++) {
    if (x < 6 || x > OVL_W - 7) continue;
    wput(back, W, H, STRIDE, OVL_X + x, OVL_Y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, OVL_X + x, OVL_Y + OVL_H - 1, cyan_r, cyan_g, cyan_b);
  }
  for (int y = 0; y < OVL_H; y++) {
    if (y < 6 || y > OVL_H - 7) continue;
    wput(back, W, H, STRIDE, OVL_X, OVL_Y + y, cyan_r, cyan_g, cyan_b);
    wput(back, W, H, STRIDE, OVL_X + OVL_W - 1, OVL_Y + y, cyan_r, cyan_g, cyan_b);
  }

  int px = OVL_X + 16, py = OVL_Y + 12;
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

  // transcript area (small font, one line per row)
  int ty = py + 26;
  int line_h = 11;   // scale-1 glyph is 7px tall + gap
  if (!nc->have || nc->nlines == 0) {
    wtext(back, W, H, STRIDE, px, ty, nc->have ? "(EMPTY RESPONSE)" : "NO RUNS YET",
          1, dim_r, dim_g, dim_b);
  } else {
    // how many lines fit above the button row
    int avail = (NS_BTN_Y - 6 - ty) / line_h;
    if (avail > nc->nlines) avail = nc->nlines;
    if (avail > NS_MAX_LINES) avail = NS_MAX_LINES;
    int first = nc->nlines - avail; if (first < 0) first = 0;
    for (int k = 0; k < avail; k++) {
      wtext(back, W, H, STRIDE, px, ty + k * line_h, nc->lines[first + k], 1,
            dim_r, dim_g, dim_b);
    }
  }

  // RUN NOW button (bottom-right), bordered rectangle
  {
    float br = firing_active ? 0.95f : cyan_r;
    float bg = firing_active ? 0.75f : cyan_g;
    float bb = firing_active ? 0.20f : cyan_b;
    // fill
    wfill(back, W, H, STRIDE, NS_BTN_X, NS_BTN_Y, NS_BTN_W, NS_BTN_H,
          0.08f, 0.12f, 0.16f);
    // border
    for (int x = 0; x < NS_BTN_W; x++) {
      wput(back, W, H, STRIDE, NS_BTN_X + x, NS_BTN_Y, br, bg, bb);
      wput(back, W, H, STRIDE, NS_BTN_X + x, NS_BTN_Y + NS_BTN_H - 1, br, bg, bb);
    }
    for (int y = 0; y < NS_BTN_H; y++) {
      wput(back, W, H, STRIDE, NS_BTN_X, NS_BTN_Y + y, br, bg, bb);
      wput(back, W, H, STRIDE, NS_BTN_X + NS_BTN_W - 1, NS_BTN_Y + y, br, bg, bb);
    }
    const char *label = firing_active ? "FIRING" : "RUN NOW";
    int lw = (int)strlen(label) * 6 * 2;
    wtext(back, W, H, STRIDE, NS_BTN_X + (NS_BTN_W - lw) / 2, NS_BTN_Y + 10,
          label, 2, br, bg, bb);
  }
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
} AgentStats;

// Extract a "key":value from a small JSON blob without a parser.
// Returns pointer just after the ':' for `key`, or NULL.
static const char *json_find(const char *buf, const char *key) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(buf, pat);
  if (!p) return NULL;
  p = strchr(p + strlen(pat), ':');
  return p ? p + 1 : NULL;
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

  // gateway_state.json — read whole small file
  f = fopen("/home/humdan/.hermes/gateway_state.json", "r");
  if (f) {
    char buf[4096]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0;
    fclose(f);
    const char *p;
    if ((p = json_find(buf, "gateway_state"))) a->gateway_up = (strstr(p, "running") && strstr(p, "running") < p + 20);
    // telegram connected?
    const char *tg = strstr(buf, "\"telegram\"");
    if (tg && (p = json_find(tg, "state"))) a->telegram_ok = (strstr(p, "connected") && strstr(p, "connected") < p + 20);
    if ((p = json_find(buf, "active_agents"))) a->active_agents = atoi(p);
    if ((p = json_find(buf, "code_version"))) {
      const char *q = strchr(p, '"');
      if (q) { q++; int i = 0; while (*q && *q != '"' && i < 15) a->version[i++] = *q++; a->version[i] = 0; }
    }
    if ((p = json_find(buf, "updated_at"))) {
      const char *q = strchr(p, '"');
      if (q) { time_t up = parse_iso_utc(q + 1); if (up) { time_t d = time(NULL) - up; a->last_active_s = d < 0 ? 0 : (int)d; } }
    }
  }
}

// Bottom full-width agent panel.
static void draw_agent_panel(uint16_t *back, int W, int H, int STRIDE, const AgentStats *a) {
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

  // AGENTS working
  int ax = 500;
  snprintf(buf, sizeof(buf), "AGENTS %d", a->active_agents);
  wtext(back, W, H, STRIDE, ax, y, buf, 2,
        a->active_agents > 0 ? cyan_r : dim_r, a->active_agents > 0 ? cyan_g : dim_g, a->active_agents > 0 ? cyan_b : dim_b);

  // LAST ACTIVE
  int ls = a->last_active_s;
  if (ls < 60) snprintf(buf, sizeof(buf), "SEEN %dS", ls);
  else if (ls < 3600) snprintf(buf, sizeof(buf), "SEEN %dM", ls / 60);
  else snprintf(buf, sizeof(buf), "SEEN %dH", ls / 3600);
  wtext(back, W, H, STRIDE, ax, y + 24, buf, 1, dim_r, dim_g, dim_b);
}

#endif
