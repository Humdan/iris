// iris_fb.c — smooth neural network with gentle continuous rotation

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

#define NNODES 80
#define MAXDEG 5
#define NPULSE 48
#define NTRAILS 128

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float frand(void) { return rand() / (float)RAND_MAX; }
static float smoothstep(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }

typedef struct { int to; } Edge;
typedef struct { float px, py; float base_x, base_y, base_z; float energy; int deg; Edge e[MAXDEG]; } Node;
typedef struct { int a, b; float t, spd, alive; } Pulse;
typedef struct { float x, y, vx, vy, life; } Trail;

static Node nodes[NNODES];
static Pulse pulses[NPULSE];
static Trail trails[NTRAILS];

static void fire(int from, int avoid, float act) {
    Node *n = &nodes[from];
    int live[MAXDEG], nl = 0;
    for (int q = 0; q < n->deg; q++) if (n->e[q].to != avoid) live[nl++] = n->e[q].to;
    if (nl == 0) return;
    for (int i = 0; i < NPULSE; i++) if (pulses[i].alive <= 0) {
        pulses[i] = (Pulse){ from, live[rand() % nl], 0, 0.35f + frand() * 0.25f + 1.2f * act, 1.0f };
        return;
    }
}

static inline void blend_pixel_16(uint16_t *p, float r, float g, float b) {
    uint16_t rv = (uint16_t)(r * 31.0f) & 0x1F;
    uint16_t gv = (uint16_t)(g * 63.0f) & 0x3F;
    uint16_t bv = (uint16_t)(b * 31.0f) & 0x1F;
    
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
            float k = (1 - d2); k *= k * k;
            blend_pixel_16(&row[x], r * k, g * k, b * k);
        }
    }
}

