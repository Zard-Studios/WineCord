#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define WINECORD_VERSION "0.1.10"
#define SERVICE_NAME "WineCordBridge"
#define CONFIG_PATH "C:\\users\\Public\\WineCord\\config.ini"
#define LOG_PATH "C:\\users\\Public\\WineCord\\bridge.log"
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 38477
#define PIPE_COUNT 10
#define BUFFER_SIZE 16384

typedef struct {
    char host[64];
    int port;
    char token[129];
} Config;

typedef struct {
    HANDLE pipe;
    SOCKET socket;
    char pipe_name[64];
    LONG logged_client_frame;
    LONG logged_discord_frame;
} BridgePair;

typedef struct {
    int index;
    Config config;
} PipeThreadArgs;

static volatile LONG g_stop = 0;
static SERVICE_STATUS_HANDLE g_service_status = NULL;
static SERVICE_STATUS g_status;

static void log_line(const char *fmt, ...) {
    CreateDirectoryA("C:\\users\\Public\\WineCord", NULL);
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) f = stderr;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u ",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");

    if (f != stderr) fclose(f);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end-- = '\0';
    }
    return s;
}

static void load_config(Config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    lstrcpynA(cfg->host, DEFAULT_HOST, sizeof(cfg->host));
    cfg->port = DEFAULT_PORT;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *p = trim(line);
            if (*p == '\0' || *p == '#') continue;
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *key = trim(p);
            char *value = trim(eq + 1);
            if (strcmp(key, "host") == 0 && *value) {
                lstrcpynA(cfg->host, value, sizeof(cfg->host));
            } else if (strcmp(key, "port") == 0 && *value) {
                cfg->port = atoi(value);
            } else if (strcmp(key, "token") == 0 && *value) {
                lstrcpynA(cfg->token, value, sizeof(cfg->token));
            }
        }
        fclose(f);
    }

    char env[256];
    DWORD n = GetEnvironmentVariableA("WINECORD_AGENT_HOST", env, sizeof(env));
    if (n > 0 && n < sizeof(env)) lstrcpynA(cfg->host, env, sizeof(cfg->host));
    n = GetEnvironmentVariableA("WINECORD_AGENT_PORT", env, sizeof(env));
    if (n > 0 && n < sizeof(env)) cfg->port = atoi(env);
    n = GetEnvironmentVariableA("WINECORD_TOKEN", env, sizeof(env));
    if (n > 0 && n < sizeof(env)) lstrcpynA(cfg->token, env, sizeof(cfg->token));

    if (cfg->port <= 0 || cfg->port > 65535) cfg->port = DEFAULT_PORT;
}

static bool send_all_socket(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool write_all_pipe(HANDLE pipe, const char *buf, DWORD len) {
    DWORD written_total = 0;
    while (written_total < len) {
        DWORD written = 0;
        if (!WriteFile(pipe, buf + written_total, len - written_total, &written, NULL)) return false;
        if (written == 0) return false;
        written_total += written;
    }
    return true;
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
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
    needle[sizeof(needle) - 1] = '\0';
    size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len < payload_len; i++) {
        if (memcmp(payload + i, needle, needle_len) != 0) continue;

        size_t j = i + needle_len;
        while (j < payload_len && payload[j] != ':') j++;
        if (j >= payload_len) return;
        j++;
        while (j < payload_len && (payload[j] == ' ' || payload[j] == '\t' ||
                                   payload[j] == '\r' || payload[j] == '\n')) {
            j++;
        }
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

static void log_ipc_frame(const char *pipe_name, const char *direction,
                          const unsigned char *buf, DWORD n) {
    if (n < 8) {
        log_line("%s %s IPC bytes: %lu (partial frame)", pipe_name, direction, (unsigned long)n);
        return;
    }

    uint32_t opcode = read_le32(buf);
    uint32_t declared_len = read_le32(buf + 4);
    size_t available = n > 8 ? (size_t)n - 8 : 0;
    size_t payload_len = declared_len < available ? declared_len : available;
    const unsigned char *payload = buf + 8;

    char client_id[80];
    char command[80];
    extract_json_string(payload, payload_len, "client_id", client_id, sizeof(client_id));
    extract_json_string(payload, payload_len, "cmd", command, sizeof(command));

    char extra[192];
    extra[0] = '\0';
    if (client_id[0]) {
        _snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " client_id=%s", client_id);
        extra[sizeof(extra) - 1] = '\0';
    }
    if (command[0]) {
        _snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " cmd=%s", command);
        extra[sizeof(extra) - 1] = '\0';
    }
    if ((uint32_t)payload_len < declared_len) {
        _snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), " payload=partial");
        extra[sizeof(extra) - 1] = '\0';
    }

    log_line("%s %s IPC frame: opcode=%lu length=%lu%s",
             pipe_name, direction, (unsigned long)opcode, (unsigned long)declared_len, extra);
}

