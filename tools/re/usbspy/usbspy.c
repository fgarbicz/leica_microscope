/*
 * usbspy: logging proxy for libusb.dll (32-bit).
 * Placed next to DijSDK.dll as "libusb.dll", it forwards every call to the
 * real "libusb-1.0.dll" and records all USB traffic to usbspy.log
 * (plus raw bulk-in stream data of the image endpoint to usbspy_stream.bin).
 * Used to learn the Jenoptik/Leica DMC6200 protocol.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include "libusb.h"

static HMODULE g_real;
static FILE *g_log;
static FILE *g_stream;
static CRITICAL_SECTION g_cs;
static LARGE_INTEGER g_freq, g_t0;
static long long g_streamBytes;
static int g_streamLogged;

#define MAX_STREAM_BYTES (400ll * 1024 * 1024)

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void init(void)
{
    if (g_real)
        return;
    InitializeCriticalSection(&g_cs);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_real = LoadLibraryA("libusb-1.0.dll");
    g_log = fopen("usbspy.log", "w");
    g_stream = fopen("usbspy_stream.bin", "wb");
    if (g_log)
        fprintf(g_log, "usbspy started, real libusb=%p\n", (void *)g_real);
}

static void hexdump(const unsigned char *d, int n, int max)
{
    int i;
    if (!g_log || !d)
        return;
    for (i = 0; i < n && i < max; ++i) {
        if (i % 32 == 0)
            fprintf(g_log, "\n      %04x:", i);
        fprintf(g_log, " %02x", d[i]);
    }
    if (n > max)
        fprintf(g_log, "\n      ... (%d bytes total)", n);
    if (n > max && n > 64) {
        fprintf(g_log, "\n      tail:");
        for (i = n - 64; i < n; ++i)
            fprintf(g_log, " %02x", d[i]);
    }
    fprintf(g_log, "\n");
}

#define REAL(name) ((name##_t)GetProcAddress(g_real, #name))
#define LOG(...) do { EnterCriticalSection(&g_cs); if (g_log) { fprintf(g_log, "%10.3f ", now_ms()); fprintf(g_log, __VA_ARGS__); fflush(g_log); } LeaveCriticalSection(&g_cs); } while (0)

/* ---------- simple forwarded functions ---------- */
typedef int (LIBUSB_CALL *libusb_init_t)(libusb_context **);
int LIBUSB_CALL libusb_init(libusb_context **ctx) { init(); int r = REAL(libusb_init)(ctx); LOG("libusb_init -> %d\n", r); return r; }

typedef void (LIBUSB_CALL *libusb_exit_t)(libusb_context *);
void LIBUSB_CALL libusb_exit(libusb_context *ctx) { init(); LOG("libusb_exit\n"); REAL(libusb_exit)(ctx); }

typedef ssize_t (LIBUSB_CALL *libusb_get_device_list_t)(libusb_context *, libusb_device ***);
ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx, libusb_device ***list) { init(); return REAL(libusb_get_device_list)(ctx, list); }

typedef void (LIBUSB_CALL *libusb_free_device_list_t)(libusb_device **, int);
void LIBUSB_CALL libusb_free_device_list(libusb_device **list, int unref) { init(); REAL(libusb_free_device_list)(list, unref); }

typedef int (LIBUSB_CALL *libusb_get_device_descriptor_t)(libusb_device *, struct libusb_device_descriptor *);
int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc) { init(); return REAL(libusb_get_device_descriptor)(dev, desc); }

typedef int (LIBUSB_CALL *libusb_get_configuration_t)(libusb_device_handle *, int *);
int LIBUSB_CALL libusb_get_configuration(libusb_device_handle *h, int *c) { init(); int r = REAL(libusb_get_configuration)(h, c); LOG("get_configuration -> %d (%d)\n", r, c ? *c : -1); return r; }

