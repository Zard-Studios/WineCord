#define _DARWIN_C_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define WINECORD_VERSION "0.1.4"
#define WINECORD_LABEL "com.zardstudios.winecord.agent"
#define WINECORD_DEFAULT_PORT 38477
#define WINECORD_PIPE_COUNT 10
#define WINECORD_TOKEN_BYTES 32
#define MAX_CANDIDATES 64

typedef struct {
    char app_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char config_path[PATH_MAX];
    char host[64];
    int port;
    char token[(WINECORD_TOKEN_BYTES * 2) + 1];
} Config;

typedef struct {
    int client_fd;
    char token[(WINECORD_TOKEN_BYTES * 2) + 1];
} ClientContext;

static void usage(FILE *out);

static const char *home_dir(void) {
    const char *home = getenv("HOME");
    return (home && *home) ? home : ".";
}

static void chomp(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static bool path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool is_directory(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_socket_path(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path) return -1;

    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;

    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int parent_dir(const char *path, char *out, size_t out_len) {
    if (!path || !out || out_len == 0) return -1;
    if (snprintf(out, out_len, "%s", path) >= (int)out_len) return -1;
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, out_len, ".");
        return 0;
    }
    if (slash == out) {
        slash[1] = '\0';
        return 0;
    }
    *slash = '\0';
    return 0;
}

static int current_executable(char *out, size_t out_len) {
    uint32_t size = (uint32_t)out_len;
    if (_NSGetExecutablePath(out, &size) != 0) return -1;
    char resolved[PATH_MAX];
    if (realpath(out, resolved)) {
        snprintf(out, out_len, "%s", resolved);
    }
    return 0;
}

static void launch_executable(char *out, size_t out_len) {
    if (current_executable(out, out_len) != 0) {
        snprintf(out, out_len, "winecord");
        return;
    }

    const char marker[] = "/Cellar/winecord/";
    char *cellar = strstr(out, marker);
    if (!cellar) return;

    char prefix[PATH_MAX];
    size_t prefix_len = (size_t)(cellar - out);
    if (prefix_len == 0 || prefix_len >= sizeof(prefix)) return;
    memcpy(prefix, out, prefix_len);
    prefix[prefix_len] = '\0';

    char stable[PATH_MAX];
    if (snprintf(stable, sizeof(stable), "%s/opt/winecord/bin/winecord", prefix) >= (int)sizeof(stable)) return;
    if (path_exists(stable)) snprintf(out, out_len, "%s", stable);
}

static void default_config(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->app_dir, sizeof(cfg->app_dir),
             "%s/Library/Application Support/WineCord", home_dir());
    snprintf(cfg->log_dir, sizeof(cfg->log_dir),
             "%s/Library/Logs/WineCord", home_dir());
    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s/config.ini", cfg->app_dir);
    snprintf(cfg->host, sizeof(cfg->host), "127.0.0.1");
    cfg->port = WINECORD_DEFAULT_PORT;
}

static int generate_token(char *out, size_t out_len) {
    if (out_len < (WINECORD_TOKEN_BYTES * 2) + 1) return -1;

    unsigned char bytes[WINECORD_TOKEN_BYTES];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t off = 0;
        while (off < sizeof(bytes)) {
            ssize_t n = read(fd, bytes + off, sizeof(bytes) - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            off += (size_t)n;
        }
        close(fd);
        if (off != sizeof(bytes)) fd = -1;
    }

    if (fd < 0) {
        srand((unsigned int)(time(NULL) ^ getpid()));
        for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)(rand() & 0xff);
    }

    for (size_t i = 0; i < sizeof(bytes); i++) {
        snprintf(out + (i * 2), out_len - (i * 2), "%02x", bytes[i]);
    }
    out[WINECORD_TOKEN_BYTES * 2] = '\0';
    return 0;
}

static int save_config(const Config *cfg) {
    if (mkdir_p(cfg->app_dir, 0700) != 0) {
        fprintf(stderr, "Could not create %s: %s\n", cfg->app_dir, strerror(errno));
        return -1;
    }

    FILE *f = fopen(cfg->config_path, "w");
    if (!f) {
        fprintf(stderr, "Could not write %s: %s\n", cfg->config_path, strerror(errno));
        return -1;
    }

    fprintf(f, "host=%s\n", cfg->host);
    fprintf(f, "port=%d\n", cfg->port);
    fprintf(f, "token=%s\n", cfg->token);
    fclose(f);
    chmod(cfg->config_path, 0600);
    return 0;
}

