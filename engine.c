// FULL engine.c (clean + fixed)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "monitor_ioctl.h"

#define STACK_SIZE 1024*1024
#define SOCK_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"

typedef struct container {
    char id[32];
    pid_t pid;
    int running;
    struct container *next;
} container_t;

container_t *head = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- Child ---------- */
int child_func(void *arg) {
    char **argv = arg;

    sethostname(argv[0], strlen(argv[0]));
    chroot(argv[1]);
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);

    execv(argv[2], &argv[2]);
    perror("exec");
    return 1;
}

/* ---------- Start container ---------- */
void start_container(char *id, char *rootfs, char *cmd) {
    char *args[] = {id, rootfs, cmd, NULL};

    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(child_func, stack + STACK_SIZE,
        CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
        args);

    pthread_mutex_lock(&lock);

    container_t *c = malloc(sizeof(container_t));
    strcpy(c->id, id);
    c->pid = pid;
    c->running = 1;
    c->next = head;
    head = c;

    pthread_mutex_unlock(&lock);

    printf("Started %s pid=%d\n", id, pid);
}

/* ---------- PS ---------- */
void list_containers() {
    pthread_mutex_lock(&lock);
    container_t *c = head;
    while (c) {
        printf("%s pid=%d\n", c->id, c->pid);
        c = c->next;
    }
    pthread_mutex_unlock(&lock);
}

/* ---------- STOP ---------- */
void stop_container(char *id) {
    pthread_mutex_lock(&lock);
    container_t *c = head;
    while (c) {
        if (strcmp(c->id, id) == 0) {
            kill(c->pid, SIGTERM);
            c->running = 0;
            printf("Stopped %s\n", id);
        }
        c = c->next;
    }
    pthread_mutex_unlock(&lock);
}

/* ---------- Supervisor ---------- */
void run_supervisor() {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    unlink(SOCK_PATH);
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(sock, 5);

    printf("Supervisor running...\n");

    while (1) {
        int client = accept(sock, NULL, NULL);
        char buf[256] = {0};

        read(client, buf, sizeof(buf));

        if (strncmp(buf, "start", 5) == 0) {
            char id[32], rootfs[128], cmd[128];
            sscanf(buf, "start %s %s %s", id, rootfs, cmd);
            start_container(id, rootfs, cmd);
        }
        else if (strncmp(buf, "ps", 2) == 0) {
            list_containers();
        }
        else if (strncmp(buf, "stop", 4) == 0) {
            char id[32];
            sscanf(buf, "stop %s", id);
            stop_container(id);
        }

        close(client);
    }
}

/* ---------- Client ---------- */
void send_cmd(char *cmd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, SOCK_PATH);

    connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    write(sock, cmd, strlen(cmd));

    close(sock);
}

/* ---------- MAIN ---------- */
int main(int argc, char *argv[]) {
    if (argc < 2) return 0;

    if (strcmp(argv[1], "supervisor") == 0) {
        run_supervisor();
    }
    else if (strcmp(argv[1], "start") == 0) {
        char cmd[256];
        sprintf(cmd, "start %s %s %s", argv[2], argv[3], argv[4]);
        send_cmd(cmd);
    }
    else if (strcmp(argv[1], "ps") == 0) {
        send_cmd("ps");
    }
    else if (strcmp(argv[1], "stop") == 0) {
        char cmd[64];
        sprintf(cmd, "stop %s", argv[2]);
        send_cmd(cmd);
    }

    return 0;
}
