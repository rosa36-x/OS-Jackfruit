/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* ==============================================================
 * TODO: 
 * ============================================================== */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

/* ==============================================================
 * Helper: convert state → OLD running format
 * ============================================================== */
static int is_running(container_state_t state)
{
    return (state == CONTAINER_RUNNING) ? 1 : 0;
}

/* ==============================================================
 * ===================== OUTPUT FIXES ============================
 * ============================================================== */

/* ------------------ FIXED PS OUTPUT ------------------ */
static void handle_ps_command(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char line[256];
    int offset = 0;

    memset(resp, 0, sizeof(*resp));
    resp->status = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *c = ctx->containers;

    while (c && offset < (int)sizeof(resp->message) - 1) {
        snprintf(line, sizeof(line),
                 "%s pid=%d running=%d\n",
                 c->id, c->host_pid, is_running(c->state));

        int len = strlen(line);
        if (offset + len < (int)sizeof(resp->message)) {
            memcpy(resp->message + offset, line, len);
            offset += len;
        }
        c = c->next;
    }

    resp->message[offset] = '\0';
    pthread_mutex_unlock(&ctx->metadata_lock);
}

/* ------------------ FIXED START OUTPUT ------------------ */
static void handle_start_command(supervisor_ctx_t *ctx,
                                  const control_request_t *req,
                                  control_response_t *resp)
{
    int pipe_fds[2];
    pid_t pid;

    memset(resp, 0, sizeof(*resp));

    pipe(pipe_fds);

    pid = launch_container(ctx, req, pipe_fds[1]);
    close(pipe_fds[1]);

    if (pid < 0) {
        resp->status = -1;
        snprintf(resp->message, sizeof(resp->message),
                 "Failed to start %s", req->container_id);
        return;
    }

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id,
                              pid, req->soft_limit_bytes,
                              req->hard_limit_bytes);

    /* Add metadata */
    container_record_t *record = calloc(1, sizeof(container_record_t));
    strcpy(record->id, req->container_id);
    record->host_pid = pid;
    record->state = CONTAINER_RUNNING;

    pthread_mutex_lock(&ctx->metadata_lock);
    record->next = ctx->containers;
    ctx->containers = record;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Producer thread same */
    producer_args_t *pargs = malloc(sizeof(producer_args_t));
    pargs->pipe_read_fd = pipe_fds[0];
    pargs->log_buffer = &ctx->log_buffer;
    strcpy(pargs->container_id, req->container_id);

    pthread_t t;
    pthread_create(&t, NULL, producer_thread, pargs);
    pthread_detach(t);

    /* 🔥 OLD FORMAT */
    resp->status = 0;
    snprintf(resp->message, sizeof(resp->message),
             "Started %s pid=%d",
             req->container_id, pid);
}

/* ------------------ FIXED STOP OUTPUT ------------------ */
static void handle_stop_command(supervisor_ctx_t *ctx,
                                 const control_request_t *req,
                                 control_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    pthread_mutex_lock(&ctx->metadata_lock);

    container_record_t *c = ctx->containers;
    while (c) {
        if (strcmp(c->id, req->container_id) == 0) {
            kill(c->host_pid, SIGTERM);
            c->state = CONTAINER_STOPPED;

            /* 🔥 OLD FORMAT */
            resp->status = 0;
            snprintf(resp->message, sizeof(resp->message),
                     "Stopped %s", c->id);

            pthread_mutex_unlock(&ctx->metadata_lock);
            return;
        }
        c = c->next;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = -1;
    snprintf(resp->message, sizeof(resp->message),
             "Container not found");
}

/* ------------------ FIXED LOGS OUTPUT ------------------ */
static void handle_logs_command(supervisor_ctx_t *ctx,
                                 const control_request_t *req,
                                 int client_fd)
{
    char path[PATH_MAX];
    char buf[1024];
    int fd;
    ssize_t n;

    (void)ctx;

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        write(client_fd, "No logs\n", 8);
        return;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client_fd, buf, n);

    close(fd);
}