static int load_config(Config *cfg) {
    default_config(cfg);

    const char *override = getenv("WINECORD_CONFIG");
    if (override && *override) snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", override);

    FILE *f = fopen(cfg->config_path, "r");
    if (!f) {
        if (generate_token(cfg->token, sizeof(cfg->token)) != 0) return -1;
        return save_config(cfg);
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);
        if (strcmp(key, "host") == 0 && *value) {
            snprintf(cfg->host, sizeof(cfg->host), "%s", value);
        } else if (strcmp(key, "port") == 0 && *value) {
            cfg->port = atoi(value);
        } else if (strcmp(key, "token") == 0 && *value) {
            snprintf(cfg->token, sizeof(cfg->token), "%s", value);
        }
    }
    fclose(f);

    const char *env_port = getenv("WINECORD_PORT");
    const char *env_token = getenv("WINECORD_TOKEN");
    if (env_port && *env_port) cfg->port = atoi(env_port);
    if (env_token && *env_token) snprintf(cfg->token, sizeof(cfg->token), "%s", env_token);

    if (cfg->port <= 0 || cfg->port > 65535) cfg->port = WINECORD_DEFAULT_PORT;
    if (cfg->token[0] == '\0') {
        if (generate_token(cfg->token, sizeof(cfg->token)) != 0) return -1;
        return save_config(cfg);
    }

    return 0;
}

static void token_redacted(const Config *cfg, char *out, size_t out_len) {
    size_t len = strlen(cfg->token);
    if (len < 12) {
        snprintf(out, out_len, "<set>");
        return;
    }
    snprintf(out, out_len, "%.6s...%.6s", cfg->token, cfg->token + len - 6);
}

static bool add_candidate(char paths[][PATH_MAX], int *count, const char *path) {
    if (!path || !*path || *count >= MAX_CANDIDATES) return false;
    if (!is_socket_path(path)) return false;

    for (int i = 0; i < *count; i++) {
        if (strcmp(paths[i], path) == 0) return false;
    }
    snprintf(paths[*count], PATH_MAX, "%s", path);
    (*count)++;
    return true;
}

static void add_from_dir(char paths[][PATH_MAX], int *count, const char *dir) {
    if (!dir || !*dir) return;
    char clean[PATH_MAX];
    if (snprintf(clean, sizeof(clean), "%s", dir) >= (int)sizeof(clean)) return;
    size_t len = strlen(clean);
    while (len > 1 && clean[len - 1] == '/') clean[--len] = '\0';
    for (int i = 0; i < WINECORD_PIPE_COUNT; i++) {
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/discord-ipc-%d", clean, i) < (int)sizeof(path)) {
            add_candidate(paths, count, path);
        }
    }
}

static void add_from_glob(char paths[][PATH_MAX], int *count, const char *pattern) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(pattern, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            add_candidate(paths, count, g.gl_pathv[i]);
        }
    }
    globfree(&g);
}

static int collect_discord_sockets(char paths[][PATH_MAX], int max_paths) {
    (void)max_paths;
    int count = 0;

    const char *explicit_path = getenv("WINECORD_DISCORD_IPC");
    if (explicit_path && *explicit_path) add_candidate(paths, &count, explicit_path);

    add_from_dir(paths, &count, getenv("XDG_RUNTIME_DIR"));
    add_from_dir(paths, &count, getenv("TMPDIR"));
    add_from_dir(paths, &count, getenv("TMP"));
    add_from_dir(paths, &count, getenv("TEMP"));
    add_from_dir(paths, &count, "/tmp");

    add_from_glob(paths, &count, "/var/folders/*/*/T/discord-ipc-*");
    add_from_glob(paths, &count, "/var/folders/*/*/*/discord-ipc-*");

    return count;
}

