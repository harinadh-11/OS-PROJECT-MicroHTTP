#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define PORT 8080
#define WORKER_COUNT 4
#define QUEUE_SIZE 64
#define REQUEST_SIZE 8192
#define TARGET_SIZE 2048
#define IDLE_TIMEOUT_SECONDS 15
#define WEB_ROOT "."

typedef struct {
    int sockets[QUEUE_SIZE];
    size_t head, tail, count;
    int stopping;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty, not_full;
} job_queue_t;

typedef struct {
    char data[REQUEST_SIZE];
    size_t used;
} input_buffer_t;

static job_queue_t jobs;
static volatile sig_atomic_t stop_requested;
static int listening_fd = -1;

static void handle_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int send_all(int fd, const void *data, size_t length) {
    const char *bytes = data;
    size_t sent = 0;
    while (sent < length) {
        ssize_t n = send(fd, bytes + sent, length - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int send_response(int fd, int status, const char *reason,
                         const char *type, const void *body, size_t length,
                         int keep_alive) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: %s\r\n\r\n", status, reason, type, length,
        keep_alive ? "keep-alive" : "close");
    if (n < 0 || (size_t)n >= sizeof(header) || send_all(fd, header, (size_t)n)) return -1;
    return length ? send_all(fd, body, length) : 0;
}

static int open_web_file(const char *path) {
    int directory = open(WEB_ROOT, O_RDONLY | O_DIRECTORY);
    if (directory < 0) return -1;
    char relative[TARGET_SIZE];
    if (strlen(path + 1) >= sizeof(relative)) { close(directory); return -1; }
    strcpy(relative, path + 1);
    char *save = NULL;
    char *part = strtok_r(relative, "/", &save);
    if (part == NULL) { close(directory); return -1; }
    for (;;) {
        char *next = strtok_r(NULL, "/", &save);
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) { close(directory); return -1; }
        int flags = O_RDONLY | O_NOFOLLOW;
        if (next != NULL) flags |= O_DIRECTORY;
        int opened = openat(directory, part, flags);
        close(directory);
        if (opened < 0) return -1;
        if (next == NULL) return opened;
        directory = opened;
        part = next;
    }
}

static int send_file_response(int fd, const char *filename, int keep_alive) {
    int file_fd = open_web_file(filename);
    if (file_fd < 0) {
        static const char missing[] = "404 Not Found\n";
        return send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                             missing, sizeof(missing) - 1, keep_alive);
    }
    struct stat info;
    if (fstat(file_fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        close(file_fd);
        static const char missing[] = "404 Not Found\n";
        return send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                             missing, sizeof(missing) - 1, keep_alive);
    }
    const char *ext = strrchr(filename, '.');
    const char *type = "application/octet-stream";
    if (ext != NULL) {
        if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) type = "text/html; charset=utf-8";
        else if (!strcasecmp(ext, ".css")) type = "text/css; charset=utf-8";
        else if (!strcasecmp(ext, ".js")) type = "text/javascript; charset=utf-8";
        else if (!strcasecmp(ext, ".txt")) type = "text/plain; charset=utf-8";
        else if (!strcasecmp(ext, ".json")) type = "application/json";
        else if (!strcasecmp(ext, ".png")) type = "image/png";
        else if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) type = "image/jpeg";
        else if (!strcasecmp(ext, ".gif")) type = "image/gif";
        else if (!strcasecmp(ext, ".svg")) type = "image/svg+xml";
    }
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lld\r\n"
        "Connection: %s\r\n\r\n", type, (long long)info.st_size,
        keep_alive ? "keep-alive" : "close");
    int result = -1;
    if (n >= 0 && (size_t)n < sizeof(header) && send_all(fd, header, (size_t)n) == 0) {
        char chunk[4096];
        off_t remaining = info.st_size;
        result = 0;
        while (remaining > 0) {
            size_t want = remaining < (off_t)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
            ssize_t got = read(file_fd, chunk, want);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0 || send_all(fd, chunk, (size_t)got) != 0) { result = -1; break; }
            remaining -= got;
        }
    }
    close(file_fd);
    return result;
}

