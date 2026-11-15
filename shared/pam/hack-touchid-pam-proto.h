/*
 * hack-touchid-pam-proto.h
 *
 * Wire protocol and small helpers shared by the PAM module
 * (pam_hacktouchid.c, runs inside sudo, su, ...) and the daemon side of the
 * socket (hack-touchid-pam-server.h, runs inside the root fingerprint daemon).
 * Pure POSIX, no macOS-only calls except getpeereid().
 *
 * Conversation, one line per message, "\n" terminated, 256 bytes at most:
 *
 *   module -> daemon   HTID-PAM 1 AUTH <user> <timeout_ms>
 *   daemon -> module   SWIPE          sensor armed, tell the user to swipe
 *   daemon -> module   one final line:
 *                        OK           the finger matched
 *                        FAIL         swipes did not match (daemon gave up)
 *                        TIMEOUT      nobody swiped in time
 *                        BUSY         another episode (lock screen, padlock,
 *                                     another PAM request) owns the sensor
 *                        UNAVAILABLE  switched off, paused, no sensor, no templates
 *                        DENIED       this user is not enabled for PAM
 *
 * The module closing the socket cancels the episode on the daemon side.
 * Anything but OK makes the module fall through to the normal password
 * prompt, so a password always still works.
 *
 * Keep the strings below in sync with pam_hacktouchid.c and the install
 * script hack-touchid-pam-install.sh (socket path and users file).
 */

#ifndef HACK_TOUCHID_PAM_PROTO_H
#define HACK_TOUCHID_PAM_PROTO_H

#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

/* Overridable at compile time, used by the self-test. */
#ifndef HTID_PAM_SOCKET_PATH
#define HTID_PAM_SOCKET_PATH "/var/run/hack-touchid-pam.sock"
#endif
#ifndef HTID_PAM_USERS_CONF
#define HTID_PAM_USERS_CONF "/usr/local/libexec/hack-touchid/pam_users.conf"
#endif

/* The on/off switch (client Settings [P], preference pane). Present = fingerprint
 * for sudo/su is OFF, absent = ON, the same pattern as the scanning_disabled flag.
 * Must match VFS5011_PAM_DISABLED_FLAG_PATH in hack-touchid-menubar-ipc.h. */
#ifndef HTID_PAM_DISABLED_FLAG
#define HTID_PAM_DISABLED_FLAG "/Library/Application Support/VFS5011/pam_disabled"
#endif

#define HTID_PAM_PROTO_VERSION     1
#define HTID_PAM_REQUEST_FMT       "HTID-PAM 1 AUTH %s %d\n"

#define HTID_PAM_MSG_SWIPE         "SWIPE"
#define HTID_PAM_MSG_OK            "OK"
#define HTID_PAM_MSG_FAIL          "FAIL"
#define HTID_PAM_MSG_TIMEOUT       "TIMEOUT"
#define HTID_PAM_MSG_BUSY          "BUSY"
#define HTID_PAM_MSG_UNAVAILABLE   "UNAVAILABLE"
#define HTID_PAM_MSG_DENIED        "DENIED"

#define HTID_PAM_MAX_LINE          256
#define HTID_PAM_USER_MAX          64
#define HTID_PAM_TIMEOUT_MIN_MS    3000
#define HTID_PAM_TIMEOUT_MAX_MS    60000
#define HTID_PAM_TIMEOUT_DEFAULT_MS 15000

/* 1 if the name is safe to put on the wire and into a users file:
 * 1..HTID_PAM_USER_MAX characters of letters, digits, '.', '_', '-', and
 * not starting with '-'. Nothing with spaces or newlines gets through. */
static inline int htid_pam_valid_username(const char *u) {
    if (!u || !*u || u[0] == '-') return 0;
    size_t n = 0;
    for (const char *p = u; *p; p++, n++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || n >= HTID_PAM_USER_MAX) return 0;
    }
    return 1;
}

/* Effective uid of the process on the other end of a connected unix socket.
 * Returns 0 on success. Used on both ends: the daemon only accepts root
 * (sudo and su are setuid root when PAM runs), and the module only talks to
 * a socket that root owns. */
static inline int htid_pam_peer_euid(int fd, uid_t *euid_out) {
#ifdef __APPLE__
    gid_t gid;
    return getpeereid(fd, euid_out, &gid);
#else
    struct htid_ucred { pid_t pid; uid_t uid; gid_t gid; } cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, 17 /* SO_PEERCRED */, &cred, &len) != 0) return -1;
    *euid_out = cred.uid;
    return 0;
#endif
}

#endif /* HACK_TOUCHID_PAM_PROTO_H */