static int connect_unix_socket(const char *path) {
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Discord IPC path too long for AF_UNIX: %s\n", path);
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
    addr.sun_len = sizeof(addr);
#endif
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

#ifdef SUN_LEN
    socklen_t len = (socklen_t)SUN_LEN(&addr);
#else
    socklen_t len = (socklen_t)(sizeof(addr.sun_family) + strlen(addr.sun_path) + 1);
#endif
    if (connect(fd, (struct sockaddr *)&addr, len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int connect_discord(char *chosen, size_t chosen_len) {
    char paths[MAX_CANDIDATES][PATH_MAX];
    int count = collect_discord_sockets(paths, MAX_CANDIDATES);
    for (int i = 0; i < count; i++) {
        int fd = connect_unix_socket(paths[i]);
        if (fd >= 0) {
            snprintf(chosen, chosen_len, "%s", paths[i]);
            return fd;
        }
    }
    return -1;
}

static ssize_t write_all(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return (ssize_t)total;
}

static uint32_t read_le32(const unsigned char *p) {
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void extract_json_string(const unsigned char *payload, size_t payload_len,
                                const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    if (!payload || !key || out_len == 0) return;

    char needle[96];
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) return;
    size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len < payload_len; i++) {
        if (memcmp(payload + i, needle, needle_len) != 0) continue;

        size_t j = i + needle_len;
        while (j < payload_len && payload[j] != ':') j++;
        if (j >= payload_len) return;
        j++;
        while (j < payload_len && isspace(payload[j])) j++;
        if (j >= payload_len || payload[j] != '"') return;
        j++;

        size_t pos = 0;
        while (j < payload_len && payload[j] != '"' && pos + 1 < out_len) {
            if (payload[j] == '\\' && j + 1 < payload_len) j++;
            unsigned char c = payload[j++];
            out[pos++] = (c >= 32 && c < 127) ? (char)c : '?';
        }
        out[pos] = '\0';
        return;
    }
}

static void log_ipc_frame(const char *direction, const unsigned char *buf, ssize_t n) {
    if (n < 8) {
        fprintf(stdout, "%s IPC bytes: %zd (partial frame)\n", direction, n);
        fflush(stdout);
        return;
    }

    uint32_t opcode = read_le32(buf);
    uint32_t declared_len = read_le32(buf + 4);
    size_t available = (size_t)n > 8 ? (size_t)n - 8 : 0;
    size_t payload_len = declared_len < available ? declared_len : available;
    const unsigned char *payload = buf + 8;

    char client_id[80];
    char command[80];
    extract_json_string(payload, payload_len, "client_id", client_id, sizeof(client_id));
    extract_json_string(payload, payload_len, "cmd", command, sizeof(command));

    fprintf(stdout, "%s IPC frame: opcode=%u length=%u",
            direction, opcode, declared_len);
    if (client_id[0]) fprintf(stdout, " client_id=%s", client_id);
    if (command[0]) fprintf(stdout, " cmd=%s", command);
    if ((uint32_t)payload_len < declared_len) fprintf(stdout, " payload=partial");
    fprintf(stdout, "\n");
    fflush(stdout);
}

static int bridge_loop(int a, int b) {
    unsigned char buf[16384];
    struct pollfd fds[2];
    bool logged_client_frame = false;
    bool logged_discord_frame = false;
    fds[0].fd = a;
    fds[0].events = POLLIN;
    fds[1].fd = b;
    fds[1].events = POLLIN;

    for (;;) {
        int pr = poll(fds, 2, -1);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return -1;

        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & POLLIN) {
                int from = fds[i].fd;
                int to = fds[i == 0 ? 1 : 0].fd;
                ssize_t n = read(from, buf, sizeof(buf));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return 0;
                if (from == a && !logged_client_frame) {
                    log_ipc_frame("Client -> Discord", buf, n);
                    logged_client_frame = true;
                } else if (from == b && !logged_discord_frame) {
                    log_ipc_frame("Discord -> Client", buf, n);
                    logged_discord_frame = true;
                }
                if (write_all(to, buf, (size_t)n) < 0) return -1;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) return 0;
        }
    }
}

static int read_preamble(int fd, char *out, size_t out_len) {
    size_t pos = 0;
    while (pos + 1 < out_len) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        out[pos++] = c;
        if (c == '\n') break;
    }
    out[pos] = '\0';
    chomp(out);
    return 0;
}

static void *client_thread(void *arg) {
    ClientContext *ctx = (ClientContext *)arg;
    int client_fd = ctx->client_fd;
    char expected[(WINECORD_TOKEN_BYTES * 2) + 1];
    snprintf(expected, sizeof(expected), "%s", ctx->token);
    free(ctx);

    char preamble[256];
    if (read_preamble(client_fd, preamble, sizeof(preamble)) != 0) {
        close(client_fd);
        return NULL;
    }

    const char prefix[] = "WINECORD/1 ";
    if (strncmp(preamble, prefix, strlen(prefix)) != 0 ||
        strcmp(preamble + strlen(prefix), expected) != 0) {
        fprintf(stderr, "Rejected bridge client with invalid token\n");
        close(client_fd);
        return NULL;
    }

    char discord_path[PATH_MAX];
    int discord_fd = connect_discord(discord_path, sizeof(discord_path));
    if (discord_fd < 0) {
        fprintf(stderr, "Could not connect to Discord IPC socket. Is Discord open?\n");
        close(client_fd);
        return NULL;
    }

    fprintf(stdout, "Bridge client connected -> %s\n", discord_path);
    fflush(stdout);
    bridge_loop(client_fd, discord_fd);
    close(discord_fd);
    close(client_fd);
    fprintf(stdout, "Bridge client disconnected\n");
    fflush(stdout);
    return NULL;
}

