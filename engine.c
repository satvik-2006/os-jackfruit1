/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

#define MAX_CONTAINERS 32

typedef struct {
    int used;
    pid_t pid;
    char id[64];
    char state[32];
    char rootfs[256];
    char command[256];
} runtime_entry_t;

static runtime_entry_t runtime_table[MAX_CONTAINERS];
static pthread_mutex_t runtime_lock = PTHREAD_MUTEX_INITIALIZER;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
static int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *msg)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *msg;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}
/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
static int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *msg)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *msg = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}
/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
log_item_t msg;
    FILE *fp;
    char path[256];

    while (bounded_buffer_pop(buffer, &msg) == 0) {
        snprintf(path, sizeof(path), "logs/%s.log", msg.container_id);

        mkdir("logs", 0755);

        fp = fopen(path, "a");
        if (fp) {
            fwrite(msg.data, 1, msg.length, fp);
            fclose(fp);
        }
    }

    return NULL;
}
/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    if (unshare(CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWUTS |
                CLONE_NEWIPC | CLONE_NEWNET) != 0) {
        perror("unshare failed");
        exit(1);
    }

    if (chroot(config->rootfs) != 0) {
        perror("chroot failed");
        exit(1);
    }

    chdir("/");

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount failed");
    }

    execl("/bin/sh", "/bin/sh", "-c", config->command, NULL);

    perror("exec failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}
static void add_container(pid_t pid, const char *id, const char *rootfs, const char *cmd)
{
    int i;
    pthread_mutex_lock(&runtime_lock);

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (!runtime_table[i].used) {
            runtime_table[i].used = 1;
            runtime_table[i].pid = pid;
            strncpy(runtime_table[i].id, id, sizeof(runtime_table[i].id) - 1);
            strncpy(runtime_table[i].rootfs, rootfs, sizeof(runtime_table[i].rootfs) - 1);
            strncpy(runtime_table[i].command, cmd, sizeof(runtime_table[i].command) - 1);
            strcpy(runtime_table[i].state, "running");
            break;
        }
    }

    pthread_mutex_unlock(&runtime_lock);
}

static void mark_container_exit(pid_t pid)
{
    int i;
    pthread_mutex_lock(&runtime_lock);

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (runtime_table[i].used && runtime_table[i].pid == pid) {
            strcpy(runtime_table[i].state, "exited");
        }
    }

    pthread_mutex_unlock(&runtime_lock);
}

static void print_containers(int fd)
{
    int i;
    dprintf(fd, "ID\tPID\tSTATE\tCOMMAND\n");

    pthread_mutex_lock(&runtime_lock);

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (runtime_table[i].used) {
            dprintf(fd, "%s\t%d\t%s\t%s\n",
                runtime_table[i].id,
                runtime_table[i].pid,
                runtime_table[i].state,
                runtime_table[i].command);
        }
    }

    pthread_mutex_unlock(&runtime_lock);
}

static void stop_container_by_id(int fd, const char *id)
{
    int i;
    pthread_mutex_lock(&runtime_lock);

    for (i = 0; i < MAX_CONTAINERS; i++) {
        if (runtime_table[i].used && strcmp(runtime_table[i].id, id) == 0) {
            kill(runtime_table[i].pid, SIGTERM);
            strcpy(runtime_table[i].state, "stopped");
            dprintf(fd, "Stopped %s\n", id);
            pthread_mutex_unlock(&runtime_lock);
            return;
        }
    }

    pthread_mutex_unlock(&runtime_lock);
    dprintf(fd, "Container not found\n");
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */

static int run_supervisor(const char *rootfs)
{
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char buffer[512];

    unlink(CONTROL_PATH);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) return 1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
    if (listen(server_fd, 5) < 0) return 1;

    printf("[SUPERVISOR] Running...\n");

    while (1) {
pid_t dead;
while ((dead = waitpid(-1, NULL, WNOHANG)) > 0)
    mark_container_exit(dead);

        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        memset(buffer, 0, sizeof(buffer));
        read(client_fd, buffer, sizeof(buffer) - 1);

        if (strncmp(buffer, "ps", 2) == 0) {
            print_containers(client_fd);
        }

        else if (strncmp(buffer, "logs ", 5) == 0) {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "cat logs/%s.log 2>/dev/null", buffer + 5);

            FILE *fp = popen(cmd, "r");
            if (fp) {
                while (fgets(cmd, sizeof(cmd), fp))
                    write(client_fd, cmd, strlen(cmd));
                pclose(fp);
            }
        }

        else if (strncmp(buffer, "stop ", 5) == 0) {
            stop_container_by_id(client_fd, buffer + 5);
        }

        else if (strncmp(buffer, "start ", 6) == 0) {
            char id[64], rootfs_path[256], command[256];
            pid_t pid;

            sscanf(buffer + 6, "%63s %255s %255[^\n]", id, rootfs_path, command);

            pid = fork();

            if (pid == 0) {
                char logfile[256];

                snprintf(logfile, sizeof(logfile), "logs/%s.log", id);
                mkdir("logs", 0755);
                freopen(logfile, "a", stdout);
                freopen(logfile, "a", stderr);

                execl("./engine", "./engine", "run", id, rootfs_path, command, NULL);
                exit(1);
            }

            add_container(pid, id, rootfs_path, command);
            dprintf(client_fd, "Started %s with PID %d\n", id, pid);
        }

        else {
            dprintf(client_fd, "Unknown command\n");
        }

        close(client_fd);
    }

    return 0;
}
    /*
     * TODO:
     *   1) open /dev/container_monitor
     *   2) create the control socket / FIFO / shared-memory channel
     *   3) install SIGCHLD / SIGINT / SIGTERM handling
     *   4) spawn the logger thread
     *   5) enter the supervisor event loop
     */

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    char buffer[1024];
    char message[512];
    int n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_PS)
        snprintf(message, sizeof(message), "ps");
    else if (req->kind == CMD_LOGS)
        snprintf(message, sizeof(message), "logs %s", req->container_id);
    else if (req->kind == CMD_STOP)
        snprintf(message, sizeof(message), "stop %s", req->container_id);
    else if (req->kind == CMD_START)
        snprintf(message, sizeof(message), "start %s %s %s",
                 req->container_id, req->rootfs, req->command);
    else
        snprintf(message, sizeof(message), "unknown");

    write(fd, message, strlen(message));

    while ((n = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = 0;
        printf("%s", buffer);
    }

    close(fd);
    return 0;
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    /*
     * TODO:
     * The supervisor should respond with container metadata.
     * Keep the rendering format simple enough for demos and debugging.
     */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}
static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    memset(&req, 0, sizeof(req));

    if (argc < 5) {
        fprintf(stderr, "usage: engine start <id> <rootfs> <command>\n");
        return 1;
    }

    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    pid_t pid;
    int status;
    char command[512];

    if (argc < 5) {
        fprintf(stderr, "usage: engine run <id> <rootfs> <command>\n");
        return 1;
    }

    printf("[ENGINE] Starting container %s\n", argv[2]);

    snprintf(command, sizeof(command),
             "chroot %s /bin/sh -c '%s'",
             argv[3], argv[4]);

    pid = fork();

    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command, NULL);
        exit(1);
    }

    waitpid(pid, &status, 0);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
