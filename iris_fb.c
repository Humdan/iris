// iris_fb.c — particle sphere-shell visualizer, activity-driven.
// Particles drift on a slowly rotating sphere shell; cyan/blue on black.
// Idle: slow gentle drift, dim. Thinking (act->1): faster, brighter swarm + sparks.
// Reuses the proven scaffolding: mmap fb0, RGB565, float compose in back buffer,
// one memcpy blit per frame, absolute-clock pacing.

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include "iris_widgets.h"

#define NPART 700          // particles on the shell
#define NSPARK 64          // travelling sparks (activity)

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(void) { return rand() / (float)RAND_MAX; }
static float smoothstep(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }

// Cardiac lub-dub envelope over a normalized phase [0,1): a strong first beat
// (lub) and a quick smaller second beat (dub) on a continuous baseline.
// Built from two Gaussians (infinitely smooth, taper naturally to rest) so
// expansion and contraction flow with no velocity snap. Roughly [-0.2, 1.0].
static float heartbeat(float ph) {
    ph -= floorf(ph);
    // wrap-aware distance so the curve is continuous across the 1.0->0.0 seam
    float d_lub = ph - 0.14f; if (d_lub > 0.5f) d_lub -= 1.0f; if (d_lub < -0.5f) d_lub += 1.0f;
    float d_dub = ph - 0.30f; if (d_dub > 0.5f) d_dub -= 1.0f; if (d_dub < -0.5f) d_dub += 1.0f;
    float lub = expf(-(d_lub * d_lub) / (2.0f * 0.075f * 0.075f));         // strong, wide
    float dub = 0.55f * expf(-(d_dub * d_dub) / (2.0f * 0.060f * 0.060f)); // smaller, tighter
    // gentle diastolic dip centered in the long rest (also a Gaussian -> smooth)
    float d_rest = ph - 0.68f; if (d_rest > 0.5f) d_rest -= 1.0f; if (d_rest < -0.5f) d_rest += 1.0f;
    float dip = -0.18f * expf(-(d_rest * d_rest) / (2.0f * 0.14f * 0.14f));
    return lub + dub + dip;
}

// A particle lives in 3D on/near a unit sphere shell. It drifts by a small
// angular velocity in spherical space so it never leaves the shell (smooth,
// no popping, no re-seeding jitter).
typedef struct {
    float theta, phi;      // spherical position (drifts)
    float dtheta, dphi;    // angular drift velocity
    float r;               // radius ~1 with small per-particle offset
    float twinkle;         // phase for gentle brightness variation
    float tw_spd;
} Part;

// A spark rides outward from the shell surface and fades — only when active.
typedef struct { float x, y, z, vx, vy, vz, life; } Spark;

static Part parts[NPART];
static Spark sparks[NSPARK];

static inline void blend_pixel_16(uint16_t *p, float r, float g, float b) {
    uint16_t rv = (uint16_t)(clampf(r, 0, 1) * 31.0f) & 0x1F;
    uint16_t gv = (uint16_t)(clampf(g, 0, 1) * 63.0f) & 0x3F;
    uint16_t bv = (uint16_t)(clampf(b, 0, 1) * 31.0f) & 0x1F;
    uint16_t cr = (*p >> 11) & 0x1F;
    uint16_t cg = (*p >> 5) & 0x3F;
    uint16_t cb = *p & 0x1F;
    cr = (cr + rv) > 0x1F ? 0x1F : cr + rv;
    cg = (cg + gv) > 0x3F ? 0x3F : cg + gv;
    cb = (cb + bv) > 0x1F ? 0x1F : cb + bv;
    *p = (cr << 11) | (cg << 5) | cb;
}

static void dot_16(uint16_t *fb, int W, int H, int STRIDE, float cx, float cy, float rad, float r, float g, float b) {
    int x0 = (int)(cx - rad), x1 = (int)(cx + rad + 1), y0 = (int)(cy - rad), y1 = (int)(cy + rad + 1);
    float inv = 1.0f / (rad * rad + 0.1f);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= H) continue;
        uint16_t *row = fb + y * (STRIDE / 2);
        for (int x = x0; x <= x1; x++) {
            if (x < 0 || x >= W) continue;
            float dx = x - cx, dy = y - cy, d2 = (dx * dx + dy * dy) * inv;
            if (d2 > 1.0f) continue;
            float k = (1 - d2); k *= k;
            blend_pixel_16(&row[x], r * k, g * k, b * k);
        }
    }
}