static int run_agent(const Config *cfg) {
    signal(SIGPIPE, SIG_IGN);
    if (mkdir_p(cfg->log_dir, 0700) != 0) {
        fprintf(stderr, "Could not create log dir %s: %s\n", cfg->log_dir, strerror(errno));
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "socket(AF_INET): %s\n", strerror(errno));
        return 1;
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg->port);
    if (inet_pton(AF_INET, cfg->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid listen host: %s\n", cfg->host);
        close(listen_fd);
        return 1;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind(%s:%d): %s\n", cfg->host, cfg->port, strerror(errno));
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 32) != 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        close(listen_fd);
        return 1;
    }

    fprintf(stdout, "WineCord agent listening on %s:%d\n", cfg->host, cfg->port);
    fflush(stdout);

    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0 && errno == EINTR) continue;
        if (client_fd < 0) {
            fprintf(stderr, "accept: %s\n", strerror(errno));
            continue;
        }

        ClientContext *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->client_fd = client_fd;
        snprintf(ctx->token, sizeof(ctx->token), "%s", cfg->token);

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, ctx) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            close(client_fd);
            free(ctx);
            continue;
        }
        pthread_detach(tid);
    }
}

static int command_capture(const char *cmd, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';

    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (!fgets(out, (int)out_len, p)) {
        pclose(p);
        return -1;
    }
    int status = pclose(p);
    chomp(out);
    return status == 0 && out[0] != '\0' ? 0 : -1;
}

static void print_discord_sockets(void) {
    char paths[MAX_CANDIDATES][PATH_MAX];
    int count = collect_discord_sockets(paths, MAX_CANDIDATES);
    if (count == 0) {
        printf("Discord IPC: not found\n");
        return;
    }
    printf("Discord IPC candidates:\n");
    for (int i = 0; i < count; i++) {
        int fd = connect_unix_socket(paths[i]);
        if (fd >= 0) {
            close(fd);
            printf("  %s (connectable)\n", paths[i]);
        } else {
            printf("  %s (stale or not accepting)\n", paths[i]);
        }
    }
}

static void discover_steam_bottle(char *out, size_t out_len) {
    out[0] = '\0';

    const char *env = getenv("WINECORD_BOTTLE");
    if (env && *env && is_directory(env)) {
        snprintf(out, out_len, "%s", env);
        return;
    }

    char base[PATH_MAX];
    const char *commands[] = {
        "defaults read com.codeweavers.CrossOver BottleDir 2>/dev/null",
        "defaults read com.codeweavers.CrossOverGames BottleDir 2>/dev/null",
        NULL
    };

    for (int i = 0; commands[i]; i++) {
        if (command_capture(commands[i], base, sizeof(base)) == 0) {
            char candidate[PATH_MAX];
            snprintf(candidate, sizeof(candidate), "%s/Steam", base);
            if (is_directory(candidate)) {
                snprintf(out, out_len, "%s", candidate);
                return;
            }
        }
    }

    snprintf(base, sizeof(base), "%s/Library/Application Support/CrossOver/Bottles/Steam", home_dir());
    if (is_directory(base)) snprintf(out, out_len, "%s", base);
}

static int doctor(const Config *cfg) {
    char token[80];
    token_redacted(cfg, token, sizeof(token));
    printf("WineCord %s\n", WINECORD_VERSION);
    printf("Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n");
    printf("\nConfig:\n");
    printf("  path:  %s\n", cfg->config_path);
    printf("  agent: %s:%d\n", cfg->host, cfg->port);
    printf("  token: %s\n", token);
    printf("\n");
    print_discord_sockets();

    char bottle[PATH_MAX];
    discover_steam_bottle(bottle, sizeof(bottle));
    printf("\nCrossOver:\n");
    printf("  app:    %s\n", is_directory("/Applications/CrossOver.app") ? "/Applications/CrossOver.app" : "not found");
    printf("  bottle: %s\n", bottle[0] ? bottle : "not found; pass --bottle PATH");

    if (bottle[0]) {
        char helper[PATH_MAX];
        snprintf(helper, sizeof(helper), "%s/drive_c/windows/winecord-bridge.exe", bottle);
        printf("  helper: %s\n", path_exists(helper) ? helper : "not installed");
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "launchctl print gui/%d/%s 2>/dev/null", getuid(), WINECORD_LABEL);
    FILE *p = popen(cmd, "r");
    char state[64] = "not loaded";
    char program[PATH_MAX] = "not loaded";
    if (p) {
        char line[1024];
        while (fgets(line, sizeof(line), p)) {
            char *t = trim(line);
            const char state_prefix[] = "state = ";
            const char program_prefix[] = "program = ";
            if (strncmp(t, state_prefix, strlen(state_prefix)) == 0) {
                snprintf(state, sizeof(state), "%s", t + strlen(state_prefix));
            } else if (strncmp(t, program_prefix, strlen(program_prefix)) == 0) {
                snprintf(program, sizeof(program), "%s", t + strlen(program_prefix));
            }
        }
        pclose(p);
    }

    char expected[PATH_MAX];
    launch_executable(expected, sizeof(expected));
    printf("\nLaunchAgent:\n");
    printf("  state:    %s\n", state);
    printf("  program:  %s\n", program);
    printf("  expected: %s\n", expected);
    if (strcmp(program, "not loaded") != 0 && strcmp(program, expected) != 0) {
        printf("  warning: LaunchAgent is still using an older WineCord path; run `winecord setup` to reload it.\n");
    }
    return 0;
}

