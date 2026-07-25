#include "gui.h"
#include "client_app.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void client_gui_state_init(client_gui_state_t *g)
{
    memset(g, 0, sizeof(*g));
    pthread_mutex_init(&g->lock, NULL);
    snprintf(g->status, sizeof(g->status), "%s", "idle");
    snprintf(g->current_version, sizeof(g->current_version), "%s", "-");
    snprintf(g->latest_version, sizeof(g->latest_version), "%s", "-");
    g->show_progress = 0;
}

void client_gui_set_status(client_gui_state_t *g, const char *status)
{
    pthread_mutex_lock(&g->lock);
    snprintf(g->status, sizeof(g->status), "%s", status ? status : "");
    pthread_mutex_unlock(&g->lock);
}

void client_gui_set_versions(client_gui_state_t *g, const char *current,
                             const char *latest)
{
    pthread_mutex_lock(&g->lock);
    if (current) {
        snprintf(g->current_version, sizeof(g->current_version), "%s", current);
    }
    if (latest) {
        snprintf(g->latest_version, sizeof(g->latest_version), "%s", latest);
    }
    pthread_mutex_unlock(&g->lock);
}

void client_gui_set_progress(client_gui_state_t *g, unsigned long long done,
                             unsigned long long total, double speed_bps)
{
    pthread_mutex_lock(&g->lock);
    g->bytes_done = done;
    g->bytes_total = total;
    g->speed_bps = speed_bps;
    pthread_mutex_unlock(&g->lock);
}

void client_gui_set_progress_visible(client_gui_state_t *g, int visible)
{
    pthread_mutex_lock(&g->lock);
    g->show_progress = visible ? 1 : 0;
    pthread_mutex_unlock(&g->lock);
}

void client_gui_set_controls(client_gui_state_t *g, int pause_allowed,
                             int resume_allowed)
{
    pthread_mutex_lock(&g->lock);
    g->pause_allowed = pause_allowed ? 1 : 0;
    g->resume_allowed = resume_allowed ? 1 : 0;
    if (!g->pause_allowed) {
        g->pause_requested = 0;
    }
    if (!g->resume_allowed) {
        g->resume_requested = 0;
    }
    pthread_mutex_unlock(&g->lock);
}

int client_gui_consume_pause_request(client_gui_state_t *g)
{
    int requested;
    pthread_mutex_lock(&g->lock);
    requested = g->pause_requested;
    g->pause_requested = 0;
    pthread_mutex_unlock(&g->lock);
    return requested;
}

int client_gui_consume_resume_request(client_gui_state_t *g)
{
    int requested;
    pthread_mutex_lock(&g->lock);
    requested = g->resume_requested;
    g->resume_requested = 0;
    pthread_mutex_unlock(&g->lock);
    return requested;
}

void client_gui_add_log(client_gui_state_t *g, const char *line)
{
    pthread_mutex_lock(&g->lock);
    if (g->log_count < 8) {
        snprintf(g->log_lines[g->log_count++], sizeof(g->log_lines[0]), "%s", line);
    } else {
        for (int i = 1; i < 8; i++) {
            snprintf(g->log_lines[i - 1], sizeof(g->log_lines[0]), "%s", g->log_lines[i]);
        }
        snprintf(g->log_lines[7], sizeof(g->log_lines[0]), "%s", line);
    }
    pthread_mutex_unlock(&g->lock);
}

#if USE_GUI
#include <GL/glut.h>
#include <stdlib.h>

#if defined(GLUT_ACTION_ON_WINDOW_CLOSE) && defined(GLUT_ACTION_GLUTMAINLOOP_RETURNS)
#define HAVE_GLUT_MAINLOOP_CONTROL 1
#endif

typedef struct {
    int (*fn)(void *);
    void *arg;
} gui_worker_arg_t;

static client_gui_state_t *g_state;
static volatile int g_running;
static int g_result;
static pthread_t g_worker_thread;
static int g_worker_started;

static void draw_text(float x, float y, const char *s)
{
    glRasterPos2f(x, y);
    for (const char *p = s; *p; p++) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, *p);
    }
}

