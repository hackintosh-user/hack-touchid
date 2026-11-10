/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Minimal test double declarations, not a substitute public libusb header. */
#ifndef ELAN_TEST_LIBUSB_H
#define ELAN_TEST_LIBUSB_H
#include <sys/types.h>
#include <stdint.h>
typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;
struct libusb_device_descriptor { uint16_t idVendor, idProduct; };
#define LIBUSB_ERROR_IO -1
#define LIBUSB_ERROR_INVALID_PARAM -2
#define LIBUSB_ERROR_ACCESS -3
#define LIBUSB_ERROR_NO_DEVICE -4
#define LIBUSB_ERROR_BUSY -6
#define LIBUSB_ERROR_TIMEOUT -7
#define LIBUSB_ERROR_NO_MEM -11
#define LIBUSB_ERROR_NOT_SUPPORTED -12
#define LIBUSB_ERROR_OTHER -99
int libusb_init(libusb_context **);
void libusb_exit(libusb_context *);
ssize_t libusb_get_device_list(libusb_context *, libusb_device ***);
void libusb_free_device_list(libusb_device **, int);
int libusb_get_device_descriptor(libusb_device *, struct libusb_device_descriptor *);
int libusb_open(libusb_device *, libusb_device_handle **);
void libusb_close(libusb_device_handle *);
int libusb_claim_interface(libusb_device_handle *, int);
int libusb_release_interface(libusb_device_handle *, int);
int libusb_bulk_transfer(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned);
const char *libusb_error_name(int);
#endif
