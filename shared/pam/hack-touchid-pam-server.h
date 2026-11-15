/*
 * hack-touchid-pam-server.h
 *
 * Daemon-side half of the PAM fingerprint socket (v2.1.0). #include this from
 * vfs5011_daemon.c (and any other daemon that owns a sensor) and call
 * htid_pam_server_start() once at startup. Header-only, pure POSIX, so it can
 * be tested away from a Mac (see pam_selftest.c).
 *
 * What it does: listens on HTID_PAM_SOCKET_PATH (root only, mode 0600). For
 * each connection it checks the peer is root, parses one request line, checks
 * the user is listed in HTID_PAM_USERS_CONF, then asks the daemon (through the
 * hooks below) to arm the sensor for one episode and waits for the verdict,
 * which the daemon delivers with htid_pam_report(). It never sees a finger,
 * a template or a password.
 *
 * The daemon supplies two hooks and one report call:
 *   begin()   arm the sensor for one PAM episode. 0 = armed, non-zero = could
 *             not (busy, paused, no sensor). Called from a server thread.
 *   cancel()  stop that episode now (timeout or the module hung up).
 *   htid_pam_report(HTID_PAM_RESULT_OK or _FAIL) from the daemon's polling
 *             thread when the episode ends by itself.
 * Only one episode runs at a time, a second request gets BUSY.
 *
 * Protocol and constants: hack-touchid-pam-proto.h.
 */

#ifndef HACK_TOUCHID_PAM_SERVER_H
#define HACK_TOUCHID_PAM_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hack-touchid-pam-proto.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    HTID_PAM_PENDING = 0,
    HTID_PAM_RESULT_OK = 1,
    HTID_PAM_RESULT_FAIL = 2
};

typedef struct {
    int  (*begin)(void);
    void (*cancel)(void);
} htid_pam_hooks_t;

static atomic_int g_htid_pam_active = 0;  /* an episode is running */
static atomic_int g_htid_pam_result = HTID_PAM_PENDING;
static pthread_mutex_t g_htid_pam_episode_lock = PTHREAD_MUTEX_INITIALIZER;
static htid_pam_hooks_t g_htid_pam_hooks;
static int g_htid_pam_listen_fd = -1;

/* Called by the daemon when the PAM episode ends on its own. Ignored when no
 * episode is running, so a late report can never leak into the next one. */
static inline void htid_pam_report(int result) {
    if (!atomic_load(&g_htid_pam_active)) return;
    int expected = HTID_PAM_PENDING;
    atomic_compare_exchange_strong(&g_htid_pam_result, &expected, result);
}

static inline long long htid_pam_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void htid_pam_send_line(int fd, const char *line) {
    char out[HTID_PAM_MAX_LINE];
    int n = snprintf(out, sizeof(out), "%s\n", line);
    if (n > 0) (void)send(fd, out, (size_t)n, MSG_NOSIGNAL);
}

/* 1 if the user is listed in the users file. The file must be a regular file
 * owned by root and not writable by group or others, otherwise nobody is
 * enabled (a writable users file would let any user enable themselves). */
