// iris_fb.c — smooth per-pixel "iris" animation for the Raspberry Pi framebuffer.
//
// Idle:     slow breathing, soft glow, dim drifting particles.
// Thinking: iris contracts, ring spins fast, bright rays, flying sparks.
// Transitions are eased; state read from a file (default /tmp/iris_state).
//
// build: gcc -O2 -o iris_fb iris_fb.c -lm

#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#define NTHREADS 4

#define NPART 96

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float smoothstep(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t);
}
static float frand(void) { return rand() / (float)RAND_MAX; }

typedef struct { float ang, rad, spd, life, size; } Particle;

static void respawn(Particle *p, float think) {
    p->ang = frand() * 6.2831853f;
    p->rad = 0.35f + frand() * 0.25f;
    p->spd = (0.15f + frand() * 0.35f) * (1 + 2.5f * think);
    p->life = 1.0f;
    p->size = 1.5f + frand() * 2.0f;
}

struct Job { uint8_t *back; float *pr, *pa; int W, H, bpp, stride, y0, y1;
             float think, T, spin, breath, pupil, iris_r, glow, baseR, baseG, baseB; };

static void *render_rows(void *arg) {
    struct Job *j = arg;
    uint8_t *back = j->back; float *pr = j->pr, *pa = j->pa;
    int W = j->W, bpp = j->bpp, stride = j->stride;
    float think = j->think, T = j->T, spin = j->spin, breath = j->breath, pupil = j->pupil,
          iris_r = j->iris_r, glow = j->glow, baseR = j->baseR, baseG = j->baseG, baseB = j->baseB;
        for (int y = j->y0; y < j->y1; y++) {
            uint8_t *row = back + (size_t)y * stride;
            for (int x = 0; x < W; x++) {
                float r = pr[y * W + x], a = pa[y * W + x];
                float R = 0, G = 0, B = 0;
                if (r > 1.75f) { if (bpp == 16) ((uint16_t *)row)[x] = 0; else ((uint32_t *)row)[x] = 0xFF000000u; continue; }

                // iris body: radial fibres + spinning ring
                if (r > pupil && r < iris_r) {
                    float u = (r - pupil) / (iris_r - pupil);           // 0 inner .. 1 outer
                    float fib = 0.55f + 0.45f * sinf(a * 24 + spin * 2 + u * 6);
                    float fib2 = 0.5f + 0.5f * sinf(a * 7 - spin * 1.3f);
                    float band = 0.6f + 0.4f * sinf(u * 18 - spin * 4);
                    float shade = (0.35f + 0.65f * fib * fib2) * (0.5f + 0.5f * band);
                    float edge = smoothstep(0, 0.08f, u) * (1 - smoothstep(0.80f, 1.0f, u));
                    float k = shade * edge * (0.7f + 0.5f * glow);
                    R += baseR * k; G += baseG * k; B += baseB * k;
                    // hot inner rim
                    float rim = expf(-u * 14) * (0.6f + 0.8f * think);
                    R += 120 * rim; G += 240 * rim; B += 255 * rim;
                }
                // pupil: near-black with faint core pulse
                if (r <= pupil) {
                    float core = expf(-r * r * 90) * (0.15f + 0.5f * think) * breath;
                    R += 80 * core; G += 200 * core; B += 255 * core;
                }
                // outer halo
                float halo = expf(-(r - iris_r) * (r - iris_r) * 60) * glow * 0.9f;
                if (r >= iris_r - 0.04f) { R += baseR * halo * 0.8f; G += baseG * halo * 0.8f; B += baseB * halo; }
                // thinking rays sweeping outwards
                if (think > 0.02f && r > iris_r) {
                    float ray = powf(0.5f + 0.5f * sinf(a * 7 + spin * 3), 24);
                    float fade = expf(-(r - iris_r) * 3);
                    float k = ray * fade * think * 0.7f;
                    R += 60 * k; G += 220 * k; B += 255 * k;
                }
                // distant scanline shimmer keeps idle from looking static
                float shimmer = 0.5f + 0.5f * sinf(y * 0.08f - T * 1.5f);
                B += 4 * shimmer * (1 - think);

                uint8_t r8 = R > 255 ? 255 : (uint8_t)R, g8 = G > 255 ? 255 : (uint8_t)G, b8 = B > 255 ? 255 : (uint8_t)B;
                if (bpp == 16) ((uint16_t *)row)[x] = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
                else           ((uint32_t *)row)[x] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
            }
        }
    return NULL;
}

