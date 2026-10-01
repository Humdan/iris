// iris_organism.h — the "digital organism" orb.
//
// An electric-blue, see-through holographic sphere: nested shells of circuit-like
// traces around a hot, spinning core, with sweeping arcs and radial filaments.
// It behaves like something alive:
//   - each shell faces the viewer and spins at its own rate (concentric rings +
//     radial runs, like a dial of gyroscopes), wobbling slowly on its axis
//   - traces are born (drawn in with a bright growing tip), live, and fade;
//     more of them are alive, and they turn over faster, when Iris is busy
//   - every heartbeat sends a ripple of light outward through the shells
//   - data pulses travel along traces; filaments flicker; the core breathes
//   - the whole lattice undulates (low-frequency radial wobble)
//
// Rendering: everything is splatted (bilinear, sub-pixel — no shimmer) into a
// float HDR buffer around the orb, a cheap quarter-res bloom is added, and a
// tone-map LUT turns overlapping light electric blue -> white-hot, then RGB565.

#include <math.h>
#include <stdint.h>
#include <string.h>

#define ORG_N       448                 // HDR buffer size (square, centered on the orb)
#define ORG_BS      4                   // bloom downsample factor
#define ORG_BN      (ORG_N / ORG_BS)
#define ORG_EXTENT  1.10f               // max on-screen radius in units of R (wobble + filaments + perspective)

#define ORG_SHELLS     4
#define ORG_SEGS       400
#define ORG_SEG_PTS    72
#define ORG_FILS       44
#define ORG_CORE_RINGS 5
#define ORG_ARCS       3
#define ORG_WAVES      6
#define ORG_EMBERS     96
#define ORG_LUT        2048
#define ORG_LUT_MAX    8.0f

static float org_acc[ORG_N * ORG_N * 3];
static float org_bl[ORG_BN * ORG_BN * 3], org_bl2[ORG_BN * ORG_BN * 3];
static uint16_t org_lut_r[ORG_LUT], org_lut_g[ORG_LUT], org_lut_b[ORG_LUT];

static uint32_t org_rng = 0x9E3779B9u;
static inline float org_rand(void) {
  org_rng ^= org_rng << 13; org_rng ^= org_rng >> 17; org_rng ^= org_rng << 5;
  return (org_rng & 0xFFFFFF) / 16777216.0f;
}

// ---- tiny 3x3 matrix helpers ----
typedef struct { float m[9]; } M3;
static M3 m3_mul(M3 a, M3 b) {
  M3 r;
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      r.m[i*3+j] = a.m[i*3+0]*b.m[0*3+j] + a.m[i*3+1]*b.m[1*3+j] + a.m[i*3+2]*b.m[2*3+j];
  return r;
}
static M3 m3_rx(float a) { float c = cosf(a), s = sinf(a); M3 r = {{1,0,0, 0,c,-s, 0,s,c}}; return r; }
static M3 m3_ry(float a) { float c = cosf(a), s = sinf(a); M3 r = {{c,0,s, 0,1,0, -s,0,c}}; return r; }
static M3 m3_rz(float a) { float c = cosf(a), s = sinf(a); M3 r = {{c,-s,0, s,c,0, 0,0,1}}; return r; }

// ---- state ----
static const float org_shell_r[ORG_SHELLS] = { 0.34f, 0.56f, 0.78f, 1.0f };
static const int   org_shell_n[ORG_SHELLS] = { 40, 80, 120, 160 };   // sums to ORG_SEGS

typedef struct {
  float spin, spin_spd;      // in-plane dial rotation (rad, rad/s)
  float tilt_x, tilt_z;      // fixed tilt of the pole away from the viewer
  float wob_ph;              // phase of the slow axis wobble
} OrgShell;

typedef struct {
  int   shell;
  int   npts;
  float pts[ORG_SEG_PTS][3]; // local unit-sphere directions (pole = +Y)
  float age, life, dormant;  // dormant > 0: dead, waiting to respawn
  float bright;
  float pulse_spd, pulse_off;
  float flick;               // brief hologram dropout, decays back to 0
} OrgSeg;

typedef struct { float dir[3]; float len, ph, freq; } OrgFil;
typedef struct { float axis_tilt, axis_rot, radius, spin, spd; } OrgRing;
typedef struct { float r, strength; } OrgWave;
typedef struct { float x, y, z, vx, vy, vz, life; } OrgEmber;

static OrgShell org_shells[ORG_SHELLS];
static OrgSeg   org_segs[ORG_SEGS];
static OrgFil   org_fils[ORG_FILS];
static OrgRing  org_core[ORG_CORE_RINGS];
static OrgRing  org_arcs[ORG_ARCS];
static float    org_arc_span[ORG_ARCS];
static OrgWave  org_waves[ORG_WAVES];
static OrgEmber org_embers[ORG_EMBERS];
static float    org_prev_beat_phase = 0;
static float    org_R = 120;   // radius the segment sampling was built for
static float    org_hotk[3] = { 0.55f, 0.85f, 1.0f };   // white-hot mix, set per frame by palette

