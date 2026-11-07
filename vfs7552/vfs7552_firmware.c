/*
 * vfs7552_firmware.c
 *
 * See vfs7552_firmware.h for the why/licensing rationale. Structure
 * follows metallica_mis_firmware.c (shell out to curl and the unpacker,
 * print user-facing messages ourselves), plus SHA-256 checks of both the
 * downloaded package and the extracted firmware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>

#include "vfs7552_firmware.h"

#define VFS7552_DRIVER_URL      "https://ftp.hp.com/pub/softpaq/sp135501-136000/sp135737.exe"
#define VFS7552_DRIVER_FILENAME "sp135737.exe"
#define VFS7552_DOWNLOAD_PAGE   "https://support.hp.com/us-en/drivers/hp-elitebook-x360-1030-g2-notebook-pc/14169372"

/* Pinned values, measured on the SoftPaq HP serves (Nov 2026). */
#define VFS7552_DRIVER_SHA256 "27c0093624b76e3f01c14ec751a93065c7a2e53dd3fad26f5e7b2230fe601472"
#define VFS7552_DRIVER_SIZE   7015528L
#define VFS7552_FW_SHA256     "85b946490b7ee2677baf00d53c1791e9d8312e44a556a2ef06100c4f8938ad9a"
#define VFS7552_FW_SIZE       307197L

static void fw_err(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void fw_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void remove_dir(const char *dir) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    run_cmd(cmd);
}

int vfs7552_firmware_path(char *out_path, int out_path_size) {
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') return -1;
    int n = snprintf(out_path, (size_t)out_path_size,
                     "%s/.hack-touchid/firmware/%s", home, VFS7552_FW_FILENAME);
    if (n < 0 || n >= out_path_size) return -1;
    return 0;
}

bool vfs7552_firmware_is_present(void) {
    char path[PATH_MAX];
    if (vfs7552_firmware_path(path, sizeof(path)) != 0) return false;
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return st.st_size == VFS7552_FW_SIZE;
}

/* Prints the lowercase SHA-256 of a file into out (65 bytes). Uses
 * shasum, which ships with macOS. Returns 0 on success. */
static int file_sha256(const char *path, char *out) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "shasum -a 256 '%s' 2>/dev/null", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[256] = {0};
    char *got = fgets(line, sizeof(line), fp);
    pclose(fp);
    if (!got) return -1;
    for (int i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)line[i])) return -1;
        out[i] = (char)tolower((unsigned char)line[i]);
    }
    out[64] = '\0';
    return 0;
}

/* Finds a 7-Zip binary. sudo often strips Homebrew from PATH, so the
 * known Homebrew locations are tried first. Writes the command into out. */
static bool find_7zip(char *out, size_t out_size) {
    static const char *candidates[] = {
        "/usr/local/bin/7zz", "/opt/homebrew/bin/7zz",
        "/usr/local/bin/7z",  "/opt/homebrew/bin/7z",
        "/usr/local/bin/7za", "/opt/homebrew/bin/7za",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], X_OK) == 0) {
            snprintf(out, out_size, "%s", candidates[i]);
            return true;
        }
    }
    static const char *names[] = { "7zz", "7z", "7za" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char probe[64];
        snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", names[i]);
        if (run_cmd(probe) == 0) {
            snprintf(out, out_size, "%s", names[i]);
            return true;
        }
    }
    return false;
}