/* Decode escapes before checking segments; reject encoded separators and dot-dot. */
static int decode_path(const char *source, char *dest, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; source[i] && source[i] != '?'; ++i) {
        unsigned char c = (unsigned char)source[i];
        if (c == '%') {
            if (!source[i + 1] || !source[i + 2]) return -1;
            int hi = source[i + 1] >= '0' && source[i + 1] <= '9' ? source[i + 1] - '0' :
                     source[i + 1] >= 'a' && source[i + 1] <= 'f' ? source[i + 1] - 'a' + 10 :
                     source[i + 1] >= 'A' && source[i + 1] <= 'F' ? source[i + 1] - 'A' + 10 : -1;
            int lo = source[i + 2] >= '0' && source[i + 2] <= '9' ? source[i + 2] - '0' :
                     source[i + 2] >= 'a' && source[i + 2] <= 'f' ? source[i + 2] - 'a' + 10 :
                     source[i + 2] >= 'A' && source[i + 2] <= 'F' ? source[i + 2] - 'A' + 10 : -1;
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        }
        if (out + 1 >= capacity || c == '\\' || c < 32 || c == 127) return -1;
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
    if (dest[0] != '/') return -1;
    const char *segment = dest + 1;
    for (const char *p = segment;; ++p) {
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if ((len == 2 && segment[0] == '.' && segment[1] == '.') || (len == 1 && segment[0] == '.')) return -1;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return 0;
}

static int header_has_token(const char *value, const char *wanted) {
    size_t wanted_len = strlen(wanted);
    while (*value) {
        while (*value == ' ' || *value == '\t' || *value == ',') ++value;
        const char *start = value;
        while (*value && *value != ',') ++value;
        const char *end = value;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if ((size_t)(end - start) == wanted_len && strncasecmp(start, wanted, wanted_len) == 0) return 1;
        if (*value == ',') ++value;
    }
    return 0;
}

static ssize_t find_header_end(const char *data, size_t length) {
    for (size_t i = 3; i < length; ++i)
        if (data[i - 3] == '\r' && data[i - 2] == '\n' && data[i - 1] == '\r' && data[i] == '\n')
            return (ssize_t)(i + 1);
    return -1;
}

static int read_request(int fd, input_buffer_t *input, char *method, size_t method_cap,
                        char *target, size_t target_cap, int *keep_alive) {
    (void)method_cap;
    (void)target_cap;
    ssize_t end;
    while ((end = find_header_end(input->data, input->used)) < 0) {
        if (input->used >= sizeof(input->data) - 1) return -1;
        ssize_t n = recv(fd, input->data + input->used, sizeof(input->data) - input->used - 1, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        input->used += (size_t)n;
        input->data[input->used] = '\0';
    }
    char *line_end = strstr(input->data, "\r\n");
    if (!line_end || (size_t)(line_end - input->data) >= (size_t)end) return -1;
    *line_end = '\0';
    char version[16];
    if (sscanf(input->data, "%15s %2047s %15s", method, target, version) != 3 ||
        strcmp(version, "HTTP/1.1") != 0) return -1;
    *keep_alive = 1;
    int has_host = 0;
    int has_length = 0;
    unsigned long long content_length = 0;
    char *line = line_end + 2;
    while (line < input->data + end - 2) {
        char *next = strstr(line, "\r\n");
        if (!next) return -1;
        if (next == line) break;
        *next = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line) return -1;
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *value_end = colon + strlen(colon);
        while (value_end > colon && (value_end[-1] == ' ' || value_end[-1] == '\t')) *--value_end = '\0';
        if (!strcasecmp(line, "Host")) {
            if (*colon == '\0') return -1;
            has_host = 1;
        } else if (!strcasecmp(line, "Connection")) {
            if (header_has_token(colon, "close")) *keep_alive = 0;
            else if (header_has_token(colon, "keep-alive")) *keep_alive = 1;
        } else if (!strcasecmp(line, "Content-Length")) {
            char *tail;
            errno = 0;
            content_length = strtoull(colon, &tail, 10);
            if (errno || tail == colon || *tail || has_length) return -1;
            has_length = 1;
        } else if (!strcasecmp(line, "Transfer-Encoding")) {
            return -1;
        }
        line = next + 2;
    }
    if (!has_host || (has_length && content_length != 0)) return -1;
    size_t consumed = (size_t)end;
    memmove(input->data, input->data + consumed, input->used - consumed);
    input->used -= consumed;
    input->data[input->used] = '\0';
    return 1;
}

static int workers_stopping(void) {
    pthread_mutex_lock(&jobs.mutex);
    int stopping = jobs.stopping;
    pthread_mutex_unlock(&jobs.mutex);
    return stopping;
}

static void handle_client(int fd) {
    struct timeval timeout = { IDLE_TIMEOUT_SECONDS, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    input_buffer_t input = { .used = 0 };
    for (;;) {
        char method[16], target[TARGET_SIZE];
        int keep_alive;
        int parsed = read_request(fd, &input, method, sizeof(method), target, sizeof(target), &keep_alive);
        if (parsed <= 0) {
            if (parsed < 0) {
                static const char bad[] = "400 Bad Request\n";
                (void)send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                    bad, sizeof(bad) - 1, 0);
            }
            return;
        }
        if (strcmp(method, "GET") != 0) {
            static const char bad[] = "405 Method Not Allowed\n";
            (void)send_response(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                                bad, sizeof(bad) - 1, 0);
            return;
        }
        char path[TARGET_SIZE];
        if (decode_path(target, path, sizeof(path)) != 0) {
            static const char bad[] = "404 Not Found\n";
            (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                bad, sizeof(bad) - 1, keep_alive);
        } else {
            if (strcmp(path, "/") == 0) strcpy(path, "/index.html");
            char filename[4096];
            int n = snprintf(filename, sizeof(filename), "%s", path);
            if (n < 0 || (size_t)n >= sizeof(filename)) {
                static const char bad[] = "404 Not Found\n";
                (void)send_response(fd, 404, "Not Found", "text/plain; charset=utf-8",
                                    bad, sizeof(bad) - 1, keep_alive);
            } else if (send_file_response(fd, filename, keep_alive) != 0) {
                return;
            }
        }
        if (!keep_alive || workers_stopping()) return;
    }
}

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&jobs.mutex);
        while (jobs.count == 0 && !jobs.stopping) pthread_cond_wait(&jobs.not_empty, &jobs.mutex);
        if (jobs.count == 0 && jobs.stopping) {
            pthread_mutex_unlock(&jobs.mutex);
            return NULL;
        }
        int fd = jobs.sockets[jobs.head];
        jobs.head = (jobs.head + 1) % QUEUE_SIZE;
        --jobs.count;
        pthread_cond_signal(&jobs.not_full);
        pthread_mutex_unlock(&jobs.mutex);
        handle_client(fd);
        close(fd);
    }
}

