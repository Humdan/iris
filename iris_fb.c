// iris_fb.c — neural network / knowledge-graph animation for the Pi framebuffer.
//
// Nodes drift on a black field, linked by edges to their neighbours.
// Idle:     slow drift, dim edges, an occasional lazy pulse.
// Thinking: nodes flare, pulses race along edges, the graph rewires and churns.
// State file (default /tmp/iris_state) holds an activity level 0.0-1.0 — the
// graph's firing rate, drift and brightness scale continuously with it.
// The words "thinking" (=1) and "idle" (=0) are also accepted.
//
// build: make   (gcc -O2 -ffast-math iris_fb.c -lm -lpthread)

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NTHREADS 4
#define NNODES 64
#define MAXDEG 4
#define NPULSE 160
#define LINK_DIST 0.42f      // in normalised units (half-height = 1)

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(void) { return rand() / (float)RAND_MAX; }
static float smoothstep(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }

// ---------- framebuffer + float accumulation buffer ----------
static int W, H, BPP, STRIDE;
static float *acc;   // RGB float accumulation, W*H*3

static inline void add_px(int x, int y, float r, float g, float b) {
    if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)H) return;
    float *p = acc + ((size_t)y * W + x) * 3; p[0] += r; p[1] += g; p[2] += b;
}

// soft round dot with gaussian-ish falloff
static void dot(float cx, float cy, float rad, float r, float g, float b) {
    int x0 = (int)(cx - rad - 1), x1 = (int)(cx + rad + 1), y0 = (int)(cy - rad - 1), y1 = (int)(cy + rad + 1);
    float inv = 1.0f / (rad * rad);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = (dx * dx + dy * dy) * inv;
        if (d2 > 1.0f) continue;
        float k = (1 - d2); k *= k;
        add_px(x, y, r * k, g * k, b * k);
    }
}

// anti-aliased soft line of given half-width
static void line(float x0, float y0, float x1, float y1, float hw, float r, float g, float b) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    int steps = (int)(len / 0.9f) + 1;
    float sx = dx / steps, sy = dy / steps;
    float px = x0, py = y0;
    for (int i = 0; i <= steps; i++, px += sx, py += sy) {
        int ix = (int)px, iy = (int)py;
        int R = (int)hw + 1;
        for (int yy = -R; yy <= R; yy++) for (int xx = -R; xx <= R; xx++) {
            float ex = ix + xx + 0.5f - px, ey = iy + yy + 0.5f - py;
            float d = sqrtf(ex * ex + ey * ey) - hw;
            float k = d <= 0 ? 1.0f : d >= 1.0f ? 0.0f : 1.0f - d;
            if (k <= 0) continue;
            add_px(ix + xx, iy + yy, r * k * 0.55f, g * k * 0.55f, b * k * 0.55f);
        }
    }
}

// ---------- background nebula, rendered in threads ----------
struct Job { int y0, y1; float T, think; };

static float hash2(int x, int y) { uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u; h = (h ^ (h >> 13)) * 1274126177u; return (h ^ (h >> 16)) / 4294967296.0f; }
static float vnoise(float x, float y) {           // value noise
    int xi = (int)floorf(x), yi = (int)floorf(y); float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

#define NEB 4                       // nebula downscale factor
static int NW, NH; static float *neb;   // low-res nebula, NW*NH*3

static void *nebula(void *arg) {
    struct Job *j = arg; float T = j->T, th = j->think;
    float sc = 2.6f / NH;
    for (int y = j->y0; y < j->y1; y++) {
        float *row = neb + (size_t)y * NW * 3;
        for (int x = 0; x < NW; x++) {
            float nx = x * sc, ny = y * sc;
            float n = vnoise(nx + T * 0.05f, ny - T * 0.03f) * 0.6f
                    + vnoise(nx * 2.1f - T * 0.08f, ny * 2.1f + T * 0.05f) * 0.3f
                    + vnoise(nx * 4.3f + T * (0.1f + 0.6f * th), ny * 4.3f) * 0.1f * (1 + 2 * th);
            n = n * n;                                 // darken
            float k = 0.22f + 0.18f * th;
            // vignette
            float vx = (x - NW * 0.5f) / (NW * 0.5f), vy = (y - NH * 0.5f) / (NH * 0.5f);
            float vig = 1.0f - 0.55f * (vx * vx + vy * vy);
            k *= vig > 0 ? vig : 0;
            row[x * 3 + 0] = n * k * 40;
            row[x * 3 + 1] = n * k * 110;
            row[x * 3 + 2] = n * k * 170;
        }
    }
    return NULL;
}

static void *upscale(void *arg) {
    struct Job *j = arg;
    for (int y = j->y0; y < j->y1; y++) {
        float fy = (y + 0.5f) / NEB - 0.5f; int y0 = (int)fy; float ty = fy - y0; if (y0 < 0) { y0 = 0; ty = 0; } int y1 = y0 + 1 < NH ? y0 + 1 : y0;
        float *row = acc + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            float fx = (x + 0.5f) / NEB - 0.5f; int x0 = (int)fx; float tx = fx - x0; if (x0 < 0) { x0 = 0; tx = 0; } int x1 = x0 + 1 < NW ? x0 + 1 : x0;
            float *a = neb + ((size_t)y0 * NW + x0) * 3, *b = neb + ((size_t)y0 * NW + x1) * 3, *c = neb + ((size_t)y1 * NW + x0) * 3, *d = neb + ((size_t)y1 * NW + x1) * 3;
            for (int k = 0; k < 3; k++) row[x * 3 + k] = (a[k] + (b[k] - a[k]) * tx) * (1 - ty) + (c[k] + (d[k] - c[k]) * tx) * ty;
        }
    }
    return NULL;
}

