#ifndef GUI_H
#define GUI_H

#include "common.h"
#include "stats.h"
#include <pthread.h>
#include <signal.h>

#ifndef USE_GUI
#define USE_GUI 1
#endif
#if !USE_GUI
#error "The OpenGL GUI is mandatory for this project. Build with USE_GUI=1."
#endif

typedef struct {
    pthread_mutex_t lock;
    char status[32];
    char current_version[MAX_VERSION_STR];
    char latest_version[MAX_VERSION_STR];
    unsigned long long bytes_done;
    unsigned long long bytes_total;
    double speed_bps;
    int show_progress;
    int pause_allowed;
    int resume_allowed;
    int pause_requested;
    int resume_requested;
    char log_lines[8][160];
    int log_count;
} client_gui_state_t;

void client_gui_state_init(client_gui_state_t *g);
void client_gui_set_status(client_gui_state_t *g, const char *status);
void client_gui_set_versions(client_gui_state_t *g, const char *current,
                             const char *latest);
void client_gui_set_progress(client_gui_state_t *g, unsigned long long done,
                             unsigned long long total, double speed_bps);
void client_gui_set_progress_visible(client_gui_state_t *g, int visible);
void client_gui_set_controls(client_gui_state_t *g, int pause_allowed,
                             int resume_allowed);
int client_gui_consume_pause_request(client_gui_state_t *g);
int client_gui_consume_resume_request(client_gui_state_t *g);
void client_gui_add_log(client_gui_state_t *g, const char *line);
int client_gui_run(client_gui_state_t *g, int argc, char **argv,
                   int (*worker_fn)(void *), void *worker_arg);
void server_dashboard_run(stats_t *stats, volatile sig_atomic_t *running,
                          int argc, char **argv);

#endif
