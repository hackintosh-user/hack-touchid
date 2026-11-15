/*
 * pam_hacktouchid.c
 *
 * PAM auth module for Hackintosh-TouchID (v2.1.0): lets a fingerprint stand in
 * for the password in sudo, su and any other PAM-aware command in any
 * terminal. It never touches the sensor. It asks the root fingerprint daemon
 * over a root-only unix socket (see hack-touchid-pam-proto.h) and reports the
 * daemon's verdict to PAM.
 *
 * pam.d line (written by hack-touchid-pam-install.sh into
 * /etc/pam.d/sudo_local, which survives macOS updates):
 *
 *   auth       sufficient     /usr/local/lib/pam/pam_hacktouchid.so
 *
 * Options: timeout=<seconds> (3..60, default 15), allow_ssh, debug.
 *
 * Fail-open to the password, never fail-open to access: every result except
 * an explicit OK from the daemon leaves the stack to the next module, so the
 * normal password prompt appears. The module is skipped (PAM_IGNORE) when
 * there is no terminal (sudo -n, cron, scripts) and, unless allow_ssh is
 * given, inside an SSH session, because the finger has to be on the sensor
 * of the machine itself.
 *
 * Build: scripts/build_pam.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define PAM_SM_AUTH
#include <security/pam_appl.h>
#include <security/pam_modules.h>

#include "hack-touchid-pam-proto.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
    int timeout_ms;
    int allow_ssh;
    int debug;
} opts_t;

static void parse_opts(opts_t *o, int argc, const char **argv) {
    o->timeout_ms = HTID_PAM_TIMEOUT_DEFAULT_MS;
    o->allow_ssh = 0;
    o->debug = 0;
    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "timeout=", 8) == 0) {
            int s = atoi(argv[i] + 8);
            if (s * 1000 < HTID_PAM_TIMEOUT_MIN_MS) s = HTID_PAM_TIMEOUT_MIN_MS / 1000;
            if (s * 1000 > HTID_PAM_TIMEOUT_MAX_MS) s = HTID_PAM_TIMEOUT_MAX_MS / 1000;
            o->timeout_ms = s * 1000;
        } else if (strcmp(argv[i], "allow_ssh") == 0) {
            o->allow_ssh = 1;
        } else if (strcmp(argv[i], "debug") == 0) {
            o->debug = 1;
        }
    }
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* One informational line shown by sudo/su; failures to show it are ignored. */
static void say(pam_handle_t *pamh, const char *text) {
    const void *item = NULL;
    if (pam_get_item(pamh, PAM_CONV, &item) != PAM_SUCCESS || !item) return;
    const struct pam_conv *conv = (const struct pam_conv *)item;
    if (!conv->conv) return;
    char copy[160]; /* OpenPAM wants a writable char *, Linux-PAM a const one */
    snprintf(copy, sizeof(copy), "%s", text);
    struct pam_message msg;
    msg.msg_style = PAM_TEXT_INFO;
    msg.msg = copy;
    const struct pam_message *msgp = &msg;
    struct pam_response *resp = NULL;
    conv->conv(1, &msgp, &resp, conv->appdata_ptr);
    if (resp) {
        free(resp->resp);
        free(resp);
    }
}

static int connect_daemon(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(HTID_PAM_SOCKET_PATH) >= sizeof(sa.sun_path)) { close(fd); return -1; }
    strcpy(sa.sun_path, HTID_PAM_SOCKET_PATH);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    /* Only talk to a daemon that runs as root. */
    uid_t peer = 1;
    if (htid_pam_peer_euid(fd, &peer) != 0 || peer != 0) { close(fd); return -1; }
    return fd;
}

/* Reads lines from the daemon until a final verdict, the deadline, or EOF.
 * Returns the final line in verdict (without the newline), or an empty string
 * on timeout / EOF / error. */
static void read_verdict(pam_handle_t *pamh, int fd, long long deadline_ms,
                         char *verdict, size_t verdict_size) {
    char buf[HTID_PAM_MAX_LINE * 2] = {0};
    size_t have = 0;
    verdict[0] = '\0';
    for (;;) {
        char *nl = memchr(buf, '\n', have);
        while (nl) {
            *nl = '\0';
            if (strcmp(buf, HTID_PAM_MSG_SWIPE) == 0) {
                say(pamh, "Swipe your finger on the sensor, or wait for the password prompt.");
            } else {
                snprintf(verdict, verdict_size, "%s", buf);
                return;
            }
            size_t used = (size_t)(nl - buf) + 1;
            memmove(buf, buf + used, have - used);
            have -= used;
            nl = memchr(buf, '\n', have);
        }
        if (have >= sizeof(buf) - 1) return; /* oversized line, treat as no answer */
        long long left = deadline_ms - now_ms();
        if (left <= 0) return;
        struct pollfd p = { fd, POLLIN, 0 };
        int pr = poll(&p, 1, (int)(left > 1000 ? 1000 : left));
        if (pr < 0) { if (errno == EINTR) continue; return; }
        if (pr == 0) continue;
        ssize_t n = read(fd, buf + have, sizeof(buf) - 1 - have);
        if (n <= 0) return; /* daemon closed or error */
        have += (size_t)n;
    }
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)flags;
    opts_t o;
    parse_opts(&o, argc, argv);

    /* No terminal (sudo -n, cron, scripts): nobody to swipe for. */
    const void *item = NULL;
    if (pam_get_item(pamh, PAM_TTY, &item) != PAM_SUCCESS || !item || !*(const char *)item)
        return PAM_IGNORE;

    if (!o.allow_ssh && (getenv("SSH_CONNECTION") || getenv("SSH_TTY")))
        return PAM_IGNORE;

    const char *user = NULL;
    item = NULL;
    if (pam_get_item(pamh, PAM_USER, &item) != PAM_SUCCESS || !item) return PAM_IGNORE;
    user = (const char *)item;
    if (!htid_pam_valid_username(user)) return PAM_IGNORE;

    int fd = connect_daemon();
    if (fd < 0) return PAM_IGNORE; /* daemon not running: plain password */

    char req[HTID_PAM_MAX_LINE];
    int rn = snprintf(req, sizeof(req), HTID_PAM_REQUEST_FMT, user, o.timeout_ms);
    if (rn <= 0 || (size_t)rn >= sizeof(req) ||
        send(fd, req, (size_t)rn, MSG_NOSIGNAL) != (ssize_t)rn) {
        close(fd);
        return PAM_IGNORE;
    }

    char verdict[HTID_PAM_MAX_LINE];
    /* The daemon enforces the timeout itself, the extra margin only covers a
     * daemon that has hung. */
    read_verdict(pamh, fd, now_ms() + o.timeout_ms + 3000, verdict, sizeof(verdict));
    close(fd); /* also cancels the episode if it is still running */

    if (o.debug) fprintf(stderr, "pam_hacktouchid: daemon verdict '%s'\n", verdict);

    if (strcmp(verdict, HTID_PAM_MSG_OK) == 0) return PAM_SUCCESS;
    if (strcmp(verdict, HTID_PAM_MSG_FAIL) == 0) {
        say(pamh, "Fingerprint not recognised, use your password.");
        return PAM_AUTH_ERR;
    }
    if (strcmp(verdict, HTID_PAM_MSG_BUSY) == 0 ||
        strcmp(verdict, HTID_PAM_MSG_UNAVAILABLE) == 0 ||
        strcmp(verdict, HTID_PAM_MSG_DENIED) == 0)
        return PAM_IGNORE;
    return PAM_AUTHINFO_UNAVAIL; /* timeout, daemon gone, garbage */
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}
