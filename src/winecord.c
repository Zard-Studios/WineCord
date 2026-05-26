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
#include <stdarg.h>
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

#define WINECORD_VERSION "0.1.11"
#define WINECORD_LABEL "com.zardstudios.winecord.agent"
#define WINECORD_DEFAULT_PORT 38477
#define WINECORD_PIPE_COUNT 10
#define WINECORD_TOKEN_BYTES 32
#define MAX_CANDIDATES 64
#define MAX_TRACKED_BOTTLES 16
#define MAX_STEAM_ACTIVITY 32
#define WINECORD_UPDATE_CHECK_INTERVAL 86400
#define WINECORD_FORMULA_URL "https://raw.githubusercontent.com/Zard-Studios/homebrew-tap/main/Formula/winecord.rb"

typedef struct {
    char app_dir[PATH_MAX];
    char log_dir[PATH_MAX];
    char config_path[PATH_MAX];
    char host[64];
    int port;
    char token[(WINECORD_TOKEN_BYTES * 2) + 1];
    char wine_path[PATH_MAX];
    char bottle_paths[MAX_TRACKED_BOTTLES][PATH_MAX];
    int bottle_count;
} Config;

typedef struct {
    int client_fd;
    char token[(WINECORD_TOKEN_BYTES * 2) + 1];
    char state_path[PATH_MAX];
} ClientContext;

typedef struct {
    char client_id[80];
    long pid;
    bool saw_set_activity;
    unsigned char pending[32768];
    size_t pending_len;
} IpcSession;

typedef struct {
    char appid[32];
    int process_count;
} SteamActivityCounter;

static void usage(FILE *out);
static bool find_whisky_runner(const char *prefix, char *whisky_path, size_t whisky_len,
                               char *bottle_name, size_t name_len);
static bool find_wine_runner(const Config *cfg, const char *prefix,
                             const char *override_wine, char *out, size_t out_len);

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

static bool color_enabled(FILE *stream) {
    const char *no_color = getenv("NO_COLOR");
    const char *wc_no_color = getenv("WINECORD_NO_COLOR");
    const char *term = getenv("TERM");
    if ((no_color && *no_color) || (wc_no_color && *wc_no_color)) return false;
    if (term && strcmp(term, "dumb") == 0) return false;
    return stream && isatty(fileno(stream));
}

static void styled_printf(FILE *stream, const char *style, const char *fmt, ...) {
    va_list args;
    if (style && color_enabled(stream)) fputs(style, stream);
    va_start(args, fmt);
    vfprintf(stream, fmt, args);
    va_end(args);
    if (style && color_enabled(stream)) fputs("\033[0m", stream);
}

static void print_header(const char *text) {
    styled_printf(stdout, "\033[1;36m", "\n==> %s\n", text);
}

static void print_success(const char *fmt, ...) {
    if (color_enabled(stdout)) fputs("\033[32m", stdout);
    fputs("✓ ", stdout);
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    if (color_enabled(stdout)) fputs("\033[0m", stdout);
}

static void print_note(const char *fmt, ...) {
    if (color_enabled(stdout)) fputs("\033[2m", stdout);
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    if (color_enabled(stdout)) fputs("\033[0m", stdout);
}

static void print_warning_stdout(const char *fmt, ...) {
    if (color_enabled(stdout)) fputs("\033[33m", stdout);
    fputs("Warning: ", stdout);
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    if (color_enabled(stdout)) fputs("\033[0m", stdout);
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

static void normalize_path_copy(char *out, size_t out_len, const char *path) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!path || !*path) return;

    const char *src = path;
    char expanded[PATH_MAX];
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        if (snprintf(expanded, sizeof(expanded), "%s%s", home_dir(), path + 1) < (int)sizeof(expanded)) {
            src = expanded;
        }
    }

    if (snprintf(out, out_len, "%s", src) >= (int)out_len) {
        out[out_len - 1] = '\0';
    }
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
}

static void remember_bottle(Config *cfg, const char *path) {
    if (!cfg || !path || !*path) return;

    char normalized[PATH_MAX];
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) normalize_path_copy(normalized, sizeof(normalized), resolved);
    else normalize_path_copy(normalized, sizeof(normalized), path);
    if (!normalized[0]) return;

    for (int i = 0; i < cfg->bottle_count; i++) {
        if (strcmp(cfg->bottle_paths[i], normalized) == 0) return;
    }
    if (cfg->bottle_count >= MAX_TRACKED_BOTTLES) return;
    snprintf(cfg->bottle_paths[cfg->bottle_count++], PATH_MAX, "%s", normalized);
}

static bool add_path_target(char paths[][PATH_MAX], int *count, const char *path) {
    if (!paths || !count || !path || !*path || *count >= MAX_TRACKED_BOTTLES) return false;

    char normalized[PATH_MAX];
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) normalize_path_copy(normalized, sizeof(normalized), resolved);
    else normalize_path_copy(normalized, sizeof(normalized), path);
    if (!normalized[0]) return false;

    for (int i = 0; i < *count; i++) {
        if (strcmp(paths[i], normalized) == 0) return false;
    }
    snprintf(paths[*count], PATH_MAX, "%s", normalized);
    (*count)++;
    return true;
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
    if (cfg->wine_path[0]) fprintf(f, "wine=%s\n", cfg->wine_path);
    for (int i = 0; i < cfg->bottle_count; i++) {
        fprintf(f, "prefix=%s\n", cfg->bottle_paths[i]);
    }
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
        } else if (strcmp(key, "wine") == 0 && *value) {
            normalize_path_copy(cfg->wine_path, sizeof(cfg->wine_path), value);
        } else if ((strcmp(key, "prefix") == 0 || strcmp(key, "bottle") == 0) && *value) {
            remember_bottle(cfg, value);
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

static void state_path_for_config(const Config *cfg, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/state.ini", cfg->app_dir);
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

static void write_le32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xff);
    p[1] = (unsigned char)((value >> 8) & 0xff);
    p[2] = (unsigned char)((value >> 16) & 0xff);
    p[3] = (unsigned char)((value >> 24) & 0xff);
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
        while (j < payload_len && isspace((unsigned char)payload[j])) j++;
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

static bool extract_json_long(const unsigned char *payload, size_t payload_len,
                              const char *key, long *out) {
    if (!payload || !key || !out) return false;

    char needle[96];
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) return false;
    size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len < payload_len; i++) {
        if (memcmp(payload + i, needle, needle_len) != 0) continue;

        size_t j = i + needle_len;
        while (j < payload_len && payload[j] != ':') j++;
        if (j >= payload_len) return false;
        j++;
        while (j < payload_len && isspace((unsigned char)payload[j])) j++;
        if (j >= payload_len) return false;

        char num[32];
        size_t pos = 0;
        if (payload[j] == '-') num[pos++] = (char)payload[j++];
        while (j < payload_len && isdigit((unsigned char)payload[j]) && pos + 1 < sizeof(num)) {
            num[pos++] = (char)payload[j++];
        }
        if (pos == 0 || (pos == 1 && num[0] == '-')) return false;
        num[pos] = '\0';
        *out = strtol(num, NULL, 10);
        return true;
    }
    return false;
}

static int write_ipc_frame(int fd, uint32_t opcode, const char *json) {
    size_t len = strlen(json);
    if (len > UINT32_MAX) return -1;

    unsigned char header[8];
    write_le32(header, opcode);
    write_le32(header + 4, (uint32_t)len);
    if (write_all(fd, header, sizeof(header)) < 0) return -1;
    return write_all(fd, json, len) < 0 ? -1 : 0;
}

