// iris_layout.h — runtime layout for the LCD widgets.
//
// The web dashboard (http://<pi>:8080) is the source of truth for where the
// widgets sit and which ones are drawn: it writes ~/.config/iris/layout.conf
// and iris_fb picks the file up within a second, no restart and no rebuild.
// Everything here is position/visibility only -- widget CONTENT still comes
// from the same caches and ledgers it always did.
//
// Format is one "key=value" per line (same shape as /tmp/iris_portfolio):
// trivial to parse here, trivial to serialize from Python. Unknown keys are
// ignored so the dashboard can add fields ahead of a rebuild. Missing file =
// built-in defaults, i.e. exactly the hardcoded layout this replaced.
//
// KEEP IN SYNC: server.py's LCD_SCHEMA lists the same keys with their ranges.
#ifndef IRIS_LAYOUT_H
#define IRIS_LAYOUT_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// name in the file  ->  field in IrisLayout
#define LAYOUT_FIELDS(X)                    \
  X("clock.enabled",      clock_enabled)    \
  X("clock.x",            clock_x)          \
  X("clock.y",            clock_y)          \
  X("queue.enabled",      queue_enabled)    \
  X("queue.x",            queue_x)          \
  X("queue.y",            queue_y)          \
  X("queue.rows",         queue_rows)       \
  X("panel.enabled",      panel_enabled)    \
  X("panel.y",            panel_y)          \
  X("portfolio.enabled",  portfolio_enabled)\
  X("portfolio.x",        portfolio_x)      \
  X("orbs.enabled",       orbs_enabled)     \
  X("nightbtn.enabled",   nightbtn_enabled)

typedef struct {
  #define DECL(k, f) float f;
  LAYOUT_FIELDS(DECL)
  #undef DECL
} IrisLayout;

static IrisLayout LAY;                 // the live layout, read by every widget
#define LAYOUT_PATH_REL "/.config/iris/layout.conf"

static inline float lclampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Built-in defaults == the layout that was hardcoded before this file existed.
static void layout_defaults(IrisLayout *L, int W, int H) {
  memset(L, 0, sizeof(*L));
  L->clock_enabled = 1;  L->clock_x = W / 2.0f; L->clock_y = 10;
  L->queue_enabled = 1;  L->queue_x = 12;       L->queue_y = 116; L->queue_rows = 6;
  L->panel_enabled = 1;  L->panel_y = H - 70.0f;
  L->portfolio_enabled = 1; L->portfolio_x = W - 300.0f;
  L->orbs_enabled = 1;
  L->nightbtn_enabled = 1;
}

// Keep every widget somewhere on the panel even if the dashboard sends
// nonsense -- with instant-apply there is no Apply button to undo a typo, so a
// bad value must never push a widget off-screen where it can't be dragged back.
static void layout_clamp(IrisLayout *L, int W, int H) {
  L->clock_x = lclampf(L->clock_x, 60, W - 60);
  L->clock_y = lclampf(L->clock_y, 0, H - 40);
  L->queue_x = lclampf(L->queue_x, 0, W - 120);
  L->queue_y = lclampf(L->queue_y, 40, H - 60);
  L->queue_rows = lclampf((float)(int)(L->queue_rows + 0.5f), 1, 8);
  L->panel_y = lclampf(L->panel_y, 80, H - 12.0f);
  L->portfolio_x = lclampf(L->portfolio_x, 120, W - 60);
}

static void layout_load(IrisLayout *L, const char *path, int W, int H) {
  layout_defaults(L, W, H);
  FILE *f = fopen(path, "r");
  if (f) {
    char line[160];
    while (fgets(line, sizeof(line), f)) {
      if (line[0] == '#' || line[0] == '\n') continue;
      char key[64]; float v;
      if (sscanf(line, "%63[^=]=%f", key, &v) != 2) continue;
      char *e = key + strlen(key);          // tolerate "key = value"
      while (e > key && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
      if (0) {}
      #define TRY(k, fld) else if (!strcmp(key, k)) L->fld = v;
      LAYOUT_FIELDS(TRY)
      #undef TRY
    }
    fclose(f);
  }
  layout_clamp(L, W, H);
}

// Reload only when the file actually changed; call at the existing ~1Hz stats
// cadence, never per frame. Returns 1 if the layout was replaced.
static int layout_poll(IrisLayout *L, const char *path, int W, int H) {
  static time_t last_mtime = 0;
  static off_t  last_size = -1;
  static int    last_present = -1;
  struct stat st;
  int present = stat(path, &st) == 0;
  if (!present) {
    if (last_present == 0) return 0;       // still missing: defaults already in place
    last_present = 0; last_mtime = 0; last_size = -1;
    layout_load(L, path, W, H);
    return 1;
  }
  if (last_present == 1 && st.st_mtime == last_mtime && st.st_size == last_size) return 0;
  last_present = 1; last_mtime = st.st_mtime; last_size = st.st_size;
  layout_load(L, path, W, H);
  return 1;
}

#endif