static void draw_button(float x, float y, float w, float h, const char *label,
                        int enabled)
{
    if (enabled) {
        glColor3f(0.20f, 0.44f, 0.64f);
    } else {
        glColor3f(0.18f, 0.19f, 0.20f);
    }
    glBegin(GL_QUADS);
    glVertex2f(x, y + h); glVertex2f(x + w, y + h);
    glVertex2f(x + w, y); glVertex2f(x, y);
    glEnd();
    glColor3f(enabled ? 0.95f : 0.55f,
              enabled ? 0.97f : 0.57f,
              enabled ? 0.98f : 0.58f);
    draw_text(x + 22, y + 14, label);
}

static void draw_uptodate_panel(const char *cur, const char *latest)
{
    char line[192];

    glColor3f(0.10f, 0.30f, 0.22f);
    glBegin(GL_QUADS);
    glVertex2f(24, 275); glVertex2f(650, 275);
    glVertex2f(650, 220); glVertex2f(24, 220);
    glEnd();

    glColor3f(0.18f, 0.66f, 0.42f);
    glBegin(GL_QUADS);
    glVertex2f(24, 275); glVertex2f(32, 275);
    glVertex2f(32, 220); glVertex2f(24, 220);
    glEnd();

    glColor3f(0.90f, 0.98f, 0.94f);
    draw_text(48, 254, "UP TO DATE");
    snprintf(line, sizeof(line), "Installed version %s is already the latest version.",
             cur);
    draw_text(48, 236, line);
    snprintf(line, sizeof(line), "Current=%s  Latest=%s", cur, latest);
    draw_text(48, 220, line);
}

static void display(void)
{
    char status[32];
    char cur[MAX_VERSION_STR];
    char latest[MAX_VERSION_STR];
    unsigned long long done;
    unsigned long long total;
    double speed;
    int show_progress;
    int pause_allowed;
    int resume_allowed;
    char logs[8][160];
    int log_count;

    pthread_mutex_lock(&g_state->lock);
    snprintf(status, sizeof(status), "%s", g_state->status);
    snprintf(cur, sizeof(cur), "%s", g_state->current_version);
    snprintf(latest, sizeof(latest), "%s", g_state->latest_version);
    done = g_state->bytes_done;
    total = g_state->bytes_total;
    speed = g_state->speed_bps;
    show_progress = g_state->show_progress;
    pause_allowed = g_state->pause_allowed;
    resume_allowed = g_state->resume_allowed;
    log_count = g_state->log_count;
    for (int i = 0; i < log_count; i++) {
        snprintf(logs[i], sizeof(logs[i]), "%s", g_state->log_lines[i]);
    }
    pthread_mutex_unlock(&g_state->lock);

    glClearColor(0.08f, 0.09f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, 680, 0, 380);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glColor3f(0.93f, 0.95f, 0.96f);
    draw_text(24, 340, "Software Update Client");
    char line[192];
    snprintf(line, sizeof(line), "status=%s current=%s latest=%s", status, cur, latest);
    draw_text(24, 305, line);

    glColor3f(0.93f, 0.95f, 0.96f);
    if (show_progress) {
        glColor3f(0.18f, 0.20f, 0.22f);
        glBegin(GL_QUADS);
        glVertex2f(24, 270); glVertex2f(650, 270); glVertex2f(650, 245); glVertex2f(24, 245);
        glEnd();
        float ratio = total > 0 ? (float)((double)done / (double)total) : 0.0f;
        if (ratio > 1.0f) {
            ratio = 1.0f;
        }
        glColor3f(0.24f, 0.61f, 0.82f);
        glBegin(GL_QUADS);
        glVertex2f(24, 270); glVertex2f(24 + 626 * ratio, 270);
        glVertex2f(24 + 626 * ratio, 245); glVertex2f(24, 245);
        glEnd();

        glColor3f(0.93f, 0.95f, 0.96f);
        snprintf(line, sizeof(line), "%llu / %llu bytes    %.1f KiB/s",
                 done, total, speed / 1024.0);
        draw_text(24, 220, line);
    } else {
        glColor3f(0.93f, 0.95f, 0.96f);
        if (strcmp(status, "up to date") == 0) {
            draw_uptodate_panel(cur, latest);
        } else if (strcmp(status, "installed") == 0 ||
                   strcmp(status, "done") == 0) {
            draw_text(24, 250, "Update installed and verified successfully.");
        } else if (strcmp(status, "paused") == 0) {
            draw_text(24, 250, "Download paused. Resume data was saved.");
        } else if (strcmp(status, "reconnecting") == 0 ||
                   strcmp(status, "retrying") == 0) {
            draw_text(24, 250, "Server unavailable. Trying to reconnect.");
        } else if (strcmp(status, "failed") == 0 ||
                   strcmp(status, "checksum failed") == 0) {
            draw_text(24, 250, "Update failed. Check the client log.");
        } else {
            draw_text(24, 250, "Waiting for update status.");
        }
    }

    glColor3f(0.74f, 0.78f, 0.80f);
    for (int i = 0; i < log_count; i++) {
        draw_text(24, 180 - i * 20, logs[i]);
    }
    draw_button(410, 24, 110, 34, "Pause", pause_allowed);
    draw_button(540, 24, 110, 34, "Resume", resume_allowed);
    glutSwapBuffers();
}