// Build a new circuit trace on its shell: 1-3 legs that alternate between
// running along a latitude (appears as a concentric ring arc) and along a
// longitude (appears as a radial run), with right-angle jogs between legs.
static void org_seg_spawn(OrgSeg *s) {
  float sr = org_shell_r[s->shell];
  float y = org_rand() * 1.9f - 0.95f;
  float lat = asinf(y), lon = org_rand() * 6.2831853f;
  int legs = 1 + (org_rand() < 0.55f) + (org_rand() < 0.25f);
  int along_lat = org_rand() < 0.72f;
  float step = 1.5f / (org_R * sr);            // ~1.5px between samples
  s->npts = 0;
  for (int l = 0; l < legs && s->npts < ORG_SEG_PTS; l++) {
    float len = along_lat ? (0.15f + org_rand() * 0.75f) : (0.06f + org_rand() * 0.28f);
    float sgn = org_rand() < 0.5f ? -1.0f : 1.0f;
    float cl = fmaxf(cosf(lat), 0.25f);
    int n = (int)(len / step) + 1;
    for (int k = 0; k < n && s->npts < ORG_SEG_PTS; k++) {
      float *p = s->pts[s->npts++];
      p[0] = cosf(lat) * cosf(lon); p[1] = sinf(lat); p[2] = cosf(lat) * sinf(lon);
      if (along_lat) lon += sgn * step / cl;
      else { lat += sgn * step; if (lat > 1.35f || lat < -1.35f) { sgn = -sgn; lat += 2 * sgn * step; } }
    }
    along_lat = !along_lat;
  }
  s->age = 0;
  s->life = 4.0f + org_rand() * 10.0f;
  s->dormant = 0;
  s->bright = 0.35f + org_rand() * 0.65f;
  s->pulse_spd = org_rand() < 0.5f ? 0.3f + org_rand() * 0.9f : 0.0f;
  s->pulse_off = org_rand();
  s->flick = 0;
}

static void org_init(float R) {
  org_R = R;
  for (int i = 0; i < ORG_SHELLS; i++) {
    OrgShell *sh = &org_shells[i];
    sh->spin = org_rand() * 6.2831853f;
    sh->spin_spd = (0.05f + 0.07f * org_rand()) * ((i & 1) ? -1.0f : 1.0f);
    sh->tilt_x = (org_rand() - 0.5f) * 0.45f;
    sh->tilt_z = (org_rand() - 0.5f) * 0.45f;
    sh->wob_ph = org_rand() * 6.2831853f;
  }
  int k = 0;
  for (int sh = 0; sh < ORG_SHELLS; sh++)
    for (int j = 0; j < org_shell_n[sh]; j++, k++) {
      org_segs[k].shell = sh;
      org_seg_spawn(&org_segs[k]);
      org_segs[k].age = org_rand() * org_segs[k].life;   // start mid-life: no mass birth
    }
  for (int i = 0; i < ORG_FILS; i++) {
    float z = org_rand() * 2 - 1, a = org_rand() * 6.2831853f, r = sqrtf(1 - z * z);
    org_fils[i].dir[0] = r * cosf(a); org_fils[i].dir[1] = r * sinf(a); org_fils[i].dir[2] = z;
    org_fils[i].len = 0.95f + org_rand() * 0.12f;
    org_fils[i].ph = org_rand() * 6.2831853f;
    org_fils[i].freq = 0.4f + org_rand() * 1.6f;
  }
  for (int i = 0; i < ORG_CORE_RINGS; i++) {
    org_core[i] = (OrgRing){ org_rand() * 3.14159f, org_rand() * 6.2831853f,
                             0.07f + 0.13f * org_rand(), org_rand() * 6.2831853f,
                             (0.8f + 1.8f * org_rand()) * (i & 1 ? -1.0f : 1.0f) };
  }
  for (int i = 0; i < ORG_ARCS; i++) {
    org_arcs[i] = (OrgRing){ 0.3f + org_rand() * 1.2f, org_rand() * 6.2831853f,
                             0.45f + 0.25f * org_rand(), org_rand() * 6.2831853f,
                             (0.25f + 0.45f * org_rand()) * (i & 1 ? -1.0f : 1.0f) };
    org_arc_span[i] = 1.4f + org_rand() * 2.0f;
  }
  for (int i = 0; i < ORG_WAVES; i++) org_waves[i].strength = 0;
  for (int i = 0; i < ORG_EMBERS; i++) org_embers[i].life = 0;
  // tone-map LUT: soft exponential shoulder, so stacked gold saturates to white
  for (int i = 0; i < ORG_LUT; i++) {
    float v = (i + 0.5f) * ORG_LUT_MAX / ORG_LUT;
    float o = 1.0f - expf(-v * 2.4f);
    org_lut_r[i] = (uint16_t)(o * 31.0f + 0.5f);
    org_lut_g[i] = (uint16_t)(o * 63.0f + 0.5f);
    org_lut_b[i] = (uint16_t)(o * 31.0f + 0.5f);
  }
}

