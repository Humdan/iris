// iris_organism.h — the "digital organism" orb.
//
// An electric-blue, see-through holographic globe: curved circuit panels laid
// out in latitude bands around a hot, spinning core, turning on a tilted axis.
// It behaves like something alive:
//   - the panels assemble into a sphere: they fly in bottom-first and lock in
//     with a flash at startup, briefly come apart and snap back together when
//     a new agent starts working, and hang loose and tumble while the setup is
//     messy (order score), closing up as it gets tidy
//   - each panel is lit by which way it faces, with a brighter rim and a dimmer
//     far side, so it reads as a solid ball; a dim inner wireframe sphere spins
//     the other way for depth
//   - every heartbeat sends a ripple of light across the face; data pulses run
//     around panel outlines when busy; the core breathes
//   - agents ride three gyroscope rings outside the globe as comets
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

#define ORG_BANDS      8                // latitude bands of panels
#define ORG_TILES      96
#define ORG_TILE_PTS   400
#define ORG_SHELL_R    0.86f            // assembled globe radius (units of R); loose panels reach ~1.03
#define ORG_INNER_R    0.50f            // inner wireframe sphere
#define ORG_STAGGER    0.6f             // spread of the assembly wave (bottom band first)
#define ORG_CORE_RINGS 5
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
// One panel of the globe. Its points are stored relative to its home center
// direction c (point = c + d on the unit sphere), so a loose panel can be moved
// out, shrunk and tumbled as a rigid piece and lands exactly in place.
typedef struct {
  float c[3];                 // home center direction (unit)
  float lat;                  // center latitude
  int   npts, nedge;          // [0,nedge) outline in perimeter order, [nedge,npts) inner trace
  float d[ORG_TILE_PTS][3];
  float a;                    // 0 loose .. 1 locked in place (eased)
  float delay;                // place in the assembly wave (0 first .. ORG_STAGGER)
  float axis[3], tumble;      // tumble axis and angle while loose
  float drift[3];             // sideways drift while loose
  float bob_ph;
  float flash;                // white flash when it locks in
  float flick;                // brief hologram dropout, decays back to 0
  float pulse_spd, pulse_off;
  float bright;
} OrgTile;

typedef struct { float axis_tilt, axis_rot, radius, spin, spd; } OrgRing;
typedef struct { float r, strength; } OrgWave;
typedef struct { float x, y, z, vx, vy, vz, life; } OrgEmber;

static OrgTile  org_tiles[ORG_TILES];
static int      org_ntiles = 0;
static float    org_spin = 0;          // globe rotation (rad)
static float    org_assembled = 0;     // eased whole-globe assembly, 0 scattered .. 1 whole
static float    org_kick = 1.0f;       // knocked apart (startup, new agent); decays as it reassembles
static OrgRing  org_core[ORG_CORE_RINGS];
static OrgWave  org_waves[ORG_WAVES];
static OrgEmber org_embers[ORG_EMBERS];
static float    org_prev_beat_phase = 0;
static float    org_hotk[3] = { 0.55f, 0.85f, 1.0f };   // white-hot mix, set per frame by palette

static void org_sph(float lat, float lon, float *p) {
  p[0] = cosf(lat) * cosf(lon); p[1] = sinf(lat); p[2] = cosf(lat) * sinf(lon);
}