typedef int (LIBUSB_CALL *libusb_get_config_descriptor_by_value_t)(libusb_device *, uint8_t, struct libusb_config_descriptor **);
int LIBUSB_CALL libusb_get_config_descriptor_by_value(libusb_device *d, uint8_t v, struct libusb_config_descriptor **c) { init(); return REAL(libusb_get_config_descriptor_by_value)(d, v, c); }

typedef void (LIBUSB_CALL *libusb_free_config_descriptor_t)(struct libusb_config_descriptor *);
void LIBUSB_CALL libusb_free_config_descriptor(struct libusb_config_descriptor *c) { init(); REAL(libusb_free_config_descriptor)(c); }

typedef uint8_t (LIBUSB_CALL *libusb_get_bus_number_t)(libusb_device *);
uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *d) { init(); return REAL(libusb_get_bus_number)(d); }

typedef uint8_t (LIBUSB_CALL *libusb_get_device_address_t)(libusb_device *);
uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *d) { init(); return REAL(libusb_get_device_address)(d); }

typedef int (LIBUSB_CALL *libusb_get_device_speed_t)(libusb_device *);
int LIBUSB_CALL libusb_get_device_speed(libusb_device *d) { init(); int r = REAL(libusb_get_device_speed)(d); LOG("get_device_speed -> %d\n", r); return r; }

typedef libusb_device *(LIBUSB_CALL *libusb_get_device_t)(libusb_device_handle *);
libusb_device *LIBUSB_CALL libusb_get_device(libusb_device_handle *h) { init(); return REAL(libusb_get_device)(h); }

typedef int (LIBUSB_CALL *libusb_open_t)(libusb_device *, libusb_device_handle **);
int LIBUSB_CALL libusb_open(libusb_device *d, libusb_device_handle **h)
{
    struct libusb_device_descriptor dd;
    init();
    REAL(libusb_get_device_descriptor)(d, &dd);
    int r = REAL(libusb_open)(d, h);
    LOG("libusb_open %04x:%04x -> %d\n", dd.idVendor, dd.idProduct, r);
    return r;
}

typedef void (LIBUSB_CALL *libusb_close_t)(libusb_device_handle *);
void LIBUSB_CALL libusb_close(libusb_device_handle *h) { init(); LOG("libusb_close\n"); REAL(libusb_close)(h); }

typedef int (LIBUSB_CALL *libusb_set_configuration_t)(libusb_device_handle *, int);
int LIBUSB_CALL libusb_set_configuration(libusb_device_handle *h, int c) { init(); int r = REAL(libusb_set_configuration)(h, c); LOG("set_configuration(%d) -> %d\n", c, r); return r; }

typedef int (LIBUSB_CALL *libusb_claim_interface_t)(libusb_device_handle *, int);
int LIBUSB_CALL libusb_claim_interface(libusb_device_handle *h, int i) { init(); int r = REAL(libusb_claim_interface)(h, i); LOG("claim_interface(%d) -> %d\n", i, r); return r; }

typedef int (LIBUSB_CALL *libusb_release_interface_t)(libusb_device_handle *, int);
int LIBUSB_CALL libusb_release_interface(libusb_device_handle *h, int i) { init(); int r = REAL(libusb_release_interface)(h, i); LOG("release_interface(%d) -> %d\n", i, r); return r; }

typedef int (LIBUSB_CALL *libusb_reset_device_t)(libusb_device_handle *);
int LIBUSB_CALL libusb_reset_device(libusb_device_handle *h) { init(); int r = REAL(libusb_reset_device)(h); LOG("reset_device -> %d\n", r); return r; }

typedef int (LIBUSB_CALL *libusb_get_string_descriptor_ascii_t)(libusb_device_handle *, uint8_t, unsigned char *, int);
int LIBUSB_CALL libusb_get_string_descriptor_ascii(libusb_device_handle *h, uint8_t idx, unsigned char *d, int len)
{
    init();
    int r = REAL(libusb_get_string_descriptor_ascii)(h, idx, d, len);
    LOG("get_string_descriptor_ascii(%u) -> %d '%.*s'\n", idx, r, r > 0 ? r : 0, r > 0 ? (const char *)d : "");
    return r;
}