static SOCKET connect_agent(const Config *cfg) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        log_line("socket failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)cfg->port);
    if (InetPtonA(AF_INET, cfg->host, &addr.sin_addr) != 1) {
        log_line("invalid agent host: %s", cfg->host);
        closesocket(s);
        return INVALID_SOCKET;
    }

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_line("connect %s:%d failed: %d", cfg->host, cfg->port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    char preamble[256];
    _snprintf(preamble, sizeof(preamble), "WINECORD/1 %s\n", cfg->token);
    preamble[sizeof(preamble) - 1] = '\0';
    if (!send_all_socket(s, preamble, (int)strlen(preamble))) {
        log_line("could not send auth preamble");
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

static DWORD WINAPI pipe_to_socket_thread(LPVOID param) {
    BridgePair *pair = (BridgePair *)param;
    char buffer[BUFFER_SIZE];

    for (;;) {
        DWORD read_bytes = 0;
        if (!ReadFile(pair->pipe, buffer, sizeof(buffer), &read_bytes, NULL)) break;
        if (read_bytes == 0) break;
        if (InterlockedCompareExchange(&pair->logged_client_frame, 1, 0) == 0) {
            log_ipc_frame(pair->pipe_name, "client -> Discord", (const unsigned char *)buffer, read_bytes);
        }
        if (!send_all_socket(pair->socket, buffer, (int)read_bytes)) break;
    }

    shutdown(pair->socket, SD_BOTH);
    CancelIo(pair->pipe);
    return 0;
}

static DWORD WINAPI socket_to_pipe_thread(LPVOID param) {
    BridgePair *pair = (BridgePair *)param;
    char buffer[BUFFER_SIZE];

    for (;;) {
        int n = recv(pair->socket, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        if (InterlockedCompareExchange(&pair->logged_discord_frame, 1, 0) == 0) {
            log_ipc_frame(pair->pipe_name, "Discord -> client", (const unsigned char *)buffer, (DWORD)n);
        }
        if (!write_all_pipe(pair->pipe, buffer, (DWORD)n)) break;
    }

    shutdown(pair->socket, SD_BOTH);
    CancelIo(pair->pipe);
    return 0;
}

static void bridge_connection(HANDLE pipe, SOCKET s, const char *pipe_name) {
    BridgePair pair;
    pair.pipe = pipe;
    pair.socket = s;
    lstrcpynA(pair.pipe_name, pipe_name, sizeof(pair.pipe_name));
    pair.logged_client_frame = 0;
    pair.logged_discord_frame = 0;

    HANDLE threads[2];
    threads[0] = CreateThread(NULL, 0, pipe_to_socket_thread, &pair, 0, NULL);
    threads[1] = CreateThread(NULL, 0, socket_to_pipe_thread, &pair, 0, NULL);

    if (!threads[0] || !threads[1]) {
        log_line("CreateThread failed: %lu", GetLastError());
        if (threads[0]) CloseHandle(threads[0]);
        if (threads[1]) CloseHandle(threads[1]);
        return;
    }

    WaitForMultipleObjects(2, threads, FALSE, INFINITE);
    shutdown(s, SD_BOTH);
    CancelIo(pipe);
    CancelSynchronousIo(threads[0]);
    CancelSynchronousIo(threads[1]);
    WaitForMultipleObjects(2, threads, TRUE, 5000);
    CloseHandle(threads[0]);
    CloseHandle(threads[1]);
}

static DWORD WINAPI pipe_listener_thread(LPVOID param) {
    PipeThreadArgs *args = (PipeThreadArgs *)param;
    int index = args->index;
    Config cfg = args->config;
    free(args);

    char pipe_name[64];
    _snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\discord-ipc-%d", index);
    pipe_name[sizeof(pipe_name) - 1] = '\0';

    log_line("listening on %s", pipe_name);

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) {
        HANDLE pipe = CreateNamedPipeA(
            pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            BUFFER_SIZE,
            BUFFER_SIZE,
            0,
            NULL);

        if (pipe == INVALID_HANDLE_VALUE) {
            log_line("CreateNamedPipe(%s) failed: %lu", pipe_name, GetLastError());
            Sleep(2000);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            Sleep(250);
            continue;
        }

        log_line("%s connected", pipe_name);
        SOCKET s = connect_agent(&cfg);
        if (s != INVALID_SOCKET) {
            bridge_connection(pipe, s, pipe_name);
            closesocket(s);
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        log_line("%s disconnected", pipe_name);
    }

    return 0;
}

static int run_bridge(void) {
    Config cfg;
    load_config(&cfg);
    if (cfg.token[0] == '\0') {
        log_line("missing token in %s", CONFIG_PATH);
        return 1;
    }

    WSADATA wsa;
    int wr = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (wr != 0) {
        log_line("WSAStartup failed: %d", wr);
        return 1;
    }

    log_line("WineCord bridge %s starting; agent=%s:%d", WINECORD_VERSION, cfg.host, cfg.port);

    HANDLE threads[PIPE_COUNT];
    memset(threads, 0, sizeof(threads));
    for (int i = 0; i < PIPE_COUNT; i++) {
        PipeThreadArgs *args = (PipeThreadArgs *)calloc(1, sizeof(*args));
        if (!args) continue;
        args->index = i;
        args->config = cfg;
        threads[i] = CreateThread(NULL, 0, pipe_listener_thread, args, 0, NULL);
        if (!threads[i]) {
            log_line("CreateThread listener %d failed: %lu", i, GetLastError());
            free(args);
        }
    }

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) Sleep(1000);

    for (int i = 0; i < PIPE_COUNT; i++) {
        if (threads[i]) CloseHandle(threads[i]);
    }
    WSACleanup();
    log_line("WineCord bridge stopped");
    return 0;
}

static void set_service_status(DWORD state, DWORD win32_exit_code) {
    if (!g_service_status) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_status.dwWin32ExitCode = win32_exit_code;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint = 0;
    g_status.dwWaitHint = 0;
    SetServiceStatus(g_service_status, &g_status);
}

static DWORD WINAPI service_control_handler(DWORD control, DWORD event_type, LPVOID event_data, LPVOID context) {
    (void)event_type;
    (void)event_data;
    (void)context;
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        set_service_status(SERVICE_STOP_PENDING, NO_ERROR);
        InterlockedExchange(&g_stop, 1);
        set_service_status(SERVICE_STOPPED, NO_ERROR);
        return NO_ERROR;
    }
    return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, LPSTR *argv) {
    (void)argc;
    (void)argv;
    g_service_status = RegisterServiceCtrlHandlerExA(SERVICE_NAME, service_control_handler, NULL);
    if (!g_service_status) return;

    set_service_status(SERVICE_START_PENDING, NO_ERROR);
    set_service_status(SERVICE_RUNNING, NO_ERROR);
    int rc = run_bridge();
    set_service_status(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

static int run_service_dispatcher(void) {
    SERVICE_TABLE_ENTRYA table[] = {
        { SERVICE_NAME, service_main },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherA(table)) {
        log_line("StartServiceCtrlDispatcher failed: %lu", GetLastError());
        return 1;
    }
    return 0;
}

static bool wait_service_state(SC_HANDLE svc, DWORD desired_state, DWORD timeout_ms) {
    DWORD waited = 0;
    while (waited <= timeout_ms) {
        SERVICE_STATUS_PROCESS status;
        DWORD needed = 0;
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                                  sizeof(status), &needed)) {
            return false;
        }
        if (status.dwCurrentState == desired_state) return true;
        Sleep(250);
        waited += 250;
    }
    return false;
}

static void stop_service_if_running(SC_HANDLE svc) {
    SERVICE_STATUS_PROCESS status;
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, (LPBYTE)&status,
                              sizeof(status), &needed)) {
        return;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) return;

    SERVICE_STATUS stop_status;
    ControlService(svc, SERVICE_CONTROL_STOP, &stop_status);
    wait_service_state(svc, SERVICE_STOPPED, 10000);
}

