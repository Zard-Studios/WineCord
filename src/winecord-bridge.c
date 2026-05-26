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
#include <string.h>

#define WINECORD_VERSION "0.1.0"
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
        if (!write_all_pipe(pair->pipe, buffer, (DWORD)n)) break;
    }

    shutdown(pair->socket, SD_BOTH);
    CancelIo(pair->pipe);
    return 0;
}

static void bridge_connection(HANDLE pipe, SOCKET s) {
    BridgePair pair;
    pair.pipe = pipe;
    pair.socket = s;

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
            bridge_connection(pipe, s);
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

static int install_service(void) {
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n", GetLastError());
        return 1;
    }

    char command[MAX_PATH + 32];
    _snprintf(command, sizeof(command), "\"%s\" --service", exe);
    command[sizeof(command) - 1] = '\0';

    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

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
        svc = OpenServiceA(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    }
    if (!svc) {
        fprintf(stderr, "Create/OpenService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_DESCRIPTIONA desc;
    desc.lpDescription = "Forwards Discord Rich Presence IPC from Wine/CrossOver games to WineCord on macOS.";
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    StartServiceA(svc, 0, NULL);
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
        fprintf(stderr, "OpenService failed: %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }

    SERVICE_STATUS status;
    ControlService(svc, SERVICE_CONTROL_STOP, &status);
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
