/*
 * engine.c - Supervised Multi-Container Runtime
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define SOCK_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define BUF_SIZE 4096
#define BUFFER_CAP 16

/* ==============================================================
 * TODO 1: Container metadata
 * ============================================================== */
typedef struct container {
    char id[32];
    pid_t pid;
    int running;
    int exit_code;

    /* ✅ ADDED (required by spec) */
    int stop_requested;
    time_t start_time;
    unsigned long soft_limit;
    unsigned long hard_limit;

    struct container *next;
} container_t;

/* ==============================================================
 * TODO 2: Global state + synchronization
 * ============================================================== */
container_t *head = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ==============================================================
 * TODO 3: Bounded buffer for logging
 * ============================================================== */
typedef struct {
    char id[32];
    size_t len;
    char data[BUF_SIZE];
} log_item;

typedef struct {
    log_item items[BUFFER_CAP];
    int head, tail, count;
    int shutdown;
    pthread_mutex_t mtx;
    pthread_cond_t not_empty, not_full;
} buffer_t;

buffer_t logbuf;

/* ==============================================================
 * Buffer functions
 * ============================================================== */
void buf_init(buffer_t *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mtx, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

void buf_push(buffer_t *b, log_item *item) {
    pthread_mutex_lock(&b->mtx);
    while (b->count == BUFFER_CAP)
        pthread_cond_wait(&b->not_full, &b->mtx);

    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % BUFFER_CAP;
    b->count++;

    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
}

int buf_pop(buffer_t *b, log_item *item) {
    pthread_mutex_lock(&b->mtx);

    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->mtx);

    if (b->count == 0 && b->shutdown) {
        pthread_mutex_unlock(&b->mtx);
        return -1;
    }

    *item = b->items[b->head];
    b->head = (b->head + 1) % BUFFER_CAP;
    b->count--;

    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
    return 0;
}

/* ==============================================================
 * TODO 3: Logger thread (consumer)
 * ============================================================== */
void *logger(void *arg) {
    log_item item;
    char path[256];
    int fd;

    while (buf_pop(&logbuf, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.id);
        fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, item.data, item.len);
            close(fd);
        }
    }
    return NULL;
}

/* ==============================================================
 * TODO 3: Producer thread (reads pipe)
 * ============================================================== */
typedef struct {
    int fd;
    char id[32];
} prod_arg;

void *producer(void *arg) {
    prod_arg *p = arg;
    log_item item;

    while (1) {
        ssize_t n = read(p->fd, item.data, BUF_SIZE);
        if (n <= 0) break;

        item.len = n;
        strcpy(item.id, p->id);
        buf_push(&logbuf, &item);
    }

    close(p->fd);
    free(p);
    return NULL;
}

/* ==============================================================
 * TODO 1: Container process setup
 * ============================================================== */
int child_fn(void *arg) {
    char **argv = arg;

    sethostname(argv[0], strlen(argv[0]));
    chroot(argv[1]);
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    execv(argv[2], &argv[2]);
    perror("exec");
    return 1;
}

/* ==============================================================
 * TODO 4: Register with kernel monitor
 * ============================================================== */
void register_monitor(int fd, char *id, pid_t pid,
                      unsigned long soft, unsigned long hard) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strcpy(req.container_id, id);

    ioctl(fd, MONITOR_REGISTER, &req);
}

/* ==============================================================
 * SIGCHLD HANDLER (FIXED)
 * ============================================================== */
void sigchld_handler(int sig) {
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&lock);
        container_t *c = head;

        while (c) {
            if (c->pid == pid) {
                c->running = 0;

                if (WIFEXITED(status))
                    c->exit_code = WEXITSTATUS(status);
                else if (WIFSIGNALED(status))
                    c->exit_code = 128 + WTERMSIG(status);

                break;
            }
            c = c->next;
        }

        pthread_mutex_unlock(&lock);
    }
}

/* ==============================================================
 * TODO 1 + 2: Start container
 * ============================================================== */