static void org_norm3(float *v) {
  float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void org_cross(const float *a, const float *b, float *o) {
  o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}

// rotation by angle a around unit axis k (Rodrigues)
static M3 m3_axis(const float *k, float a) {
  float c = cosf(a), s = sinf(a), v = 1 - c, x = k[0], y = k[1], z = k[2];
  M3 r = {{ c + x * x * v,     x * y * v - z * s, x * z * v + y * s,
            y * x * v + z * s, c + y * y * v,     y * z * v - x * s,
            z * x * v - y * s, z * y * v + x * s, c + z * z * v }};
  return r;
}

// A straight run in (lat, lon): along a latitude it's a ring arc, along a
// longitude a meridian arc. Points land ~`step` apart on screen.
static void org_tile_run(OrgTile *T, float la0, float lo0, float la1, float lo1, float step) {
  float cl = fmaxf(cosf(0.5f * (la0 + la1)), 0.2f);
  float len = sqrtf((la1 - la0) * (la1 - la0) + (lo1 - lo0) * (lo1 - lo0) * cl * cl);
  int n = (int)(len / step) + 1;
  for (int k = 0; k < n && T->npts < ORG_TILE_PTS; k++) {
    float u = (float)k / n, p[3];
    org_sph(la0 + (la1 - la0) * u, lo0 + (lo1 - lo0) * u, p);
    float *d = T->d[T->npts++];
    d[0] = p[0] - T->c[0]; d[1] = p[1] - T->c[1]; d[2] = p[2] - T->c[2];
  }
}

// Lay the globe out: ORG_BANDS latitude bands (poles left open), each cut
// into sectors so panels stay roughly square, with a thin seam between them.
// Every panel gets its outline plus one of a few circuit traces inside.
static void org_build_tiles(float R) {
  const float LAT_MAX = 1.36f;                    // ~78 degrees
  float step = 1.4f / (R * ORG_SHELL_R);          // ~1.4 px between samples
  int nt = 0;
  for (int b = 0; b < ORG_BANDS; b++) {
    float la0 = -LAT_MAX + 2 * LAT_MAX * b / ORG_BANDS, la1 = -LAT_MAX + 2 * LAT_MAX * (b + 1) / ORG_BANDS;
    float mid = 0.5f * (la0 + la1);
    int sectors = (int)(16 * cosf(mid) + 0.5f);
    if (sectors < 5) sectors = 5;
    float off = org_rand() * 6.2831853f;          // stagger seams between bands, like brickwork
    for (int s = 0; s < sectors && nt < ORG_TILES; s++) {
      OrgTile *T = &org_tiles[nt++];
      float lo0 = off + 6.2831853f * s / sectors, lo1 = off + 6.2831853f * (s + 1) / sectors;
      float gl = 0.022f, go = 0.022f / fmaxf(cosf(mid), 0.3f);
      float a0 = la0 + gl, a1 = la1 - gl, o0 = lo0 + go, o1 = lo1 - go;
      org_sph(mid, 0.5f * (lo0 + lo1), T->c);
      T->lat = mid; T->npts = 0;
      org_tile_run(T, a0, o0, a0, o1, step); org_tile_run(T, a0, o1, a1, o1, step);
      org_tile_run(T, a1, o1, a1, o0, step); org_tile_run(T, a1, o0, a0, o0, step);
      T->nedge = T->npts;
      float ma = 0.5f * (a0 + a1), mo = 0.5f * (o0 + o1), ha = 0.5f * (a1 - a0), ho = 0.5f * (o1 - o0);
      switch ((int)(org_rand() * 4)) {
        case 0:   // run, then a right-angle jog down
          org_tile_run(T, ma + 0.35f * ha, o0 + 0.2f * ho, ma + 0.35f * ha, mo + 0.3f * ho, step);
          org_tile_run(T, ma + 0.35f * ha, mo + 0.3f * ho, ma - 0.45f * ha, mo + 0.3f * ho, step);
          break;
        case 1:   // a chip
          org_tile_run(T, ma - 0.3f * ha, mo - 0.3f * ho, ma - 0.3f * ha, mo + 0.3f * ho, step);
          org_tile_run(T, ma - 0.3f * ha, mo + 0.3f * ho, ma + 0.3f * ha, mo + 0.3f * ho, step);
          org_tile_run(T, ma + 0.3f * ha, mo + 0.3f * ho, ma + 0.3f * ha, mo - 0.3f * ho, step);
          org_tile_run(T, ma + 0.3f * ha, mo - 0.3f * ho, ma - 0.3f * ha, mo - 0.3f * ho, step);
          break;
        case 2:   // drop from the top, then run right
          org_tile_run(T, a1 - 0.2f * ha, mo - 0.5f * ho, ma, mo - 0.5f * ho, step);
          org_tile_run(T, ma, mo - 0.5f * ho, ma, o1 - 0.2f * ho, step);
          break;
        default:  // plain panel
          break;
      }
      // assembly order: bottom band first, a little shuffled within a band
      T->delay = ORG_STAGGER * (0.75f * (mid + LAT_MAX) / (2 * LAT_MAX) + 0.25f * org_rand());
      float r3[3] = { org_rand() * 2 - 1, org_rand() * 2 - 1, org_rand() * 2 - 1 };
      org_cross(T->c, r3, T->axis); org_norm3(T->axis);
      org_cross(T->c, T->axis, T->drift); org_norm3(T->drift);
      T->tumble = (0.8f + 1.2f * org_rand()) * (org_rand() < 0.5f ? -1.0f : 1.0f);
      T->bob_ph = org_rand() * 6.2831853f;
      T->a = 0; T->flash = 0; T->flick = 0;
      T->pulse_spd = org_rand() < 0.5f ? 0.25f + org_rand() * 0.6f : 0.0f;
      T->pulse_off = org_rand();
      T->bright = 0.6f + 0.4f * org_rand();
    }
  }
  org_ntiles = nt;
}

static void org_init(float R) {
  org_build_tiles(R);
  for (int i = 0; i < ORG_CORE_RINGS; i++) {
    org_core[i] = (OrgRing){ org_rand() * 3.14159f, org_rand() * 6.2831853f,
                             0.07f + 0.13f * org_rand(), org_rand() * 6.2831853f,
                             (0.8f + 1.8f * org_rand()) * (i & 1 ? -1.0f : 1.0f) };
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
      org_kick = fmaxf(org_kick, 0.55f);   // a new agent: the globe comes apart and snaps back
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
  M3 G = m3_mul(m3_ry(yaw), m3_rx(pitch));
  float chaos = org_chaos(order);

  // --- assembly: a tidy setup closes into a whole sphere, a messy one leaves
  //     panels hanging loose; kicks (startup, new agent) knock it apart ---
  org_kick = fmaxf(org_kick - dt * 0.30f, 0.0f);
  float A_target = fminf(1.0f - 0.6f * chaos, 1.0f - org_kick);
  org_assembled += (A_target - org_assembled) * (1.0f - expf(-3.0f * dt));

  // the globe turns on a tilted axis; light comes from the upper left, in front
  org_spin += 0.16f * speed * dt;
  M3 GW = m3_mul(G, m3_mul(m3_rx(0.38f), m3_ry(org_spin)));   // globe world matrix
  const float L[3] = { -0.45f, 0.55f, 0.70f };

  // --- panels ---
  for (int i = 0; i < org_ntiles; i++) {
    OrgTile *T = &org_tiles[i];
    float tgt = org_assembled * (1.0f + ORG_STAGGER) - T->delay;
    tgt = tgt < 0 ? 0 : tgt > 1 ? 1 : tgt;
    float prev = T->a;
    T->a += (tgt - T->a) * (1.0f - expf(-5.0f * dt));
    if (prev < 0.97f && T->a >= 0.97f) T->flash = 1.0f;     // locks in
    T->flash = fmaxf(T->flash - dt * 2.5f, 0.0f);
    if (T->flick > 0) T->flick -= dt * 4.0f;
    else if (org_rand() < dt * (0.002f + 0.01f * act + 0.25f * chaos)) T->flick = 1.0f;   // dropout

    float l = 1.0f - T->a, ls = l * l * (3 - 2 * l);         // eased looseness
    float bob = ls * sinf(t * 1.3f + T->bob_ph);
    float out = 0.20f * ls + 0.02f * bob;
    float scl = ORG_SHELL_R * (1.0f - 0.40f * ls);
    M3 Mt = m3_axis(T->axis, T->tumble * ls + 0.15f * bob);
    M3 P = m3_mul(GW, Mt);
    float home[3] = { T->c[0] * ORG_SHELL_R * (1 + out) + T->drift[0] * 0.10f * ls,
                      T->c[1] * ORG_SHELL_R * (1 + out) + T->drift[1] * 0.10f * ls,
                      T->c[2] * ORG_SHELL_R * (1 + out) + T->drift[2] * 0.10f * ls };
    float base[3], n[3];
    org_xf(&GW, home[0], home[1], home[2], base);
    org_xf(&P, T->c[0], T->c[1], T->c[2], n);

    // lit by facing: diffuse + a bright rim at the silhouette; far side dimmer
    float diff = fmaxf(n[0] * L[0] + n[1] * L[1] + n[2] * L[2], 0.0f);
    float rim = 1.0f - fabsf(n[2]); rim = rim * rim * rim;
    float shade = (0.40f + 0.60f * diff + 0.55f * rim) * (n[2] < 0 ? 0.55f : 1.0f);
    // heartbeat ripple spreads outward from the middle of the face
    float pos = acosf(fminf(fmaxf(n[2], -1.0f), 1.0f)) / 3.14159f * 1.3f, wave = 0;
    for (int w = 0; w < ORG_WAVES; w++) {
      if (org_waves[w].strength <= 0.01f) continue;
      float dd = (pos - org_waves[w].r) / 0.10f;
      wave += org_waves[w].strength * expf(-dd * dd);
    }
    float I0 = 0.55f * level * T->bright * shade * (0.6f + 0.4f * T->a) * (1.0f - 0.85f * fmaxf(T->flick, 0.0f));
    I0 *= 1.0f + wave;
    float hot0 = 0.12f * act + 1.2f * T->flash + 0.9f * wave;
    float pulse_on = T->pulse_spd > 0 ? (0.06f + 1.59f * act) : 0.0f;
    float head = org_frac(t * T->pulse_spd * speed * 0.25f + T->pulse_off) * T->nedge;

    for (int k = 0; k < T->npts; k++) {
      const float *d = T->d[k];
      float q[3]; org_xf(&P, d[0] * scl, d[1] * scl, d[2] * scl, q);
      float I = I0, hot = hot0;
      if (k >= T->nedge) I *= 0.55f;                          // inner trace is fainter
      else if (pulse_on > 0) {
        float dk = fabsf(k - head); if (dk > T->nedge * 0.5f) dk = T->nedge - dk;
        float pb = pulse_on * expf(-(dk / 2.5f) * (dk / 2.5f));
        I += 0.30f * pb; hot += pb;
      }
      org_plot(&pj, base[0] + q[0], base[1] + q[1], base[2] + q[2], I, hot, gold);
    }
  }

  // --- inner wireframe sphere: dim, counter-rotating, for depth ---
  {
    M3 Wi = m3_mul(G, m3_mul(m3_rx(0.38f), m3_ry(-1.6f * org_spin)));
    float r = ORG_INNER_R, step = 1.6f / (R * r);
    float I = level * (0.09f + 0.06f * act);
    for (int la = -2; la <= 2; la++) {                        // 5 latitudes
      float lat = la * 0.50f;
      int n = (int)(6.2831853f * cosf(lat) / step);
      for (int k = 0; k < n; k++) {
        float p[3], q[3]; org_sph(lat, 6.2831853f * k / n, p);
        org_xf(&Wi, p[0] * r, p[1] * r, p[2] * r, q);
        org_plot(&pj, q[0], q[1], q[2], I, 0.1f, gold);
      }
    }
    int n = (int)(6.2831853f / step);
    for (int m = 0; m < 6; m++) {                             // 12 meridians
      float lon = 3.14159f * m / 6;
      for (int k = 0; k < n; k++) {
        float a = 6.2831853f * k / n;
        if (fabsf(sinf(a)) > 0.97f) continue;                 // keep the poles from piling up
        float p[3] = { cosf(a) * cosf(lon) * r, sinf(a) * r, cosf(a) * sinf(lon) * r }, q[3];
        org_xf(&Wi, p[0], p[1], p[2], q);
        org_plot(&pj, q[0], q[1], q[2], I, 0.1f, gold);
      }
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
        org_embers[i] = (OrgEmber){ x * ORG_SHELL_R, y * ORG_SHELL_R, z * ORG_SHELL_R, x * sp, y * sp, z * sp, 1.0f };
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