static void spawn_spark(float theta, float phi, float r, float act) {
    // Direction: mostly radially outward from the shell.
    float sx = sinf(phi) * cosf(theta);
    float sy = cosf(phi);
    float sz = sinf(phi) * sinf(theta);
    for (int i = 0; i < NSPARK; i++) if (sparks[i].life <= 0) {
        float sp = 0.4f + 0.6f * act + frand() * 0.3f;
        sparks[i] = (Spark){ sx * r, sy * r, sz * r,
                             sx * sp, sy * sp, sz * sp, 1.0f };
        return;
    }
}

int main(int argc, char **argv) {
    const char *state_file = argc > 1 ? argv[1] : "/tmp/iris_state";
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v);
    ioctl(fd, FBIOGET_FSCREENINFO, &f);

    int W = v.xres, H = v.yres, BPP = v.bits_per_pixel, STRIDE = f.line_length;
    size_t fbsize = (size_t)STRIDE * H;
    uint8_t *fb_raw = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_raw == MAP_FAILED) { perror("mmap"); return 1; }
    uint16_t *fb = (uint16_t *)fb_raw;
    uint8_t *back_raw = malloc(fbsize);

    fprintf(stderr, "iris_fb (particles): %dx%d %dbpp stride %d\n", W, H, BPP, STRIDE);

    srand(42);
    // Orb offset into the free area: left column for stats, top strip for the
    // clock, bottom strip for the agent panel. Sized as large as fits.
    float ox = 545.0f, oy = 242.0f;
    float scale = 110.0f;

    // Seed particles uniformly on the shell (Fibonacci-ish) with slow drift.
    for (int i = 0; i < NPART; i++) {
        float y = 1.0f - 2.0f * (i + 0.5f) / NPART;   // -1..1
        parts[i].phi = acosf(clampf(y, -1, 1));
        parts[i].theta = 3.14159f * (1.0f + sqrtf(5.0f)) * i;
        parts[i].r = 0.94f + frand() * 0.12f;          // slight shell thickness
        // small tangential drift, random sign — very slow
        parts[i].dtheta = (frand() - 0.5f) * 0.10f;
        parts[i].dphi   = (frand() - 0.5f) * 0.04f;
        parts[i].twinkle = frand() * 6.2831853f;
        parts[i].tw_spd  = 0.4f + frand() * 0.8f;
    }
    for (int i = 0; i < NSPARK; i++) sparks[i].life = 0;

    float act = 0, target = 0;
    double t0 = now(), tlast = t0, tcheck = 0, tfps = t0, tnext = t0, tstats = 0;
    int frames = 0;
    float global_time = 0, spark_time = 0;
    Stats stats; read_stats(&stats);   // widget stats, refreshed ~1 Hz below
    AgentStats agent; read_agent_stats(&agent);

    while (running) {
        double t = now();
        float dt = (float)(t - tlast); tlast = t;
        if (dt > 0.05f) dt = 0.05f;
        global_time += dt;

        // --- read activity state (0..1, or thinking/idle keywords) ---
        if (t - tcheck > 0.1) {
            tcheck = t;
            FILE *sf = fopen(state_file, "r");
            if (sf) {
                char buf[64] = {0};
                if (fgets(buf, 63, sf)) {
                    if (strncmp(buf, "thinking", 8) == 0) target = 1.0f;
                    else if (strncmp(buf, "idle", 4) == 0) target = 0.0f;
                    else { char *end; float x = strtof(buf, &end); target = end != buf ? clampf(x, 0, 1) : 0.0f; }
                }
                fclose(sf);
            }
        }
        // rise fast, fall slow (eased)
        act += (target - act) * (1.0f - expf(-(target > act ? 4.0f : 0.8f) * dt));

        // --- slow eased forward/back rotation of the whole shell (Y axis) ---
        float period = 80.0f;
        float phase = fmodf(global_time, period) / period;
        float tri = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
        float eased = tri * tri * tri * (tri * (tri * 6.0f - 15.0f) + 10.0f);
        float rot = eased * 6.2831853f;
        float cr = cosf(rot), sr = sinf(rot);

        // drift speed scales gently with activity
        float drift = 0.5f + 1.3f * act;

        // Sphere expands when working AND breathes like a heartbeat.
        //  - base growth: eases toward +35% at full activity (bigger than before)
        //  - heartbeat: a periodic expand/contract, amplitude scales with activity
        //    so idle barely breathes and thinking pulses clearly.
        static float scale_dyn = 1.0f;
        float base_target = 1.0f + 0.35f * act;
        scale_dyn += (base_target - scale_dyn) * (1.0f - expf(-2.5f * dt));
        // Cardiac lub-dub. Beat rate rises with activity (~0.5 Hz calm -> ~1.3 Hz busy);
        // amplitude tiny at idle, deep when working (up to +/-20%).
        static float beat_phase = 0.0f;
        float beat_hz = 0.5f + 0.5f * act;
        beat_phase += beat_hz * dt;
        float amp = 0.015f + 0.20f * act;
        float breathe = 1.0f + amp * heartbeat(beat_phase);
        float escale = scale * scale_dyn * breathe;

        // --- draw ---
        memset(back_raw, 0, fbsize);
        uint16_t *back = (uint16_t *)back_raw;

        for (int i = 0; i < NPART; i++) {
            Part *p = &parts[i];
            // advance drift (wrap theta; clamp phi so it doesn't flip poles)
            p->theta += p->dtheta * drift * dt;
            p->phi   += p->dphi   * drift * dt;
            if (p->phi < 0.05f)  { p->phi = 0.05f;  p->dphi = -p->dphi; }
            if (p->phi > 3.0916f){ p->phi = 3.0916f; p->dphi = -p->dphi; }
            p->twinkle += p->tw_spd * dt;

            // 3D position on shell
            float sx = sinf(p->phi) * cosf(p->theta) * p->r;
            float sy = cosf(p->phi) * p->r;
            float sz = sinf(p->phi) * sinf(p->theta) * p->r;
            // rotate about Y
            float xr = sx * cr + sz * sr;
            float zr = -sx * sr + sz * cr;

            float depth = 0.80f + 0.20f * zr;   // 0.6..1.0, front = brighter/bigger
            float px = ox + xr * escale * depth;
            float py = oy + sy * escale * depth;

            // brightness: brighter baseline, gentle twinkle, lifts with activity, dims with depth
            float tw = 0.75f + 0.25f * sinf(p->twinkle);
            float bright = (0.62f + 0.38f * act) * tw * depth;
            float rad = 1.6f + 1.0f * act + 0.8f * depth;

            // cyan idle -> slightly warmer (more green/white) when active
            float rr = bright * (0.05f + 0.35f * act);
            float gg = bright * (0.70f + 0.25f * act);
            float bb = bright * (0.90f);
            dot_16(back, W, H, STRIDE, px, py, rad, rr, gg, bb);
        }

        // --- sparks: emitted when active, fly outward, fade ---
        spark_time += dt;
        float spark_interval = 0.5f - 0.45f * act;   // idle: rare/none, active: frequent
        if (act > 0.15f && spark_time > spark_interval) {
            spark_time = 0;
            int src = rand() % NPART;
            spawn_spark(parts[src].theta, parts[src].phi, parts[src].r, act);
        }
        for (int i = 0; i < NSPARK; i++) {
            Spark *s = &sparks[i];
            if (s->life <= 0) continue;
            s->x += s->vx * dt; s->y += s->vy * dt; s->z += s->vz * dt;
            s->life -= dt * 0.9f;
            float xr = s->x * cr + s->z * sr;
            float zr = -s->x * sr + s->z * cr;
            float depth = 0.80f + 0.20f * zr;
            float px = ox + xr * escale * depth;
            float py = oy + s->y * escale * depth;
            float fade = smoothstep(0, 0.2f, s->life);
            dot_16(back, W, H, STRIDE, px, py, 2.0f + 2.0f * fade,
                   0.5f * fade, 0.8f * fade, 1.0f * fade);
        }

        // --- widgets: clock + system stats + agent panel on top of the orb ---
        if (t - tstats > 1.0) { tstats = t; read_stats(&stats); read_agent_stats(&agent); }
        draw_widgets(back, W, H, STRIDE, &stats);
        draw_agent_panel(back, W, H, STRIDE, &agent);

        // --- blit atomically ---
        memcpy(fb_raw, back_raw, fbsize);

        frames++;
        if (t - tfps > 5) {
            fprintf(stderr, "fps %.1f act %.2f\n", frames / (t - tfps), act);
            frames = 0; tfps = t;
        }

        // absolute-clock pacing ~60 Hz
        tnext += 1.0 / 60.0;
        double sleep_s = tnext - now();
        if (sleep_s > 0) usleep((useconds_t)(sleep_s * 1e6));
        else if (sleep_s < -0.1) tnext = now();   // resync if far behind
    }

    fprintf(stderr, "iris_fb: shutting down\n");
    memset(fb_raw, 0, fbsize);
    free(back_raw);
    munmap(fb_raw, fbsize);
    close(fd);
    return 0;
}
