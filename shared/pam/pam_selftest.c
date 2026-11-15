/*
 * pam_selftest.c
 *
 * End-to-end test of the PAM module against the daemon-side socket server,
 * with no sensor and no real libpam: the module source is included directly,
 * the few libpam calls it makes are stubbed, and the server runs with fake
 * begin/cancel hooks. Runs as root (the module only talks to a root-owned
 * socket and the server only accepts root).
 *
 *   cc -Wall -Wextra -DHTID_PAM_SOCKET_PATH='"/tmp/htid-pam-test.sock"' \
 *      -DHTID_PAM_USERS_CONF='"/tmp/htid-pam-users.conf"' \
 *      -I<openpam include dir> -Ishared/pam shared/pam/pam_selftest.c \
 *      -o pam_selftest -lpthread && sudo ./pam_selftest
 *
 * On a Mac the system PAM headers are used and scripts/build_pam.sh --test
 * runs this.
 */

#ifndef HTID_PAM_SOCKET_PATH
#define HTID_PAM_SOCKET_PATH "/tmp/htid-pam-test.sock"
#endif
#ifndef HTID_PAM_USERS_CONF
#define HTID_PAM_USERS_CONF "/tmp/htid-pam-users.conf"
#endif

#include "pam_hacktouchid.c"
#include "hack-touchid-pam-server.h"

/* ---- fake libpam ------------------------------------------------------ */

struct pam_handle {
    const char *user;
    const char *tty;
    struct pam_conv conv;
    int info_count;
    char last_info[160];
};

static int fake_conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data) {
    struct pam_handle *h = (struct pam_handle *)data;
    (void)n; (void)resp;
    if (msg[0]->msg_style == PAM_TEXT_INFO) {
        h->info_count++;
        snprintf(h->last_info, sizeof(h->last_info), "%s", msg[0]->msg);
    }
    return PAM_SUCCESS;
}

int pam_get_item(const pam_handle_t *pamh, int item, const void **out) {
    const struct pam_handle *h = (const struct pam_handle *)pamh;
    switch (item) {
        case PAM_USER: *out = h->user; return PAM_SUCCESS;
        case PAM_TTY:  *out = h->tty;  return PAM_SUCCESS;
        case PAM_CONV: *out = &h->conv; return PAM_SUCCESS;
        default: *out = NULL; return PAM_BAD_ITEM;
    }
}

static void init_handle(struct pam_handle *h, const char *user, const char *tty) {
    memset(h, 0, sizeof(*h));
    h->user = user;
    h->tty = tty;
    h->conv.conv = fake_conv;
    h->conv.appdata_ptr = h;
}

/* ---- fake daemon hooks ------------------------------------------------ */

static atomic_int g_begin_calls, g_cancel_calls;
static int g_begin_rc;          /* what begin() returns */
static int g_report_after_ms;   /* report this long after begin(), 0 = never */
static int g_report_value;

static void *reporter(void *arg) {
    (void)arg;
    usleep((useconds_t)g_report_after_ms * 1000);
    htid_pam_report(g_report_value);
    return NULL;
}
static int fake_begin(void) {
    atomic_fetch_add(&g_begin_calls, 1);
    if (g_begin_rc != 0) return g_begin_rc;
    if (g_report_after_ms > 0) {
        pthread_t t;
        pthread_create(&t, NULL, reporter, NULL);
        pthread_detach(t);
    }
    return 0;
}
static void fake_cancel(void) { atomic_fetch_add(&g_cancel_calls, 1); }

/* ---- tiny test runner ------------------------------------------------- */