static int enqueue(int fd) {
    pthread_mutex_lock(&jobs.mutex);
    while (jobs.count == QUEUE_SIZE && !jobs.stopping && !stop_requested) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 250000000L;
        if (deadline.tv_nsec >= 1000000000L) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000L; }
        (void)pthread_cond_timedwait(&jobs.not_full, &jobs.mutex, &deadline);
    }
    if (jobs.stopping || stop_requested) { pthread_mutex_unlock(&jobs.mutex); return -1; }
    jobs.sockets[jobs.tail] = fd;
    jobs.tail = (jobs.tail + 1) % QUEUE_SIZE;
    ++jobs.count;
    pthread_cond_signal(&jobs.not_empty);
    pthread_mutex_unlock(&jobs.mutex);
    return 0;
}

static void stop_workers(void) {
    pthread_mutex_lock(&jobs.mutex);
    jobs.stopping = 1;
    pthread_cond_broadcast(&jobs.not_empty);
    pthread_cond_broadcast(&jobs.not_full);
    pthread_mutex_unlock(&jobs.mutex);
}

int main(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction"); return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    int sync_error = pthread_mutex_init(&jobs.mutex, NULL);
    if (sync_error != 0) { fprintf(stderr, "pthread_mutex_init: %s\n", strerror(sync_error)); return 1; }
    sync_error = pthread_cond_init(&jobs.not_empty, NULL);
    if (sync_error != 0) {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(sync_error));
        pthread_mutex_destroy(&jobs.mutex);
        return 1;
    }
    sync_error = pthread_cond_init(&jobs.not_full, NULL);
    if (sync_error != 0) {
        fprintf(stderr, "pthread_cond_init: %s\n", strerror(sync_error));
        pthread_cond_destroy(&jobs.not_empty);
        pthread_mutex_destroy(&jobs.mutex);
        return 1;
    }
    pthread_t workers[WORKER_COUNT];
    size_t created = 0;
    for (; created < WORKER_COUNT; ++created) {
        int error = pthread_create(&workers[created], NULL, worker, NULL);
        if (error != 0) { fprintf(stderr, "pthread_create: %s\n", strerror(error)); break; }
    }
    if (created != WORKER_COUNT) {
        stop_workers();
        for (size_t i = 0; i < created; ++i) pthread_join(workers[i], NULL);
        pthread_cond_destroy(&jobs.not_full); pthread_cond_destroy(&jobs.not_empty);
        pthread_mutex_destroy(&jobs.mutex); return 1;
    }
    listening_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listening_fd < 0) { perror("socket"); stop_workers(); goto shutdown; }
    int opt = 1;
    if (setsockopt(listening_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) perror("setsockopt");
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);
    if (bind(listening_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listening_fd, QUEUE_SIZE) < 0) {
        perror("bind/listen"); close(listening_fd); listening_fd = -1; stop_workers(); goto shutdown;
    }
    printf("MicroHTTP listening on port %d with %d workers (idle timeout %d seconds)\n",
           PORT, WORKER_COUNT, IDLE_TIMEOUT_SECONDS);
    while (!stop_requested) {
        struct pollfd pfd = { listening_fd, POLLIN, 0 };
        int ready = poll(&pfd, 1, 500);
        if (ready < 0) { if (errno == EINTR) continue; perror("poll"); break; }
        if (ready == 0) continue;
        if (stop_requested) break;
        int client_fd = accept(listening_fd, NULL, NULL);
        if (client_fd < 0) { if (errno == EINTR) continue; perror("accept"); continue; }
        if (enqueue(client_fd) != 0) close(client_fd);
    }
    close(listening_fd);
    listening_fd = -1;
    puts("Stopping: draining queued clients and waiting for workers.");
    stop_workers();
shutdown:
    for (size_t i = 0; i < created; ++i) pthread_join(workers[i], NULL);
    pthread_cond_destroy(&jobs.not_full);
    pthread_cond_destroy(&jobs.not_empty);
    pthread_mutex_destroy(&jobs.mutex);
    return 0;
}