bool vfs7552_firmware_fetch(void) {
    char fw_dest_path[PATH_MAX];
    if (vfs7552_firmware_path(fw_dest_path, sizeof(fw_dest_path)) != 0) {
        fw_err("Could not resolve $HOME to determine the firmware install path.\n");
        return false;
    }
    if (vfs7552_firmware_is_present()) return true;

    fw_info("Installing firmware from official source (HP SoftPaq SP135737)...\n");

    char sevenzip[PATH_MAX];
    if (!find_7zip(sevenzip, sizeof(sevenzip))) {
        fw_err("7-Zip not found. Install it first:\n");
        fw_err("  brew install sevenzip\n");
        return false;
    }

    char work_dir[] = "/tmp/hack-touchid-fw7552-XXXXXX";
    if (mkdtemp(work_dir) == NULL) {
        fw_err("Could not create a temporary working directory.\n");
        return false;
    }

    char driver_path[PATH_MAX];
    snprintf(driver_path, sizeof(driver_path), "%s/%s", work_dir, VFS7552_DRIVER_FILENAME);

    fw_info("Downloading the driver package from HP...\n");
    char dl_cmd[PATH_MAX + 256];
    snprintf(dl_cmd, sizeof(dl_cmd), "curl -fL --progress-bar -o '%s' '%s'",
             driver_path, VFS7552_DRIVER_URL);
    if (run_cmd(dl_cmd) != 0) {
        fw_err("Download failed. HP may have moved this file.\n");
        fw_err("Get SP135737 manually (the Synaptics fingerprint driver for the\n");
        fw_err("HP EliteBook x360 1030 G2) from:\n");
        fw_err("  %s\n", VFS7552_DRIVER_URL);
        fw_err("or the HP support page:\n");
        fw_err("  %s\n", VFS7552_DOWNLOAD_PAGE);
        fw_err("then unpack it with 7-Zip and copy %s to:\n", VFS7552_FW_FILENAME);
        fw_err("  %s\n", fw_dest_path);
        remove_dir(work_dir);
        return false;
    }

    char sum[65] = {0};
    struct stat st;
    long dl_size = (stat(driver_path, &st) == 0) ? (long)st.st_size : -1;
    if (file_sha256(driver_path, sum) != 0) {
        fw_err("Could not compute a SHA-256 of the download (is shasum available?).\n");
        remove_dir(work_dir);
        return false;
    }
    if (strcmp(sum, VFS7552_DRIVER_SHA256) != 0) {
        fw_err("The downloaded package does not match the expected hash, not using it.\n");
        fw_err("  expected: %s (%ld bytes)\n", VFS7552_DRIVER_SHA256, (long)VFS7552_DRIVER_SIZE);
        fw_err("  got:      %s (%ld bytes)\n", sum, dl_size);
        fw_err("HP may have replaced this SoftPaq. Please report this on the issue tracker.\n");
        remove_dir(work_dir);
        return false;
    }
    fw_info("Download verified (SHA-256 matches).\n");

    fw_info("Extracting %s...\n", VFS7552_FW_FILENAME);
    char extract_cmd[PATH_MAX * 3];
    snprintf(extract_cmd, sizeof(extract_cmd), "cd '%s' && '%s' x -y -oextracted '%s' >/dev/null",
             work_dir, sevenzip, VFS7552_DRIVER_FILENAME);
    if (run_cmd(extract_cmd) != 0) {
        fw_err("7-Zip failed to unpack the driver package.\n");
        remove_dir(work_dir);
        return false;
    }

    char find_cmd[PATH_MAX * 2];
    snprintf(find_cmd, sizeof(find_cmd), "find '%s/extracted' -name '%s' -type f 2>/dev/null | head -n1",
             work_dir, VFS7552_FW_FILENAME);
    FILE *fp = popen(find_cmd, "r");
    char extracted_path[PATH_MAX] = {0};
    if (fp) {
        if (fgets(extracted_path, sizeof(extracted_path), fp)) {
            size_t len = strlen(extracted_path);
            if (len > 0 && extracted_path[len - 1] == '\n') extracted_path[len - 1] = '\0';
        }
        pclose(fp);
    }
    if (extracted_path[0] == '\0') {
        fw_err("Extraction ran, but %s was not found in the package.\n", VFS7552_FW_FILENAME);
        remove_dir(work_dir);
        return false;
    }

    char fw_sum[65] = {0};
    long fw_size = (stat(extracted_path, &st) == 0) ? (long)st.st_size : -1;
    if (file_sha256(extracted_path, fw_sum) != 0 || strcmp(fw_sum, VFS7552_FW_SHA256) != 0 ||
        fw_size != VFS7552_FW_SIZE) {
        fw_err("The extracted firmware does not match the expected hash or size, not installing it.\n");
        fw_err("  expected: %s (%ld bytes)\n", VFS7552_FW_SHA256, (long)VFS7552_FW_SIZE);
        fw_err("  got:      %s (%ld bytes)\n", fw_sum[0] ? fw_sum : "(unreadable)", fw_size);
        remove_dir(work_dir);
        return false;
    }
    fw_info("Extracted: %ld bytes, SHA-256 matches.\n", fw_size);

    char fw_dest_dir[PATH_MAX];
    snprintf(fw_dest_dir, sizeof(fw_dest_dir), "%s", fw_dest_path);
    char *slash = strrchr(fw_dest_dir, '/');
    if (slash) *slash = '\0';

    char mkdir_cmd[PATH_MAX + 16];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p '%s'", fw_dest_dir);
    run_cmd(mkdir_cmd);

    char cp_cmd[PATH_MAX * 2 + 16];
    snprintf(cp_cmd, sizeof(cp_cmd), "cp '%s' '%s'", extracted_path, fw_dest_path);
    bool ok = (run_cmd(cp_cmd) == 0);
    remove_dir(work_dir);

    if (!ok) {
        fw_err("Failed to install the firmware to %s\n", fw_dest_path);
        return false;
    }
    fw_info("Firmware installed: %s\n", fw_dest_path);
    return true;
}
