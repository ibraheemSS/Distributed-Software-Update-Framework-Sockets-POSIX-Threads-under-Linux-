#include "gui.h"
#include "logger.h"
#include "server_app.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if USE_GUI
#include <GL/glut.h>

#if defined(GLUT_ACTION_ON_WINDOW_CLOSE) && defined(GLUT_ACTION_GLUTMAINLOOP_RETURNS)
#define HAVE_GLUT_MAINLOOP_CONTROL 1
#endif

static stats_t *g_stats;
static volatile sig_atomic_t *g_running;
static unsigned long long g_prev_bytes;
static double g_throughput;

static void draw_text(float x, float y, const char *s)
{
    glRasterPos2f(x, y);
    for (const char *p = s; *p; p++) {
        glutBitmapCharacter(GLUT_BITMAP_8_BY_13, *p);
    }
}

static void display(void)
{
    glClearColor(0.07f, 0.08f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluOrtho2D(0, 900, 0, 520);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    unsigned long active = atomic_load(&g_stats->active_transfers);
    unsigned long served = atomic_load(&g_stats->updates_served);
    unsigned long uptodate = atomic_load(&g_stats->uptodate_responses);
    unsigned long auth_fail = atomic_load(&g_stats->auth_failures);
    unsigned long errors = atomic_load(&g_stats->errors);
    int busy = atomic_load(&g_stats->busy_workers);
    int maxw = g_stats->max_workers > 0 ? g_stats->max_workers : 1;
    char active_clients[6][ACTIVE_CLIENT_LABEL];
    int active_client_count = stats_active_client_snapshot(g_stats, active_clients, 6);
    int active_display_count = active_client_count < 6 ? active_client_count : 6;

    glColor3f(0.92f, 0.95f, 0.96f);
    draw_text(28, 485, "Distributed Update Server");

    glColor3f(0.18f, 0.20f, 0.22f);
    glBegin(GL_QUADS);
    glVertex2f(28, 430); glVertex2f(860, 430); glVertex2f(860, 405); glVertex2f(28, 405);
    glEnd();
    float width = 832.0f * ((float)busy / (float)maxw);
    glColor3f(0.20f, 0.64f, 0.48f);
    glBegin(GL_QUADS);
    glVertex2f(28, 430); glVertex2f(28 + width, 430); glVertex2f(28 + width, 405); glVertex2f(28, 405);
    glEnd();

    char line[256];
    glColor3f(0.92f, 0.95f, 0.96f);
    snprintf(line, sizeof(line), "Workers busy: %d / %d", busy, maxw);
    draw_text(28, 445, line);
    snprintf(line, sizeof(line),
             "active_clients=%d active_transfers=%lu updates=%lu up_to_date=%lu auth_failures=%lu errors=%lu",
             active_client_count, active, served, uptodate, auth_fail, errors);
    draw_text(28, 370, line);
    snprintf(line, sizeof(line), "throughput=%.1f KiB/s", g_throughput / 1024.0);
    draw_text(28, 345, line);

    glColor3f(0.74f, 0.78f, 0.80f);
    draw_text(28, 310, "Active clients");
    if (active_client_count == 0) {
        draw_text(28, 285, "none");
    } else {
        for (int i = 0; i < active_display_count; i++) {
            draw_text(28, 285 - i * 20, active_clients[i]);
        }
        if (active_client_count > active_display_count) {
            snprintf(line, sizeof(line), "... %d more active clients",
                     active_client_count - active_display_count);
            draw_text(28, 285 - active_display_count * 20, line);
        }
    }

    char logs[12][LOG_LINE_MAX];
    int n = logger_recent(logs, 7);
    glColor3f(0.74f, 0.78f, 0.80f);
    draw_text(28, 145, "Recent log events");
    for (int i = 0; i < n; i++) {
        draw_text(28, 120 - i * 18, logs[i]);
    }

    glutSwapBuffers();
}

static void tick(int value)
{
    (void)value;
    unsigned long long bytes = atomic_load(&g_stats->bytes_sent);
    g_throughput = (double)(bytes - g_prev_bytes) * 10.0;
    g_prev_bytes = bytes;
    if (!*g_running) {
        server_request_shutdown();
#ifdef HAVE_GLUT_MAINLOOP_CONTROL
        glutLeaveMainLoop();
#else
        exit(EXIT_SUCCESS);
#endif
        return;
    }
    glutPostRedisplay();
    glutTimerFunc(100, tick, 0);
}

static void close_cb(void)
{
    server_request_shutdown();
    if (g_running) {
        *g_running = 0;
    }
}

void server_dashboard_run(stats_t *stats, volatile sig_atomic_t *running,
                          int argc, char **argv)
{
    g_stats = stats;
    g_running = running;
    int glut_argc = argc;
    glutInit(&glut_argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(900, 520);
    glutCreateWindow("Update Server Dashboard");
    glutDisplayFunc(display);
    glutTimerFunc(100, tick, 0);
#ifdef HAVE_GLUT_MAINLOOP_CONTROL
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glutCloseFunc(close_cb);
#endif
    glutMainLoop();
}

#else
void server_dashboard_run(stats_t *stats, volatile sig_atomic_t *running,
                          int argc, char **argv)
{
    (void)stats;
    (void)argc;
    (void)argv;
    while (*running) {
        sleep(1);
    }
}
#endif
