// iris_fb.c — "digital organism" visualizer, activity-driven.
// A golden holographic lattice (iris_organism.h) that breathes, grows and
// sheds circuit traces and pulses with a heartbeat. Idle: deep amber, slow.
// Thinking (act->1): brighter gold, faster, more traces, ripples and embers.
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
#include <sys/file.h>
#include "iris_widgets.h"
#include "iris_organism.h"

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

// Set the BLE LED by exec'ing a wrapper script in a detached child (same
// fork+exec pattern as ns_fire_job — NEVER blocks the 60Hz render loop; the
// BLE write can take a couple seconds and that's fine in the child).
static void led_set(const char *script) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); }
        execl("/bin/bash", "bash", script, (char *)NULL);
        _exit(127);
    }
}

// --- NIGHT pill, shared with the web dashboard -----------------------------
// /tmp/iris_ns_manual holds the manual-night-shift flag. The panel publishes
// its own taps here and adopts changes made anywhere else (the dashboard's
// POST /api/lcd/night) at the 1Hz stats cadence, so the pill means the same
// thing on both screens and survives a restart.
#define NS_MANUAL_FILE "/tmp/iris_ns_manual"
static time_t ns_manual_mtime = 0;

static void ns_manual_write(int on) {
    FILE *f = fopen(NS_MANUAL_FILE, "w");
    if (f) { fprintf(f, "%d\n", on ? 1 : 0); fclose(f); }
    struct stat st;
    if (stat(NS_MANUAL_FILE, &st) == 0) ns_manual_mtime = st.st_mtime;
}

// Returns the flag's value if the file changed since we last saw it, else cur.
static int ns_manual_poll(int cur) {
    struct stat st;
    if (stat(NS_MANUAL_FILE, &st) != 0) return cur;
    if (st.st_mtime == ns_manual_mtime) return cur;
    ns_manual_mtime = st.st_mtime;
    FILE *f = fopen(NS_MANUAL_FILE, "r");
    if (!f) return cur;
    char b[8] = {0};
    char *got = fgets(b, sizeof(b), f);
    fclose(f);
    return got ? (b[0] == '1') : cur;
}

// Edge-triggered LED: red while manual night mode is on, green when it goes
// off (unless the real job is still running).
static void ns_manual_led(int on, int running) {
    if (on) led_set("/home/humdan/.hermes/scripts/led-red.sh");
    else if (!running) led_set("/home/humdan/.hermes/scripts/led-green-bright.sh");
}

// Manual night-shift toggle: the visible NIGHT pill drawn by draw_agent_panel()
// (NS_PILL_* in iris_widgets.h), padded ~10px so a fingertip reliably lands.
#define NS_TOGGLE_X (NS_PILL_X - 10)
#define NS_TOGGLE_Y (NS_PILL_Y(H) - 10)
#define NS_TOGGLE_W (NS_PILL_W + 20)
#define NS_TOGGLE_H (NS_PILL_H + 20)

