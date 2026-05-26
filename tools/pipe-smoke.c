#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PIPE_NAME "\\\\.\\pipe\\discord-ipc-0"

static int write_all(HANDLE h, const void *buf, DWORD len) {
    const char *p = (const char *)buf;
    DWORD total = 0;
    while (total < len) {
        DWORD written = 0;
        if (!WriteFile(h, p + total, len - total, &written, NULL)) return 1;
        if (written == 0) return 1;
        total += written;
    }
    return 0;
}

static int read_all(HANDLE h, void *buf, DWORD len) {
    char *p = (char *)buf;
    DWORD total = 0;
    while (total < len) {
        DWORD got = 0;
        if (!ReadFile(h, p + total, len - total, &got, NULL)) return 1;
        if (got == 0) return 1;
        total += got;
    }
    return 0;
}

int main(void) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 50; i++) {
        pipe = CreateFileA(PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "CreateFile(%s) failed: %lu\n", PIPE_NAME, GetLastError());
            return 1;
        }
        Sleep(200);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Pipe did not become available: %s\n", PIPE_NAME);
        return 1;
    }

    const char payload[] = "{\"v\":1,\"client_id\":\"123456789012345678\"}";
    uint32_t header[2];
    header[0] = 0;
    header[1] = (uint32_t)strlen(payload);

    if (write_all(pipe, header, sizeof(header)) || write_all(pipe, payload, (DWORD)strlen(payload))) {
        fprintf(stderr, "Failed to write handshake: %lu\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }

    uint32_t response_header[2];
    if (read_all(pipe, response_header, sizeof(response_header))) {
        fprintf(stderr, "Failed to read response header: %lu\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }

    DWORD len = response_header[1];
    if (len > 4096) len = 4096;
    char body[4097];
    if (read_all(pipe, body, len)) {
        fprintf(stderr, "Failed to read response body: %lu\n", GetLastError());
        CloseHandle(pipe);
        return 1;
    }
    body[len] = '\0';

    printf("opcode=%lu length=%lu\n", (unsigned long)response_header[0], (unsigned long)response_header[1]);
    printf("%s\n", body);
    CloseHandle(pipe);
    return 0;
}