static int shell_quote(const char *in, char *out, size_t out_len) {
    size_t pos = 0;
    if (pos + 1 >= out_len) return -1;
    out[pos++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            const char repl[] = "'\\''";
            size_t n = strlen(repl);
            if (pos + n >= out_len) return -1;
            memcpy(out + pos, repl, n);
            pos += n;
        } else {
            if (pos + 1 >= out_len) return -1;
            out[pos++] = *p;
        }
    }
    if (pos + 2 > out_len) return -1;
    out[pos++] = '\'';
    out[pos] = '\0';
    return 0;
}

static int show_logs(const Config *cfg, int argc, char **argv) {
    const char *bottle = NULL;
    bool follow = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--bottle") == 0 && i + 1 < argc) {
            bottle = argv[++i];
        } else if (strcmp(argv[i], "--follow") == 0 || strcmp(argv[i], "-f") == 0) {
            follow = true;
        } else {
            fprintf(stderr, "Unknown logs option: %s\n", argv[i]);
            return 1;
        }
    }

    char discovered[PATH_MAX];
    if (!bottle) {
        discover_steam_bottle(discovered, sizeof(discovered));
        bottle = discovered[0] ? discovered : NULL;
    }

    char agent_log[PATH_MAX];
    char bridge_log[PATH_MAX];
    snprintf(agent_log, sizeof(agent_log), "%s/agent.log", cfg->log_dir);
    if (bottle) {
        snprintf(bridge_log, sizeof(bridge_log), "%s/drive_c/users/Public/WineCord/bridge.log", bottle);
    } else {
        bridge_log[0] = '\0';
    }

    printf("Agent log:  %s\n", agent_log);
    if (bridge_log[0]) printf("Bridge log: %s\n", bridge_log);
    printf("\n");
    fflush(stdout);

    char q_agent[PATH_MAX + 8];
    char q_bridge[PATH_MAX + 8];
    if (shell_quote(agent_log, q_agent, sizeof(q_agent)) != 0) return 1;

    char cmd[(PATH_MAX * 3) + 128];
    const char *mode = follow ? "-f" : "";
    if (bridge_log[0] && path_exists(bridge_log) && shell_quote(bridge_log, q_bridge, sizeof(q_bridge)) == 0) {
        snprintf(cmd, sizeof(cmd), "tail -n 80 %s %s %s", mode, q_agent, q_bridge);
    } else {
        snprintf(cmd, sizeof(cmd), "tail -n 80 %s %s", mode, q_agent);
    }
    int rc = system(cmd);
    return rc == 0 ? 0 : 1;
}