static int install_service(void) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }

    char command[MAX_PATH + 32];
    _snprintf(command, sizeof(command), "\"%s\" --service", exe);
    command[sizeof(command) - 1] = '\0';

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    bool existed = false;
    SC_HANDLE svc = CreateServiceA(
        scm,
        SERVICE_NAME,
        "WineCord Discord IPC Bridge",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        command,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL);

    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        existed = true;
        svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    }
    if (!svc) {
        fprintf(stderr, "Create/OpenService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    if (existed) {
        stop_service_if_running(svc);
        if (!ChangeServiceConfigA(
                svc,
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL,
                command,
                NULL,
                NULL,
                NULL,
                NULL,
                NULL,
                "WineCord Discord IPC Bridge")) {
            fprintf(stderr, "ChangeServiceConfig failed: %lu\n", GetLastError());
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return 1;
        }
    }

    SERVICE_DESCRIPTIONA desc;
    desc.lpDescription = "Forwards Discord Rich Presence IPC from Wine games to WineCord on macOS.";
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    if (!StartServiceA(svc, 0, NULL) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        fprintf(stderr, "StartService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    printf("Installed %s service.\n", SERVICE_NAME);
    return 0;
}

static int remove_service(void) {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_STOP | DELETE);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            CloseServiceHandle(scm);
            printf("%s service is not installed.\n", SERVICE_NAME);
            return 0;
        }
        fprintf(stderr, "OpenService failed: %lu\n", err);
        CloseServiceHandle(scm);
        return 1;
    }

    stop_service_if_running(svc);
    if (!DeleteService(svc)) {
        fprintf(stderr, "DeleteService failed: %lu\n", GetLastError());
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return 1;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    printf("Removed %s service.\n", SERVICE_NAME);
    return 0;
}

static void usage(void) {
    printf("WineCord Bridge %s\n", WINECORD_VERSION);
    printf("Created by Zard Studios. Copyright (c) 2026 Zard Studios.\n\n");
    printf("Usage:\n");
    printf("  winecord-bridge.exe             Run in foreground\n");
    printf("  winecord-bridge.exe --install   Install Wine service\n");
    printf("  winecord-bridge.exe --remove    Remove Wine service\n");
    printf("  winecord-bridge.exe --service   Run as Wine service\n");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "--install") == 0) return install_service();
        if (strcmp(argv[1], "--remove") == 0) return remove_service();
        if (strcmp(argv[1], "--service") == 0) return run_service_dispatcher();
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage();
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("WineCord Bridge %s\n", WINECORD_VERSION);
            return 0;
        }
    }

    return run_bridge();
}