static int read_full_timeout(int fd, unsigned char *buf, size_t len, int timeout_ms) {
    size_t total = 0;
    while (total < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return -1;
        ssize_t n = read(fd, buf + total, len - total);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int read_ipc_frame_timeout(int fd, uint32_t *opcode, char *payload,
                                  size_t payload_len, int timeout_ms) {
    unsigned char header[8];
    if (read_full_timeout(fd, header, sizeof(header), timeout_ms) != 0) return -1;
    uint32_t op = read_le32(header);
    uint32_t len = read_le32(header + 4);

    size_t keep = len;
    if (payload && payload_len > 0 && keep >= payload_len) keep = payload_len - 1;
    if (keep > 0 && read_full_timeout(fd, (unsigned char *)payload, keep, timeout_ms) != 0) return -1;
    if (payload && payload_len > 0) payload[keep] = '\0';

    unsigned char discard[512];
    uint32_t remaining = len > keep ? len - (uint32_t)keep : 0;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(discard) ? remaining : sizeof(discard);
        if (read_full_timeout(fd, discard, chunk, timeout_ms) != 0) return -1;
        remaining -= (uint32_t)chunk;
    }

    if (opcode) *opcode = op;
    return 0;
}

static void save_activity_state(const char *state_path, const IpcSession *session) {
    if (!state_path || !*state_path || !session || !session->client_id[0]) return;

    char parent[PATH_MAX];
    if (parent_dir(state_path, parent, sizeof(parent)) != 0 || mkdir_p(parent, 0700) != 0) return;

    FILE *f = fopen(state_path, "w");
    if (!f) return;
    fprintf(f, "client_id=%s\n", session->client_id);
    fprintf(f, "pid=%ld\n", session->pid > 0 ? session->pid : (long)getpid());
    fprintf(f, "updated=%ld\n", (long)time(NULL));
    fclose(f);
    chmod(state_path, 0600);
}

static void inspect_client_ipc_frame(uint32_t opcode, const unsigned char *payload,
                                     size_t payload_len, IpcSession *session,
                                     const char *state_path) {
    if (opcode == 0) {
        char client_id[sizeof(session->client_id)];
        extract_json_string(payload, payload_len, "client_id", client_id, sizeof(client_id));
        if (client_id[0]) snprintf(session->client_id, sizeof(session->client_id), "%s", client_id);
    } else if (opcode == 1) {
        char command[80];
        extract_json_string(payload, payload_len, "cmd", command, sizeof(command));
        if (strcmp(command, "SET_ACTIVITY") == 0) {
            long pid = 0;
            if (extract_json_long(payload, payload_len, "pid", &pid) && pid > 0) {
                session->pid = pid;
            }
            session->saw_set_activity = true;
            save_activity_state(state_path, session);
        }
    }
}

static void inspect_client_ipc_frames(const unsigned char *buf, ssize_t n,
                                      IpcSession *session, const char *state_path) {
    if (!buf || n <= 0 || !session) return;

    size_t incoming = (size_t)n;
    if (incoming > sizeof(session->pending) ||
        session->pending_len > sizeof(session->pending) - incoming) {
        session->pending_len = 0;
        if (incoming > sizeof(session->pending)) return;
    }
    memcpy(session->pending + session->pending_len, buf, incoming);
    session->pending_len += incoming;

    size_t pos = 0;
    while (pos + 8 <= session->pending_len) {
        uint32_t opcode = read_le32(session->pending + pos);
        uint32_t declared_len = read_le32(session->pending + pos + 4);
        if (declared_len > sizeof(session->pending) - 8) {
            session->pending_len = 0;
            return;
        }

        size_t frame_len = 8 + (size_t)declared_len;
        if (frame_len > session->pending_len - pos) break;
        inspect_client_ipc_frame(opcode, session->pending + pos + 8,
                                 declared_len, session, state_path);
        pos += frame_len;
    }

    if (pos > 0) {
        session->pending_len -= pos;
        memmove(session->pending, session->pending + pos, session->pending_len);
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

static int send_clear_activity(int discord_fd, const IpcSession *session) {
    if (!session || !session->client_id[0] || !session->saw_set_activity) return 0;

    long pid = session->pid > 0 ? session->pid : (long)getpid();
    char json[512];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":null},"
             "\"nonce\":\"winecord-clear-%ld\"}",
             pid, (long)time(NULL));

    if (write_ipc_frame(discord_fd, 1, json) != 0) {
        fprintf(stderr, "Could not clear Discord activity for client_id=%s\n", session->client_id);
        return -1;
    }
    fprintf(stdout, "Cleared Discord activity for client_id=%s pid=%ld\n", session->client_id, pid);
    fflush(stdout);
    usleep(100000);
    return 0;
}

static int bridge_loop(int a, int b, IpcSession *session, const char *state_path) {
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
                if (from == a) inspect_client_ipc_frames(buf, n, session, state_path);
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
    char state_path[PATH_MAX];
    snprintf(state_path, sizeof(state_path), "%s", ctx->state_path);
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
    IpcSession session;
    memset(&session, 0, sizeof(session));
    bridge_loop(client_fd, discord_fd, &session, state_path);
    send_clear_activity(discord_fd, &session);
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
        state_path_for_config(cfg, ctx->state_path, sizeof(ctx->state_path));

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

static bool is_wine_prefix(const char *path) {
    if (!path || !is_directory(path)) return false;
    char drive_c[PATH_MAX];
    snprintf(drive_c, sizeof(drive_c), "%s/drive_c", path);
    return is_directory(drive_c);
}

static bool path_has_component(const char *path, const char *needle) {
    return path && needle && strstr(path, needle) != NULL;
}

static void add_prefix_candidate(char paths[][PATH_MAX], int *count, const char *path) {
    if (!path || !*path || !paths || !count || *count >= MAX_TRACKED_BOTTLES) return;

    char normalized[PATH_MAX];
    char resolved[PATH_MAX];
    char clean[PATH_MAX];
    normalize_path_copy(clean, sizeof(clean), path);
    if (realpath(clean, resolved)) normalize_path_copy(normalized, sizeof(normalized), resolved);
    else normalize_path_copy(normalized, sizeof(normalized), clean);
    if (!is_wine_prefix(normalized)) return;

    for (int i = 0; i < *count; i++) {
        if (strcmp(paths[i], normalized) == 0) return;
    }
    snprintf(paths[*count], PATH_MAX, "%s", normalized);
    (*count)++;
}

static void add_prefix_glob(char paths[][PATH_MAX], int *count, const char *pattern) {
    glob_t g;
    memset(&g, 0, sizeof(g));
    if (glob(pattern, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && *count < MAX_TRACKED_BOTTLES; i++) {
            add_prefix_candidate(paths, count, g.gl_pathv[i]);
        }
    }
    globfree(&g);
}

static void add_whisky_cli_prefixes(char paths[][PATH_MAX], int *count) {
    FILE *p = popen("command -v whisky >/dev/null 2>&1 && whisky list 2>/dev/null", "r");
    if (!p) return;

    char line[4096];
    while (fgets(line, sizeof(line), p) && *count < MAX_TRACKED_BOTTLES) {
        if (line[0] != '|') continue;
        char *last = strrchr(line, '|');
        if (!last) continue;
        *last = '\0';
        char *prev = strrchr(line, '|');
        if (!prev) continue;
        char *path = trim(prev + 1);
        if (*path == '~' || *path == '/') add_prefix_candidate(paths, count, path);
    }
    pclose(p);
}

static void add_crossOver_prefixes(char paths[][PATH_MAX], int *count) {
    char base[PATH_MAX];
    const char *commands[] = {
        "defaults read com.codeweavers.CrossOver BottleDir 2>/dev/null",
        "defaults read com.codeweavers.CrossOver.plist BottleDir 2>/dev/null",
        "defaults read com.codeweavers.CrossOverGames BottleDir 2>/dev/null",
        "defaults read com.codeweavers.CrossOverGames.plist BottleDir 2>/dev/null",
        "defaults read com.codeweavers.CrossOver ManagedBottleDirs 2>/dev/null",
        "defaults read com.codeweavers.CrossOver.plist ManagedBottleDirs 2>/dev/null",
        NULL
    };

    for (int i = 0; commands[i]; i++) {
        if (command_capture(commands[i], base, sizeof(base)) == 0) {
            char pattern[PATH_MAX];
            snprintf(pattern, sizeof(pattern), "%s/*", base);
            add_prefix_glob(paths, count, pattern);
        }
    }

    snprintf(base, sizeof(base), "%s/Library/Application Support/CrossOver/Bottles/*", home_dir());
    add_prefix_glob(paths, count, base);
}

static int discover_wine_prefixes(const Config *cfg, char paths[][PATH_MAX], int max_paths) {
    int count = 0;
    if (max_paths <= 0) return 0;

    const char *envs[] = {
        getenv("WINECORD_PREFIX"),
        getenv("WINECORD_BOTTLE"),
        getenv("WINEPREFIX")
    };
    for (int i = 0; i < 3 && count < max_paths; i++) add_prefix_candidate(paths, &count, envs[i]);

    if (cfg) {
        for (int i = 0; i < cfg->bottle_count && count < max_paths; i++) {
            add_prefix_candidate(paths, &count, cfg->bottle_paths[i]);
        }
    }

    add_crossOver_prefixes(paths, &count);
    add_whisky_cli_prefixes(paths, &count);

    char pattern[PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s/Library/Containers/com.isaacmarovitz.Whisky/Bottles/*", home_dir());
    add_prefix_glob(paths, &count, pattern);
    snprintf(pattern, sizeof(pattern), "%s/Library/Containers/com.franke.Whisky/Bottles/*", home_dir());
    add_prefix_glob(paths, &count, pattern);

    snprintf(pattern, sizeof(pattern), "%s/Games/Heroic/Prefixes/*", home_dir());
    add_prefix_glob(paths, &count, pattern);
    snprintf(pattern, sizeof(pattern), "%s/Games/Heroic/Prefixes/default/*", home_dir());
    add_prefix_glob(paths, &count, pattern);
    snprintf(pattern, sizeof(pattern), "%s/Library/Application Support/heroic/Prefixes/*", home_dir());
    add_prefix_glob(paths, &count, pattern);

    snprintf(pattern, sizeof(pattern), "%s/Applications/*.app/Contents/SharedSupport/prefix", home_dir());
    add_prefix_glob(paths, &count, pattern);
    add_prefix_glob(paths, &count, "/Applications/*.app/Contents/SharedSupport/prefix");

    snprintf(pattern, sizeof(pattern), "%s/.wine", home_dir());
    add_prefix_candidate(paths, &count, pattern);
    snprintf(pattern, sizeof(pattern), "%s/wineprefixes/*", home_dir());
    add_prefix_glob(paths, &count, pattern);
    snprintf(pattern, sizeof(pattern), "%s/Wine Prefixes/*", home_dir());
    add_prefix_glob(paths, &count, pattern);

    return count;
}

static void resolve_bottle(const Config *cfg, char *out, size_t out_len) {
    out[0] = '\0';

    const char *env = getenv("WINECORD_PREFIX");
    if (env && *env && is_wine_prefix(env)) {
        normalize_path_copy(out, out_len, env);
        return;
    }

    env = getenv("WINECORD_BOTTLE");
    if (env && *env && is_wine_prefix(env)) {
        normalize_path_copy(out, out_len, env);
        return;
    }

    if (cfg) {
        for (int i = 0; i < cfg->bottle_count; i++) {
            if (is_wine_prefix(cfg->bottle_paths[i])) {
                normalize_path_copy(out, out_len, cfg->bottle_paths[i]);
                return;
            }
        }
    }

    char prefixes[MAX_TRACKED_BOTTLES][PATH_MAX];
    int count = discover_wine_prefixes(cfg, prefixes, MAX_TRACKED_BOTTLES);
    if (count > 0) snprintf(out, out_len, "%s", prefixes[0]);
}

static bool steam_root_for_prefix(const char *prefix, char *out, size_t out_len) {
    if (!prefix || !*prefix || !out || out_len == 0) return false;

    char candidate[PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/drive_c/Program Files (x86)/Steam", prefix);
    if (is_directory(candidate)) {
        snprintf(out, out_len, "%s", candidate);
        return true;
    }

    snprintf(candidate, sizeof(candidate), "%s/drive_c/Program Files/Steam", prefix);
    if (is_directory(candidate)) {
        snprintf(out, out_len, "%s", candidate);
        return true;
    }

    out[0] = '\0';
    return false;
}

static bool parse_gameid_after(const char *line, const char *marker,
                               char *out, size_t out_len) {
    if (!line || !marker || !out || out_len == 0) return false;
    const char *p = strstr(line, marker);
    if (!p) return false;
    p += strlen(marker);

    while (*p && !isdigit((unsigned char)*p)) p++;
    size_t pos = 0;
    while (isdigit((unsigned char)*p) && pos + 1 < out_len) {
        out[pos++] = *p++;
    }
    out[pos] = '\0';
    return out[0] != '\0';
}

static int steam_activity_slot(SteamActivityCounter games[], int *count,
                               const char *appid) {
    if (!games || !count || !appid || !*appid) return -1;
    for (int i = 0; i < *count; i++) {
        if (strcmp(games[i].appid, appid) == 0) return i;
    }
    if (*count >= MAX_STEAM_ACTIVITY) return -1;
    snprintf(games[*count].appid, sizeof(games[*count].appid), "%s", appid);
    games[*count].process_count = 0;
    return (*count)++;
}

static bool read_vdf_value(const char *line, const char *key, char *out, size_t out_len) {
    if (!line || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(line, needle);
    if (!p) return false;
    p += strlen(needle);

    const char *start = strchr(p, '"');
    if (!start) return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end || end <= start) return false;

    size_t len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

static bool steam_app_name(const char *steam_root, const char *appid,
                           char *out, size_t out_len) {
    if (!steam_root || !appid || !out || out_len == 0) return false;
    out[0] = '\0';

    char manifest[PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/steamapps/appmanifest_%s.acf", steam_root, appid);
    FILE *f = fopen(manifest, "r");
    if (!f) return false;

    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char name[256];
        if (read_vdf_value(line, "name", name, sizeof(name))) {
            snprintf(out, out_len, "%s", name);
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static int collect_steam_activity_for_prefix(const char *prefix,
                                             SteamActivityCounter games[],
                                             int max_games,
                                             bool *saw_log) {
    (void)max_games;
    if (saw_log) *saw_log = false;
    if (!prefix || !games) return 0;

    char steam_root[PATH_MAX];
    if (!steam_root_for_prefix(prefix, steam_root, sizeof(steam_root))) return 0;

    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/logs/streaming_log.txt", steam_root);
    FILE *f = fopen(log_path, "r");
    if (!f) return 0;
    if (saw_log) *saw_log = true;

    int count = 0;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char appid[sizeof(games[0].appid)];
        int slot = -1;

        if (strstr(line, "Adding process") &&
            parse_gameid_after(line, "for gameID ", appid, sizeof(appid))) {
            slot = steam_activity_slot(games, &count, appid);
            if (slot >= 0) games[slot].process_count++;
        } else if (strstr(line, "Removing process") &&
                   parse_gameid_after(line, "for gameID ", appid, sizeof(appid))) {
            slot = steam_activity_slot(games, &count, appid);
            if (slot >= 0 && games[slot].process_count > 0) games[slot].process_count--;
        } else if (parse_gameid_after(line, "game stopped [gameid=", appid, sizeof(appid))) {
            slot = steam_activity_slot(games, &count, appid);
            if (slot >= 0) games[slot].process_count = 0;
        }
    }
    fclose(f);
    return count;
}

static int print_steam_activity_for_prefixes(char prefixes[][PATH_MAX], int prefix_count) {
    printf("\nSteam Activity:\n");
    if (prefix_count <= 0) {
        printf("  prefixes: not found\n");
        return 0;
    }

    bool saw_any_log = false;
    int active_total = 0;
    for (int i = 0; i < prefix_count; i++) {
        SteamActivityCounter games[MAX_STEAM_ACTIVITY];
        memset(games, 0, sizeof(games));
        bool saw_log = false;
        int game_count = collect_steam_activity_for_prefix(prefixes[i], games,
                                                          MAX_STEAM_ACTIVITY, &saw_log);
        saw_any_log = saw_any_log || saw_log;
        if (!saw_log) continue;

        char steam_root[PATH_MAX] = "";
        steam_root_for_prefix(prefixes[i], steam_root, sizeof(steam_root));
        for (int j = 0; j < game_count; j++) {
            if (games[j].process_count <= 0) continue;
            char name[256] = "";
            steam_app_name(steam_root, games[j].appid, name, sizeof(name));
            printf("  active: %s%s%s (Steam AppID %s, %d process%s)\n",
                   name[0] ? name : "Steam app ",
                   name[0] ? "" : games[j].appid,
                   name[0] ? "" : "",
                   games[j].appid,
                   games[j].process_count,
                   games[j].process_count == 1 ? "" : "es");
            printf("    prefix: %s\n", prefixes[i]);
            active_total++;
        }
    }

    if (!saw_any_log) {
        printf("  logs: not found in configured Steam prefixes\n");
    } else if (active_total == 0) {
        printf("  active games: none detected from Steam logs\n");
    }
    return active_total;
}

static int steam_activity_command(const Config *cfg) {
    char prefixes[MAX_TRACKED_BOTTLES][PATH_MAX];
    int prefix_count = discover_wine_prefixes(cfg, prefixes, MAX_TRACKED_BOTTLES);
    printf("WineCord Steam activity detector\n");
    print_steam_activity_for_prefixes(prefixes, prefix_count);
    return 0;
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
    char state_path[PATH_MAX];
    state_path_for_config(cfg, state_path, sizeof(state_path));
    printf("  state: %s\n", state_path);
    printf("\n");
    print_discord_sockets();

    char prefixes[MAX_TRACKED_BOTTLES][PATH_MAX];
    int prefix_count = discover_wine_prefixes(cfg, prefixes, MAX_TRACKED_BOTTLES);
    printf("\nWine:\n");
    printf("  CrossOver: %s\n", is_directory("/Applications/CrossOver.app") ? "/Applications/CrossOver.app" : "not found");
    printf("  Whisky:    %s\n", is_directory("/Applications/Whisky.app") ? "/Applications/Whisky.app" : "not found");
    printf("  saved wine runner: %s\n", cfg->wine_path[0] ? cfg->wine_path : "not set");
    if (prefix_count == 0) {
        printf("  prefixes: not found; pass --prefix PATH\n");
    } else {
        printf("  prefixes:\n");
    }
    for (int i = 0; i < prefix_count; i++) {
        const char *bottle = prefixes[i];
        char runner[PATH_MAX] = "";
        char whisky_path[PATH_MAX] = "";
        char whisky_name[256] = "";
        char helper[PATH_MAX];
        snprintf(helper, sizeof(helper), "%s/drive_c/windows/winecord-bridge.exe", bottle);
        if (find_whisky_runner(bottle, whisky_path, sizeof(whisky_path), whisky_name, sizeof(whisky_name))) {
            snprintf(runner, sizeof(runner), "%s run %s", whisky_path, whisky_name);
        } else {
            find_wine_runner(cfg, bottle, NULL, runner, sizeof(runner));
        }
        printf("    %s\n", bottle);
        printf("      runner: %s\n", runner[0] ? runner : "not found; pass --wine PATH");
        printf("      helper: %s\n", path_exists(helper) ? helper : "not installed");
    }

    print_steam_activity_for_prefixes(prefixes, prefix_count);

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
        if ((strcmp(argv[i], "--bottle") == 0 || strcmp(argv[i], "--prefix") == 0) && i + 1 < argc) {
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
        resolve_bottle(cfg, discovered, sizeof(discovered));
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

static bool load_activity_state(const char *state_path, char *client_id,
                                size_t client_id_len, long *pid) {
    if (client_id && client_id_len > 0) client_id[0] = '\0';
    if (pid) *pid = 0;
    FILE *f = fopen(state_path, "r");
    if (!f) return false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        char *p = trim(line);
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);
        if (strcmp(key, "client_id") == 0 && client_id && client_id_len > 0) {
            snprintf(client_id, client_id_len, "%s", value);
        } else if (strcmp(key, "pid") == 0 && pid) {
            *pid = strtol(value, NULL, 10);
        }
    }
    fclose(f);
    return client_id && client_id[0];
}

static bool parse_last_client_id_from_log(const Config *cfg, char *client_id, size_t client_id_len) {
    if (!client_id || client_id_len == 0) return false;
    client_id[0] = '\0';

    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/agent.log", cfg->log_dir);
    FILE *f = fopen(log_path, "r");
    if (!f) return false;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "client_id=");
        if (!p) continue;
        p += strlen("client_id=");
        size_t pos = 0;
        while (isdigit((unsigned char)p[pos]) && pos + 1 < client_id_len) {
            client_id[pos] = p[pos];
            pos++;
        }
        client_id[pos] = '\0';
    }
    fclose(f);
    return client_id[0] != '\0';
}

static int clear_activity_for_client(const char *client_id, long pid) {
    if (!client_id || !*client_id) {
        fprintf(stderr, "No Discord client_id available to clear.\n");
        return 1;
    }
    if (pid <= 0) pid = (long)getpid();

    char discord_path[PATH_MAX];
    int fd = connect_discord(discord_path, sizeof(discord_path));
    if (fd < 0) {
        fprintf(stderr, "Could not connect to Discord IPC socket. Is Discord open?\n");
        return 1;
    }

    char json[512];
    snprintf(json, sizeof(json), "{\"v\":1,\"client_id\":\"%s\"}", client_id);
    if (write_ipc_frame(fd, 0, json) != 0) {
        fprintf(stderr, "Could not send Discord handshake.\n");
        close(fd);
        return 1;
    }

    uint32_t opcode = 0;
    char response[4096];
    if (read_ipc_frame_timeout(fd, &opcode, response, sizeof(response), 2500) != 0 || opcode == 2) {
        fprintf(stderr, "Discord rejected or did not answer the handshake for client_id=%s.\n", client_id);
        close(fd);
        return 1;
    }

    snprintf(json, sizeof(json),
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%ld,\"activity\":null},"
             "\"nonce\":\"winecord-clear-%ld\"}",
             pid, (long)time(NULL));
    if (write_ipc_frame(fd, 1, json) != 0) {
        fprintf(stderr, "Could not send clear activity request.\n");
        close(fd);
        return 1;
    }

    read_ipc_frame_timeout(fd, &opcode, response, sizeof(response), 1000);
    close(fd);
    printf("Cleared Discord activity for client_id=%s pid=%ld\n", client_id, pid);
    return 0;
}

static int clear_activity_command(const Config *cfg, int argc, char **argv) {
    char client_id[80] = "";
    long pid = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--client-id") == 0 && i + 1 < argc) {
            snprintf(client_id, sizeof(client_id), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            pid = strtol(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "Unknown clear option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!client_id[0]) {
        char state_path[PATH_MAX];
        state_path_for_config(cfg, state_path, sizeof(state_path));
        load_activity_state(state_path, client_id, sizeof(client_id), &pid);
    }
    if (!client_id[0]) parse_last_client_id_from_log(cfg, client_id, sizeof(client_id));

    return clear_activity_for_client(client_id, pid);
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

    print_success("Installed LaunchAgent\n");
    print_note("  %s\n", plist);
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
    print_success("Removed LaunchAgent\n");
    print_note("  %s\n", plist);
    return 0;
}

static int start_agent(const Config *cfg) {
    char plist[PATH_MAX];
    snprintf(plist, sizeof(plist), "%s/Library/LaunchAgents/%s.plist", home_dir(), WINECORD_LABEL);
    if (!path_exists(plist)) return install_agent(cfg);

    char cmd[(PATH_MAX * 2) + 128];
    char qplist[PATH_MAX + 8];
    if (shell_quote(plist, qplist, sizeof(qplist)) != 0) return 1;

    for (int attempt = 0; attempt < 8; attempt++) {
        snprintf(cmd, sizeof(cmd), "launchctl print gui/%d/%s >/dev/null 2>&1",
                 getuid(), WINECORD_LABEL);
        if (system(cmd) != 0) {
            snprintf(cmd, sizeof(cmd), "launchctl bootstrap gui/%d %s >/dev/null 2>&1",
                     getuid(), qplist);
            system(cmd);
        }

        snprintf(cmd, sizeof(cmd), "launchctl kickstart -k gui/%d/%s >/dev/null 2>&1",
                 getuid(), WINECORD_LABEL);
        if (system(cmd) == 0) return 0;
        usleep(250000);
    }

    fprintf(stderr, "Could not start LaunchAgent. Run `winecord setup` to repair it.\n");
    return 1;
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

static bool is_safe_command_name(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (isalnum(*p) || *p == '_' || *p == '-' || *p == '.' || *p == '+') continue;
        return false;
    }
    return true;
}

static bool command_path(const char *name, char *out, size_t out_len) {
    if (!is_safe_command_name(name)) return false;
    char cmd[128];
    if (snprintf(cmd, sizeof(cmd), "command -v %s 2>/dev/null", name) >= (int)sizeof(cmd)) return false;
    return command_capture(cmd, out, out_len) == 0 && path_exists(out);
}

static char *read_text_file_limited(const char *path, size_t limit) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || (size_t)size > limit) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = calloc((size_t)size + 1, 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static bool json_string_after_key(const char *json, const char *key, char *out, size_t out_len) {
    if (!json || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    char needle[128];
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return false;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return false;
    p++;

    size_t pos = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            if (!c) return false;
            switch (c) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'n':
                    c = '\n';
                    break;
                case 'r':
                    c = '\r';
                    break;
                case 't':
                    c = '\t';
                    break;
                default:
                    break;
            }
        }
        if (pos + 1 >= out_len) return false;
        out[pos++] = c;
    }
    if (*p != '"') return false;
    out[pos] = '\0';
    return out[0] != '\0';
}

static bool prefix_matches_configured_prefix(const char *prefix, const char *configured) {
    char normalized_prefix[PATH_MAX];
    char normalized_configured[PATH_MAX];
    char resolved[PATH_MAX];

    if (realpath(prefix, resolved)) normalize_path_copy(normalized_prefix, sizeof(normalized_prefix), resolved);
    else normalize_path_copy(normalized_prefix, sizeof(normalized_prefix), prefix);

    if (realpath(configured, resolved)) normalize_path_copy(normalized_configured, sizeof(normalized_configured), resolved);
    else normalize_path_copy(normalized_configured, sizeof(normalized_configured), configured);

    if (strcmp(normalized_prefix, normalized_configured) == 0) return true;

    size_t len = strlen(normalized_configured);
    return len > 0 &&
           strncmp(normalized_prefix, normalized_configured, len) == 0 &&
           normalized_prefix[len] == '/';
}

static bool config_tracks_prefix(const Config *cfg, const char *prefix) {
    if (!cfg || !prefix || !*prefix) return false;
    for (int i = 0; i < cfg->bottle_count; i++) {
        if (prefix_matches_configured_prefix(prefix, cfg->bottle_paths[i])) return true;
    }
    return false;
}

static bool heroic_runner_from_json(const char *path, const char *prefix, char *out, size_t out_len) {
    char *json = read_text_file_limited(path, 1024 * 1024);
    if (!json) return false;

    char configured_prefix[PATH_MAX];
    char runner[PATH_MAX];
    bool found = false;
    if (json_string_after_key(json, "winePrefix", configured_prefix, sizeof(configured_prefix)) &&
        prefix_matches_configured_prefix(prefix, configured_prefix) &&
        json_string_after_key(json, "bin", runner, sizeof(runner)) &&
        path_exists(runner)) {
        normalize_path_copy(out, out_len, runner);
        found = true;
    }
    free(json);
    return found;
}

static bool find_heroic_runner(const char *prefix, char *out, size_t out_len) {
    if (!prefix || !*prefix) return false;

    char path[PATH_MAX];
    const char *bases[] = {
        "%s/Library/Application Support/heroic/GamesConfig/*.json",
        "%s/Library/Application Support/Heroic/GamesConfig/*.json",
        NULL
    };

    for (int i = 0; bases[i]; i++) {
        if (snprintf(path, sizeof(path), bases[i], home_dir()) >= (int)sizeof(path)) continue;
        glob_t g;
        memset(&g, 0, sizeof(g));
        if (glob(path, 0, NULL, &g) == 0) {
            for (size_t j = 0; j < g.gl_pathc; j++) {
                if (heroic_runner_from_json(g.gl_pathv[j], prefix, out, out_len)) {
                    globfree(&g);
                    return true;
                }
            }
        }
        globfree(&g);
    }

    const char *configs[] = {
        "%s/Library/Application Support/heroic/config.json",
        "%s/Library/Application Support/heroic/store/config.json",
        "%s/Library/Application Support/Heroic/config.json",
        "%s/Library/Application Support/Heroic/store/config.json",
        NULL
    };
    for (int i = 0; configs[i]; i++) {
        if (snprintf(path, sizeof(path), configs[i], home_dir()) >= (int)sizeof(path)) continue;
        if (heroic_runner_from_json(path, prefix, out, out_len)) return true;
    }

    if (path_has_component(prefix, "/Games/Heroic/Prefixes/")) {
        const char *fallbacks[] = {
            "%s/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine/bin/wine64",
            "%s/Library/Application Support/Heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/Resources/wine/bin/wine64",
            "%s/Library/Application Support/heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/MacOS/wine",
            "%s/Library/Application Support/Heroic/tools/game-porting-toolkit/Game-Porting-Toolkit-latest/Contents/MacOS/wine",
            NULL
        };
        for (int i = 0; fallbacks[i]; i++) {
            if (snprintf(path, sizeof(path), fallbacks[i], home_dir()) >= (int)sizeof(path)) continue;
            if (path_exists(path)) {
                normalize_path_copy(out, out_len, path);
                return true;
            }
        }
    }

    return false;
}

static bool is_crossover_prefix(const char *prefix) {
    return path_has_component(prefix, "/CrossOver/Bottles/");
}

static bool is_crossover_cli_runner(const char *wine) {
    return path_has_component(wine, "/CrossOver/bin/wine");
}

static bool is_crossover_runner(const char *wine) {
    return is_crossover_cli_runner(wine) ||
           path_has_component(wine, "/CrossOver-Hosted Application/wine");
}

static bool find_whisky_runner(const char *prefix, char *whisky_path, size_t whisky_len,
                               char *bottle_name, size_t name_len) {
    if (!prefix || !*prefix) return false;
    if (!command_path("whisky", whisky_path, whisky_len)) {
        const char *fallback = "/usr/local/bin/whisky";
        if (!path_exists(fallback)) return false;
        normalize_path_copy(whisky_path, whisky_len, fallback);
    }

    char target[PATH_MAX];
    char resolved[PATH_MAX];
    if (realpath(prefix, resolved)) normalize_path_copy(target, sizeof(target), resolved);
    else normalize_path_copy(target, sizeof(target), prefix);

    const char *list_cmd = strcmp(whisky_path, "/usr/local/bin/whisky") == 0
        ? "/usr/local/bin/whisky list 2>/dev/null"
        : "whisky list 2>/dev/null";
    FILE *p = popen(list_cmd, "r");
    if (!p) return false;

    bool found = false;
    char line[4096];
    while (fgets(line, sizeof(line), p)) {
        if (line[0] != '|') continue;

        char *fields[4] = {0};
        char *saveptr = NULL;
        int field_count = 0;
        for (char *part = strtok_r(line, "|", &saveptr);
             part && field_count < 4;
             part = strtok_r(NULL, "|", &saveptr)) {
            fields[field_count++] = trim(part);
        }

        if (field_count < 3 || strcmp(fields[0], "Name") == 0 || strcmp(fields[2], "Path") == 0) continue;

        char candidate[PATH_MAX];
        char candidate_resolved[PATH_MAX];
        normalize_path_copy(candidate, sizeof(candidate), fields[2]);
        if (realpath(candidate, candidate_resolved)) {
            normalize_path_copy(candidate, sizeof(candidate), candidate_resolved);
        }

        if (strcmp(candidate, target) == 0) {
            snprintf(bottle_name, name_len, "%s", fields[0]);
            found = bottle_name[0] != '\0';
            break;
        }
    }
    pclose(p);
    return found;
}

static bool find_wine_runner(const Config *cfg, const char *prefix,
                             const char *override_wine, char *out, size_t out_len) {
    out[0] = '\0';

    const char *env_wine = getenv("WINECORD_WINE");
    const char *candidates[] = {
        override_wine,
        env_wine,
        "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine",
        (cfg && config_tracks_prefix(cfg, prefix)) ? cfg->wine_path : NULL,
        "/opt/homebrew/bin/wine64",
        "/opt/homebrew/bin/wine",
        "/usr/local/bin/wine64",
        "/usr/local/bin/wine",
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine64",
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine",
    };

    size_t candidate_count = sizeof(candidates) / sizeof(candidates[0]);
    for (size_t i = 0; i < candidate_count; i++) {
        if (!candidates[i] || !*candidates[i]) continue;
        if (i == 2 && !is_crossover_prefix(prefix)) continue;
        if (!strchr(candidates[i], '/')) {
            if (command_path(candidates[i], out, out_len)) return true;
            continue;
        }
        if (path_exists(candidates[i])) {
            normalize_path_copy(out, out_len, candidates[i]);
            return true;
        }
    }

    char candidate[PATH_MAX];
    if (find_heroic_runner(prefix, candidate, sizeof(candidate))) {
        normalize_path_copy(out, out_len, candidate);
        return true;
    }

    snprintf(candidate, sizeof(candidate), "%s/Library/Application Support/com.isaacmarovitz.Whisky/Libraries/Wine/bin/wine64", home_dir());
    if (path_exists(candidate)) {
        normalize_path_copy(out, out_len, candidate);
        return true;
    }
    snprintf(candidate, sizeof(candidate), "%s/Library/Application Support/com.franke.Whisky/Libraries/Wine/bin/wine64", home_dir());
    if (path_exists(candidate)) {
        normalize_path_copy(out, out_len, candidate);
        return true;
    }

    const char *marker = strstr(prefix, ".app/Contents/SharedSupport/prefix");
    if (!marker) marker = strstr(prefix, ".app/Contents/Resources");
    if (marker) {
        char app[PATH_MAX];
        size_t app_len = (size_t)(marker - prefix) + strlen(".app");
        if (app_len < sizeof(app)) {
            memcpy(app, prefix, app_len);
            app[app_len] = '\0';

            const char *formats[] = {
                "%s/Contents/Resources/wine/bin/wine64",
                "%s/Contents/Resources/wine/bin/wine",
                "%s/Contents/SharedSupport/wine/bin/wine64",
                "%s/Contents/SharedSupport/wine/bin/wine",
                "%s/Contents/Frameworks/wswine.bundle/bin/wine64",
                "%s/Contents/Frameworks/wswine.bundle/bin/wine",
                NULL
            };
            for (int i = 0; formats[i]; i++) {
                snprintf(candidate, sizeof(candidate), formats[i], app);
                if (path_exists(candidate)) {
                    normalize_path_copy(out, out_len, candidate);
                    return true;
                }
            }
        }
    }

    if (command_path("wine64", out, out_len)) return true;
    if (command_path("wine", out, out_len)) return true;
    return false;
}

static int run_whisky_helper(const char *prefix, const char *whisky_path, const char *bottle_name,
                             const char *helper_path, const char *helper_arg, bool quiet) {
    if (!path_exists(whisky_path) || !bottle_name || !*bottle_name) return 1;

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        execl(whisky_path, "whisky", "run", bottle_name, helper_path, helper_arg, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!quiet) {
            fprintf(stderr, "Whisky command failed through prefix %s (status %d).\n", prefix, status);
            fprintf(stderr, "Manual command:\n  \"%s\" run \"%s\" \"%s\" %s\n",
                    whisky_path, bottle_name, helper_path, helper_arg);
        }
        return 1;
    }
    return 0;
}

static bool prefix_path_to_windows_c(const char *prefix, const char *path, char *out, size_t out_len) {
    if (!prefix || !path || !out || out_len == 0) return false;
    out[0] = '\0';

    char root[PATH_MAX];
    if (snprintf(root, sizeof(root), "%s/drive_c/", prefix) >= (int)sizeof(root)) return false;
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) return false;

    const char *relative = path + root_len;
    if (snprintf(out, out_len, "C:\\%s", relative) >= (int)out_len) return false;
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    return true;
}

static int run_crossover_helper(const char *bottle, const char *wine,
                                const char *helper_path, const char *helper_arg, bool quiet) {
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
        if (is_crossover_cli_runner(wine)) {
            char windows_path[PATH_MAX];
            const char *cx_app = prefix_path_to_windows_c(bottle, helper_path, windows_path, sizeof(windows_path))
                ? windows_path
                : helper_path;
            execl(wine, "wine", "--bottle", bottle_name, "--no-gui", "--cx-app", cx_app, helper_arg, (char *)NULL);
        } else {
            execl(wine, "wine", "--bottle", bottle_name, "--no-gui", helper_path, helper_arg, (char *)NULL);
        }
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!quiet) {
            fprintf(stderr, "CrossOver command failed through bottle %s (status %d).\n", bottle_name, status);
            if (is_crossover_cli_runner(wine)) {
                char windows_path[PATH_MAX];
                const char *cx_app = prefix_path_to_windows_c(bottle, helper_path, windows_path, sizeof(windows_path))
                    ? windows_path
                    : helper_path;
                fprintf(stderr, "Manual command:\n  CX_BOTTLE_PATH=\"%s\" \"%s\" --bottle \"%s\" --no-gui --cx-app \"%s\" %s\n",
                        bottle_parent, wine, bottle_name, cx_app, helper_arg);
            } else {
                fprintf(stderr, "Manual command:\n  CX_BOTTLE_PATH=\"%s\" \"%s\" --bottle \"%s\" --no-gui \"%s\" %s\n",
                        bottle_parent, wine, bottle_name, helper_path, helper_arg);
            }
        }
        return 1;
    }
    return 0;
}

static int run_generic_wine_helper(const char *prefix, const char *wine,
                                   const char *helper_path, const char *helper_arg, bool quiet) {
    if (!path_exists(wine)) {
        if (!quiet) fprintf(stderr, "Wine runner not found: %s\n", wine);
        return quiet ? 0 : 1;
    }

    char wine_dir[PATH_MAX];
    parent_dir(wine, wine_dir, sizeof(wine_dir));

    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        const char *old_path = getenv("PATH");
        char new_path[PATH_MAX * 2];
        snprintf(new_path, sizeof(new_path), "%s:%s", wine_dir, old_path ? old_path : "/usr/bin:/bin:/usr/sbin:/sbin");
        setenv("PATH", new_path, 1);

        char wine_root[PATH_MAX];
        char wine_lib[PATH_MAX];
        parent_dir(wine_dir, wine_root, sizeof(wine_root));
        snprintf(wine_lib, sizeof(wine_lib), "%s/lib", wine_root);
        if (is_directory(wine_lib)) {
            const char *old_dyld = getenv("DYLD_FALLBACK_LIBRARY_PATH");
            char new_dyld[PATH_MAX * 2];
            snprintf(new_dyld, sizeof(new_dyld), "%s:%s", wine_lib,
                     old_dyld ? old_dyld : "/usr/lib");
            setenv("DYLD_FALLBACK_LIBRARY_PATH", new_dyld, 1);
        }

        setenv("WINE", base_name(wine), 1);
        setenv("WINEPREFIX", prefix, 1);
        chdir(wine_dir);
        execl(wine, base_name(wine), helper_path, helper_arg, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (!quiet) {
            fprintf(stderr, "Wine command failed through prefix %s (status %d).\n", prefix, status);
            fprintf(stderr, "Manual command:\n  WINEPREFIX=\"%s\" \"%s\" \"%s\" %s\n",
                    prefix, wine, helper_path, helper_arg);
        }
        return 1;
    }
    return 0;
}

static int run_prefix_helper(const Config *cfg, const char *prefix, const char *wine_override,
                             const char *helper_path, const char *helper_arg, bool quiet,
                             char *used_wine, size_t used_wine_len) {
    const char *env_wine = getenv("WINECORD_WINE");
    if ((!wine_override || !*wine_override) && (!env_wine || !*env_wine)) {
        char whisky_path[PATH_MAX];
        char bottle_name[256];
        if (find_whisky_runner(prefix, whisky_path, sizeof(whisky_path), bottle_name, sizeof(bottle_name))) {
            if (used_wine && used_wine_len > 0) snprintf(used_wine, used_wine_len, "whisky:%s", bottle_name);
            if (run_whisky_helper(prefix, whisky_path, bottle_name, helper_path, helper_arg, quiet) == 0) {
                return 0;
            }
        }
    }

    char wine[PATH_MAX];
    if (!find_wine_runner(cfg, prefix, wine_override, wine, sizeof(wine))) {
        if (!quiet) {
            fprintf(stderr, "No Wine runner found for prefix: %s\n", prefix);
            fprintf(stderr, "Pass --wine /path/to/wine or set WINECORD_WINE.\n");
        }
        return 1;
    }
    if (used_wine && used_wine_len > 0) snprintf(used_wine, used_wine_len, "%s", wine);
    if (is_crossover_runner(wine)) return run_crossover_helper(prefix, wine, helper_path, helper_arg, quiet);
    return run_generic_wine_helper(prefix, wine, helper_path, helper_arg, quiet);
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
    print_success("Wrote prefix config\n");
    print_note("  %s\n", path);
    return 0;
}

static int register_windows_service(const Config *cfg, const char *prefix,
                                    const char *wine, const char *helper_dest,
                                    char *used_wine, size_t used_wine_len) {
    if (run_prefix_helper(cfg, prefix, wine, helper_dest, "--install", false,
                          used_wine, used_wine_len) != 0) {
        return 1;
    }
    print_success("Registered WineCordBridge service\n");
    print_note("  %s\n", prefix);
    return 0;
}

static int install_one_prefix(Config *cfg, const char *prefix, const char *helper,
                              const char *wine, bool no_register, bool required) {
    if (!prefix || !is_wine_prefix(prefix)) {
        if (required) fprintf(stderr, "Wine prefix not found or not initialized: %s\n", prefix ? prefix : "<none>");
        return 1;
    }

    if (!no_register) {
        const char *env_wine = getenv("WINECORD_WINE");
        char runner[PATH_MAX];
        char whisky_path[PATH_MAX];
        char whisky_name[256];
        bool has_runner = find_wine_runner(cfg, prefix, wine, runner, sizeof(runner));
        if (!has_runner && (!wine || !*wine) && (!env_wine || !*env_wine)) {
            has_runner = find_whisky_runner(prefix, whisky_path, sizeof(whisky_path), whisky_name, sizeof(whisky_name));
        }
        if (!has_runner) {
            if (required) {
                fprintf(stderr, "No usable Wine runner found for prefix: %s\n", prefix);
                fprintf(stderr, "Pass --wine /path/to/wine if this prefix uses a custom runner.\n");
                return 1;
            }
            print_warning_stdout("Skipped prefix without a usable Wine runner\n");
            print_note("  %s\n", prefix);
            return 2;
        }
    }

    char helper_dest[PATH_MAX];
    snprintf(helper_dest, sizeof(helper_dest), "%s/drive_c/windows/winecord-bridge.exe", prefix);

    if (write_bottle_config(cfg, prefix) != 0) return 1;
    if (copy_file(helper, helper_dest, 0755) != 0) return 1;
    print_success("Installed helper\n");
    print_note("  %s\n", helper_dest);

    char used_wine[PATH_MAX] = "";
    if (!no_register) {
        if (register_windows_service(cfg, prefix, wine, helper_dest, used_wine, sizeof(used_wine)) != 0) {
            return 1;
        }
    } else {
        find_wine_runner(cfg, prefix, wine, used_wine, sizeof(used_wine));
        print_warning_stdout("Skipped service registration (--no-register).\n");
    }

    remember_bottle(cfg, prefix);
    if (used_wine[0] && path_exists(used_wine) && !is_crossover_runner(used_wine)) {
        normalize_path_copy(cfg->wine_path, sizeof(cfg->wine_path), used_wine);
    }
    if (save_config(cfg) != 0) return 1;
    return 0;
}

static int install_bottle(Config *cfg, int argc, char **argv) {
    char explicit_prefix[PATH_MAX] = "";
    const char *wine = NULL;
    const char *helper = NULL;
    bool no_register = false;

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--bottle") == 0 || strcmp(argv[i], "--prefix") == 0) && i + 1 < argc) {
            normalize_path_copy(explicit_prefix, sizeof(explicit_prefix), argv[++i]);
        } else if (strcmp(argv[i], "--wine") == 0 && i + 1 < argc) {
            wine = argv[++i];
        } else if (strcmp(argv[i], "--helper") == 0 && i + 1 < argc) {
            helper = argv[++i];
        } else if (strcmp(argv[i], "--no-register") == 0) {
            no_register = true;
        } else {
            fprintf(stderr, "Unknown setup option: %s\n", argv[i]);
            return 1;
        }
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

    char prefixes[MAX_TRACKED_BOTTLES][PATH_MAX];
    int prefix_count = 0;
    if (explicit_prefix[0]) {
        add_prefix_candidate(prefixes, &prefix_count, explicit_prefix);
        if (prefix_count == 0) {
            fprintf(stderr, "This does not look like an initialized Wine prefix: %s\n", explicit_prefix);
            return 1;
        }
    } else {
        prefix_count = discover_wine_prefixes(cfg, prefixes, MAX_TRACKED_BOTTLES);
    }

    if (prefix_count == 0) {
        fprintf(stderr,
                "No Wine prefix found. Open your Wine app once, or pass --prefix /path/to/prefix.\n");
        return 1;
    }

    int installed = 0;
    int skipped = 0;
    int failed = 0;
    for (int i = 0; i < prefix_count; i++) {
        print_header("Configuring Wine prefix");
        print_note("  %s\n", prefixes[i]);
        int result = install_one_prefix(cfg, prefixes[i], helper, wine, no_register, explicit_prefix[0] != '\0');
        if (result == 0) {
            installed++;
        } else if (result == 2) {
            skipped++;
        } else {
            failed++;
            if (explicit_prefix[0]) return 1;
        }
    }

    if (installed == 0) return 1;
    if (skipped > 0) {
        print_warning_stdout("WineCord configured %d prefix(es) and skipped %d prefix(es) without a usable runner.\n",
                             installed, skipped);
    }
    if (failed > 0) {
        fprintf(stderr, "\nWineCord configured %d prefix(es), but skipped %d prefix(es) with errors.\n", installed, failed);
    }
    return 0;
}

static bool remove_file_if_exists(const char *path) {
    if (unlink(path) == 0) {
        print_success("Removed\n");
        print_note("  %s\n", path);
        return true;
    } else if (errno != ENOENT) {
        fprintf(stderr, "Could not remove %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static bool remove_dir_if_empty(const char *path) {
    if (rmdir(path) == 0) {
        print_success("Removed\n");
        print_note("  %s\n", path);
        return true;
    }
    if (errno == ENOENT) return true;
    fprintf(stderr, "Could not remove %s: %s\n", path, strerror(errno));
    return false;
}

static int remove_bottle_setup(const Config *cfg, const char *bottle, const char *wine) {
    if (!bottle || !is_wine_prefix(bottle)) return 1;
    int failures = 0;

    char helper_dest[PATH_MAX];
    snprintf(helper_dest, sizeof(helper_dest), "%s/drive_c/windows/winecord-bridge.exe", bottle);

    char helper_runner[PATH_MAX] = "";
    if (path_exists(helper_dest)) {
        snprintf(helper_runner, sizeof(helper_runner), "%s", helper_dest);
    } else if (!find_helper(helper_runner, sizeof(helper_runner))) {
        helper_runner[0] = '\0';
    }

    if (helper_runner[0]) {
        if (run_prefix_helper(cfg, bottle, wine, helper_runner, "--remove", false, NULL, 0) != 0) {
            fprintf(stderr, "Could not remove the WineCordBridge service from prefix: %s\n", bottle);
            failures++;
        }
    } else {
        fprintf(stderr, "Could not find winecord-bridge.exe to remove the Wine service in prefix: %s\n", bottle);
        failures++;
    }

    if (path_exists(helper_dest) && !remove_file_if_exists(helper_dest)) {
        failures++;
    }

    char file[PATH_MAX];
    snprintf(file, sizeof(file), "%s/drive_c/users/Public/WineCord/config.ini", bottle);
    if (!remove_file_if_exists(file)) failures++;
    snprintf(file, sizeof(file), "%s/drive_c/users/Public/WineCord/bridge.log", bottle);
    if (!remove_file_if_exists(file)) failures++;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/drive_c/users/Public/WineCord", bottle);
    if (!remove_dir_if_empty(dir)) failures++;

    if (failures == 0) {
        print_success("Cleaned prefix\n");
        print_note("  %s\n", bottle);
    }
    return failures == 0 ? 0 : 1;
}

static int setup_all(Config *cfg, int argc, char **argv) {
    print_header("Setting up WineCord");
    fflush(stdout);
    if (install_agent(cfg) != 0) return 1;
    if (install_bottle(cfg, argc, argv) != 0) return 1;
    print_success("Done. Keep Discord for macOS open, then launch the Windows game from your Wine app.\n");
    return 0;
}

static int uninstall_all(const Config *cfg, int argc, char **argv) {
    char bottle_targets[MAX_TRACKED_BOTTLES][PATH_MAX];
    int bottle_target_count = 0;
    const char *wine = NULL;
    bool keep_config = false;
    bool keep_logs = false;
    bool skip_bottle = false;

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--bottle") == 0 || strcmp(argv[i], "--prefix") == 0) && i + 1 < argc) {
            add_path_target(bottle_targets, &bottle_target_count, argv[++i]);
        } else if (strcmp(argv[i], "--wine") == 0 && i + 1 < argc) {
            wine = argv[++i];
        } else if (strcmp(argv[i], "--keep-config") == 0) {
            keep_config = true;
        } else if (strcmp(argv[i], "--keep-logs") == 0) {
            keep_logs = true;
        } else if (strcmp(argv[i], "--no-bottle") == 0 || strcmp(argv[i], "--no-prefix") == 0) {
            skip_bottle = true;
        } else {
            fprintf(stderr, "Unknown uninstall option: %s\n", argv[i]);
            return 1;
        }
    }

    print_header("Uninstalling WineCord setup");
    fflush(stdout);

    int bottle_failures = 0;
    if (!skip_bottle) {
        if (bottle_target_count == 0) {
            for (int i = 0; i < cfg->bottle_count; i++) {
                add_path_target(bottle_targets, &bottle_target_count, cfg->bottle_paths[i]);
            }

            if (bottle_target_count == 0) {
                char discovered[MAX_TRACKED_BOTTLES][PATH_MAX];
                int discovered_count = discover_wine_prefixes(cfg, discovered, MAX_TRACKED_BOTTLES);
                for (int i = 0; i < discovered_count; i++) {
                    add_path_target(bottle_targets, &bottle_target_count, discovered[i]);
                }
            }
        }

        if (bottle_target_count == 0) {
            print_warning_stdout("No Wine prefix recorded or discovered; skipped prefix cleanup.\n");
        }

        for (int i = 0; i < bottle_target_count; i++) {
            const char *target = bottle_targets[i];
            if (!is_wine_prefix(target)) {
                fprintf(stderr,
                        "Could not access the configured Wine prefix:\n"
                        "  %s\n"
                        "If this prefix is on an external volume, connect that volume and run `winecord uninstall` again.\n",
                        target);
                bottle_failures++;
                continue;
            }
            if (remove_bottle_setup(cfg, target, wine) != 0) bottle_failures++;
        }
    }

    uninstall_agent();

    if (bottle_failures > 0) {
        keep_config = true;
        keep_logs = true;
        fprintf(stderr,
                "\nWineCord uninstall is incomplete. Kept local config and logs so the remaining bottle cleanup can be retried.\n"
                "Reconnect any external volume that contains a Wine prefix, then run `winecord uninstall` again.\n");
    }

    if (!keep_config) {
        char state_path[PATH_MAX];
        state_path_for_config(cfg, state_path, sizeof(state_path));
        remove_file_if_exists(state_path);
        remove_file_if_exists(cfg->config_path);
        remove_dir_if_empty(cfg->app_dir);
    }

    if (!keep_logs) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/agent.log", cfg->log_dir);
        remove_file_if_exists(path);
        snprintf(path, sizeof(path), "%s/agent.err.log", cfg->log_dir);
        remove_file_if_exists(path);
        remove_dir_if_empty(cfg->log_dir);
    }

    if (bottle_failures > 0) return 1;

    print_success("WineCord setup removed. You can now run `brew uninstall winecord` to remove the package.\n");
    return 0;
}

static int version_compare(const char *a, const char *b) {
    const char *pa = a ? a : "";
    const char *pb = b ? b : "";

    while (*pa || *pb) {
        while (*pa && !isdigit((unsigned char)*pa)) pa++;
        while (*pb && !isdigit((unsigned char)*pb)) pb++;

        long va = 0;
        long vb = 0;
        while (*pa && isdigit((unsigned char)*pa)) {
            va = (va * 10) + (*pa - '0');
            pa++;
        }
        while (*pb && isdigit((unsigned char)*pb)) {
            vb = (vb * 10) + (*pb - '0');
            pb++;
        }

        if (va > vb) return 1;
        if (va < vb) return -1;
        if (!*pa && !*pb) return 0;
    }

    return 0;
}

static void update_cache_path(const Config *cfg, char *out, size_t out_len) {
    snprintf(out, out_len, "%s/update-check.ini", cfg->app_dir);
}

static bool read_update_cache(const Config *cfg, long *checked_at, char *latest, size_t latest_len) {
    char path[PATH_MAX];
    update_cache_path(cfg, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;

    if (checked_at) *checked_at = 0;
    if (latest && latest_len > 0) latest[0] = '\0';

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        chomp(line);
        char *p = trim(line);
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(p);
        char *value = trim(eq + 1);
        if (strcmp(key, "checked_at") == 0 && checked_at) {
            *checked_at = strtol(value, NULL, 10);
        } else if (strcmp(key, "latest") == 0 && latest && latest_len > 0) {
            snprintf(latest, latest_len, "%s", value);
        }
    }
    fclose(f);
    return latest && latest[0] != '\0';
}

static void write_update_cache(const Config *cfg, const char *latest) {
    if (!latest || !*latest) return;
    if (mkdir_p(cfg->app_dir, 0700) != 0) return;

    char path[PATH_MAX];
    update_cache_path(cfg, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "checked_at=%ld\nlatest=%s\n", (long)time(NULL), latest);
    fclose(f);
    chmod(path, 0600);
}

static void clear_update_cache(const Config *cfg) {
    char path[PATH_MAX];
    update_cache_path(cfg, path, sizeof(path));
    unlink(path);
}

static bool fetch_latest_version(char *out, size_t out_len) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "/usr/bin/curl -fsSL --connect-timeout 1 --max-time 2 %s 2>/dev/null | "
             "/usr/bin/sed -n 's/.*winecord-\\([0-9][0-9.]*\\)-macos-universal.*/\\1/p' | "
             "/usr/bin/head -n 1",
             WINECORD_FORMULA_URL);
    return command_capture(cmd, out, out_len) == 0;
}

static bool should_skip_update_notice(const char *cmd) {
    if (!cmd || !*cmd) return true;
    return strcmp(cmd, "agent") == 0 ||
           strcmp(cmd, "update") == 0 ||
           strcmp(cmd, "--version") == 0 ||
           strcmp(cmd, "version") == 0 ||
           strcmp(cmd, "--help") == 0 ||
           strcmp(cmd, "help") == 0;
}

static void maybe_print_update_notice(const Config *cfg, const char *cmd) {
    const char *disabled = getenv("WINECORD_NO_UPDATE_CHECK");
    if (disabled && *disabled && strcmp(disabled, "0") != 0) return;
    if (should_skip_update_notice(cmd)) return;
    if (!isatty(STDERR_FILENO)) return;

    char latest[64] = "";
    long checked_at = 0;
    time_t now = time(NULL);
    bool fresh = read_update_cache(cfg, &checked_at, latest, sizeof(latest)) &&
                 checked_at > 0 &&
                 now >= (time_t)checked_at &&
                 now - (time_t)checked_at < WINECORD_UPDATE_CHECK_INTERVAL;

    if (!fresh) {
        latest[0] = '\0';
        if (!fetch_latest_version(latest, sizeof(latest))) return;
        write_update_cache(cfg, latest);
    }

    if (version_compare(latest, WINECORD_VERSION) > 0) {
        styled_printf(stderr, "\033[1;33m",
                      "WARNING: You are using WineCord version %s; however, version %s is available.\n",
                      WINECORD_VERSION, latest);
        fprintf(stderr, "You should consider upgrading via the 'winecord update' command.\n\n");
    }
}

static int run_process_env(char *const argv[], bool homebrew_no_auto_update) {
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (homebrew_no_auto_update) {
            setenv("HOMEBREW_NO_AUTO_UPDATE", "1", 1);
            setenv("HOMEBREW_NO_ENV_HINTS", "1", 1);
        }
        execv(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (!WIFEXITED(status)) return 1;
    return WEXITSTATUS(status);
}

static int run_process(char *const argv[]) {
    return run_process_env(argv, false);
}

static bool brew_prefix_for_winecord(const char *brew, char *out, size_t out_len) {
    char qbrew[PATH_MAX + 8];
    char cmd[(PATH_MAX * 2) + 64];
    if (shell_quote(brew, qbrew, sizeof(qbrew)) != 0) return false;
    snprintf(cmd, sizeof(cmd), "%s --prefix winecord 2>/dev/null", qbrew);
    return command_capture(cmd, out, out_len) == 0 && is_directory(out);
}

static bool brew_tap_repo(const char *brew, char *out, size_t out_len) {
    char qbrew[PATH_MAX + 8];
    char cmd[(PATH_MAX * 2) + 96];
    if (shell_quote(brew, qbrew, sizeof(qbrew)) != 0) return false;
    snprintf(cmd, sizeof(cmd), "%s --repo zard-studios/tap 2>/dev/null", qbrew);
    return command_capture(cmd, out, out_len) == 0 && is_directory(out);
}

static int ensure_zard_tap(const char *brew, char *tap_repo, size_t tap_repo_len) {
    if (brew_tap_repo(brew, tap_repo, tap_repo_len)) return 0;

    print_header("Installing Zard Studios tap");
    char *tap_argv[] = { (char *)brew, "tap", "zard-studios/tap", NULL };
    int status = run_process_env(tap_argv, true);
    if (status != 0) return status;

    if (brew_tap_repo(brew, tap_repo, tap_repo_len)) return 0;
    fprintf(stderr, "Could not locate zard-studios/tap after tapping it.\n");
    return 1;
}

static int refresh_zard_tap_only(const char *brew, char *tap_repo, size_t tap_repo_len) {
    int status = ensure_zard_tap(brew, tap_repo, tap_repo_len);
    if (status != 0) return status;

    char git[PATH_MAX];
    if (!command_path("git", git, sizeof(git))) {
        fprintf(stderr, "Git was not found. Homebrew taps need git to update.\n");
        return 1;
    }

    print_header("Refreshing Zard Studios tap");
    print_note("  %s\n", tap_repo);
    char *pull_argv[] = { git, "-C", tap_repo, "pull", "--ff-only", NULL };
    return run_process(pull_argv);
}

static int update_winecord(const Config *cfg, int argc, char **argv) {
    bool refresh_setup = true;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-setup") == 0) {
            refresh_setup = false;
        } else {
            fprintf(stderr, "Unknown update option: %s\n", argv[i]);
            return 1;
        }
    }

    char brew[PATH_MAX];
    if (!command_path("brew", brew, sizeof(brew))) {
        fprintf(stderr, "Homebrew was not found. Install WineCord with Homebrew to use `winecord update`.\n");
        return 1;
    }

    print_header("Updating WineCord");
    print_note("  This refreshes only zard-studios/tap, not Homebrew/core.\n");

    char tap_repo[PATH_MAX] = "";
    int tap_status = refresh_zard_tap_only(brew, tap_repo, sizeof(tap_repo));
    if (tap_status != 0) return tap_status;

    print_header("Upgrading WineCord package");
    char *upgrade_argv[] = { brew, "upgrade", "zard-studios/tap/winecord", NULL };
    int upgrade_status = run_process_env(upgrade_argv, true);
    if (upgrade_status != 0) return upgrade_status;

    clear_update_cache(cfg);
    if (!refresh_setup) {
        print_success("WineCord update complete.\n");
        return 0;
    }

    char updated_exe[PATH_MAX] = "";
    char brew_prefix[PATH_MAX] = "";
    if (brew_prefix_for_winecord(brew, brew_prefix, sizeof(brew_prefix))) {
        snprintf(updated_exe, sizeof(updated_exe), "%s/bin/winecord", brew_prefix);
    }
    if (!path_exists(updated_exe)) launch_executable(updated_exe, sizeof(updated_exe));

    print_header("Refreshing WineCord setup");
    char *setup_argv[] = { updated_exe, "setup", NULL };
    int setup_status = run_process(setup_argv);
    if (setup_status != 0) return setup_status;

    print_success("WineCord update complete.\n");
    return 0;
}

static void usage(FILE *out) {
    styled_printf(out, "\033[1;36m", "WineCord %s\n", WINECORD_VERSION);
    fprintf(out, "Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n\n");

    styled_printf(out, "\033[1m", "Usage\n");
    styled_printf(out, "\033[1m", "  Setup\n");
    fprintf(out, "    winecord setup [--prefix PATH] [--wine PATH] [--helper PATH] [--no-register]\n");
    fprintf(out, "    winecord update [--no-setup]\n");
    fprintf(out, "    winecord uninstall [--prefix PATH] [--wine PATH] [--keep-config] [--keep-logs] [--no-prefix]\n\n");

    styled_printf(out, "\033[1m", "  Diagnostics\n");
    fprintf(out, "    winecord doctor\n");
    fprintf(out, "    winecord steam\n");
    fprintf(out, "    winecord logs [--follow] [--prefix PATH]\n");
    fprintf(out, "    winecord clear [--client-id ID] [--pid PID]\n\n");

    styled_printf(out, "\033[1m", "  Agent\n");
    fprintf(out, "    winecord start\n");
    fprintf(out, "    winecord stop\n");
    fprintf(out, "    winecord agent\n");
    fprintf(out, "    winecord install-agent\n");
    fprintf(out, "    winecord uninstall-agent\n\n");

    styled_printf(out, "\033[1m", "  Advanced\n");
    fprintf(out, "    winecord install-bottle [--prefix PATH] [--wine PATH] [--helper PATH] [--no-register]\n");
    fprintf(out, "    winecord install-prefix [--prefix PATH] [--wine PATH] [--helper PATH] [--no-register]\n");
    fprintf(out, "    winecord --version\n");
}

int main(int argc, char **argv) {
    Config cfg;
    if (load_config(&cfg) != 0) return 1;

    if (argc < 2) {
        usage(stdout);
        return 0;
    }

    const char *cmd = argv[1];
    maybe_print_update_notice(&cfg, cmd);

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("WineCord %s\n", WINECORD_VERSION);
        printf("Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n");
        return 0;
    }
    if (strcmp(cmd, "setup") == 0) return setup_all(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "uninstall") == 0) return uninstall_all(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "agent") == 0) return run_agent(&cfg);
    if (strcmp(cmd, "doctor") == 0) return doctor(&cfg);
    if (strcmp(cmd, "steam") == 0) return steam_activity_command(&cfg);
    if (strcmp(cmd, "clear") == 0) return clear_activity_command(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "logs") == 0) return show_logs(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "install-agent") == 0) return install_agent(&cfg);
    if (strcmp(cmd, "uninstall-agent") == 0) return uninstall_agent();
    if (strcmp(cmd, "start") == 0) return start_agent(&cfg);
    if (strcmp(cmd, "stop") == 0) return stop_agent();
    if (strcmp(cmd, "update") == 0) return update_winecord(&cfg, argc - 2, argv + 2);
    if (strcmp(cmd, "install-bottle") == 0 || strcmp(cmd, "install-prefix") == 0) {
        return install_bottle(&cfg, argc - 2, argv + 2);
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    usage(stderr);
    return 1;
}
