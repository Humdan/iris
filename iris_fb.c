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
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <linux/input.h>
#include "iris_widgets.h"

#define NPART 700          // particles on the shell
#define NSPARK 64          // travelling sparks (activity)

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

// Fire the Night shift cron job in a detached child so the render loop never
// blocks. SIGCHLD is set to SIG_IGN at startup so exited children are reaped
// automatically (no zombies, no waitpid in the loop).
static void ns_fire_job(void) {
    pid_t pid = fork();
    if (pid == 0) {
        // child: silence stdio, exec the hermes CLI, then vanish
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execl("/home/humdan/.local/bin/hermes", "hermes", "cron", "run",
              NS_JOB_ID, (char *)NULL);
        _exit(127);   // exec failed
    }
    // parent returns immediately; SIG_IGN on SIGCHLD reaps the child.
}

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

// ---------------------------------------------------------------------------
// Touch input: a dedicated thread reads /dev/input/event4 (evdev, FT5x06) and
// publishes the single active finger's state into a mutex-guarded struct. The
// render loop only SAMPLES it once per frame — never reads the device — so the
// 60Hz absolute-clock pacing is never blocked by touch I/O.
// ---------------------------------------------------------------------------
#define TOUCH_DEV "/dev/input/event4"

typedef struct {
    pthread_mutex_t m;
    int   down;         // finger currently on the glass
    int   x, y;         // latest position (screen pixels, 800x480)
    // per-contact event stream: monotonically bumped so the loop can detect
    // fresh down/up transitions and movement without racing.
    int   seq_down;     // incremented on each finger-down
    int   seq_up;       // incremented on each finger-up
    int   down_x, down_y; // where the current/last contact began
    int   up_x, up_y;     // where the last contact ended
} TouchState;

static TouchState touch = { .m = PTHREAD_MUTEX_INITIALIZER, .down = 0, .x = 0, .y = 0,
                            .seq_down = 0, .seq_up = 0 };

static void *touch_thread(void *arg) {
    (void)arg;
    int tfd = open(TOUCH_DEV, O_RDONLY);
    if (tfd < 0) {
        fprintf(stderr, "touch: open %s failed: %s (gestures disabled)\n", TOUCH_DEV, strerror(errno));
        return NULL;
    }
    fprintf(stderr, "touch: reading %s\n", TOUCH_DEV);

    int cur_x = 0, cur_y = 0;   // accumulated within the current SYN frame
    int have_x = 0, have_y = 0;
    int contact = 0;            // tracking-id >= 0 means finger present

    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    struct input_event ev;

    while (running) {
        int pr = poll(&pfd, 1, 200);   // 200ms wakeups so we notice `running`
        if (pr <= 0) continue;
        ssize_t n = read(tfd, &ev, sizeof(ev));
        if (n != (ssize_t)sizeof(ev)) continue;

        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X) { cur_x = ev.value; have_x = 1; }
            else if (ev.code == ABS_MT_POSITION_Y) { cur_y = ev.value; have_y = 1; }
            else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value < 0) {          // finger lifted
                    pthread_mutex_lock(&touch.m);
                    if (touch.down) { touch.up_x = touch.x; touch.up_y = touch.y; touch.seq_up++; }
                    touch.down = 0;
                    pthread_mutex_unlock(&touch.m);
                    contact = 0;
                } else {                      // new contact
                    contact = 1;
                }
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            // commit this frame's accumulated position
            if (have_x || have_y || contact) {
                pthread_mutex_lock(&touch.m);
                if (have_x) touch.x = cur_x;
                if (have_y) touch.y = cur_y;
                if (contact && !touch.down) {  // rising edge: finger-down
                    touch.down = 1;
                    touch.down_x = touch.x;
                    touch.down_y = touch.y;
                    touch.seq_down++;
                }
                pthread_mutex_unlock(&touch.m);
            }
            have_x = have_y = 0;
        }
    }
    close(tfd);
    return NULL;
}

