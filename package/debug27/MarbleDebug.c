// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only kernel diagnostics over the device loopback, reached via usbmuxd.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MARBLE_DEBUG_SOURCE
#define MARBLE_DEBUG_SOURCE "unrecorded"
#endif
#define PORT 23456
#define MAX_LOG (16u << 20)
#ifndef MARBLE_DEBUG_AUTH
#define MARBLE_DEBUG_AUTH ""
#endif
extern int marble_copy_unified_log(void **, size_t *);
extern int marble_copy_boot_network_log(void **, size_t *);
extern void marble_start_boot_network_log(void);
extern int marble_register_webkit_extensions(void);

static int send_all(int fd, const void *data, size_t bytes)
{
    const unsigned char *at = data;
    while (bytes) {
        ssize_t wrote = send(fd, at, bytes, 0);
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) return -1;
        at += wrote;
        bytes -= (size_t)wrote;
    }
    return 0;
}

static void respond(int fd, unsigned status, const void *data, size_t size)
{
    char header[256];
    int count = snprintf(header, sizeof(header),
        "HTTP/1.0 %u\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, size);
    if (count > 0 && (size_t)count < sizeof(header) && !send_all(fd, header, (size_t)count))
        (void)send_all(fd, data, size);
}

static uint64_t number(const char *name)
{
    uint64_t value = 0;
    size_t bytes = sizeof(value);
    if (sysctlbyname(name, &value, &bytes, NULL, 0)) return 0;
    return value;
}

static void serve(int fd, int local)
{
    struct timeval timeout = {10, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[512];
    size_t received = 0;
    while (received + 1 < sizeof(request)) {
        ssize_t count = recv(fd, request + received, sizeof(request) - received - 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return;
        received += (size_t)count;
        request[received] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }
    const char expected[] = "\r\nAuthorization: Bearer " MARBLE_DEBUG_AUTH "\r\n";
    const int authenticated = MARBLE_DEBUG_AUTH[0] && strstr(request, expected);
    const char unified[] = "GET /unified.log HTTP/1.";
    const char registration[] = "GET /webkit-status.json HTTP/1.";
    const char network[] = "GET /network-boot.log HTTP/1.";
    if ((!local || !strncmp(request, unified, sizeof(unified)-1) || !strncmp(request, registration, sizeof(registration)-1) ||
         !strncmp(request, network, sizeof(network)-1)) && !authenticated) {
        const char message[] = "Authentication required\n";
        respond(fd, 403, message, sizeof(message)-1); return;
    }
    if (!strncmp(request, unified, sizeof(unified)-1)) {
        void *data = NULL; size_t size = 0;
        int error = marble_copy_unified_log(&data, &size);
        if (data) {respond(fd, error ? 503 : 200, data, size); free(data);}
        else {const char message[] = "Log allocation failed\n"; respond(fd, 503, message, sizeof(message)-1);}
        return;
    }
    if (!strncmp(request, network, sizeof(network)-1)) {
        void *data = NULL; size_t size = 0;
        int error = marble_copy_boot_network_log(&data, &size);
        if (data) {respond(fd, 200, data, size); free(data);}
        else {const char message[] = "Boot network log unavailable\n"; respond(fd, error == ENOENT ? 404 : 503, message, sizeof(message)-1);}
        return;
    }
    if (!strncmp(request, registration, sizeof(registration)-1)) {
        const char *path = "/var/mobile/Library/Logs/UniXNU-webkit-registration.json";
        FILE *stream = fopen(path, "rb");
        if (!stream) { const char text[]="Registration report pending\n"; respond(fd,404,text,sizeof(text)-1); return; }
        char *data = malloc(1u<<20);
        if (!data) { fclose(stream); return; }
        size_t bytes = fread(data,1,1u<<20,stream); int error = ferror(stream); fclose(stream);
        respond(fd,error ? 503 : 200,data,bytes); free(data); return;
    }
    if (!strncmp(request, "GET /kernel.log HTTP/1.", 22)) {
        size_t bytes = 0;
        if (sysctlbyname("kern.marble_log", NULL, &bytes, NULL, 0) || !bytes || bytes > MAX_LOG) {
            const char message[] = "kern.marble_log query failed\n";
            respond(fd, 503, message, sizeof(message) - 1);
            return;
        }
        void *data = malloc(bytes);
        if (!data) return;
        if (sysctlbyname("kern.marble_log", data, &bytes, NULL, 0)) {
            const char message[] = "kern.marble_log read failed\n";
            respond(fd, 503, message, sizeof(message) - 1);
        } else respond(fd, 200, data, bytes);
        free(data);
    } else if (!strncmp(request, "GET /status HTTP/1.", 18)) {
        char status[512], build[64] = {0}, version[256] = {0};
        size_t bytes = sizeof(build);
        (void)sysctlbyname("kern.osversion", build, &bytes, NULL, 0);
        bytes = sizeof(version);
        (void)sysctlbyname("kern.version", version, &bytes, NULL, 0);
        build[sizeof(build)-1] = version[sizeof(version)-1] = 0;
        // Kernel strings are printable OS data; delimiters are normalized for JSON.
        for (char *p=version; *p; ++p) if (*p<' ' || *p=='"' || *p=='\\') *p=' ';
        int count = snprintf(status, sizeof(status),
            "{\"protocol\":1,\"target_build\":\"24B119\",\"kernel_build\":\"%s\","
            "\"source\":\"%s\",\"cpus\":%llu,\"memsize\":%llu,\"kernel\":\"%s\"}\n",
            build, MARBLE_DEBUG_SOURCE, (unsigned long long)number("hw.ncpu"),
            (unsigned long long)number("hw.memsize"), version);
        if (count > 0 && (size_t)count < sizeof(status)) respond(fd, 200, status, (size_t)count);
    } else {
        const char message[] = "GET /status or GET /kernel.log\n";
        respond(fd, 404, message, sizeof(message) - 1);
    }
}

int main(int argc, const char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--register-webkit")) return marble_register_webkit_extensions();
    if (argc != 1) return 64;
    signal(SIGPIPE, SIG_IGN);
    marble_start_boot_network_log();
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    int yes = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {.sin_len=sizeof(address), .sin_family=AF_INET,
                                 .sin_port=htons(PORT), .sin_addr={.s_addr=htonl(MARBLE_DEBUG_AUTH[0] ? INADDR_ANY : INADDR_LOOPBACK)}};
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 4)) return 2;
    for (;;) {
        struct sockaddr_in peer = {0}; socklen_t peer_size = sizeof(peer);
        int fd = accept(listener, (struct sockaddr *)&peer, &peer_size);
        if (fd < 0 && errno == EINTR) continue;
        if (fd < 0) return 3;
        serve(fd, peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        close(fd);
    }
}