// ---- splatting ----
static inline void org_splat(float bx, float by, float r, float g, float b) {
  int ix = (int)bx, iy = (int)by;
  if (bx < 0 || by < 0 || ix >= ORG_N - 1 || iy >= ORG_N - 1) return;
  float fx = bx - ix, fy = by - iy;
  float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
  float *p = &org_acc[(iy * ORG_N + ix) * 3];
  p[0] += r * w00; p[1] += g * w00; p[2] += b * w00;
  p[3] += r * w10; p[4] += g * w10; p[5] += b * w10;
  p += ORG_N * 3;
  p[0] += r * w01; p[1] += g * w01; p[2] += b * w01;
  p[3] += r * w11; p[4] += g * w11; p[5] += b * w11;
}

typedef struct { float R, half; } OrgProj;

// Project an orb-space point (units of R, +Z toward the viewer) and splat it
// with depth cueing (the far side of the hologram shows through, dimmer).
static inline void org_plot(const OrgProj *pj, float x, float y, float z,
                            float I, float hot, const float *gold) {
  float f = 4.0f / (4.0f - z);
  float bx = pj->half + x * f * pj->R, by = pj->half - y * f * pj->R;
  float dc = 0.30f + 0.70f * (0.5f + 0.5f * z);
  I *= dc;
  // `hot` pushes the color from blue toward white-hot
  float r = I * (gold[0] + org_hotk[0] * hot), g = I * (gold[1] + org_hotk[1] * hot), b = I * (gold[2] + org_hotk[2] * hot);
  org_splat(bx, by, r, g, b);
  org_splat(bx + 0.6f, by + 0.6f, r * 0.5f, g * 0.5f, b * 0.5f);   // ~1.5px line weight
}

static inline void org_xf(const M3 *M, float x, float y, float z, float *o) {
  o[0] = M->m[0]*x + M->m[1]*y + M->m[2]*z;
  o[1] = M->m[3]*x + M->m[4]*y + M->m[5]*z;
  o[2] = M->m[6]*x + M->m[7]*y + M->m[8]*z;
}

// ---- bloom (quarter-res box blur) + tone-map + write RGB565 ----
static void org_blur_pass(const float *src, float *dst, int dx, int dy) {
  const int r = 2; const float inv = 1.0f / (2 * r + 1);
  for (int y = 0; y < ORG_BN; y++)
    for (int x = 0; x < ORG_BN; x++) {
      float s0 = 0, s1 = 0, s2 = 0;
      for (int k = -r; k <= r; k++) {
        int xx = x + k * dx, yy = y + k * dy;
        if (xx < 0 || yy < 0 || xx >= ORG_BN || yy >= ORG_BN) continue;
        const float *p = &src[(yy * ORG_BN + xx) * 3];
        s0 += p[0]; s1 += p[1]; s2 += p[2];
      }
      float *o = &dst[(y * ORG_BN + x) * 3];
      o[0] = s0 * inv; o[1] = s1 * inv; o[2] = s2 * inv;
    }
}