// tonemap LUT: input 0..2047 -> 0..255 soft clip
static uint8_t tmap[2048];
struct TJob { int y0, y1; uint8_t *back; };
static void *tonemap(void *arg) {
    struct TJob *j = arg;
    for (int y = j->y0; y < j->y1; y++) {
        uint8_t *row = j->back + (size_t)y * STRIDE; float *src = acc + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            int ri = (int)src[x * 3], gi = (int)src[x * 3 + 1], bi = (int)src[x * 3 + 2];
            uint8_t r8 = tmap[ri > 2047 ? 2047 : ri < 0 ? 0 : ri], g8 = tmap[gi > 2047 ? 2047 : gi < 0 ? 0 : gi], b8 = tmap[bi > 2047 ? 2047 : bi < 0 ? 0 : bi];
            if (BPP == 16) ((uint16_t *)row)[x] = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
            else ((uint32_t *)row)[x] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
        }
    }
    return NULL;
}

// ---------- graph ----------
#define MAXEDGE 8            // slots per node: active neighbours + fading-out ones
typedef struct { int to; float w, target; } Edge;
typedef struct { float x, y, vx, vy, phase, energy, etarget, size; int deg; Edge e[MAXEDGE]; } Node;
typedef struct { int a, b; float t, spd, alive; } Pulse;

static Node nodes[NNODES]; static Pulse pulses[NPULSE];

static void rewire(void) {
    for (int i = 0; i < NNODES; i++) {
        Node *n = &nodes[i];
        int want[MAXDEG], nw = 0;
        for (int k = 0; k < MAXDEG; k++) {
            int best = -1; float bd = LINK_DIST * LINK_DIST;
            for (int j = 0; j < NNODES; j++) {
                if (j == i) continue;
                int dup = 0; for (int q = 0; q < nw; q++) if (want[q] == j) dup = 1;
                if (dup) continue;
                float dx = n->x - nodes[j].x, dy = n->y - nodes[j].y, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = j; }
            }
            if (best < 0) break;
            want[nw++] = best;
        }
        // existing edges: keep if wanted, else fade out
        for (int q = 0; q < n->deg; q++) {
            int keep = 0; for (int k = 0; k < nw; k++) if (want[k] == n->e[q].to) keep = 1;
            n->e[q].target = keep ? 1.0f : 0.0f;
        }
        // wanted edges not present: add fading in (reuse a dead slot or append)
        for (int k = 0; k < nw; k++) {
            int have = 0; for (int q = 0; q < n->deg; q++) if (n->e[q].to == want[k]) have = 1;
            if (have) continue;
            int slot = -1;
            for (int q = 0; q < n->deg; q++) if (n->e[q].w < 0.02f && n->e[q].target == 0) { slot = q; break; }
            if (slot < 0 && n->deg < MAXEDGE) slot = n->deg++;
            if (slot >= 0) n->e[slot] = (Edge){ want[k], 0.0f, 1.0f };
        }
    }
}

static void ease_edges(float dt) {
    float k = clampf(dt * 2.2f, 0, 1);                       // ~0.7s fade
    for (int i = 0; i < NNODES; i++) {
        Node *n = &nodes[i];
        for (int q = 0; q < n->deg; q++) n->e[q].w += (n->e[q].target - n->e[q].w) * k;
        // compact fully-faded trailing slots
        while (n->deg > 0 && n->e[n->deg - 1].target == 0 && n->e[n->deg - 1].w < 0.01f) n->deg--;
    }
}