static int install_agent(const Config *cfg) {
    char exe[PATH_MAX];
    launch_executable(exe, sizeof(exe));

    char agents_dir[PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/Library/LaunchAgents", home_dir());
    if (mkdir_p(agents_dir, 0700) != 0) {
        fprintf(stderr, "Could not create %s: %s\n", agents_dir, strerror(errno));
        return 1;
    }
    mkdir_p(cfg->log_dir, 0700);

    char plist[PATH_MAX];
    snprintf(plist, sizeof(plist), "%s/%s.plist", agents_dir, WINECORD_LABEL);

    FILE *f = fopen(plist, "w");
    if (!f) {
        fprintf(stderr, "Could not write %s: %s\n", plist, strerror(errno));
        return 1;
    }
    fprintf(f,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "  <key>Label</key><string>%s</string>\n"
            "  <key>ProgramArguments</key>\n"
            "  <array>\n"
            "    <string>%s</string>\n"
            "    <string>agent</string>\n"
            "  </array>\n"
            "  <key>RunAtLoad</key><true/>\n"
            "  <key>KeepAlive</key><false/>\n"
            "  <key>StandardOutPath</key><string>%s/agent.log</string>\n"
            "  <key>StandardErrorPath</key><string>%s/agent.err.log</string>\n"
            "</dict>\n"
            "</plist>\n",
            WINECORD_LABEL, exe, cfg->log_dir, cfg->log_dir);
    fclose(f);

    char qplist[PATH_MAX + 8];
    char cmd[(PATH_MAX * 2) + 128];
    shell_quote(plist, qplist, sizeof(qplist));
    snprintf(cmd, sizeof(cmd), "launchctl bootout gui/%d/%s 2>/dev/null || true", getuid(), WINECORD_LABEL);
    system(cmd);
    snprintf(cmd, sizeof(cmd), "launchctl bootstrap gui/%d %s", getuid(), qplist);
    if (system(cmd) != 0) {
        fprintf(stderr, "Could not load LaunchAgent with launchctl.\n");
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "launchctl kickstart -k gui/%d/%s 2>/dev/null || true", getuid(), WINECORD_LABEL);
    system(cmd);

    printf("Installed LaunchAgent: %s\n", plist);
    return 0;
}

static int uninstall_agent(void) {
    char plist[PATH_MAX];
    snprintf(plist, sizeof(plist), "%s/Library/LaunchAgents/%s.plist", home_dir(), WINECORD_LABEL);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "launchctl bootout gui/%d/%s 2>/dev/null || true", getuid(), WINECORD_LABEL);
    system(cmd);

    if (unlink(plist) != 0 && errno != ENOENT) {
        fprintf(stderr, "Could not remove %s: %s\n", plist, strerror(errno));
        return 1;
    }
    printf("Removed LaunchAgent: %s\n", plist);
    return 0;
}

static int start_agent(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "launchctl kickstart -k gui/%d/%s", getuid(), WINECORD_LABEL);
    int rc = system(cmd);
    return rc == 0 ? 0 : 1;
}

static int stop_agent(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "launchctl bootout gui/%d/%s 2>/dev/null || true", getuid(), WINECORD_LABEL);
    system(cmd);
    return 0;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        fprintf(stderr, "Could not open %s: %s\n", src, strerror(errno));
        return -1;
    }

    char parent[PATH_MAX];
    if (parent_dir(dst, parent, sizeof(parent)) != 0 || mkdir_p(parent, 0755) != 0) {
        fprintf(stderr, "Could not create %s: %s\n", parent, strerror(errno));
        close(in);
        return -1;
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) {
        fprintf(stderr, "Could not write %s: %s\n", dst, strerror(errno));
        close(in);
        return -1;
    }

    char buf[65536];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            fprintf(stderr, "Read failed from %s: %s\n", src, strerror(errno));
            close(in);
            close(out);
            return -1;
        }
        if (n == 0) break;
        if (write_all(out, buf, (size_t)n) < 0) {
            fprintf(stderr, "Write failed to %s: %s\n", dst, strerror(errno));
            close(in);
            close(out);
            return -1;
        }
    }

    close(in);
    close(out);
    chmod(dst, mode);
    return 0;
}

static bool find_helper(char *out, size_t out_len) {
    const char *env = getenv("WINECORD_BRIDGE_EXE");
    if (env && *env && path_exists(env)) {
        snprintf(out, out_len, "%s", env);
        return true;
    }

    const char *candidates[] = {
        "build/winecord-bridge.exe",
        "dist/winecord-bridge.exe",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (path_exists(candidates[i])) {
            char resolved[PATH_MAX];
            if (realpath(candidates[i], resolved)) snprintf(out, out_len, "%s", resolved);
            else snprintf(out, out_len, "%s", candidates[i]);
            return true;
        }
    }

    char exe[PATH_MAX];
    if (current_executable(exe, sizeof(exe)) == 0) {
        char dir[PATH_MAX];
        parent_dir(exe, dir, sizeof(dir));
        const char *relative[] = {
            "%s/winecord-bridge.exe",
            "%s/../libexec/winecord/winecord-bridge.exe",
            "%s/../share/winecord/winecord-bridge.exe",
            NULL
        };
        for (int i = 0; relative[i]; i++) {
            char candidate[PATH_MAX];
            if (snprintf(candidate, sizeof(candidate), relative[i], dir) >= (int)sizeof(candidate)) continue;
            char resolved[PATH_MAX];
            const char *check = realpath(candidate, resolved) ? resolved : candidate;
            if (path_exists(check)) {
                snprintf(out, out_len, "%s", check);
                return true;
            }
        }

        const char marker[] = "/Cellar/winecord/";
        char *cellar = strstr(exe, marker);
        if (cellar) {
            char prefix[PATH_MAX];
            size_t prefix_len = (size_t)(cellar - exe);
            if (prefix_len > 0 && prefix_len < sizeof(prefix)) {
                memcpy(prefix, exe, prefix_len);
                prefix[prefix_len] = '\0';
                char candidate[PATH_MAX];
                snprintf(candidate, sizeof(candidate), "%s/opt/winecord/libexec/winecord/winecord-bridge.exe", prefix);
                if (path_exists(candidate)) {
                    snprintf(out, out_len, "%s", candidate);
                    return true;
                }
            }
        }
    }
    return false;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int run_crossover_helper(const char *bottle, const char *helper_path, const char *helper_arg, bool quiet) {
    const char *wine = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine";
    if (!path_exists(wine)) {
        if (!quiet) printf("CrossOver wine wrapper not found: %s\n", wine);
        return quiet ? 0 : 1;
    }

    const char *bottle_name = base_name(bottle);
    char bottle_parent[PATH_MAX];
    parent_dir(bottle, bottle_parent, sizeof(bottle_parent));

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        setenv("CX_BOTTLE_PATH", bottle_parent, 1);
        execl(wine, "wine", "--bottle", bottle_name, "--no-gui", helper_path, helper_arg, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!quiet) {
            fprintf(stderr, "CrossOver command failed through bottle %s (status %d).\n", bottle_name, status);
            fprintf(stderr, "Manual command:\n  CX_BOTTLE_PATH=\"%s\" \"%s\" --bottle \"%s\" --no-gui \"%s\" %s\n",
                    bottle_parent, wine, bottle_name, helper_path, helper_arg);
        }
        return 1;
    }
    return 0;
}