static void org_composite(uint16_t *back, int W, int H, int STRIDE, int ox, int oy, float bloom_k) {
  // downsample
  const float inv = 1.0f / (ORG_BS * ORG_BS);
  for (int by = 0; by < ORG_BN; by++)
    for (int bx = 0; bx < ORG_BN; bx++) {
      float s0 = 0, s1 = 0, s2 = 0;
      for (int y = 0; y < ORG_BS; y++) {
        const float *p = &org_acc[((by * ORG_BS + y) * ORG_N + bx * ORG_BS) * 3];
        for (int x = 0; x < ORG_BS; x++) { s0 += p[0]; s1 += p[1]; s2 += p[2]; p += 3; }
      }
      float *o = &org_bl[(by * ORG_BN + bx) * 3];
      o[0] = s0 * inv; o[1] = s1 * inv; o[2] = s2 * inv;
    }
  org_blur_pass(org_bl, org_bl2, 1, 0); org_blur_pass(org_bl2, org_bl, 0, 1);
  org_blur_pass(org_bl, org_bl2, 1, 0); org_blur_pass(org_bl2, org_bl, 0, 1);

  // bilinear bloom upsample indices, shared by all rows/cols
  static int   ui[ORG_N]; static float uw[ORG_N]; static int init = 0;
  if (!init) {
    for (int i = 0; i < ORG_N; i++) {
      float u = (i + 0.5f) / ORG_BS - 0.5f;
      if (u < 0) u = 0;
      if (u > ORG_BN - 1.001f) u = ORG_BN - 1.001f;
      ui[i] = (int)u; uw[i] = u - (int)u;
    }
    init = 1;
  }
  const float lut_s = ORG_LUT / ORG_LUT_MAX;
  int x0 = ox - ORG_N / 2, y0 = oy - ORG_N / 2;
  for (int py = 0; py < ORG_N; py++) {
    int sy = y0 + py; if (sy < 0 || sy >= H) continue;
    uint16_t *row = back + sy * (STRIDE / 2);
    const float *a = &org_acc[py * ORG_N * 3];
    const float *b0 = &org_bl[ui[py] * ORG_BN * 3], *b1 = b0 + ORG_BN * 3;
    float wy = uw[py];
    for (int px = 0; px < ORG_N; px++, a += 3) {
      int sx = x0 + px; if (sx < 0 || sx >= W) continue;
      int bi = ui[px] * 3; float wx = uw[px];
      float w00 = (1 - wx) * (1 - wy), w10 = wx * (1 - wy), w01 = (1 - wx) * wy, w11 = wx * wy;
      float r = a[0] + bloom_k * (b0[bi]   * w00 + b0[bi+3] * w10 + b1[bi]   * w01 + b1[bi+3] * w11);
      float g = a[1] + bloom_k * (b0[bi+1] * w00 + b0[bi+4] * w10 + b1[bi+1] * w01 + b1[bi+4] * w11);
      float b = a[2] + bloom_k * (b0[bi+2] * w00 + b0[bi+5] * w10 + b1[bi+2] * w01 + b1[bi+5] * w11);
      if (r < 0.004f && g < 0.004f && b < 0.004f) continue;
      int ir = (int)(r * lut_s), ig = (int)(g * lut_s), ib = (int)(b * lut_s);
      if (ir >= ORG_LUT) ir = ORG_LUT - 1;
      if (ig >= ORG_LUT) ig = ORG_LUT - 1;
      if (ib >= ORG_LUT) ib = ORG_LUT - 1;
      row[sx] = (uint16_t)((org_lut_r[ir] << 11) | (org_lut_g[ig] << 5) | org_lut_b[ib]);
    }
  }
}

static inline float org_frac(float x) { return x - floorf(x); }

// ---- Agent gyroscope ----------------------------------------------------
// A permanent armature of three tilted orbital rings (Iris's skeleton, always
// present, slowly precessing). Each running agent is a comet riding one of
// the rings: Claude = white-hot head / cyan tail, Hermes = orange. A comet's
// speed and tail length follow its agent's activity; the more total work, the
// brighter and faster the whole frame turns.
// Purpose: 'u' = working for Humdan (drives nucleus dilation), 's' = working for itself (drives ring precession)
#define ORG_GYRO_RINGS 3
#define ORG_AGENTS     8
typedef struct { uint32_t id; char kind; float activity; char purpose; } OrgAgentIn;   // kind 'c'/'h', purpose 'u'/'s'
typedef struct { uint32_t id; char kind; float act, vis, pos; int ring, alive; char purpose; } OrgComet;
static OrgComet org_comets[ORG_AGENTS];
static int      org_ncomets = 0;
static const float org_gyro_r[ORG_GYRO_RINGS] = { 1.00f, 0.92f, 1.04f };
static const float org_gyro_tilt[ORG_GYRO_RINGS][2] = { { 1.20f, 0.25f }, { 0.40f, 2.00f }, { 2.25f, -1.00f } };
static float org_gyro_prec[ORG_GYRO_RINGS];

// Feed the current agent list (call ~1 Hz). Comets are matched by id so they
// keep their place; new agents fade in, vanished ones fade out.
static void org_set_agents(const OrgAgentIn *in, int n) {
  for (int k = 0; k < org_ncomets; k++) org_comets[k].alive = 0;
  for (int i = 0; i < n; i++) {
    OrgComet *c = NULL;
    for (int k = 0; k < org_ncomets; k++) if (org_comets[k].id == in[i].id) { c = &org_comets[k]; break; }
    if (!c) {
      if (org_ncomets >= ORG_AGENTS) continue;
      c = &org_comets[org_ncomets++];
      int load[ORG_GYRO_RINGS] = { 0 };
      for (int k = 0; k < org_ncomets - 1; k++) load[org_comets[k].ring]++;
      int best = 0;
      for (int r = 1; r < ORG_GYRO_RINGS; r++) if (load[r] < load[best]) best = r;
      *c = (OrgComet){ in[i].id, in[i].kind, 0, 0, (in[i].id % 628) / 100.0f, best, 1, in[i].purpose };
    }
    c->alive = 1; c->kind = in[i].kind;
    c->act = in[i].activity < 0 ? 0 : in[i].activity > 1 ? 1 : in[i].activity;
    c->purpose = in[i].purpose;
  }
}