static void line_16(uint16_t *fb, int W, int H, int STRIDE, float x0, float y0, float x1, float y1, float r, float g, float b) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx*dx + dy*dy);
    if (dist < 1.0f) return;
    int steps = (int)(dist * 1.5f) + 1;
    float step_x = dx / steps, step_y = dy / steps;
    for (int i = 0; i <= steps; i++) {
        float x = x0 + step_x * i;
        float y = y0 + step_y * i;
        int ix = (int)x, iy = (int)y;
        if (ix >= 0 && ix < W && iy >= 0 && iy < H) {
            blend_pixel_16(&fb[iy * (STRIDE / 2) + ix], r, g, b);
        }
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
    
    int W = v.xres; 
    int H = v.yres; 
    int BPP = v.bits_per_pixel; 
    int STRIDE = f.line_length;
    
    size_t fbsize = (size_t)STRIDE * H;
    uint8_t *fb_raw = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb_raw == MAP_FAILED) { perror("mmap"); return 1; }
    
    uint16_t *fb = (uint16_t *)fb_raw;
    uint8_t *back_raw = malloc(fbsize);
    uint16_t *back = (uint16_t *)back_raw;
    
    fprintf(stderr, "iris_fb: %dx%d %dbpp stride %d\n", W, H, BPP, STRIDE);
    
    // Initialize nodes on sphere
    srand(42);
    float ox = W * 0.5f, oy = H * 0.5f;
    float scale = fminf(W, H) * 0.32f;
    
    for (int i = 0; i < NNODES; i++) {
        float phi = acosf(1.0f - 2.0f * i / (float)NNODES);
        float theta = 3.14159f * (1.0f + sqrtf(5.0f)) * i;
        nodes[i].base_x = sinf(phi) * cosf(theta);
        nodes[i].base_y = cosf(phi);
        nodes[i].base_z = sinf(phi) * sinf(theta);
        nodes[i].px = ox + nodes[i].base_x * scale;
        nodes[i].py = oy + nodes[i].base_y * scale;
        nodes[i].energy = 0;
        nodes[i].deg = 0;
    }
    
    // Build denser edges
    for (int i = 0; i < NNODES; i++) {
        float dists[NNODES];
        for (int j = 0; j < NNODES; j++) {
            if (i == j) { dists[j] = 1e9; continue; }
            float dx = nodes[j].px - nodes[i].px;
            float dy = nodes[j].py - nodes[i].py;
            dists[j] = dx*dx + dy*dy;
        }
        for (int k = 0; k < MAXDEG && nodes[i].deg < MAXDEG; k++) {
            int best = -1; float bd = 1e9;
            for (int j = 0; j < NNODES; j++) {
                if (dists[j] < bd) { bd = dists[j]; best = j; }
            }
            if (best < 0) break;
            nodes[i].e[nodes[i].deg++] = (Edge){ best };
            dists[best] = 1e9;
        }
    }
    
    for (int i = 0; i < NTRAILS; i++) trails[i].life = 0;
    
    float act = 0, target = 0;
    double t0 = now(), tlast = t0, tcheck = 0, tfps = t0;
    int frames = 0;
    float pattern_time = 0;
    int pattern_idx = 0;
    float global_time = 0;
    
    while (running) {
        double t = now(); 
        float dt = (float)(t - tlast); 
        tlast = t; 
        if (dt > 0.05f) dt = 0.05f;
        
        global_time += dt;
        
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
        
        act += (target - act) * (1.0f - expf(-(target > act ? 4.0f : 0.8f) * dt));
        
        // Update energy
        for (int i = 0; i < NNODES; i++) {
            nodes[i].energy *= expf(-dt * (1.0f + 1.8f * act));
        }
        
        // Pattern-based firing
        pattern_time += dt;
        if (pattern_time > 0.15f) {
            pattern_time = 0;
            pattern_idx = (pattern_idx + 1) % NNODES;
            nodes[pattern_idx].energy = 1.0f;
            fire(pattern_idx, -1, act);
            
            if (act > 0.3f) {
                Node *n = &nodes[pattern_idx];
                for (int q = 0; q < n->deg; q++) {
                    nodes[n->e[q].to].energy = 0.6f;
                }
            }
        }
        
        // Propagate pulses
        for (int i = 0; i < NPULSE; i++) {
            Pulse *p = &pulses[i]; 
            if (p->alive <= 0) continue;
            p->t += dt * p->spd;
            if (p->t >= 1) {
                p->alive = 0; 
                nodes[p->b].energy = 1.0f;
                if (frand() < 0.3f + 0.6f * act) fire(p->b, p->a, act);
            }
        }
        
        // Update trails
        for (int i = 0; i < NTRAILS; i++) {
            if (trails[i].life <= 0) continue;
            trails[i].x += trails[i].vx * dt;
            trails[i].y += trails[i].vy * dt;
            trails[i].vx *= 0.92f;
            trails[i].vy *= 0.92f;
            trails[i].life -= dt * 3.0f;
        }
        
        // Spawn trails
        for (int i = 0; i < NPULSE; i++) {
            Pulse *p = &pulses[i];
            if (p->alive <= 0) continue;
            if (frand() < 0.3f) {
                for (int j = 0; j < NTRAILS; j++) {
                    if (trails[j].life <= 0) {
                        float x = nodes[p->a].px + (nodes[p->b].px - nodes[p->a].px) * p->t;
                        float y = nodes[p->a].py + (nodes[p->b].py - nodes[p->a].py) * p->t;
                        trails[j] = (Trail){ x, y, (frand()-0.5f)*30, (frand()-0.5f)*30, 1.0f };
                        break;
                    }
                }
            }
        }
        
        // Smooth eased forward/back rotation on Y axis.
        // Angle sweeps ~30s one way then ~30s back, eased so direction never snaps.
        // period = 60s round trip; phase in [0,1); smootherstep gives zero-velocity turns.
        float period = 60.0f;
        float phase = fmodf(global_time, period) / period;   // 0..1
        float tri = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;  // 0..1..0
        float eased = tri * tri * tri * (tri * (tri * 6.0f - 15.0f) + 10.0f); // smootherstep
        float rot_angle = eased * 6.2831853f;   // full turn each direction
        float cos_a = cosf(rot_angle);
        float sin_a = sinf(rot_angle);

        for (int i = 0; i < NNODES; i++) {
            // Proper Y-axis rotation: x/z rotate, y is the axis and stays fixed.
            float x = nodes[i].base_x;
            float z = nodes[i].base_z;
            float x_rot = x * cos_a + z * sin_a;
            float z_rot = -x * sin_a + z * cos_a;

            // Perspective-ish depth: nodes toward viewer (z_rot>0) slightly larger.
            float depth = 0.85f + 0.15f * z_rot;   // 0.70..1.00
            nodes[i].px = ox + x_rot * scale * depth;
            nodes[i].py = oy + nodes[i].base_y * scale * depth;
        }
        
        // Clear back
        memset(back_raw, 0, fbsize);
        
        // Draw edges only - minimal
        for (int i = 0; i < NNODES; i++) {
            for (int q = 0; q < nodes[i].deg; q++) {
                int j = nodes[i].e[q].to;
                if (j < i) continue;
                float glow = nodes[i].energy * 0.3f + nodes[j].energy * 0.3f;
                float b = 0.1f + glow * 0.5f;
                line_16(back, W, H, STRIDE, nodes[i].px, nodes[i].py, nodes[j].px, nodes[j].py,
                       0.01f, 0.05f + glow * 0.2f, b);
            }
        }
        
        // Draw pulses - clean bright lines
        for (int i = 0; i < NPULSE; i++) {
            Pulse *p = &pulses[i]; 
            if (p->alive <= 0) continue;
            float x = nodes[p->a].px + (nodes[p->b].px - nodes[p->a].px) * p->t;
            float y = nodes[p->a].py + (nodes[p->b].py - nodes[p->a].py) * p->t;
            float fade = smoothstep(0, 0.1f, p->t) * (1 - smoothstep(0.9f, 1.0f, p->t));
            dot_16(back, W, H, STRIDE, x, y, 4.0f + 3.0f * fade, 0.5f*fade, 0.8f*fade, fade);
        }
        
        // Draw nodes - clean minimal glow
        for (int i = 0; i < NNODES; i++) {
            float e = nodes[i].energy;
            
            // Outer faint glow
            dot_16(back, W, H, STRIDE, nodes[i].px, nodes[i].py, 6.0f + 4.0f * e,
                  0.02f, 0.08f + 0.2f * e, 0.12f + 0.3f * e);
            
            // Core - bright cyan when active
            float core = 2.0f + 1.5f * e;
            dot_16(back, W, H, STRIDE, nodes[i].px, nodes[i].py, core,
                  0.1f + 0.2f * e, 0.4f + 0.5f * e, 0.6f + 0.4f * e);
        }
        
        // Blit
        memcpy(fb_raw, back_raw, fbsize);
        
        frames++;
        if (t - tfps > 5) {
            fprintf(stderr, "fps %.1f act %.2f\n", frames / (t - tfps), act);
            frames = 0;
            tfps = t;
        }
        
        usleep(16667);
    }
    
    fprintf(stderr, "iris_fb: shutting down\n");
    memset(fb_raw, 0, fbsize);
    free(back_raw);
    munmap(fb_raw, fbsize); 
    close(fd);
    return 0;
}