typedef int (LIBUSB_CALL *libusb_handle_events_t)(libusb_context *);
int LIBUSB_CALL libusb_handle_events(libusb_context *ctx) { init(); return REAL(libusb_handle_events)(ctx); }

/* ---------- synchronous transfers ---------- */
typedef int (LIBUSB_CALL *libusb_control_transfer_t)(libusb_device_handle *, uint8_t, uint8_t, uint16_t, uint16_t, unsigned char *, uint16_t, unsigned int);
int LIBUSB_CALL libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                                        unsigned char *data, uint16_t len, unsigned int timeout)
{
    init();
    if (!(rt & 0x80)) {
        EnterCriticalSection(&g_cs);
        if (g_log) { fprintf(g_log, "%10.3f CTRL OUT rt=%02x req=%02x val=%04x idx=%04x len=%u", now_ms(), rt, req, val, idx, len); hexdump(data, len, 4096); fflush(g_log); }
        LeaveCriticalSection(&g_cs);
    }
    int r = REAL(libusb_control_transfer)(h, rt, req, val, idx, data, len, timeout);
    EnterCriticalSection(&g_cs);
    if (g_log) {
        if (rt & 0x80) { fprintf(g_log, "%10.3f CTRL IN  rt=%02x req=%02x val=%04x idx=%04x len=%u -> %d", now_ms(), rt, req, val, idx, len, r); hexdump(data, r, 4096); }
        else fprintf(g_log, "%10.3f   -> %d\n", now_ms(), r);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

typedef int (LIBUSB_CALL *libusb_bulk_transfer_t)(libusb_device_handle *, unsigned char, unsigned char *, int, int *, unsigned int);
int LIBUSB_CALL libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *actual, unsigned int timeout)
{
    init();
    if (!(ep & 0x80)) {
        EnterCriticalSection(&g_cs);
        if (g_log) { fprintf(g_log, "%10.3f BULK OUT ep=%02x len=%d tmo=%u", now_ms(), ep, len, timeout); hexdump(data, len, 4096); fflush(g_log); }
        LeaveCriticalSection(&g_cs);
    }
    int r = REAL(libusb_bulk_transfer)(h, ep, data, len, actual, timeout);
    int n = actual ? *actual : 0;
    EnterCriticalSection(&g_cs);
    if (g_log) {
        if (ep & 0x80) {
            fprintf(g_log, "%10.3f BULK IN  ep=%02x len=%d tmo=%u -> %d actual=%d", now_ms(), ep, len, timeout, r, n);
            hexdump(data, n, ep == 0x83 ? 256 : 4096);
            if (ep == 0x83 && g_stream && g_streamBytes < MAX_STREAM_BYTES && n > 0) { fwrite(data, 1, n, g_stream); g_streamBytes += n; }
        } else
            fprintf(g_log, "%10.3f   -> %d actual=%d\n", now_ms(), r, n);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

/* ---------- asynchronous transfers ---------- */
typedef struct { struct libusb_transfer *t; libusb_transfer_cb_fn cb; void *ud; } Wrap;
#define MAX_WRAPS 1024
static Wrap g_wraps[MAX_WRAPS];

static void LIBUSB_CALL spy_cb(struct libusb_transfer *t)
{
    libusb_transfer_cb_fn cb = NULL;
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_WRAPS; ++i)
        if (g_wraps[i].t == t) { cb = g_wraps[i].cb; g_wraps[i].t = NULL; break; }
    if (g_log) {
        fprintf(g_log, "%10.3f ASYNC DONE ep=%02x type=%d status=%d len=%d actual=%d", now_ms(), t->endpoint, t->type, t->status, t->length, t->actual_length);
        if (t->endpoint & 0x80) {
            if (t->endpoint == 0x83) {
                if (g_streamLogged < 4000) { hexdump(t->buffer, t->actual_length, 128); g_streamLogged++; } else fprintf(g_log, "\n");
                if (g_stream && g_streamBytes < MAX_STREAM_BYTES && t->actual_length > 0) {
                    fwrite(t->buffer, 1, t->actual_length, g_stream);
                    g_streamBytes += t->actual_length;
                    fflush(g_stream);
                }
            } else
                hexdump(t->buffer, t->actual_length, 4096);
        } else
            fprintf(g_log, "\n");
        fflush(g_log);
    }
    LeaveCriticalSection(&g_cs);
    t->callback = cb;
    if (cb)
        cb(t);
}

typedef struct libusb_transfer *(LIBUSB_CALL *libusb_alloc_transfer_t)(int);
struct libusb_transfer *LIBUSB_CALL libusb_alloc_transfer(int iso) { init(); return REAL(libusb_alloc_transfer)(iso); }

typedef void (LIBUSB_CALL *libusb_free_transfer_t)(struct libusb_transfer *);
void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *t)
{
    int i;
    init();
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_WRAPS; ++i)
        if (g_wraps[i].t == t) g_wraps[i].t = NULL;
    LeaveCriticalSection(&g_cs);
    REAL(libusb_free_transfer)(t);
}