int main(int argc, char **argv) {
    const char *state_file = argc > 1 ? argv[1] : "/tmp/iris_state";
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open /dev/fb0"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
    int W = v.xres, H = v.yres, bpp = v.bits_per_pixel, stride = f.line_length;
    size_t fbsize = (size_t)stride * v.yres_virtual;
    uint8_t *fb = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    fprintf(stderr, "iris_fb: %dx%d %dbpp stride %d\n", W, H, bpp, stride);

    uint8_t *back = malloc((size_t)stride * H);

    // Precompute polar coords per pixel (radius normalised to half-height, angle).
    float cx = W * 0.5f, cy = H * 0.5f, scale = 1.0f / (H * 0.5f);
    float *pr = malloc(sizeof(float) * W * H), *pa = malloc(sizeof(float) * W * H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float dx = (x - cx) * scale, dy = (y - cy) * scale;
        pr[y * W + x] = sqrtf(dx * dx + dy * dy);
        pa[y * W + x] = atan2f(dy, dx);
    }

    Particle parts[NPART];
    srand(7);
    for (int i = 0; i < NPART; i++) { respawn(&parts[i], 0); parts[i].life = frand(); }

    float think = 0.0f, target = 0.0f;   // eased activity level 0..1
    double t0 = now(), tlast = t0, tcheck = 0;
    float spin = 0; int frames = 0; double tfps = t0;

    while (running) {
        double t = now(); float dt = (float)(t - tlast); tlast = t;
        if (dt > 0.1f) dt = 0.1f;

        if (t - tcheck > 0.15) {          // poll state file
            tcheck = t;
            FILE *sf = fopen(state_file, "r");
            if (sf) { char buf[32] = {0}; if (fgets(buf, 31, sf)) target = strncmp(buf, "thinking", 8) == 0 ? 1.0f : 0.0f; fclose(sf); }
        }
        // ease towards target (fast ramp up, slower cool-down)
        float rate = target > think ? 3.0f : 1.2f;
        think += (target - think) * clampf(rate * dt, 0, 1);

        float T = (float)(t - t0);
        spin += dt * (0.25f + 3.5f * think);
        float breath = 0.5f + 0.5f * sinf(T * (0.6f + 2.5f * think));
        float pupil  = 0.14f + 0.06f * breath - 0.06f * think;          // contracts when thinking
        float iris_r = 0.52f + 0.03f * breath + 0.05f * think;
        float glow   = 0.35f + 0.25f * breath + 0.5f * think;

        // colour: idle deep teal -> thinking electric cyan/white
        float baseR = 10 + 60 * think, baseG = 120 + 100 * think, baseB = 150 + 105 * think;

        struct Job jobs[NTHREADS]; pthread_t th[NTHREADS];
        for (int i = 0; i < NTHREADS; i++) {
            jobs[i] = (struct Job){ back, pr, pa, W, H, bpp, stride, i * H / NTHREADS, (i + 1) * H / NTHREADS,
                                    think, T, spin, breath, pupil, iris_r, glow, baseR, baseG, baseB };
            pthread_create(&th[i], NULL, render_rows, &jobs[i]);
        }
        for (int i = 0; i < NTHREADS; i++) pthread_join(th[i], NULL);

        // particles / sparks drifting outward
        for (int i = 0; i < NPART; i++) {
            Particle *p = &parts[i];
            p->rad += p->spd * dt * (0.3f + think);
            p->ang += dt * (0.1f + 1.5f * think);
            p->life -= dt * (0.25f + 0.9f * think);
            if (p->life <= 0 || p->rad > 1.9f) respawn(p, think);
            float px = cx + cosf(p->ang) * p->rad / scale, py = cy + sinf(p->ang) * p->rad / scale;
            float br = p->life * (0.35f + 0.65f * think);
            int sz = (int)(p->size * (0.6f + think));
            for (int yy = -sz; yy <= sz; yy++) for (int xx = -sz; xx <= sz; xx++) {
                int X = (int)px + xx, Y = (int)py + yy;
                if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
                float d = 1.0f - sqrtf((float)(xx * xx + yy * yy)) / (sz + 1);
                if (d <= 0) continue;
                float k = d * br;
                uint8_t *row = back + (size_t)Y * stride;
                uint8_t r8, g8, b8;
                if (bpp == 16) {
                    uint16_t c = ((uint16_t *)row)[X];
                    r8 = ((c >> 11) & 31) << 3; g8 = ((c >> 5) & 63) << 2; b8 = (c & 31) << 3;
                } else { uint32_t c = ((uint32_t *)row)[X]; r8 = c >> 16; g8 = c >> 8; b8 = c; }
                int nr = r8 + (int)(150 * k), ng = g8 + (int)(240 * k), nb = b8 + (int)(255 * k);
                r8 = nr > 255 ? 255 : nr; g8 = ng > 255 ? 255 : ng; b8 = nb > 255 ? 255 : nb;
                if (bpp == 16) ((uint16_t *)row)[X] = (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
                else ((uint32_t *)row)[X] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
            }
        }

        memcpy(fb, back, (size_t)stride * H);
        frames++; if (t - tfps > 5) { fprintf(stderr, "fps %.1f think %.2f\n", frames / (t - tfps), think); frames = 0; tfps = t; }
        // pace: ~30fps thinking, ~20fps idle
        float target_dt = 1.0f / (20 + 10 * think);
        float spent = (float)(now() - t);
        if (spent < target_dt) usleep((useconds_t)((target_dt - spent) * 1e6f));
    }

    memset(fb, 0, (size_t)stride * H);
    munmap(fb, fbsize); close(fd); free(back); free(pr); free(pa);
    return 0;
}