// Additive splat with an explicit color (not the body palette).
static inline void org_plot_rgb(const OrgProj *pj, float x, float y, float z, float r, float g, float b) {
  float f = 4.0f / (4.0f - z);
  float bx = pj->half + x * f * pj->R, by = pj->half - y * f * pj->R;
  float dc = 0.35f + 0.65f * (0.5f + 0.5f * z);
  org_splat(bx, by, r * dc, g * dc, b * dc);
  org_splat(bx + 0.6f, by + 0.6f, r * dc * 0.5f, g * dc * 0.5f, b * dc * 0.5f);
}

// How chaotic the setup is (0 calm .. 1 chaotic) from the order score. A
// tidy setup (>= 0.95) is perfectly calm; 0.35 or below is full chaos.
static inline float org_chaos(float order) {
  float c = (0.95f - order) / 0.60f;
  return c < 0 ? 0 : c > 1 ? 1 : c;
}

static void org_gyro(const OrgProj *pj, const M3 *G, float R, float dt, float t, float act, float night,
                    float u_max, float s_max, float order) {
  // ease comets, drop fully faded ones
  float busy = 0;
  int out = 0;
  for (int k = 0; k < org_ncomets; k++) {
    OrgComet *c = &org_comets[k];
    c->vis += ((c->alive ? 1.0f : 0.0f) - c->vis) * (1.0f - expf(-2.0f * dt));
    if (!c->alive && c->vis < 0.01f) continue;
    busy += c->act * c->vis;
    org_comets[out++] = *c;
  }
  org_ncomets = out;
  float b = fminf(busy / 2.0f, 1.0f);                 // 0 idle .. 1 (two fully busy agents)
  float jitter = org_chaos(order);

  // the armature: faint rings with tick marks, tinted like the body
  // Precession speed is driven by s_max (activity for self) and act
  float tint[3] = { 0.55f + 0.40f * night, 0.78f - 0.55f * night, 1.0f - 0.75f * night };   // whiter than the body
  M3 RM[ORG_GYRO_RINGS];
  for (int r = 0; r < ORG_GYRO_RINGS; r++) {
    // Working for itself: the rings spin up (~6x faster at full s) and rock
    // on their axes like a gyroscope under load.
    org_gyro_prec[r] += (0.025f + 0.10f * b + 0.85f * s_max + 0.03f * act) * dt * (r & 1 ? -1.0f : 1.0f);
    float nod = s_max * 0.35f * sinf(t * 1.3f + r * 2.1f);
    // Chaotic setup: rings wander out of their neat aligned pose
    float drift = jitter * 0.30f * sinf(t * 0.9f + r * 2.0f);
    RM[r] = m3_mul(*G, m3_mul(m3_rz(org_gyro_tilt[r][1] + org_gyro_prec[r] + drift),
                             m3_rx(org_gyro_tilt[r][0] - drift * 0.5f + nod)));
    float rr = org_gyro_r[r];
    int n = (int)(6.2831853f * rr * R / 1.3f);
    float I0 = 0.16f + 0.16f * b + 0.30f * s_max;
    for (int k = 0; k < n; k++) {
      float a = 6.2831853f * k / n;
      float tick = fmodf(a, 6.2831853f / 24.0f) < 0.022f ? 3.0f : 1.0f;   // 24 tick marks
      float p[3]; org_xf(&RM[r], cosf(a) * rr, 0, sinf(a) * rr, p);
      float I = I0 * tick;
      org_plot_rgb(pj, p[0], p[1], p[2], I * tint[0], I * tint[1], I * tint[2]);
    }
  }

  // comets
  for (int k = 0; k < org_ncomets; k++) {
    OrgComet *c = &org_comets[k];
    c->pos += (0.35f + 1.8f * c->act) * dt;
    float rr = org_gyro_r[c->ring];
    float tail = 0.45f + 1.4f * c->act;                // radians of tail
    int n = (int)(tail * rr * R / 1.0f) + 4;
    float cr, cg, cb, hr, hg, hb;                       // tail color, head (hot) color
    if (c->kind == 'h') { cr = 1.00f; cg = 0.48f; cb = 0.08f; hr = 1.0f; hg = 0.85f; hb = 0.55f; }
    else                { cr = 0.30f; cg = 0.80f; cb = 1.00f; hr = 0.85f; hg = 0.95f; hb = 1.0f; }
    float V = c->vis * (0.55f + 0.45f * c->act);
    for (int i = 0; i < n; i++) {
      float u = (float)i / n;                            // 0 head .. 1 tail end
      float a = c->pos - u * tail;
      float p[3]; org_xf(&RM[c->ring], cosf(a) * rr, 0, sinf(a) * rr, p);
      float w = (1.0f - u); w = w * w;
      float I = V * 2.0f * w;
      float h = u < 0.10f ? 1.0f - u / 0.10f : 0.0f;     // white-hot near the head
      float rr_ = I * (cr + h * hr), gg_ = I * (cg + h * hg), bb_ = I * (cb + h * hb);
      // ~3px wide near the head, tapering to 1px at the tail end
      float wpx = (1.0f - u) * 1.1f / R;
      org_plot_rgb(pj, p[0], p[1], p[2], rr_, gg_, bb_);
      org_plot_rgb(pj, p[0] + wpx, p[1] + wpx, p[2], rr_ * 0.6f, gg_ * 0.6f, bb_ * 0.6f);
      org_plot_rgb(pj, p[0] - wpx, p[1] - wpx, p[2], rr_ * 0.6f, gg_ * 0.6f, bb_ * 0.6f);
    }
    // glowing head
    float p[3]; org_xf(&RM[c->ring], cosf(c->pos) * rr, 0, sinf(c->pos) * rr, p);
    for (int ring = 1; ring <= 3; ring++) {           // soft 3-layer glow ~4px radius
      float d = ring * 1.3f / R, k = V * (1.6f / ring);
      for (int j = 0; j < 8; j++) {
        float ja = j * 0.7853982f + ring * 0.4f;
        org_plot_rgb(pj, p[0] + cosf(ja) * d, p[1] + sinf(ja) * d, p[2],
                     k * (cr + hr), k * (cg + hg), k * (cb + hb));
      }
    }
  }
}