static inline int htid_pam_user_enabled(const char *user) {
    int fd = open(HTID_PAM_USERS_CONF, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(fd);
        return 0;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    char line[HTID_PAM_USER_MAX + 8];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        if (strcmp(line, user) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

/* Reads the request line, up to HTID_PAM_MAX_LINE bytes, 2 s at most. */
static inline int htid_pam_read_request(int fd, char *line, size_t size) {
    size_t have = 0;
    long long deadline = htid_pam_now_ms() + 2000;
    while (have + 1 < size) {
        long long left = deadline - htid_pam_now_ms();
        if (left <= 0) return -1;
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, (int)left);
        if (pr < 0) { if (errno == EINTR) continue; return -1; }
        if (pr == 0) return -1;
        ssize_t n = read(fd, line + have, 1);
        if (n <= 0) return -1;
        if (line[have] == '\n') { line[have] = '\0'; return 0; }
        have++;
    }
    return -1;
}

static inline void *htid_pam_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    uid_t peer = 1;
    if (htid_pam_peer_euid(fd, &peer) != 0 || peer != 0) { close(fd); return NULL; }

    char line[HTID_PAM_MAX_LINE];
    if (htid_pam_read_request(fd, line, sizeof(line)) != 0) { close(fd); return NULL; }

    int ver = 0, timeout_ms = 0;
    char verb[8], user[HTID_PAM_USER_MAX + 2];
    if (sscanf(line, "HTID-PAM %d %7s %65s %d", &ver, verb, user, &timeout_ms) != 4 ||
        ver != HTID_PAM_PROTO_VERSION || strcmp(verb, "AUTH") != 0 ||
        !htid_pam_valid_username(user)) {
        htid_pam_send_line(fd, HTID_PAM_MSG_UNAVAILABLE);
        close(fd);
        return NULL;
    }
    if (timeout_ms < HTID_PAM_TIMEOUT_MIN_MS) timeout_ms = HTID_PAM_TIMEOUT_MIN_MS;
    if (timeout_ms > HTID_PAM_TIMEOUT_MAX_MS) timeout_ms = HTID_PAM_TIMEOUT_MAX_MS;

    /* Switched off from the client or the preference pane. */
    struct stat flag_st;
    if (stat(HTID_PAM_DISABLED_FLAG, &flag_st) == 0) {
        htid_pam_send_line(fd, HTID_PAM_MSG_UNAVAILABLE);
        close(fd);
        return NULL;
    }
    if (!htid_pam_user_enabled(user)) {
        htid_pam_send_line(fd, HTID_PAM_MSG_DENIED);
        close(fd);
        return NULL;
    }
    if (pthread_mutex_trylock(&g_htid_pam_episode_lock) != 0) {
        htid_pam_send_line(fd, HTID_PAM_MSG_BUSY);
        close(fd);
        return NULL;
    }

    atomic_store(&g_htid_pam_result, HTID_PAM_PENDING);
    atomic_store(&g_htid_pam_active, 1);
    if (!g_htid_pam_hooks.begin || g_htid_pam_hooks.begin() != 0) {
        atomic_store(&g_htid_pam_active, 0);
        pthread_mutex_unlock(&g_htid_pam_episode_lock);
        htid_pam_send_line(fd, HTID_PAM_MSG_BUSY);
        close(fd);
        return NULL;
    }
    htid_pam_send_line(fd, HTID_PAM_MSG_SWIPE);

    long long deadline = htid_pam_now_ms() + timeout_ms;
    int result = HTID_PAM_PENDING;
    int aborted = 0;
    for (;;) {
        result = atomic_load(&g_htid_pam_result);
        if (result != HTID_PAM_PENDING) break;
        if (htid_pam_now_ms() >= deadline) break;
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, 100);
        if (pr > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[16];
            ssize_t n = read(fd, tmp, sizeof(tmp));
            if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) { aborted = 1; break; }
        }
    }

    if (result == HTID_PAM_RESULT_OK) {
        htid_pam_send_line(fd, HTID_PAM_MSG_OK);
    } else if (result == HTID_PAM_RESULT_FAIL) {
        htid_pam_send_line(fd, HTID_PAM_MSG_FAIL);
    } else {
        if (g_htid_pam_hooks.cancel) g_htid_pam_hooks.cancel();
        if (!aborted) htid_pam_send_line(fd, HTID_PAM_MSG_TIMEOUT);
    }
    atomic_store(&g_htid_pam_active, 0);
    pthread_mutex_unlock(&g_htid_pam_episode_lock);
    close(fd);
    return NULL;
}

static inline void *htid_pam_accept_thread(void *arg) {
    (void)arg;
    for (;;) {
        int cfd = accept(g_htid_pam_listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;
            return NULL;
        }
        fcntl(cfd, F_SETFD, FD_CLOEXEC);
        pthread_t t;
        if (pthread_create(&t, NULL, htid_pam_client_thread, (void *)(intptr_t)cfd) != 0) {
            close(cfd);
            continue;
        }
        pthread_detach(t);
    }
}

/* Creates the socket (root only, 0600) and starts the accept thread.
 * Returns 0 on success, -1 on failure (the daemon keeps running without PAM). */
static inline int htid_pam_server_start(const htid_pam_hooks_t *hooks) {
    if (hooks) g_htid_pam_hooks = *hooks;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(HTID_PAM_SOCKET_PATH) >= sizeof(sa.sun_path)) { close(fd); return -1; }
    strcpy(sa.sun_path, HTID_PAM_SOCKET_PATH);
    unlink(HTID_PAM_SOCKET_PATH);

    mode_t old_umask = umask(0177);
    int br = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old_umask);
    if (br != 0) { close(fd); return -1; }
    chmod(HTID_PAM_SOCKET_PATH, 0600);
    if (listen(fd, 8) != 0) { close(fd); unlink(HTID_PAM_SOCKET_PATH); return -1; }

    g_htid_pam_listen_fd = fd;
    pthread_t t;
    if (pthread_create(&t, NULL, htid_pam_accept_thread, NULL) != 0) {
        close(fd);
        unlink(HTID_PAM_SOCKET_PATH);
        g_htid_pam_listen_fd = -1;
        return -1;
    }
    pthread_detach(t);
    return 0;
}

#endif /* HACK_TOUCHID_PAM_SERVER_H */
