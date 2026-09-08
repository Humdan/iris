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

// ---- stats ----
typedef struct {
  float cpu_pct, temp_c, mem_pct;
  int mem_used_mb, mem_total_mb;
  char clock[16];   // HH:MM:SS
  char date[24];    // e.g. MON SEP 07
} Stats;

static void read_stats(Stats *st) {
  // clock/date
  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  strftime(st->clock, sizeof(st->clock), "%H:%M:%S", tm);
  strftime(st->date, sizeof(st->date), "%a %b %d", tm);
  for (char *p = st->date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; // uppercase

  // CPU % from /proc/loadavg / ncpu
  st->cpu_pct = 0;
  FILE *f = fopen("/proc/loadavg", "r");
  if (f) { float la; if (fscanf(f, "%f", &la) == 1) {
      long nc = sysconf(_SC_NPROCESSORS_ONLN); if (nc < 1) nc = 1;
      st->cpu_pct = 100.0f * la / nc; if (st->cpu_pct > 100) st->cpu_pct = 100;
    } fclose(f); }

  // memory from /proc/meminfo
  st->mem_pct = 0; st->mem_used_mb = st->mem_total_mb = 0;
  f = fopen("/proc/meminfo", "r");
  if (f) {
    long total = 0, avail = 0; char key[64]; long val;
    while (fscanf(f, "%63[^:]: %ld kB\n", key, &val) == 2) {
      if (strcmp(key, "MemTotal") == 0) total = val;
      else if (strcmp(key, "MemAvailable") == 0) avail = val;
    }
    fclose(f);
    if (total > 0) {
      long used = total - avail;
      st->mem_total_mb = (int)(total / 1024);
      st->mem_used_mb = (int)(used / 1024);
      st->mem_pct = 100.0f * used / total;
    }
  }

  // temp from thermal zone (no vcgencmd dependency in-loop)
  st->temp_c = 0;
  f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (f) { long milli; if (fscanf(f, "%ld", &milli) == 1) st->temp_c = milli / 1000.0f; fclose(f); }
}

// Render the widget frame (clock top, stats left) into the back buffer.
static void draw_widgets(uint16_t *back, int W, int H, int STRIDE, const Stats *st) {
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float dim_r = 0.40f, dim_g = 0.45f, dim_b = 0.52f;
  char buf[48];

  // --- top: big clock, centered over the whole width; date under it ---
  int tw = (int)strlen(st->clock) * 6 * 4;      // scale 4
  wtext(back, W, H, STRIDE, (W - tw) / 2, 10, st->clock, 4, cyan_r, cyan_g, cyan_b);
  int dw = (int)strlen(st->date) * 6 * 2;        // scale 2
  wtext(back, W, H, STRIDE, (W - dw) / 2, 44, st->date, 2, dim_r, dim_g, dim_b);

  // --- left column: SYSTEM stats ---
  int lx = 12, ly = 150, lh = 66;               // block start + line spacing
  wtext(back, W, H, STRIDE, lx, ly - 30, "SYSTEM", 2, dim_r, dim_g, dim_b);

  // CPU
  snprintf(buf, sizeof(buf), "CPU  %d%%", (int)(st->cpu_pct + 0.5f));
  wtext(back, W, H, STRIDE, lx, ly, buf, 2, cyan_r, cyan_g, cyan_b);
  wbar(back, W, H, STRIDE, lx, ly + 18, 180, 8, st->cpu_pct);

  // TEMP
  snprintf(buf, sizeof(buf), "TEMP %dC", (int)(st->temp_c + 0.5f));
  wtext(back, W, H, STRIDE, lx, ly + lh, buf, 2, cyan_r, cyan_g, cyan_b);
  // temp bar scaled 0..90C
  wbar(back, W, H, STRIDE, lx, ly + lh + 18, 180, 8, st->temp_c / 90.0f * 100.0f);

  // RAM
  snprintf(buf, sizeof(buf), "RAM  %d%%", (int)(st->mem_pct + 0.5f));
  wtext(back, W, H, STRIDE, lx, ly + 2*lh, buf, 2, cyan_r, cyan_g, cyan_b);
  wbar(back, W, H, STRIDE, lx, ly + 2*lh + 18, 180, 8, st->mem_pct);
  snprintf(buf, sizeof(buf), "%d/%d MB", st->mem_used_mb, st->mem_total_mb);
  wtext(back, W, H, STRIDE, lx, ly + 2*lh + 30, buf, 1, dim_r, dim_g, dim_b);
}

#endif