static int bad = 0;
static void check(const char *name, int ok) {
    printf("%-58s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) bad++;
}

static void write_users(const char *content, mode_t mode) {
    unlink(HTID_PAM_USERS_CONF);
    FILE *f = fopen(HTID_PAM_USERS_CONF, "w");
    if (f) { fputs(content, f); fclose(f); }
    chmod(HTID_PAM_USERS_CONF, mode);
}

static const char *args_t3[] = { "timeout=3" };

int main(void) {
    if (geteuid() != 0) { printf("run as root\n"); return 2; }
    unsetenv("SSH_CONNECTION");
    unsetenv("SSH_TTY");
    unlink(HTID_PAM_SOCKET_PATH);

    struct pam_handle h;
    init_handle(&h, "alice", "/dev/ttys001");
    write_users("# enabled users\nalice\n", 0644);

    check("daemon not running -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);

    htid_pam_hooks_t hooks = { fake_begin, fake_cancel };
    check("server starts", htid_pam_server_start(&hooks) == 0);
    struct stat st;
    check("socket is root only 0600", stat(HTID_PAM_SOCKET_PATH, &st) == 0 && (st.st_mode & 0777) == 0600);

    g_begin_rc = 0; g_report_after_ms = 300; g_report_value = HTID_PAM_RESULT_OK;
    init_handle(&h, "alice", "/dev/ttys001");
    check("match -> PAM_SUCCESS", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_SUCCESS);
    check("user was told to swipe", h.info_count == 1 && strstr(h.last_info, "Swipe") != NULL);

    g_report_value = HTID_PAM_RESULT_FAIL;
    init_handle(&h, "alice", "/dev/ttys001");
    check("no match -> PAM_AUTH_ERR", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_AUTH_ERR);

    g_report_after_ms = 0;
    int cancels = atomic_load(&g_cancel_calls);
    init_handle(&h, "alice", "/dev/ttys001");
    check("no swipe in time -> PAM_AUTHINFO_UNAVAIL", pam_sm_authenticate(&h, 0, 1, args_t3) == PAM_AUTHINFO_UNAVAIL);
    check("timeout cancelled the episode", atomic_load(&g_cancel_calls) == cancels + 1);

    g_begin_rc = 1;
    init_handle(&h, "alice", "/dev/ttys001");
    check("sensor busy or paused -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    g_begin_rc = 0;

    init_handle(&h, "bob", "/dev/ttys001");
    int begins = atomic_load(&g_begin_calls);
    check("user not enabled -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    check("sensor never armed for a denied user", atomic_load(&g_begin_calls) == begins);

    write_users("alice\n", 0666);
    init_handle(&h, "alice", "/dev/ttys001");
    check("world-writable users file -> nobody enabled", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    write_users("alice\n", 0644);

    init_handle(&h, "alice", NULL);
    begins = atomic_load(&g_begin_calls);
    check("no terminal -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    init_handle(&h, "alice", "");
    check("empty terminal -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    init_handle(&h, "bad user\n", "/dev/ttys001");
    check("bad user name -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    setenv("SSH_CONNECTION", "1.2.3.4 5 6.7.8.9 22", 1);
    init_handle(&h, "alice", "/dev/ttys001");
    check("ssh session -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    unsetenv("SSH_CONNECTION");
    check("none of those armed the sensor", atomic_load(&g_begin_calls) == begins);

    /* A second request while one runs gets BUSY, and the module hanging up
     * cancels the running episode. */
    g_report_after_ms = 0;
    cancels = atomic_load(&g_cancel_calls);
    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, HTID_PAM_SOCKET_PATH);
    check("raw client connects", connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    char req[HTID_PAM_MAX_LINE];
    int rn = snprintf(req, sizeof(req), HTID_PAM_REQUEST_FMT, "alice", 30000);
    (void)!write(cfd, req, (size_t)rn);
    char got[64] = {0};
    (void)!read(cfd, got, sizeof(got) - 1);
    check("raw client got SWIPE", strncmp(got, "SWIPE", 5) == 0);
    init_handle(&h, "alice", "/dev/ttys001");
    check("second request while busy -> ignore", pam_sm_authenticate(&h, 0, 0, NULL) == PAM_IGNORE);
    close(cfd);
    usleep(500000);
    check("hang up cancelled the episode", atomic_load(&g_cancel_calls) == cancels + 1);

    /* Garbage request does not arm anything. */
    begins = atomic_load(&g_begin_calls);
    cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    connect(cfd, (struct sockaddr *)&sa, sizeof(sa));
    (void)!write(cfd, "HELLO WORLD\n", 12);
    memset(got, 0, sizeof(got));
    (void)!read(cfd, got, sizeof(got) - 1);
    check("garbage request rejected", strncmp(got, "UNAVAILABLE", 11) == 0 && atomic_load(&g_begin_calls) == begins);
    close(cfd);

    unlink(HTID_PAM_SOCKET_PATH);
    unlink(HTID_PAM_USERS_CONF);
    printf(bad ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", bad);
    return bad ? 1 : 0;
}
