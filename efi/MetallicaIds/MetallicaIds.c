/*
 * MetallicaIds.c
 *
 * UEFI boot service driver for OpenCore's Drivers folder.
 *
 * Why: the Synaptics Metallica MIS / Prometheus sensors (06cb:009a, 138a:0097,
 * 138a:009d) check the host's SMBIOS system product name and serial number
 * when a session is opened. If Windows or Linux paired the sensor first, HTID
 * must use those same two strings. On macOS they cannot be read, because
 * OpenCore spoofs the SMBIOS. This driver reads them from the firmware's own
 * SMBIOS table (type 1) while it is still unpatched and writes them to the
 * EFI System Partition as Metallica-Ids.txt.
 *
 * It does nothing else: no NVRAM writes, no variables, no network, and it
 * only ever touches that one file on the volume it was loaded from.
 *
 * Rules the driver follows:
 *   - Output is plain key=value lines (see build_output()).
 *   - Strings are trimmed of trailing spaces and control characters only.
 *   - looks_spoofed=yes when the strings look like an Apple SMBIOS (the
 *     driver ran AFTER OpenCore patched the table, so the values are useless).
 *   - A file that holds real values (looks_spoofed=no) is never overwritten
 *     by spoofed ones. An identical file is not rewritten.
 *   - It always returns EFI_SUCCESS so it can never block boot.
 *
 * Build: see Makefile (gnu-efi). Install: README.md.
 */

#include <efi.h>
#include <efilib.h>

#define OUT_NAME   L"Metallica-Ids.txt"
#define STR_MAX    96
#define OUT_MAX    1024

static EFI_GUID gSmbios  = { 0xeb9d2d31, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static EFI_GUID gSmbios3 = { 0xf2fd1544, 0x9794, 0x4a2c, { 0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94 } };

typedef struct {
    char manufacturer[STR_MAX];
    char product_name[STR_MAX];
    char product_version[STR_MAX];
    char product_serial[STR_MAX];
} sysinfo_t;

/* ---- small freestanding string helpers (no libc in a UEFI driver) ---- */

static UINTN s_len(const char *s) { UINTN n = 0; while (s[n]) n++; return n; }

static int s_prefix(const char *s, const char *p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static int s_contains(const char *hay, UINTN hay_len, const char *needle) {
    UINTN nl = s_len(needle);
    if (nl == 0 || hay_len < nl) return 0;
    for (UINTN i = 0; i + nl <= hay_len; i++) {
        UINTN j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return 1;
    }
    return 0;
}

static void append(char *buf, UINTN *pos, const char *s) {
    while (*s && *pos < OUT_MAX - 1) buf[(*pos)++] = *s++;
    buf[*pos] = '\0';
}

/* ---- SMBIOS ---- */

/* strs points at the string area after a structure's formatted part.
 * idx is 1-based, 0 means "no string". Copies a trimmed printable-ASCII copy
 * into out. */
static void get_smbios_string(const UINT8 *strs, const UINT8 *end, UINT8 idx,
                              char *out) {
    out[0] = '\0';
    if (idx == 0) return;
    const UINT8 *p = strs;
    for (UINT8 i = 1; i < idx; i++) {
        while (p < end && *p) p++;
        if (p >= end) return;
        p++;
    }
    UINTN n = 0;
    while (p + n < end && p[n] && n < STR_MAX - 1) {
        UINT8 c = p[n];
        out[n] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
        n++;
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
}

/* Walks the structure table at [addr, addr+len) and fills info from type 1. */
static int parse_table(const UINT8 *addr, UINTN len, sysinfo_t *info) {
    const UINT8 *p = addr;
    const UINT8 *end = addr + len;

    while (p + 4 <= end) {
        UINT8 type = p[0];
        UINT8 flen = p[1];
        if (flen < 4 || type == 127) break;

        const UINT8 *strs = p + flen;
        const UINT8 *q = strs;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;

        if (type == 1 && flen >= 0x08) {
            get_smbios_string(strs, end, p[0x04], info->manufacturer);
            get_smbios_string(strs, end, p[0x05], info->product_name);
            get_smbios_string(strs, end, p[0x06], info->product_version);
            get_smbios_string(strs, end, p[0x07], info->product_serial);
            return 1;
        }
        p = q + 2;
    }
    return 0;
}

static int read_sysinfo(EFI_SYSTEM_TABLE *st, sysinfo_t *info) {
    const UINT8 *ep3 = NULL, *ep2 = NULL;

    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &st->ConfigurationTable[i];
        if (CompareGuid(&ct->VendorGuid, &gSmbios3) == 0) ep3 = ct->VendorTable;
        else if (CompareGuid(&ct->VendorGuid, &gSmbios) == 0) ep2 = ct->VendorTable;
    }

    if (ep3 && ep3[0] == '_' && ep3[1] == 'S' && ep3[2] == 'M' && ep3[3] == '3' && ep3[4] == '_') {
        UINT32 maxsz = *(const UINT32 *)(ep3 + 0x0C);
        UINT64 addr  = *(const UINT64 *)(ep3 + 0x10);
        if (addr && maxsz && parse_table((const UINT8 *)(UINTN)addr, maxsz, info)) return 1;
    }
    if (ep2 && ep2[0] == '_' && ep2[1] == 'S' && ep2[2] == 'M' && ep2[3] == '_') {
        UINT16 len  = *(const UINT16 *)(ep2 + 0x16);
        UINT32 addr = *(const UINT32 *)(ep2 + 0x18);
        if (addr && len && parse_table((const UINT8 *)(UINTN)addr, len, info)) return 1;
    }
    return 0;
}

static int looks_spoofed(const sysinfo_t *i) {
    return s_prefix(i->manufacturer, "Apple") ||
           s_prefix(i->product_name, "Mac") ||
           s_prefix(i->product_name, "iMac");
}

static void build_output(const sysinfo_t *i, int spoofed, char *buf) {
    UINTN pos = 0;
    buf[0] = '\0';
    append(buf, &pos, "# Metallica-Ids.txt, written by MetallicaIds.efi\n");
    append(buf, &pos, "# SMBIOS type 1 strings as the firmware reported them.\n");
    append(buf, &pos, "# Use product_name and product_serial for\n");
    append(buf, &pos, "#   hack-touchid --host-product / --host-serial\n");
    append(buf, &pos, "product_name=");   append(buf, &pos, i->product_name);   append(buf, &pos, "\n");
    append(buf, &pos, "product_serial="); append(buf, &pos, i->product_serial); append(buf, &pos, "\n");
    append(buf, &pos, "sys_vendor=");      append(buf, &pos, i->manufacturer);   append(buf, &pos, "\n");
    append(buf, &pos, "product_version="); append(buf, &pos, i->product_version); append(buf, &pos, "\n");
    append(buf, &pos, spoofed ? "looks_spoofed=yes\n" : "looks_spoofed=no\n");
}

/* ---- file output ---- */

/* Reads the existing file into old (NUL-terminated). Returns bytes read, or
 * -1 if the file does not exist. */
static INTN read_existing(EFI_FILE_HANDLE root, char *old) {
    EFI_FILE_HANDLE f = NULL;
    old[0] = '\0';
    if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, OUT_NAME, EFI_FILE_MODE_READ, 0)))
        return -1;
    UINTN sz = OUT_MAX - 1;
    if (EFI_ERROR(uefi_call_wrapper(f->Read, 3, f, &sz, old))) sz = 0;
    old[sz] = '\0';
    uefi_call_wrapper(f->Close, 1, f);
    return (INTN)sz;
}

