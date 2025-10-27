// common.h  (compartido cliente/servidor)
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <semaphore.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdbool.h>

#define SERVER_IP      "127.0.0.1"
#define SERVER_PORT    12345
#define BACKLOG        16
#define SEM_NAME       "/p2_so_ready_sem"

#define MAX_LINE       4096
#define MAX_REPLY      8192

static inline ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n; ssize_t r; char *p = (char*)buf;
    while (left > 0) {
        r = read(fd, p, left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) break;
        left -= r; p += r;
    }
    return (ssize_t)(n - left);
}

static inline ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n; ssize_t w; const char *p = (const char*)buf;
    while (left > 0) {
        w = write(fd, p, left);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        left -= w; p += w;
    }
    return (ssize_t)n;
}

static inline void rstrip_newline(char *s) {
    if (!s) return;
    size_t L = strlen(s);
    while (L > 0 && (s[L-1] == '\n' || s[L-1] == '\r')) s[--L] = '\0';
}

static inline int read_menu_option(void) {
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return -1;
    rstrip_newline(line);
    if (!*line) return -1;
    for (size_t i=0;i<strlen(line);++i) if (!isdigit((unsigned char)line[i])) return -1;
    int v = atoi(line);
    if (v < 1 || v > 4) return -1;
    return v;
}
#endif