static void fire(int from, float think) {
    Node *n = &nodes[from];
    int live[MAXEDGE], nl = 0;
    for (int q = 0; q < n->deg; q++) if (n->e[q].w > 0.5f) live[nl++] = n->e[q].to;
    if (nl == 0) return;
    for (int i = 0; i < NPULSE; i++) if (pulses[i].alive <= 0) {
        pulses[i] = (Pulse){ from, live[rand() % nl], 0, 0.9f + frand() * 0.8f + 2.5f * think, 1.0f };
        return;
    }
}

int main(int argc, char **argv) {
    const char *state_file = argc > 1 ? argv[1] : "/tmp/iris_state";
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int fd = open("/dev/fb0", O_RDWR); if (fd < 0) { perror("open /dev/fb0"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
    W = v.xres; H = v.yres; BPP = v.bits_per_pixel; STRIDE = f.line_length;
    size_t fbsize = (size_t)STRIDE * v.yres_virtual;
    uint8_t *fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    fprintf(stderr, "iris_fb: %dx%d %dbpp stride %d\n", W, H, BPP, STRIDE);
    uint8_t *back = malloc((size_t)STRIDE * H);
    acc = malloc(sizeof(float) * W * H * 3);
    NW = W / NEB; NH = H / NEB; neb = malloc(sizeof(float) * NW * NH * 3);
    for (int i = 0; i < 2048; i++) tmap[i] = (uint8_t)(255 * (1 - expf(-i / 200.0f)));

    float aspect = (float)W / H;                 // normalised coords: x in [-aspect,aspect], y in [-1,1]
    srand(11);
    for (int i = 0; i < NNODES; i++) {
        nodes[i] = (Node){ (frand() * 2 - 1) * aspect * 0.95f, (frand() * 2 - 1) * 0.92f,
                           0, 0, frand() * 6.28f, 0, 0, 2.2f + frand() * 2.5f, 0, {{0}} };
    }
    rewire();

    float think = 0, target = 0; double t0 = now(), tlast = t0, tcheck = 0, trewire = 0, tfps = t0; int frames = 0;
    float fire_acc = 0; double next_frame = now();

    while (running) {
        double t = now(); float dt = (float)(t - tlast); tlast = t; if (dt > 0.1f) dt = 0.1f;
        if (t - tcheck > 0.15) {
            tcheck = t; FILE *sf = fopen(state_file, "r");
            if (sf) {
                char buf[64] = {0};
                if (fgets(buf, 63, sf)) {
                    if (strncmp(buf, "thinking", 8) == 0) target = 1.0f;
                    else if (strncmp(buf, "idle", 4) == 0) target = 0.0f;
                    else { char *end; float v = strtof(buf, &end); target = end != buf ? clampf(v, 0, 1) : 0.0f; }
                }
                fclose(sf);
            }
        }
        think += (target - think) * clampf((target > think ? 4.0f : 1.5f) * dt, 0, 1);
        float T = (float)(t - t0);

        // --- physics: drift + gentle spring to neighbours + repulsion ---
        float drift = 0.02f + 0.10f * think;
        for (int i = 0; i < NNODES; i++) {
            Node *n = &nodes[i];
            float ax = sinf(T * 0.31f + n->phase) * drift + cosf(T * 0.17f + n->phase * 1.7f) * drift * 0.6f;
            float ay = cosf(T * 0.27f + n->phase * 1.3f) * drift;
            for (int q = 0; q < n->deg; q++) {            // spring to neighbours, weighted by edge strength
                Node *m = &nodes[n->e[q].to]; float dx = m->x - n->x, dy = m->y - n->y, d = sqrtf(dx * dx + dy * dy) + 1e-4f;
                float k = (d - 0.30f) * 0.5f * n->e[q].w; ax += dx / d * k; ay += dy / d * k;
            }
            for (int j = 0; j < NNODES; j++) if (j != i) {     // short-range repulsion
                float dx = n->x - nodes[j].x, dy = n->y - nodes[j].y, d2 = dx * dx + dy * dy;
                if (d2 < 0.07f) { float k = (0.07f - d2) * 6.0f; ax += dx * k; ay += dy * k; }
            }
            // stay on screen
            ax -= n->x * 0.03f * (fabsf(n->x) > aspect * 0.85f ? 12 : 1);
            ay -= n->y * 0.03f * (fabsf(n->y) > 0.85f ? 12 : 1);
            n->vx = (n->vx + ax * dt) * 0.97f; n->vy = (n->vy + ay * dt) * 0.97f;
            n->x += n->vx * dt; n->y += n->vy * dt;
            n->x = clampf(n->x, -aspect * 0.95f, aspect * 0.95f); n->y = clampf(n->y, -0.93f, 0.93f);
            n->etarget *= expf(-dt * (1.8f + 1.5f * think));
            n->energy += (n->etarget - n->energy) * clampf(dt * 18.0f, 0, 1);
        }
        if (t - trewire > (2.5 - 2.0 * think)) { trewire = t; rewire(); }
        ease_edges(dt);

        // --- fire pulses: rate scales hard with thinking ---
        fire_acc += dt * (0.5f + 6.0f * think + 12.0f * think * think);
        while (fire_acc >= 1) { fire_acc -= 1; fire(rand() % NNODES, think); }
        for (int i = 0; i < NPULSE; i++) {
            Pulse *p = &pulses[i]; if (p->alive <= 0) continue;
            p->t += dt * p->spd;
            if (p->t >= 1) {                              // arrive: light node, maybe propagate
                p->alive = 0; nodes[p->b].etarget = 1.0f;
                if (frand() < 0.25f + 0.55f * think) fire(p->b, think);
            }
        }

        // --- render ---
        pthread_t th[NTHREADS];
        memset(acc, 0, sizeof(float) * W * H * 3);   // solid black background

        float sx = H * 0.5f, ox = W * 0.5f, oy = H * 0.5f;   // to pixels
        #define PX(n) (ox + (n).x * sx)
        #define PY(n) (oy + (n).y * sx)

        // edges (weight w fades in/out over rewires)
        float eb = 0.30f + 0.15f * think;
        for (int i = 0; i < NNODES; i++) for (int q = 0; q < nodes[i].deg; q++) {
            int j = nodes[i].e[q].to; float w = nodes[i].e[q].w; if (w < 0.01f) continue;
            if (j < i) {   // if j also links to i, draw once from the lower index with the stronger weight
                for (int qq = 0; qq < nodes[j].deg; qq++) if (nodes[j].e[qq].to == i && nodes[j].e[qq].w >= w) { w = -1; break; }
                if (w < 0) continue;
            }
            float e = clampf(nodes[i].energy + nodes[j].energy, 0, 1);
            float k = eb * (0.35f + 0.65f * e) * w;
            line(PX(nodes[i]), PY(nodes[i]), PX(nodes[j]), PY(nodes[j]), 0.6f + 0.6f * e, 20 * k + 60 * e * w, 120 * k + 160 * e * w, 170 * k + 200 * e * w);
        }
        // pulses travelling along edges
        for (int i = 0; i < NPULSE; i++) {
            Pulse *p = &pulses[i]; if (p->alive <= 0) continue;
            float x = PX(nodes[p->a]) + (PX(nodes[p->b]) - PX(nodes[p->a])) * p->t;
            float y = PY(nodes[p->a]) + (PY(nodes[p->b]) - PY(nodes[p->a])) * p->t;
            float fade = smoothstep(0, 0.15f, p->t) * (1 - smoothstep(0.85f, 1.0f, p->t));
            dot(x, y, 4.5f + 3.0f * think, 160 * fade, 240 * fade, 255 * fade);
            dot(x, y, 11.0f, 20 * fade, 70 * fade, 110 * fade);   // halo
        }
        // nodes
        for (int i = 0; i < NNODES; i++) {
            Node *n = &nodes[i]; float e = n->energy;
            float br = 0.35f + 0.25f * sinf(T * 0.8f + n->phase) + 0.2f * think;
            float rad = n->size * (1 + 0.15f * think) + 3.0f * e;
            dot(PX(*n), PY(*n), rad * 3.2f, 8 * br + 25 * e, 40 * br + 90 * e, 70 * br + 130 * e);          // glow
            dot(PX(*n), PY(*n), rad, 90 * br + 160 * e, 200 * br + 60 * e, 235 * br + 20 * e);              // core
            if (e > 0.05f) dot(PX(*n), PY(*n), rad * 0.55f, 200 * e, 255 * e, 255 * e);                     // white-hot flash
        }

        struct TJob tj[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) { tj[i] = (struct TJob){ i * H / NTHREADS, (i + 1) * H / NTHREADS, back }; pthread_create(&th[i], NULL, tonemap, &tj[i]); }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);
        // pace on an absolute 30 Hz clock, then blit the whole frame in one go
        next_frame += 1.0 / 30.0;
        double wait = next_frame - now();
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
        else if (wait < -0.1) next_frame = now();          // fell far behind: resync
        memcpy(fb, back, (size_t)STRIDE * H);

        frames++; if (t - tfps > 5) { fprintf(stderr, "fps %.1f think %.2f\n", frames / (t - tfps), think); frames = 0; tfps = t; }
    }
    memset(fb, 0, (size_t)STRIDE * H);
    munmap(fb, fbsize); close(fd); free(back); free(acc); free(neb);
    return 0;
}