static void write_file(EFI_FILE_HANDLE root, const char *data, UINTN len) {
    EFI_FILE_HANDLE f = NULL;

    /* Replace, do not append: delete an existing file first. */
    if (!EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, OUT_NAME, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0)))
        uefi_call_wrapper(f->Delete, 1, f);   /* closes the handle too */

    f = NULL;
    if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, OUT_NAME,
                  EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
        return;
    uefi_call_wrapper(f->Write, 3, f, &len, (VOID *)data);
    uefi_call_wrapper(f->Flush, 1, f);
    uefi_call_wrapper(f->Close, 1, f);
}

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_LOADED_IMAGE *li = NULL;
    EFI_FILE_IO_INTERFACE *fs = NULL;
    EFI_FILE_HANDLE root = NULL;
    sysinfo_t info;
    char out[OUT_MAX], old[OUT_MAX];

    InitializeLib(ImageHandle, SystemTable);

    info.manufacturer[0] = info.product_name[0] = info.product_version[0] = info.product_serial[0] = '\0';
    if (!read_sysinfo(SystemTable, &info)) return EFI_SUCCESS;
    if (info.product_name[0] == '\0' && info.product_serial[0] == '\0') return EFI_SUCCESS;

    /* The volume this driver was loaded from is the ESP (EFI/OC/Drivers). */
    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, ImageHandle, &LoadedImageProtocol, (VOID **)&li)) || !li)
        return EFI_SUCCESS;
    if (EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol, 3, li->DeviceHandle, &FileSystemProtocol, (VOID **)&fs)) || !fs)
        return EFI_SUCCESS;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root)) || !root)
        return EFI_SUCCESS;

    int spoofed = looks_spoofed(&info);
    build_output(&info, spoofed, out);

    INTN got = read_existing(root, old);
    if (got >= 0) {
        if (spoofed && s_contains(old, (UINTN)got, "looks_spoofed=no")) goto done;  /* keep real values */
        if ((UINTN)got == s_len(out)) {
            UINTN same = 1;
            for (UINTN k = 0; k < (UINTN)got; k++) if (old[k] != out[k]) { same = 0; break; }
            if (same) goto done;                                                   /* unchanged */
        }
    }
    write_file(root, out, s_len(out));

done:
    uefi_call_wrapper(root->Close, 1, root);
    return EFI_SUCCESS;
}