void start_container(int client, int monitor_fd,
                     char *id, char *rootfs, char *cmd,
                     unsigned long soft, unsigned long hard, int nice_val) {

    int pipefd[2];
    pipe(pipefd);

    char *args[] = {id, rootfs, cmd, NULL};
    char *stack = malloc(STACK_SIZE);

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      args);

    if (pid < 0) {
        dprintf(client, "clone failed\n");
        return;
    }

    /* ✅ FIX: redirect stdout/stderr */
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
    }

    close(pipefd[1]);

    setpriority(PRIO_PROCESS, pid, nice_val);

    register_monitor(monitor_fd, id, pid, soft, hard);

    container_t *c = malloc(sizeof(container_t));
    strcpy(c->id, id);
    c->pid = pid;
    c->running = 1;
    c->stop_requested = 0;
    c->start_time = time(NULL);
    c->soft_limit = soft;
    c->hard_limit = hard;

    pthread_mutex_lock(&lock);
    c->next = head;
    head = c;
    pthread_mutex_unlock(&lock);

    prod_arg *p = malloc(sizeof(prod_arg));
    p->fd = pipefd[0];
    strcpy(p->id, id);

    pthread_t t;
    pthread_create(&t, NULL, producer, p);
    pthread_detach(t);

    dprintf(client, "Started %s pid=%d\n", id, pid);
}

/* ==============================================================
 * TODO 2: ps command
 * ============================================================== */
void cmd_ps(int client) {
    pthread_mutex_lock(&lock);

    container_t *c = head;
    while (c) {
        dprintf(client, "%s pid=%d running=%d exit=%d\n",
                c->id, c->pid, c->running, c->exit_code);
        c = c->next;
    }

    pthread_mutex_unlock(&lock);
}

/* ==============================================================
 * TODO 2: stop command
 * ============================================================== */
void cmd_stop(int client, char *id) {
    pthread_mutex_lock(&lock);

    container_t *c = head;
    while (c) {
        if (strcmp(c->id, id) == 0) {
            c->stop_requested = 1;
            kill(c->pid, SIGTERM);
            dprintf(client, "Stopped %s\n", id);
        }
        c = c->next;
    }

    pthread_mutex_unlock(&lock);
}

/* ==============================================================
 * TODO 2: logs command
 * ============================================================== */
void cmd_logs(int client, char *id) {
    char path[256], buf[1024];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(client, "No logs\n");
        return;
    }

    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(client, buf, n);

    close(fd);
}

/* ==============================================================
 * TODO 2: Supervisor loop (IPC)
 * ============================================================== */
void supervisor() {
    signal(SIGCHLD, sigchld_handler);

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    unlink(SOCK_PATH);

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(sock, 5);

    int monitor_fd = open("/dev/container_monitor", O_RDWR);

    mkdir(LOG_DIR, 0755);

    buf_init(&logbuf);

    pthread_t log_thread;
    pthread_create(&log_thread, NULL, logger, NULL);

    while (1) {
        int client = accept(sock, NULL, NULL);

        char cmd[256];
        read(client, cmd, sizeof(cmd));

        char *tok = strtok(cmd, " \n");

        if (!tok) continue;

        if (strcmp(tok, "start") == 0) {
            char *id = strtok(NULL, " \n");
            char *rootfs = strtok(NULL, " \n");
            char *c = strtok(NULL, " \n");

            start_container(client, monitor_fd, id, rootfs, c,
                            40 << 20, 64 << 20, 0);
        }
        else if (strcmp(tok, "ps") == 0) {
            cmd_ps(client);
        }
        else if (strcmp(tok, "stop") == 0) {
            cmd_stop(client, strtok(NULL, " \n"));
        }
        else if (strcmp(tok, "logs") == 0) {
            cmd_logs(client, strtok(NULL, " \n"));
        }

        close(client);
    }
}

/* ==============================================================
 * TODO 2: CLI client
 * ============================================================== */
void client(int argc, char *argv[]) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    connect(sock, (struct sockaddr *)&addr, sizeof(addr));

    char buf[256] = "";
    for (int i = 1; i < argc; i++) {
        strcat(buf, argv[i]);
        strcat(buf, " ");
    }

    write(sock, buf, strlen(buf));

    int n;
    while ((n = read(sock, buf, sizeof(buf))) > 0)
        write(1, buf, n);

    close(sock);
}

/* ==============================================================
 * MAIN
 * ============================================================== */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: supervisor | commands\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
        supervisor();
    else
        client(argc, argv);

    return 0;
}
