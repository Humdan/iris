// iris_services.h — the SERVICES widget in the left column.
//
// Lists the services that passed their ping on the latest heartbeat. The
// pinging is done by iris_heartbeat.py (user unit iris-heartbeat.service),
// which rewrites SVC_FILE every beat; this side only reads it at the 1 Hz
// stats cadence, so the render loop never blocks on a network probe.
//
// COMPACT (default): header with an up/total count, then the active services
// as a two-column grid. EXPANDED: one row per service with its round trip,
// active first, then the ones that failed in red with the reason. When the
// cron queue is expanded instead, this folds to its header line.
#ifndef IRIS_SERVICES_H
#define IRIS_SERVICES_H

#include <ctype.h>
#include "iris_widgets.h"

#define SVC_FILE "/tmp/iris_services"
#define SVC_MAX  16

typedef struct {
  char name[12];
  int  ok;
  int  ms;
  char detail[41];
} SvcItem;

typedef struct {
  SvcItem s[SVC_MAX];
  int n, nup;
  int stale;        // no file, or no beat for 3 intervals: trust nothing in it
} SvcStats;

static void read_services(SvcStats *sv) {
  memset(sv, 0, sizeof(*sv));
  sv->stale = 1;
  FILE *f = fopen(SVC_FILE, "r");
  if (!f) return;
  char line[160];
  long ts = 0; int interval = 15;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#') { sscanf(line, "# ts=%ld interval=%d", &ts, &interval); continue; }
    if (sv->n >= SVC_MAX) continue;
    SvcItem *it = &sv->s[sv->n];
    char name[32] = {0}, detail[64] = {0};
    int ok = 0, ms = 0;
    if (sscanf(line, "%31[^|]|%d|%d|%63[^\n]", name, &ok, &ms, detail) < 3) continue;
    snprintf(it->name, sizeof(it->name), "%s", name);
    snprintf(it->detail, sizeof(it->detail), "%s", detail);
    for (char *p = it->detail; *p; p++) *p = (char)toupper((unsigned char)*p);  // font is A-Z only
    it->ok = ok; it->ms = ms;
    if (ok) sv->nup++;
    sv->n++;
  }
  fclose(f);
  if (interval < 1) interval = 15;
  sv->stale = !ts || (time(NULL) - ts) > 3L * interval;
}

static void svc_dot(uint16_t *back, int W, int H, int STRIDE, int x, int y, int d,
                    float r, float g, float b) {
  int rr = d / 2;
  for (int a = 0; a < d; a++) for (int c = 0; c < d; c++)
    if ((a - rr) * (a - rr) + (c - rr) * (c - rr) <= rr * rr) wput(back, W, H, STRIDE, x + c, y + a, r, g, b);
}

static void draw_services(uint16_t *back, int W, int H, int STRIDE, const SvcStats *sv) {
  const float dim_r = 0.40f, dim_g = 0.45f, dim_b = 0.52f;
  const float cyan_r = 0.35f, cyan_g = 0.75f, cyan_b = 0.95f;
  const float ok_r = 0.25f, ok_g = 0.85f, ok_b = 0.45f;
  const float bad_r = 0.95f, bad_g = 0.30f, bad_b = 0.30f;
  const float amb_r = 0.95f, amb_g = 0.75f, amb_b = 0.20f;
  const int lx = QUEUE_LX, hy = svc_header_y(), bottom = LEFT_COL_BOTTOM;
  const int expanded = EXP.services;
  char buf[48];

  // --- header: SERVICES  up/total  [+] ---
  int x = wtext(back, W, H, STRIDE, lx, hy, "SERVICES", 2, dim_r, dim_g, dim_b);
  if (sv->stale) {
    wtext(back, W, H, STRIDE, x + 10, hy + 4, "NO HEARTBEAT", 1, bad_r, bad_g, bad_b);
  } else {
    snprintf(buf, sizeof(buf), "%d/%d", sv->nup, sv->n);
    int all = sv->nup == sv->n;
    wtext(back, W, H, STRIDE, x + 10, hy + 4, buf, 1,
          all ? ok_r : amb_r, all ? ok_g : amb_g, all ? ok_b : amb_b);
  }
  draw_expand_box(back, W, H, STRIDE, hy - 2, expanded);
  if (EXP.queue && !expanded) return;          // folded under an expanded queue
  if (sv->stale) return;                       // a dead heartbeat proves nothing is up

  int y = hy + SVC_HDR_H + 2;
  if (!expanded) {
    // COMPACT: active services only, two columns of dot + name
    const int colw = (QUEUE_PANEL_W - lx) / 2, rowh = 16;
    int maxrows = (bottom - y) / rowh;
    if (maxrows < 1) return;
    int slots = maxrows * 2, k = 0, drawn = 0;
    if (sv->nup == 0) {
      wtext(back, W, H, STRIDE, lx, y, "NONE RESPONDING", 1, bad_r, bad_g, bad_b);
      return;
    }
    for (int i = 0; i < sv->n; i++) {
      if (!sv->s[i].ok) continue;
      if (k == slots - 1 && sv->nup > slots) {   // last slot says what didn't fit
        snprintf(buf, sizeof(buf), "+%d MORE", sv->nup - drawn);
        wtext(back, W, H, STRIDE, lx + (k % 2) * colw + 10, y + (k / 2) * rowh, buf, 1, dim_r, dim_g, dim_b);
        break;
      }
      int cx = lx + (k % 2) * colw, cy = y + (k / 2) * rowh;
      svc_dot(back, W, H, STRIDE, cx, cy, 7, ok_r, ok_g, ok_b);
      wtext(back, W, H, STRIDE, cx + 12, cy, sv->s[i].name, 1, cyan_r, cyan_g, cyan_b);
      k++; drawn++;
    }
    return;
  }

  // EXPANDED: every service, active first (with round trip), then the failures
  const int rowh = 24;
  int row = 0, maxrows = (bottom - y) / rowh;
  for (int pass = 1; pass >= 0; pass--) {
    for (int i = 0; i < sv->n && row < maxrows; i++) {
      const SvcItem *it = &sv->s[i];
      if (it->ok != pass) continue;
      int yy = y + row * rowh;
      float r = pass ? ok_r : bad_r, g = pass ? ok_g : bad_g, b = pass ? ok_b : bad_b;
      svc_dot(back, W, H, STRIDE, lx, yy + 3, 8, r, g, b);
      wtext(back, W, H, STRIDE, lx + 14, yy, it->name, 2,
            pass ? cyan_r : bad_r, pass ? cyan_g : bad_g, pass ? cyan_b : bad_b);
      if (pass) snprintf(buf, sizeof(buf), "%dMS", it->ms);
      else      snprintf(buf, sizeof(buf), "%.10s", it->detail[0] ? it->detail : "DOWN");
      wtext_r(back, W, H, STRIDE, QUEUE_PANEL_W - 8, yy + 4, buf, 1,
              pass ? dim_r : bad_r, pass ? dim_g : bad_g, pass ? dim_b : bad_b);
      row++;
    }
  }
  if (row < sv->n)
    wtext(back, W, H, STRIDE, lx + 14, y + row * rowh, "...", 1, dim_r, dim_g, dim_b);
}

#endif