static int write_bottle_config(const Config *cfg, const char *bottle) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/drive_c/users/Public/WineCord", bottle);
    if (mkdir_p(dir, 0755) != 0) {
        fprintf(stderr, "Could not create %s: %s\n", dir, strerror(errno));
        return -1;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/config.ini", dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Could not write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "host=%s\nport=%d\ntoken=%s\n", cfg->host, cfg->port, cfg->token);
    fclose(f);
    chmod(path, 0644);
    printf("Wrote bottle config: %s\n", path);
    return 0;
}

static int register_windows_service(const char *bottle, const char *helper_dest) {
    if (run_crossover_helper(bottle, helper_dest, "--install", false) != 0) return 1;
    printf("Registered WineCordBridge service in bottle: %s\n", base_name(bottle));
    return 0;
}

static int install_bottle(const Config *cfg, int argc, char **argv) {
    const char *bottle = NULL;
    const char *helper = NULL;
    bool no_register = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--bottle") == 0 && i + 1 < argc) {
            bottle = argv[++i];
        } else if (strcmp(argv[i], "--helper") == 0 && i + 1 < argc) {
            helper = argv[++i];
        } else if (strcmp(argv[i], "--no-register") == 0) {
            no_register = true;
        } else {
            fprintf(stderr, "Unknown install-bottle option: %s\n", argv[i]);
            return 1;
        }
    }

    char discovered[PATH_MAX];
    if (!bottle) {
        discover_steam_bottle(discovered, sizeof(discovered));
        bottle = discovered[0] ? discovered : NULL;
    }
    if (!bottle || !is_directory(bottle)) {
        fprintf(stderr, "Bottle not found. Pass --bottle /path/to/Bottles/Steam\n");
        return 1;
    }

    char drive_c[PATH_MAX];
    snprintf(drive_c, sizeof(drive_c), "%s/drive_c", bottle);
    if (!is_directory(drive_c)) {
        fprintf(stderr, "This does not look like a Wine/CrossOver bottle: %s\n", bottle);
        return 1;
    }

    char helper_found[PATH_MAX];
    if (!helper) {
        if (!find_helper(helper_found, sizeof(helper_found))) {
            fprintf(stderr, "winecord-bridge.exe not found. Build it with `make windows-helper` or pass --helper PATH.\n");
            return 1;
        }
        helper = helper_found;
    }
    if (!path_exists(helper)) {
        fprintf(stderr, "Helper not found: %s\n", helper);
        return 1;
    }

    if (write_bottle_config(cfg, bottle) != 0) return 1;

    char helper_dest[PATH_MAX];
    snprintf(helper_dest, sizeof(helper_dest), "%s/drive_c/windows/winecord-bridge.exe", bottle);
    if (copy_file(helper, helper_dest, 0755) != 0) return 1;
    printf("Installed helper: %s\n", helper_dest);

    if (!no_register) return register_windows_service(bottle, helper_dest);
    printf("Skipped service registration (--no-register).\n");
    return 0;
}

static void remove_file_if_exists(const char *path) {
    if (unlink(path) == 0) {
        printf("Removed: %s\n", path);
    } else if (errno != ENOENT) {
        fprintf(stderr, "Could not remove %s: %s\n", path, strerror(errno));
    }
}