int main(int argc, char **argv) {
    const char *state_file = argc > 1 ? argv[1] : "/tmp/iris_state";
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGCHLD, SIG_IGN);   // auto-reap forked hermes-cron children (no zombies)

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
    // Orb offset into the free area, pushed right to use open space.
    float ox = 560.0f, oy = 240.0f;
    float scale = 137.0f;

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
    FeedStats stats; read_stats(&stats);   // clock + tool-call feed, refreshed ~1 Hz below
    AgentStats agent; read_agent_stats(&agent);

    // --- touch: spawn the reader thread; render loop only samples shared state ---
    pthread_t tid;
    int have_touch_thread = (pthread_create(&tid, NULL, touch_thread, NULL) == 0);
    if (!have_touch_thread) fprintf(stderr, "touch: pthread_create failed (gestures disabled)\n");

    // gesture / interaction state (owned by the render loop)
    float touch_offset = 0.0f;   // extra Y-axis (yaw) angle added to auto rotation
    float touch_vel    = 0.0f;   // yaw angular velocity injected by horizontal drag (rad/s)
    float pitch_offset = 0.0f;   // extra X-axis (pitch) angle from vertical drag
    float pitch_vel    = 0.0f;   // pitch angular velocity (rad/s)
    float scroll_f     = 0.0f;   // eased scroll position (fractional job index)
    int   scroll_target = 0;     // integer scroll goal, adjusted by vertical swipe
    int   sel_job      = -1;     // selected job for detail overlay, -1 = none

    // Night shift CONSOLE state: transcript cache refreshed ~1Hz, plus a brief
    // FIRING feedback timestamp set when the RUN NOW button is tapped.
    NightConsole ns_con; memset(&ns_con, 0, sizeof(ns_con));
    double ns_last_read = 0;     // last transcript read (monotonic)
    double ns_fire_at   = -1e9;  // time RUN NOW was tapped; FIRING shows for ~2.5s

    // per-gesture bookkeeping tracked across frames
    int   last_seq_down = 0, last_seq_up = 0;
    int   prev_down = 0;
    int   prev_x = 0, prev_y = 0;
    double prev_sample_t = now();
    int   gesture_is_vertical = 0, gesture_is_horizontal = 0, gesture_decided = 0;
    int   gesture_scroll_anchor = 0;      // scroll_target at gesture start
    int   gesture_down_y = 0, gesture_down_x = 0;

    while (running) {
        double t = now();
        float dt = (float)(t - tlast); tlast = t;
        if (dt > 0.05f) dt = 0.05f;
        global_time += dt;

        // ---------------- sample touch & drive gestures ----------------
        // Snapshot the shared touch state under the mutex, then release it fast.
        int td, tx, ty, sdn, sup, dnx, dny, upx, upy;
        pthread_mutex_lock(&touch.m);
        td = touch.down; tx = touch.x; ty = touch.y;
        sdn = touch.seq_down; sup = touch.seq_up;
        dnx = touch.down_x; dny = touch.down_y; upx = touch.up_x; upy = touch.up_y;
        pthread_mutex_unlock(&touch.m);

        // NEW finger-down this frame?
        if (sdn != last_seq_down) {
            last_seq_down = sdn;
            gesture_decided = 0; gesture_is_vertical = 0; gesture_is_horizontal = 0;
            gesture_down_x = dnx; gesture_down_y = dny;
            gesture_scroll_anchor = scroll_target;
            prev_x = tx; prev_y = ty;
        }

        // While the finger is down, classify and act on movement.
        // Region gate: a drag that STARTS on the left queue panel is a vertical
        // scroll gesture; a drag anywhere else rotates the sphere freely in BOTH
        // axes at once (horizontal -> yaw, vertical -> pitch, diagonal -> both).
        if (td) {
            double dts = t - prev_sample_t; if (dts <= 0) dts = 1.0/60.0;
            int dx = tx - prev_x, dy = ty - prev_y;
            int total_dx = tx - gesture_down_x, total_dy = ty - gesture_down_y;
            int started_in_panel = (gesture_down_x < QUEUE_PANEL_W);

            // decide gesture kind once movement exceeds a small threshold
            if (!gesture_decided && (abs(total_dx) > 8 || abs(total_dy) > 8)) {
                gesture_decided = 1;
                if (started_in_panel && abs(total_dy) >= abs(total_dx))
                    gesture_is_vertical = 1;     // swipe-scroll the queue
                else
                    gesture_is_horizontal = 1;   // free rotate the sphere (yaw + pitch)
            }

            if (gesture_is_horizontal) {
                // free-drag rotation: horizontal -> yaw, vertical -> pitch. 900px ~ 2pi.
                // Signs chosen so the sphere follows the finger: drag down tips the
                // top toward the viewer. (Yaw sign left as-is; flip if it reads reversed.)
                float ang_per_px = 6.2831853f / 900.0f;
                touch_vel   = (float)dx * ang_per_px / (float)dts;   // yaw velocity
                touch_offset += (float)dx * ang_per_px;              // immediate yaw follow
                pitch_vel   = (float)dy * ang_per_px / (float)dts;   // pitch velocity
                pitch_offset += (float)dy * ang_per_px;              // immediate pitch follow
            } else if (gesture_is_vertical) {
                // swipe up -> scroll down the list; QUEUE_ROW_H px per job
                int rows = -total_dy / QUEUE_ROW_H;   // finger up (dy<0) advances list
                scroll_target = gesture_scroll_anchor + rows;
            }
            prev_x = tx; prev_y = ty;
            prev_sample_t = t;
        }

        // finger-UP this frame? resolve tap vs. fling.
        if (sup != last_seq_up) {
            last_seq_up = sup;
            int mv = abs(upx - gesture_down_x) + abs(upy - gesture_down_y);
            if (mv < 15) {
                // TAP — hit-test against queue rows / overlay / elsewhere
                int handled = 0;
                if (sel_job >= 0 &&
                    upx >= OVL_X && upx < OVL_X + OVL_W &&
                    upy >= OVL_Y && upy < OVL_Y + OVL_H) {
                    handled = 1;   // tap inside the overlay: keep it open
                    // Night shift console: RUN NOW button fires the cron job.
                    if (strcmp(stats.names[sel_job], NS_JOB_NAME) == 0 &&
                        upx >= NS_BTN_X && upx < NS_BTN_X + NS_BTN_W &&
                        upy >= NS_BTN_Y && upy < NS_BTN_Y + NS_BTN_H) {
                        ns_fire_job();          // fork+exec detached; returns instantly
                        ns_fire_at = t;         // show FIRING feedback briefly
                    }
                }
                if (!handled && upx < QUEUE_PANEL_W && upy >= QUEUE_LY - 6) {
                    int row = (upy - QUEUE_LY) / QUEUE_ROW_H;
                    int idx = scroll_target + row;
                    if (row >= 0 && row < QUEUE_VISIBLE && idx >= 0 && idx < stats.njobs) {
                        sel_job = (sel_job == idx) ? -1 : idx;  // toggle
                        if (sel_job >= 0 && strcmp(stats.names[sel_job], NS_JOB_NAME) == 0)
                            ns_last_read = 0;   // force an immediate transcript read on open
                        handled = 1;
                    }
                }
                if (!handled) sel_job = -1;   // tap elsewhere dismisses overlay
            }
            // horizontal fling already left momentum in touch_vel; nothing more to do
        }

        // clamp scroll target to valid range and ease scroll_f toward it
        {
            int maxs = stats.njobs - QUEUE_VISIBLE; if (maxs < 0) maxs = 0;
            if (scroll_target < 0) scroll_target = 0;
            if (scroll_target > maxs) scroll_target = maxs;
            scroll_f += ((float)scroll_target - scroll_f) * (1.0f - expf(-12.0f * dt));
        }

        // Touch-driven rotation: when no finger drives it, momentum coasts and
        // the accumulated offsets DECAY smoothly back to 0 so auto-rotation
        // resumes with no snap. Everything eased — no velocity discontinuity.
        if (!(td && gesture_is_horizontal)) {
            touch_offset += touch_vel * dt;               // coast on released yaw momentum
            touch_vel   -= touch_vel * (1.0f - expf(-2.5f * dt));   // friction on yaw velocity
            touch_offset -= touch_offset * (1.0f - expf(-0.6f * dt)); // ease yaw offset home
            pitch_offset += pitch_vel * dt;               // coast on released pitch momentum
            pitch_vel   -= pitch_vel * (1.0f - expf(-2.5f * dt));    // friction on pitch velocity
            pitch_offset -= pitch_offset * (1.0f - expf(-0.6f * dt)); // ease pitch offset home
        }
        prev_down = td; (void)prev_down;
        // ---------------------------------------------------------------

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
        float rot = eased * 6.2831853f + touch_offset;   // yaw = auto + touch-driven offset
        float cr = cosf(rot), sr = sinf(rot);             // yaw (Y-axis)
        float cp = cosf(pitch_offset), sp = sinf(pitch_offset);  // pitch (X-axis), touch-only

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
            // Apply pitch FIRST (about the SCREEN x-axis) then yaw (about world Y).
            // Order matters: pitching before yaw keeps "up is always up" no matter
            // how far the sphere has been spun, so a vertical drag always tips the
            // top toward/away from the viewer instead of flipping once yaw>90deg.
            float y1 = sy * cp - sz * sp;   // pitch tilts (y,z)
            float z1 = sy * sp + sz * cp;
            float xr = sx * cr + z1 * sr;   // yaw about Y
            float zr2 = -sx * sr + z1 * cr;
            float yr2 = y1;

            float depth = 0.80f + 0.20f * zr2;   // 0.6..1.0, front = brighter/bigger
            float px = ox + xr * escale * depth;
            float py = oy + yr2 * escale * depth;

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
            // same pitch-first-then-yaw order as the particles (screen-relative pitch)
            float y1 = s->y * cp - s->z * sp;
            float z1 = s->y * sp + s->z * cp;
            float xr = s->x * cr + z1 * sr;
            float zr2 = -s->x * sr + z1 * cr;
            float yr2 = y1;
            float depth = 0.80f + 0.20f * zr2;
            float px = ox + xr * escale * depth;
            float py = oy + yr2 * escale * depth;
            float fade = smoothstep(0, 0.2f, s->life);
            dot_16(back, W, H, STRIDE, px, py, 2.0f + 2.0f * fade,
                   0.5f * fade, 0.8f * fade, 1.0f * fade);
        }

        // --- widgets: clock + system stats + agent panel on top of the orb ---
        if (t - tstats > 1.0) { tstats = t; read_stats(&stats); read_agent_stats(&agent); }
        if (sel_job >= stats.njobs) sel_job = -1;   // job vanished from queue
        // Refresh the Night shift transcript ~1Hz (only while its console is open).
        int ns_open = (sel_job >= 0 && strcmp(stats.names[sel_job], NS_JOB_NAME) == 0);
        if (ns_open && t - ns_last_read > 1.0) { ns_last_read = t; ns_read_transcript(&ns_con); }
        draw_widgets(back, W, H, STRIDE, &stats, (int)(scroll_f + 0.5f), sel_job);
        draw_agent_panel(back, W, H, STRIDE, &agent);
        if (sel_job >= 0) {
            if (ns_open) {
                int firing = (t - ns_fire_at) < 2.5;   // brief RUN NOW feedback
                draw_night_console(back, W, H, STRIDE, &stats, sel_job, &ns_con, firing);
            } else {
                draw_job_detail(back, W, H, STRIDE, &stats, sel_job);
            }
        }

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
    if (have_touch_thread) pthread_join(tid, NULL);
    memset(fb_raw, 0, fbsize);
    free(back_raw);
    munmap(fb_raw, fbsize);
    close(fd);
    return 0;
}