// Running agents for the gyroscope comets. Registry lines, one per session:
//   A|<c=claude h=hermes>|<session id>|<last active epoch>|<activity 0..1>
// Written by the Claude Code hook and the Iris Hermes plugin. An agent counts
// as running for AGENT_IDLE_S after its last activity, fading as it goes quiet.
#define AGENTS_FILE  "/tmp/iris_agents.txt"
#define AGENT_IDLE_S 120.0
static int read_agents(OrgAgentIn *out, int max) {
    FILE *f = fopen(AGENTS_FILE, "r");
    if (!f) return 0;
    char line[256]; int n = 0;
    time_t wall = time(NULL);
    while (n < max && fgets(line, sizeof(line), f)) {
        char kind, id[128]; double last; float a;
        if (sscanf(line, "A|%c|%127[^|]|%lf|%f", &kind, id, &last, &a) != 4) continue;
        double age = (double)wall - last;
        if (age > AGENT_IDLE_S || age < -60) continue;
        uint32_t h = 2166136261u;
        for (const char *q = id; *q; q++) { h ^= (uint8_t)*q; h *= 16777619u; }
        // full activity while recently active, easing down as the agent goes quiet
        float recency = age < 5 ? 1.0f : expf(-(float)(age - 5) / 40.0f);
        out[n++] = (OrgAgentIn){ h, kind == 'h' ? 'h' : 'c', a * recency };
    }
    fclose(f);
    return n;
}

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static float frand(void) { return rand() / (float)RAND_MAX; }
static float smoothstep(float e0, float e1, float x) { float t = clampf((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t); }

// Cardiac lub-dub envelope over a normalized phase [0,1): a strong first beat
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

// ---------------------------------------------------------------------------
// Touch input: a dedicated thread reads /dev/input/event4 (evdev, FT5x06) and
// publishes the single active finger's state into a mutex-guarded struct. The
// render loop only SAMPLES it once per frame — never reads the device — so the
// 60Hz absolute-clock pacing is never blocked by touch I/O.
// ---------------------------------------------------------------------------
// The touchscreen's event number is NOT stable across boots (it was event4,
// now it's event0 with the HDMI devices on 1-4), so find it by capability:
// the first /dev/input/event* that reports multitouch X/Y. IRIS_TOUCH_DEV
// overrides.
#define BITS_PER_LONG_ (sizeof(long) * 8)
static int test_bit_(int bit, const unsigned long *arr) { return (arr[bit / BITS_PER_LONG_] >> (bit % BITS_PER_LONG_)) & 1; }
static int open_touch_device(char *path, size_t n) {
    const char *env = getenv("IRIS_TOUCH_DEV");
    if (env && *env) { snprintf(path, n, "%s", env); return open(path, O_RDONLY); }
    for (int i = 0; i < 32; i++) {
        snprintf(path, n, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        unsigned long absbits[(ABS_MAX + BITS_PER_LONG_) / BITS_PER_LONG_];
        memset(absbits, 0, sizeof(absbits));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) >= 0 &&
            test_bit_(ABS_MT_POSITION_X, absbits) && test_bit_(ABS_MT_POSITION_Y, absbits))
            return fd;
        close(fd);
    }
    snprintf(path, n, "(none)");
    return -1;
}

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
    char tpath[64];
    int tfd = open_touch_device(tpath, sizeof(tpath));
    if (tfd < 0) {
        fprintf(stderr, "touch: no multitouch device found (%s): %s (gestures disabled)\n", tpath, strerror(errno));
        return NULL;
    }
    char tname[128] = "?";
    ioctl(tfd, EVIOCGNAME(sizeof(tname)), tname);
    fprintf(stderr, "touch: reading %s (%s)\n", tpath, tname);

    int cur_x = 0, cur_y = 0;   // accumulated within the current SYN frame
    int have_x = 0, have_y = 0;
    int contact = 0;            // tracking-id >= 0 means finger present

    struct pollfd pfd = { .fd = tfd, .events = POLLIN };
    struct input_event ev;

    while (running) {
        int pr = poll(&pfd, 1, 10);    // 10ms wakeups — responsive touch, still notices `running`
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
    // No SA_RESTART: SIGTERM must interrupt a blocking standby flock() below.
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGCHLD, SIG_IGN);   // auto-reap forked hermes-cron children (no zombies)

    // Single instance: two renderers blitting to the same framebuffer take
    // turns overwriting each other's frames (heavy flicker). A second copy
    // (e.g. both the system and the user iris.service) waits here as a quiet
    // standby and takes over only if the running one exits.
    // Preview mode: IRIS_SNAPSHOT=<file> renders ~2s off-screen (no framebuffer,
    // no lock, no touch) and writes the last frame as raw RGB565 800x480 to
    // <file>; view it with `lcd-shot --from <file> out.png`. Safe to run while
    // the live renderer owns the screen, so layout changes can be checked
    // without restarting Iris. IRIS_SNAPSHOT_FRAMES overrides the frame count.
    const char *snapshot = getenv("IRIS_SNAPSHOT");
    int snap_frames = getenv("IRIS_SNAPSHOT_FRAMES") ? atoi(getenv("IRIS_SNAPSHOT_FRAMES")) : 120;

    int lock_fd = snapshot ? -1 : open("/tmp/iris_fb.lock", O_RDWR | O_CREAT, 0666);
    if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "iris_fb: another instance owns the screen; waiting as standby\n");
        while (running && flock(lock_fd, LOCK_EX) < 0 && errno == EINTR) {}
        if (!running) return 0;
    }

    int fd = -1, W = 800, H = 480, BPP = 16, STRIDE = 1600;
    uint8_t *fb_raw;
    size_t fbsize = (size_t)STRIDE * H;
    if (snapshot) {
        fb_raw = calloc(1, fbsize);
    } else {
        fd = open("/dev/fb0", O_RDWR);
        if (fd < 0) { perror("open /dev/fb0"); return 1; }
        struct fb_var_screeninfo v;
        struct fb_fix_screeninfo f;
        ioctl(fd, FBIOGET_VSCREENINFO, &v);
        ioctl(fd, FBIOGET_FSCREENINFO, &f);
        W = v.xres; H = v.yres; BPP = v.bits_per_pixel; STRIDE = f.line_length;
        fbsize = (size_t)STRIDE * H;
        fb_raw = mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fb_raw == MAP_FAILED) { perror("mmap"); return 1; }
    }
    uint16_t *fb = (uint16_t *)fb_raw;
    uint8_t *back_raw = malloc(fbsize);

    fprintf(stderr, "iris_fb (particles): %dx%d %dbpp stride %d\n", W, H, BPP, STRIDE);

    srand(42);
    // Orb fills the free area: right of the queue column (ends ~x=280), above
    // the bottom panel (starts y=414). Its top passes to the right of the
    // clock/date text (which ends at x~496), so it can rise almost to the top.
    // SPHERE_R_MAX is the largest on-screen radius at full activity + heartbeat
    // peak; `scale` is derived from it so the busiest frame still fits.
    const float SPHERE_R_MAX = 195.0f;
    float ox = 575.0f, oy = 214.0f;
    const float GROW = 0.06f, AMP_IDLE = 0.015f, AMP_BUSY = 0.05f;
    float scale = SPHERE_R_MAX / (ORG_EXTENT * (1.0f + GROW) * (1.0f + (AMP_IDLE + AMP_BUSY) * 1.05f));

    org_init(scale);

    float act = 0, target = 0;
    double t0 = now(), tlast = t0, tcheck = 0, tfps = t0, tnext = t0, tstats = 0;
    int frames = 0;
    float global_time = 0;
    // Layout: dashboard-owned positions/visibility. Loaded before the first
    // frame and re-polled at the 1Hz stats cadence, so a change made in the web
    // UI shows up on the panel within a second without restarting anything.
    char layout_path[512];
    snprintf(layout_path, sizeof(layout_path), "%s%s",
             getenv("HOME") ? getenv("HOME") : "/home/humdan", LAYOUT_PATH_REL);
    layout_load(&LAY, layout_path, W, H);

    FeedStats stats; read_stats(&stats);   // clock + tool-call feed, refreshed ~1 Hz below
    AgentStats agent; read_agent_stats(&agent);
    PortfolioStats portfolio; read_portfolio_stats(&portfolio);

    // --- touch: spawn the reader thread; render loop only samples shared state ---
    pthread_t tid;
    int have_touch_thread = !snapshot && (pthread_create(&tid, NULL, touch_thread, NULL) == 0);
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
        static NsView ns_view;       // running cron lanes + announced tasks (~1Hz)
        double ns_last_read = 0;     // last transcript read (monotonic)
        double ns_fire_at   = -1e9;  // time the NIGHT SHIFT button was tapped
        // Hermes only notices a manual run on its next cron tick (every 60s,
        // not configurable), then gathers context (~30s) before the agent
        // starts. Night mode switches on at the tap; the panel shows a
        // countdown to the tick and then "warming up" until the run is live.
        #define NS_START_WINDOW 180.0
        double ns_tick_at   = 0;     // wall-clock epoch of Hermes's last cron tick
        int    ns_running   = 0;     // night shift actively running (fresh transcript mtime)
        int    ns_manual    = ns_manual_poll(0);   // manual night shift mode (shared with the dashboard)
        int    ns_manual_prev = 0;   // previous manual state for edge-triggered LED

    // Session orbs state
    SessionStats sessions = {0};

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
            int started_in_panel = LAY.queue_enabled && (gesture_down_x < QUEUE_PANEL_W);

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
            fprintf(stderr, "touch: up (%d,%d) down (%d,%d) moved %d%s\n", upx, upy,
                    gesture_down_x, gesture_down_y, mv, mv < 15 ? " -> tap" : "");
            if (mv < 15) {
                // TAP — hit-test against queue rows / overlay / elsewhere
                int handled = 0;
                // Is the large Night shift console currently on screen? It shows
                // when night mode is active (manual/running) OR the Night shift
                // job row is selected. RUN NOW lives inside that big panel.
                int ns_console_showing = ns_manual || ns_running || (t - ns_fire_at < NS_START_WINDOW) ||
                    (sel_job >= 0 && strcmp(stats.names[sel_job], NS_JOB_NAME) == 0);
                // Top-right NIGHT SHIFT button (padded tap zone). Fires only when
                // no run is live and it wasn't just tapped (no double starts).
                if (LAY.nightbtn_enabled && upx >= NS_RUN_X - 8 && upy < NS_RUN_Y + NS_RUN_H + 10) {
                    if (!ns_running && t - ns_fire_at > NS_START_WINDOW) {
                        ns_fire_job();      // fork+exec detached; returns instantly
                        ns_fire_at = t;
                    }
                    handled = 1;
                } else if (ns_console_showing &&
                    upx >= NSC_X && upx < NSC_X + NSC_W &&
                    upy >= NSC_Y && upy < NSC_Y + NSC_H) {
                    handled = 1;   // tap inside the console panel: keep it open
                } else if (sel_job >= 0 &&
                    upx >= OVL_X && upx < OVL_X + OVL_W &&
                    upy >= OVL_Y && upy < OVL_Y + OVL_H) {
                    handled = 1;   // tap inside the small job-detail overlay: keep open
                }
                // MANUAL NIGHT SHIFT TOGGLE: reachable on the normal screen (not
                // gated behind an open overlay). Small pill in the bottom agent panel.
                if (!handled && LAY.panel_enabled && upx >= NS_TOGGLE_X && upx < NS_TOGGLE_X + NS_TOGGLE_W &&
                    upy >= NS_TOGGLE_Y && upy < NS_TOGGLE_Y + NS_TOGGLE_H) {
                    ns_manual = !ns_manual;
                    ns_manual_write(ns_manual);   // tell the dashboard
                    ns_manual_led(ns_manual, ns_running);
                    ns_manual_prev = ns_manual;
                    handled = 1;
                }
                if (!handled && LAY.queue_enabled && upx < QUEUE_PANEL_W && upy >= QUEUE_LY - 6) {
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
        if (t - tcheck > 0.05) {            // 20 Hz — smoother activity tracking
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

        // --- orientation: slow organic sway (never a mechanical back-and-forth
        //     swing) plus the touch-driven yaw/pitch offsets ---
        float yaw   = 0.35f * sinf(global_time * 0.071f) + 0.12f * sinf(global_time * 0.19f + 1.3f) + touch_offset;
        float pitch = 0.22f * sinf(global_time * 0.053f + 0.7f) + pitch_offset;

        // Sphere expands when working AND breathes like a heartbeat.
        //  - base growth: eases toward +GROW at full activity (bounded by SPHERE_R_MAX)
        //  - heartbeat: a periodic expand/contract, amplitude scales with activity
        //    so idle barely breathes and thinking pulses clearly.
        static float scale_dyn = 1.0f;
        float base_target = 1.0f + GROW * act;
        scale_dyn += (base_target - scale_dyn) * (1.0f - expf(-2.5f * dt));
        // Cardiac lub-dub. Beat rate rises with activity (~0.5 Hz calm -> ~1.3 Hz busy);
        // amplitude tiny at idle, deeper when working (AMP_IDLE..AMP_IDLE+AMP_BUSY).
        static float beat_phase = 0.0f;
        float beat_hz = 0.35f + 0.65f * act;   // slow resting pulse
        beat_phase += beat_hz * dt;
        float amp = AMP_IDLE + AMP_BUSY * act;
        float breathe = 1.0f + amp * heartbeat(beat_phase);
        float escale = scale * scale_dyn * breathe;

        // --- draw ---
        memset(back_raw, 0, fbsize);
        uint16_t *back = (uint16_t *)back_raw;

        // Iris turns maroon while night shift is on (eased, ~1s fade either way).
        // ns_manual/ns_running are last frame's values: at most one frame late.
        static float night_w = 0.0f;
        int ns_starting = !ns_running && (t - ns_fire_at) < NS_START_WINDOW;
        night_w += ((ns_manual || ns_running || ns_starting ? 1.0f : 0.0f) - night_w) * (1.0f - expf(-3.0f * dt));
        org_frame(back, W, H, STRIDE, ox, oy, escale, global_time, dt, act,
                  yaw, pitch, heartbeat(beat_phase), beat_phase, night_w);

        // --- widgets: clock + system stats + agent panel on top of the orb ---
        if (t - tstats > 1.0) {
            tstats = t; read_stats(&stats); read_agent_stats(&agent); read_portfolio_stats(&portfolio);
            layout_poll(&LAY, layout_path, W, H);
            {   // NIGHT pill toggled from the dashboard?
                int nm = ns_manual_poll(ns_manual);
                if (nm != ns_manual) { ns_manual = nm; ns_manual_led(ns_manual, ns_running); }
            }
            // Running = a live Night-shift lane from the Hermes plugin (exact),
            // or the step-log heuristics (fallback while the plugin isn't loaded).
            read_ns_view(&ns_view);
            ns_tick_at = read_cron_tick();
            {
                OrgAgentIn ag[ORG_AGENTS];
                int nag = read_agents(ag, ORG_AGENTS);
                org_set_agents(ag, nag);
            }
            int lane_live = 0;
            for (int i = 0; i < ns_view.nl; i++) if (ns_view.lanes[i].is_ns) lane_live = 1;
            if (ns_view.open_cards > 0) lane_live = 1;   // shift has queued work between workers
            ns_running = lane_live || ns_running_check();
            if (lane_live) ns_fire_at = -1e9;   // run picked up: leave the STARTING state
        }
        if (sel_job >= stats.njobs) sel_job = -1;   // job vanished from queue

        // Locate the Night shift job in the current queue (index or -1).
        int ns_job_idx = -1;
        for (int i = 0; i < stats.njobs; i++)
            if (strcmp(stats.names[i], NS_JOB_NAME) == 0) { ns_job_idx = i; break; }

        // Night-shift mode active (manual toggle OR a real run detected) forces the
        // live console open so the transcript is always visible while it works.
        int ns_mode = (ns_manual || ns_running || ns_starting);
        // The console is shown when the user tapped the job row, OR whenever
        // night-shift mode is active and we know which row is the Night shift job.
        // (While it runs, the job can drop out of the queue list, so night-shift
        // mode alone is enough; it must not depend on the job's row existing.)
        int ns_open = (sel_job >= 0 && strcmp(stats.names[sel_job], NS_JOB_NAME) == 0)
                      || ns_mode;
        // Which job index the console should render for.
        int ns_console_job = (sel_job >= 0 && strcmp(stats.names[sel_job], NS_JOB_NAME) == 0)
                             ? sel_job : ns_job_idx;

        // Refresh the Night shift transcript ~1Hz whenever its console is open.
        if (ns_open && t - ns_last_read > 1.0) {
            ns_last_read = t;
            ns_read_transcript(&ns_con);
        }

        // --- Session orbs: draw around the main sphere (BACKGROUND layer) ---
        read_session_stats(&sessions, t);
        update_session_orbs(&sessions, dt);
        if (LAY.orbs_enabled)
            draw_session_orbs(back, W, H, STRIDE, &sessions, global_time, act);

        // Night shift no longer tints the whole screen or draws a top banner
        // (that hid the clock and turned Iris red); the log panel in the left
        // column carries the "night shift is on" signal instead.
        float ns_pulse = 0.5f + 0.5f * sinf(global_time * 2.0f * (float)M_PI * 0.5f); // ~0.5Hz
        int show_log = ns_open;

        // --- Widgets: clock, plus the cron queue unless the night log owns the column ---
        draw_widgets(back, W, H, STRIDE, &stats, (int)(scroll_f + 0.5f), sel_job, !show_log);

        // --- Agent panel (TOP layer - clean, no tint) ---
        if (LAY.panel_enabled)
            draw_agent_panel(back, W, H, STRIDE, &agent, ns_manual, &portfolio);

        // --- NIGHT SHIFT trigger, top-right corner (always shown) ---
        // STARTING shows from the tap until the run is detected (up to 60s).
        if (LAY.nightbtn_enabled)
            draw_ns_run_button(back, W, H, STRIDE, ns_running,
                               ns_starting, ns_pulse);

        // --- Night shift log (left column) or job detail overlay ---
        if (show_log) {
            int firing = (t - ns_fire_at) < 2.5;   // brief RUN NOW feedback
            // Task view when lanes are known; raw step log as the fallback
            // (manual mode with nothing running, or plugin not yet reloaded).
            int have_view = ns_view.nl > 0 || ns_view.open_cards > 0 || ns_view.nd > 0;
            if (ns_starting && !have_view)
                draw_night_starting(back, W, H, STRIDE, ns_tick_at, ns_pulse);
            else if (have_view)
                draw_night_tasks(back, W, H, STRIDE, &ns_view, ns_manual, ns_pulse);
            else
                draw_night_log(back, W, H, STRIDE, &ns_con, firing, ns_manual, ns_pulse);
        } else if (sel_job >= 0) {
            draw_job_detail(back, W, H, STRIDE, &stats, sel_job);
        }

        // --- blit atomically ---
        memcpy(fb_raw, back_raw, fbsize);

        if (snapshot && --snap_frames <= 0) {
            FILE *sf = fopen(snapshot, "wb");
            if (!sf) { perror(snapshot); return 1; }
            fwrite(fb_raw, 1, fbsize, sf);
            fclose(sf);
            fprintf(stderr, "iris_fb: snapshot written to %s\n", snapshot);
            return 0;
        }

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
    if (fd >= 0) { munmap(fb_raw, fbsize); close(fd); }
    return 0;
}