static int hit_button(int mx, int my, int x, int y, int w, int h)
{
    int gy = 380 - my;
    return mx >= x && mx <= x + w && gy >= y && gy <= y + h;
}

static void mouse_cb(int button, int state, int x, int y)
{
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN || !g_state) {
        return;
    }

    pthread_mutex_lock(&g_state->lock);
    if (g_state->pause_allowed && hit_button(x, y, 410, 24, 110, 34)) {
        g_state->pause_requested = 1;
    } else if (g_state->resume_allowed && hit_button(x, y, 540, 24, 110, 34)) {
        g_state->resume_requested = 1;
    }
    pthread_mutex_unlock(&g_state->lock);
}

static void timer_cb(int value)
{
    (void)value;
    if (!g_running) {
        if (g_worker_started) {
            pthread_join(g_worker_thread, NULL);
            g_worker_started = 0;
        }
#ifdef HAVE_GLUT_MAINLOOP_CONTROL
        glutLeaveMainLoop();
#else
        exit(g_result == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
#endif
        return;
    }
    glutPostRedisplay();
    glutTimerFunc(100, timer_cb, 0);
}

static void *worker_wrap(void *arg)
{
    gui_worker_arg_t *w = (gui_worker_arg_t *)arg;
    g_result = w->fn(w->arg);
    g_running = 0;
    return NULL;
}

#ifdef HAVE_GLUT_MAINLOOP_CONTROL
static void close_cb(void)
{
    client_request_stop();
    g_running = 0;
}
#endif

int client_gui_run(client_gui_state_t *g, int argc, char **argv,
                   int (*worker_fn)(void *), void *worker_arg)
{
    g_state = g;
    g_running = 1;
    g_result = -1;
    gui_worker_arg_t warg = { worker_fn, worker_arg };
    g_worker_started = 0;
    if (pthread_create(&g_worker_thread, NULL, worker_wrap, &warg) != 0) {
        return -1;
    }
    g_worker_started = 1;

    int glut_argc = argc;
    glutInit(&glut_argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(680, 380);
    glutCreateWindow("Update Client");
    glutDisplayFunc(display);
    glutMouseFunc(mouse_cb);
    glutTimerFunc(100, timer_cb, 0);
#ifdef HAVE_GLUT_MAINLOOP_CONTROL
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutCloseFunc(close_cb);
#endif
    glutMainLoop();
    if (g_worker_started) {
        pthread_join(g_worker_thread, NULL);
        g_worker_started = 0;
    }
    return g_result;
}

#else
int client_gui_run(client_gui_state_t *g, int argc, char **argv,
                   int (*worker_fn)(void *), void *worker_arg)
{
    (void)g;
    (void)argc;
    (void)argv;
    return worker_fn(worker_arg);
}
#endif