// Draw one frame of the organism into `back`, centered at (ox, oy).
//   R          on-screen radius of the outer shell (px), incl. breathing
//   t, dt      time (s) and frame delta
//   act        smoothed activity 0..1
//   yaw, pitch whole-orb orientation (auto sway + touch)
//   beat       heartbeat envelope (~-0.2..1), beat_phase its phase (cycles)
//   night      0..1 (eased by the caller): blend the palette to maroon while
//              night shift runs
//   u_max      max activity of purpose='u' agents (drives nucleus dilation)
//   s_max      max activity of purpose='s' agents (drives ring precession)
//   order      order score 0..1 (0 chaotic, 1 organized): drives shell coherence
static void org_frame(uint16_t *back, int W, int H, int STRIDE, float ox, float oy,
                      float R, float t, float dt, float act, float yaw, float pitch,
                      float beat, float beat_phase, float night,
                      float u_max, float s_max, float order) {
  memset(org_acc, 0, sizeof(org_acc));
  OrgProj pj = { R, ORG_N / 2.0f };

  // palette: deep blue when resting -> bright electric cyan-blue when busy;
  // blended toward maroon (deep red -> hot rose) during night shift
  const float blue[3]   = { 0.10f + 0.08f * act, 0.42f + 0.20f * act, 1.00f };
  const float maroon[3] = { 0.50f + 0.30f * act, 0.03f + 0.05f * act, 0.09f + 0.05f * act };
  const float hot_blue[3] = { 0.55f, 0.85f, 1.00f }, hot_maroon[3] = { 1.00f, 0.40f, 0.45f };
  float gold[3];
  for (int c = 0; c < 3; c++) {
    gold[c] = blue[c] + (maroon[c] - blue[c]) * night;
    org_hotk[c] = hot_blue[c] + (hot_maroon[c] - hot_blue[c]) * night;
  }
  float level = 0.95f + 0.05f * act;        // overall intensity (busy adds pulses/ripples/speed instead)
  float speed = 0.45f + 2.15f * act;        // slow drift at rest, fast when busy
  float beat_pos = fmaxf(beat, 0.0f);

  // heartbeat ripples: a new wave leaves the core on every "lub"
  {
    float pp = org_frac(org_prev_beat_phase), cp = org_frac(beat_phase);
    int crossed = (pp < 0.12f && cp >= 0.12f) || (cp < pp && cp >= 0.12f);
    org_prev_beat_phase = beat_phase;
    if (crossed)
      for (int i = 0; i < ORG_WAVES; i++)
        if (org_waves[i].strength <= 0.01f) { org_waves[i] = (OrgWave){ 0.0f, 0.12f + 1.08f * act }; break; }
    for (int i = 0; i < ORG_WAVES; i++) {
      if (org_waves[i].strength <= 0.01f) continue;
      org_waves[i].r += dt * 1.1f;
      org_waves[i].strength *= expf(-dt * 0.9f);
      if (org_waves[i].r > 1.3f) org_waves[i].strength = 0;
    }
  }
  float shell_wave[ORG_SHELLS];
  for (int s = 0; s < ORG_SHELLS; s++) {
    float w = 0;
    for (int i = 0; i < ORG_WAVES; i++) {
      if (org_waves[i].strength <= 0.01f) continue;
      float d = (org_shell_r[s] - org_waves[i].r) / 0.09f;
      w += org_waves[i].strength * expf(-d * d);
    }
    shell_wave[s] = w;
  }

  M3 G = m3_mul(m3_ry(yaw), m3_rx(pitch));

  // Chaotic setup: shells fall out of sync, traces wobble and drop out
  float jitter = org_chaos(order);

  // shell matrices: spin in-plane, tilt the pole to face the viewer, wobble
  M3 SM[ORG_SHELLS];
  for (int s = 0; s < ORG_SHELLS; s++) {
    OrgShell *sh = &org_shells[s];
    sh->spin += sh->spin_spd * speed * dt;
    // each shell gets its own off-beat tumble, so they stop moving as one
    float jitter_phase = jitter * 0.35f * sinf(t * 1.7f + s * 1.5f);
    float wob = 0.10f * sinf(t * 0.13f + sh->wob_ph) + jitter * 0.25f * sinf(t * 1.1f + s * 2.3f);
    SM[s] = m3_mul(G, m3_mul(m3_rx(1.5707963f + sh->tilt_x + wob),
                             m3_mul(m3_rz(sh->tilt_z - wob * 0.7f + jitter_phase), m3_ry(sh->spin))));
  }

  // --- circuit traces ---
  float wob_amp = 0.025f + 0.035f * act + 0.10f * jitter;   // more wobble when chaotic
  for (int i = 0; i < ORG_SEGS; i++) {
    OrgSeg *s = &org_segs[i];
    if (s->dormant > 0) {                   // dead: wait, then be reborn
      s->dormant -= dt * (0.4f + 5.6f * act);   // fewer traces alive at rest
      if (s->dormant <= 0) org_seg_spawn(s);
      continue;
    }
    s->age += dt * (0.35f + 1.65f * act);      // slow turnover at rest
    if (s->age >= s->life) { s->dormant = 0.5f + org_rand() * 5.0f; continue; }
    if (s->flick > 0) s->flick -= dt * 4.0f;
    else if (org_rand() < dt * (0.004f + 0.026f * act + 0.35f * jitter)) s->flick = 1.0f;   // hologram dropout (rare at rest, constant when chaotic)

    float grow = fminf(s->age / 0.7f, 1.0f);
    float fade = fminf((s->life - s->age) / 1.2f, 1.0f);
    float alpha = grow < 1.0f ? 1.0f : fade;
    alpha *= 1.0f - 0.8f * fmaxf(s->flick, 0.0f);
    int drawn = (int)(s->npts * grow);
    if (drawn < 1) continue;

    float sr = org_shell_r[s->shell];
    float I0 = (0.52f - 0.02f * act) * level * s->bright * alpha;   // busy gets pulses/ripples on top
    float wave = shell_wave[s->shell];
    float pulse_on = s->pulse_spd > 0 ? (0.06f + 1.59f * act) : 0.0f;
    float head = org_frac(s->age * s->pulse_spd * speed * 0.5f + s->pulse_off) * s->npts;
    const M3 *M = &SM[s->shell];
    for (int k = 0; k < drawn; k++) {
      const float *d = s->pts[k];
      float rs = sr * (1.0f + wob_amp * sinf(3.1f * d[0] + t * 0.9f) * sinf(2.7f * d[2] - t * 0.7f + d[1]));
      float p[3]; org_xf(M, d[0] * rs, d[1] * rs, d[2] * rs, p);
      float hot = wave * 0.9f;
      float I = I0 * (1.0f + wave * 1.0f);
      if (pulse_on > 0) {
        float dk = (k - head) / 2.5f;
        float pb = pulse_on * expf(-dk * dk);
        I += 0.30f * pb * alpha; hot += pb;
      }
      if (grow < 1.0f && k >= drawn - 3) { I += (0.15f + 0.35f * act) * alpha; hot += 0.3f + 0.7f * act; }  // growing tip
      org_plot(&pj, p[0], p[1], p[2], I, hot, gold);
    }
  }

  // --- radial filaments: faint spokes that flicker like firing nerves ---
  {
    M3 F = m3_mul(G, m3_rz(t * 0.03f * speed));
    float step = 1.8f / R;
    for (int i = 0; i < ORG_FILS; i++) {
      OrgFil *f = &org_fils[i];
      float s = 0.5f + 0.5f * sinf(t * f->freq * speed + f->ph);
      s = s * s; s = s * s; s = s * s;                  // spiky ^8
      float I0 = level * (0.010f + 0.005f * act + s * (0.03f + 0.32f * act));
      if (I0 < 0.01f) continue;
      float d[3]; org_xf(&F, f->dir[0], f->dir[1], f->dir[2], d);
      for (float r = 0.28f; r < f->len; r += step) {
        float taper = sinf((r - 0.28f) / (f->len - 0.28f) * 3.14159f);
        org_plot(&pj, d[0] * r, d[1] * r, d[2] * r, I0 * taper, s * 0.6f, gold);
      }
    }
  }

  // --- sweeping arcs: comets on tilted great circles ---
  for (int i = 0; i < ORG_ARCS; i++) {
    OrgRing *a = &org_arcs[i];
    a->spin += a->spd * speed * dt;
    M3 A = m3_mul(G, m3_mul(m3_rz(a->axis_rot + t * 0.05f), m3_rx(a->axis_tilt)));
    int n = (int)(org_arc_span[i] * a->radius * R / 1.2f);
    for (int k = 0; k < n; k++) {
      float u = (float)k / n;                          // 0 tail .. 1 head
      float ang = a->spin - (1.0f - u) * org_arc_span[i];
      float p[3]; org_xf(&A, cosf(ang) * a->radius, 0, sinf(ang) * a->radius, p);
      float I = level * (0.05f + 0.50f * u * u * u) * (0.35f + 0.95f * act);
      org_plot(&pj, p[0], p[1], p[2], I, u * u * 0.8f, gold);
    }
  }

  // --- core: spinning knot of small rings + a breathing glow ---
  // Working for Humdan: the nucleus dilates (~1.9x) and burns brighter.
  // u_max arrives eased by the caller.
  float nucleus_dilate = u_max;  // 0 idle, 1 busy for Humdan
  for (int i = 0; i < ORG_CORE_RINGS; i++) {
    OrgRing *c = &org_core[i];
    c->spin += c->spd * speed * dt;
    M3 C = m3_mul(G, m3_mul(m3_rz(c->axis_rot + t * 0.2f * (i + 1) * 0.3f), m3_mul(m3_rx(c->axis_tilt), m3_ry(c->spin))));
    float rr = c->radius * (1.0f + 0.15f * beat_pos) * (1.0f + 0.9f * nucleus_dilate);
    int n = (int)(6.2831853f * rr * R / 1.2f) + 8;
    for (int k = 0; k < n; k++) {
      float ang = 6.2831853f * k / n;
      float p[3]; org_xf(&C, cosf(ang) * rr, 0, sinf(ang) * rr, p);
      float u = org_frac((float)k / n + c->spin * 0.16f);   // bright sweep around the ring
      org_plot(&pj, p[0], p[1], p[2], level * (0.30f + 0.7f * u * u * u) * (1.0f + 0.8f * nucleus_dilate),
               0.6f + u + 0.6f * nucleus_dilate, gold);
    }
  }
  {
    float cr = 0.11f * R * (1.0f + 0.30f * beat_pos) * (1.0f + 1.0f * nucleus_dilate);
    float peak = level * (0.18f + 0.45f * beat_pos + 0.55f * nucleus_dilate);
    int rad = (int)(cr * 2.2f);
    float inv = 1.0f / (cr * cr);
    int c0 = ORG_N / 2;
    for (int y = -rad; y <= rad; y++)
      for (int x = -rad; x <= rad; x++) {
        float d2 = (x * x + y * y) * inv;
        if (d2 > 4.8f) continue;
        float k = peak * expf(-d2 * 1.6f);
        float *p = &org_acc[((c0 + y) * ORG_N + (c0 + x)) * 3];
        p[0] += k * (gold[0] + 0.5f * org_hotk[0]); p[1] += k * (gold[1] + 0.5f * org_hotk[1]); p[2] += k * (gold[2] + 0.5f * org_hotk[2]);
      }
  }

  // --- agent gyroscope: armature rings + one comet per running agent ---
  org_gyro(&pj, &G, R, dt, t, act, night, u_max, s_max, order);

  // --- embers: sparks thrown off the surface when busy ---
  {
    static float ember_t = 0;
    ember_t += dt;
    float interval = 0.6f - 0.5f * act;
    if (act > 0.12f && ember_t > interval) {
      ember_t = 0;
      for (int i = 0; i < ORG_EMBERS; i++) if (org_embers[i].life <= 0) {
        float z = org_rand() * 2 - 1, a = org_rand() * 6.2831853f, r = sqrtf(1 - z * z);
        float x = r * cosf(a), y = r * sinf(a), sp = 0.25f + 0.4f * org_rand() + 0.3f * act;
        org_embers[i] = (OrgEmber){ x * 0.9f, y * 0.9f, z * 0.9f, x * sp, y * sp, z * sp, 1.0f };
        break;
      }
    }
    for (int i = 0; i < ORG_EMBERS; i++) {
      OrgEmber *e = &org_embers[i];
      if (e->life <= 0) continue;
      e->x += e->vx * dt; e->y += e->vy * dt; e->z += e->vz * dt;
      e->life -= dt * 0.8f;
      float rr = sqrtf(e->x * e->x + e->y * e->y + e->z * e->z);
      float edge = 1.0f - fminf(fmaxf((rr - 0.95f) / 0.12f, 0.0f), 1.0f);  // die before leaving ORG_EXTENT
      float I = 0.6f * level * fminf(e->life * 3.0f, 1.0f) * edge;
      if (I <= 0.001f) continue;
      float p[3]; org_xf(&G, e->x, e->y, e->z, p);
      org_plot(&pj, p[0], p[1], p[2], I, 0.8f, gold);
    }
  }

  org_composite(back, W, H, STRIDE, (int)(ox + 0.5f), (int)(oy + 0.5f), 0.7f + 0.3f * act);
}