static int remove_bottle_setup(const char *bottle) {
    char discovered[PATH_MAX];
    if (!bottle) {
        discover_steam_bottle(discovered, sizeof(discovered));
        bottle = discovered[0] ? discovered : NULL;
    }
    if (!bottle || !is_directory(bottle)) {
        printf("No CrossOver bottle found; skipped bottle cleanup.\n");
        return 0;
    }

    char helper_dest[PATH_MAX];
    snprintf(helper_dest, sizeof(helper_dest), "%s/drive_c/windows/winecord-bridge.exe", bottle);
    if (path_exists(helper_dest)) {
        run_crossover_helper(bottle, helper_dest, "--remove", true);
        remove_file_if_exists(helper_dest);
    }

    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/drive_c/users/Public/WineCord/config.ini", bottle);
    remove_file_if_exists(file);
    snprintf(file, sizeof(file), "%s/drive_c/users/Public/WineCord/bridge.log", bottle);
    remove_file_if_exists(file);

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/drive_c/users/Public/WineCord", bottle);
    if (rmdir(dir) == 0) printf("Removed: %s\n", dir);

    printf("Cleaned bottle: %s\n", bottle);
    return 0;
}

static int setup_all(const Config *cfg, int argc, char **argv) {
    printf("Setting up WineCord...\n");
    fflush(stdout);
    if (install_agent(cfg) != 0) return 1;
    if (install_bottle(cfg, argc, argv) != 0) return 1;
    printf("\nDone. Keep Discord for macOS open, then launch the Windows game from CrossOver.\n");
    return 0;
}

static int uninstall_all(const Config *cfg, int argc, char **argv) {
    const char *bottle = NULL;
    bool keep_config = false;
    bool keep_logs = false;
    bool skip_bottle = false;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--bottle") == 0 && i + 1 < argc) {
            bottle = argv[++i];
        } else if (strcmp(argv[i], "--keep-config") == 0) {
            keep_config = true;
        } else if (strcmp(argv[i], "--keep-logs") == 0) {
            keep_logs = true;
        } else if (strcmp(argv[i], "--no-bottle") == 0) {
            skip_bottle = true;
        } else {
            fprintf(stderr, "Unknown uninstall option: %s\n", argv[i]);
            return 1;
        }
    }

    printf("Uninstalling WineCord setup...\n");
    fflush(stdout);
    if (!skip_bottle) remove_bottle_setup(bottle);
    uninstall_agent();

    if (!keep_config) {
        remove_file_if_exists(cfg->config_path);
        if (rmdir(cfg->app_dir) == 0) printf("Removed: %s\n", cfg->app_dir);
    }

    if (!keep_logs) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/agent.log", cfg->log_dir);
        remove_file_if_exists(path);
        snprintf(path, sizeof(path), "%s/agent.err.log", cfg->log_dir);
        remove_file_if_exists(path);
        if (rmdir(cfg->log_dir) == 0) printf("Removed: %s\n", cfg->log_dir);
    }

    printf("\nWineCord setup removed. You can now run `brew uninstall winecord` to remove the package.\n");
    return 0;
}

static void usage(FILE *out) {
    fprintf(out,
            "WineCord %s\n"
            "Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n\n"
            "Usage:\n"
            "  winecord setup [--bottle PATH] [--helper PATH] [--no-register]\n"
            "  winecord uninstall [--bottle PATH] [--keep-config] [--keep-logs]\n"
            "  winecord agent\n"
            "  winecord doctor\n"
            "  winecord logs [--follow] [--bottle PATH]\n"
            "  winecord install-agent\n"
            "  winecord uninstall-agent\n"
            "  winecord start\n"
            "  winecord stop\n"
            "  winecord install-bottle [--bottle PATH] [--helper PATH] [--no-register]\n"
            "  winecord --version\n",
            WINECORD_VERSION);
}

int main(int argc, char **argv) {
    Config cfg;
    if (load_config(&cfg) != 0) return 1;

    if (argc < 2) {
        usage(stdout);
        return 0;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("WineCord %s\n", WINECORD_VERSION);
        printf("Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n");
        return 0;
    }
    if (strcmp(cmd, "setup") == 0) return setup_all(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "uninstall") == 0) return uninstall_all(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "agent") == 0) return run_agent(&cfg);
    if (strcmp(cmd, "doctor") == 0) return doctor(&cfg);
    if (strcmp(cmd, "logs") == 0) return show_logs(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "install-agent") == 0) return install_agent(&cfg);
    if (strcmp(cmd, "uninstall-agent") == 0) return uninstall_agent();
    if (strcmp(cmd, "start") == 0) return start_agent();
    if (strcmp(cmd, "stop") == 0) return stop_agent();
    if (strcmp(cmd, "install-bottle") == 0) return install_bottle(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    usage(stderr);
    return 1;
}