typedef int (LIBUSB_CALL *libusb_submit_transfer_t)(struct libusb_transfer *);
int LIBUSB_CALL libusb_submit_transfer(struct libusb_transfer *t)
{
    int i, r;
    init();
    EnterCriticalSection(&g_cs);
    if (g_log) {
        fprintf(g_log, "%10.3f ASYNC SUBMIT ep=%02x type=%d len=%d tmo=%u flags=%x", now_ms(), t->endpoint, t->type, t->length, t->timeout, t->flags);
        if (t->type == LIBUSB_TRANSFER_TYPE_CONTROL)
            hexdump(t->buffer, 8 + (t->buffer[0] & 0x80 ? 0 : (t->buffer[6] | (t->buffer[7] << 8))), 4096);
        else if (!(t->endpoint & 0x80))
            hexdump(t->buffer, t->length, 4096);
        else
            fprintf(g_log, "\n");
        fflush(g_log);
    }
    if (t->callback != spy_cb) {
        for (i = 0; i < MAX_WRAPS; ++i)
            if (g_wraps[i].t == NULL || g_wraps[i].t == t) {
                g_wraps[i].t = t;
                g_wraps[i].cb = t->callback;
                g_wraps[i].ud = t->user_data;
                t->callback = spy_cb;
                break;
            }
    }
    LeaveCriticalSection(&g_cs);
    r = REAL(libusb_submit_transfer)(t);
    if (r != 0) {
        LOG("  submit -> %d\n", r);
        EnterCriticalSection(&g_cs);
        for (i = 0; i < MAX_WRAPS; ++i)
            if (g_wraps[i].t == t) { t->callback = g_wraps[i].cb; g_wraps[i].t = NULL; }
        LeaveCriticalSection(&g_cs);
    }
    return r;
}

typedef int (LIBUSB_CALL *libusb_cancel_transfer_t)(struct libusb_transfer *);
int LIBUSB_CALL libusb_cancel_transfer(struct libusb_transfer *t) { init(); LOG("ASYNC CANCEL ep=%02x\n", t->endpoint); return REAL(libusb_cancel_transfer)(t); }

/* marker written by the harness so traffic can be matched to API calls */
__declspec(dllexport) void __cdecl usbspy_mark(const char *text)
{
    init();
    LOG("========== %s ==========\n", text);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_ATTACH)
        init();
    if (reason == DLL_PROCESS_DETACH) {
        if (g_log) fclose(g_log);
        if (g_stream) fclose(g_stream);
    }
    return TRUE;
}
