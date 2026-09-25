/*
 * dijharness: drives Jenoptik's DijSDK.dll (from Leica LAS) through a scripted
 * sequence of API calls so the USB traffic can be captured by usbspy.
 *
 * usage: dijharness <cmd> [<cmd> ...]
 *   enum                 list all parameters with values
 *   seti:<id>:<val>      DijSDK_SetIntParameter
 *   setd:<id>:<val>      DijSDK_SetDoubleParameter
 *   geti:<id> getd:<id> gets:<id>
 *   start:<mode>         DijSDK_StartAcquisition
 *   grab:<n>             get n images (first one saved to frame_<k>.bin)
 *   abort                DijSDK_AbortAcquisition
 *   sleep:<ms>
 *   ioctl:<code>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *H;
typedef int (__cdecl *Init_t)(const char (*keys)[33], int numKeys, void *cb, void *ud);
typedef int (__cdecl *Exit_t)(void);
typedef int (__cdecl *Find_t)(char (*guids)[128], int *num, const char *mask);
typedef int (__cdecl *Open_t)(const char *guid, H *handle);
typedef int (__cdecl *Close_t)(H);
typedef int (__cdecl *Has_t)(H, unsigned id);
typedef int (__cdecl *GetInt_t)(H, unsigned id, int *p, int num, int query);
typedef int (__cdecl *SetInt_t)(H, unsigned id, int v);
typedef int (__cdecl *GetDbl_t)(H, unsigned id, double *p, int num);
typedef int (__cdecl *SetDbl_t)(H, unsigned id, double v);
typedef int (__cdecl *GetStr_t)(H, unsigned id, char *p, int len);
typedef int (__cdecl *Start_t)(H, int mode);
typedef int (__cdecl *Abort_t)(H);
typedef int (__cdecl *GetImage_t)(H, H *img, void **data, int timeoutMs);
typedef int (__cdecl *Release_t)(H img);
typedef int (__cdecl *Spec_t)(H, unsigned id, int *type, int *num, int *valueType, void *values, int *numValues);
typedef int (__cdecl *Ioctl_t)(H, int code, void *in, int inSize, void *out, int *outSize);
typedef void (__cdecl *Mark_t)(const char *);

static HMODULE g_dll;
static Mark_t g_mark;
#define F(t, n) ((t)GetProcAddress(g_dll, n))

static void mark(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("### %s\n", buf);
    fflush(stdout);
    if (g_mark)
        g_mark(buf);
}

static void enumerate(H h)
{
    unsigned bases[] = {0x10000000, 0x20000000, 0x30000000, 0x40000000, 0x50000000};
    for (int b = 0; b < 5; ++b) {
        for (unsigned id = bases[b]; id < bases[b] + 0x800; ++id) {
            int has = F(Has_t, "DijSDK_HasParameter")(h, id);
            if ((id >> 28) == 3) {
                double v[16] = {0};
                int r = F(GetDbl_t, "DijSDK_GetDoubleParameter")(h, id, v, 1);
                if (r >= 0)
                    printf("param 0x%08X has=%d dbl r=%d: %g\n", id, has, r, v[0]);
            } else if ((id >> 28) == 4) {
                char s[512] = {0};
                int r = F(GetStr_t, "DijSDK_GetStringParameter")(h, id, s, sizeof s - 1);
                if (r >= 0)
                    printf("param 0x%08X has=%d str r=%d: '%s'\n", id, has, r, s);
            } else {
                int v[16] = {0}, mn[16] = {0}, mx[16] = {0};
                int r = F(GetInt_t, "DijSDK_GetIntParameter")(h, id, v, 1, 0);
                if (r >= 0) {
                    int r1 = F(GetInt_t, "DijSDK_GetIntParameter")(h, id, mn, 1, 1);
                    int r2 = F(GetInt_t, "DijSDK_GetIntParameter")(h, id, mx, 1, 2);
                    printf("param 0x%08X has=%d int r=%d: %d   q1(%d)=%d q2(%d)=%d\n", id, has, r, v[0], r1, mn[0], r2, mx[0]);
                }
            }
            fflush(stdout);
        }
    }
}

static size_t readable(const void *p)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi))
        return 0;
    return (size_t)((const char *)mbi.BaseAddress + mbi.RegionSize - (const char *)p);
}

int main(int argc, char **argv)
{
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    *strrchr(dir, '\\') = 0;
    SetDllDirectoryA(dir);
    g_dll = LoadLibraryA("DijSDK.dll");
    if (!g_dll) {
        printf("cannot load DijSDK.dll (%lu)\n", GetLastError());
        return 1;
    }
    HMODULE spy = GetModuleHandleA("libusb.dll");
    if (spy)
        g_mark = (Mark_t)GetProcAddress(spy, "usbspy_mark");

    static const char keys[1][33] = {"REMOVED-SET-DIJSDK_KEY-AT-RUNTIME"};
    mark("DijSDK_Init");
    int r = F(Init_t, "DijSDK_Init")(keys, 1, NULL, NULL);
    printf("Init -> %d\n", r);
    char guids[8][128];
    memset(guids, 0, sizeof guids);
    int num = 8;
    mark("DijSDK_FindCameras");
    r = F(Find_t, "DijSDK_FindCameras")(guids, &num, NULL);
    printf("FindCameras -> %d, num=%d\n", r, num);
    int sel = -1;
    for (int i = 0; i < num; ++i) {
        printf("  [%d] %s\n", i, guids[i]);
        if (sel < 0 && strncmp(guids[i], "SynthCam", 8) != 0)
            sel = i;
    }
    if (sel < 0) {
        printf("no real camera\n");
        F(Exit_t, "DijSDK_Exit")();
        return 2;
    }
    H h = NULL;
    mark("DijSDK_OpenCamera %s", guids[sel]);
    r = F(Open_t, "DijSDK_OpenCamera")(guids[sel], &h);
    printf("OpenCamera -> %d handle=%p\n", r, h);
    if (r < 0 || !h) {
        F(Exit_t, "DijSDK_Exit")();
        return 3;
    }

    int frameNo = 0;
    for (int a = 1; a < argc; ++a) {
        char *cmd = argv[a];
        if (!strcmp(cmd, "enum")) {
            mark("enum");
            enumerate(h);
        } else if (!strncmp(cmd, "seti:", 5)) {
            unsigned id = strtoul(cmd + 5, NULL, 0);
            int v = atoi(strchr(cmd + 5, ':') + 1);
            mark("SetInt 0x%08X = %d", id, v);
            printf("  -> %d\n", F(SetInt_t, "DijSDK_SetIntParameter")(h, id, v));
        } else if (!strncmp(cmd, "setd:", 5)) {
            unsigned id = strtoul(cmd + 5, NULL, 0);
            double v = atof(strchr(cmd + 5, ':') + 1);
            mark("SetDouble 0x%08X = %g", id, v);
            printf("  -> %d\n", F(SetDbl_t, "DijSDK_SetDoubleParameter")(h, id, v));
        } else if (!strncmp(cmd, "geti:", 5)) {
            unsigned id = strtoul(cmd + 5, NULL, 0);
            int v[8] = {0};
            int rr = F(GetInt_t, "DijSDK_GetIntParameter")(h, id, v, 1, 0);
            printf("GetInt 0x%08X -> %d : %d\n", id, rr, v[0]);
        } else if (!strncmp(cmd, "getd:", 5)) {
            unsigned id = strtoul(cmd + 5, NULL, 0);
            double v[8] = {0};
            int rr = F(GetDbl_t, "DijSDK_GetDoubleParameter")(h, id, v, 1);
            printf("GetDouble 0x%08X -> %d : %g\n", id, rr, v[0]);
        } else if (!strncmp(cmd, "start:", 6)) {
            int mode = atoi(cmd + 6);
            mark("StartAcquisition %d", mode);
            printf("  -> %d\n", F(Start_t, "DijSDK_StartAcquisition")(h, mode));
        } else if (!strcmp(cmd, "abort")) {
            mark("AbortAcquisition");
            printf("  -> %d\n", F(Abort_t, "DijSDK_AbortAcquisition")(h));
        } else if (!strncmp(cmd, "grab:", 5)) {
            int n = atoi(cmd + 5);
            for (int i = 0; i < n; ++i) {
                H img = NULL;
                void *data = NULL;
                DWORD t0 = GetTickCount();
                int rr = F(GetImage_t, "DijSDK_GetImage")(h, &img, &data, 5000);
                DWORD dt = GetTickCount() - t0;
                if (rr < 0 || !img) {
                    printf("GetImage -> %d (%lu ms)\n", rr, dt);
                    continue;
                }
                /* image properties are parameters of the image handle */
                int w = 0;
                F(GetInt_t, "DijSDK_GetIntParameter")(img, 0x20000200, &w, 1, 0);
                printf("GetImage -> %d img=%p data=%p (%lu ms) readable=%zu\n", rr, img, data, dt, readable(data));
                if (i == 0) {
                    printf("-- image handle params:\n");
                    for (unsigned id = 0x20000000; id < 0x20001000; ++id) {
                        if (F(Has_t, "DijSDK_HasParameter")(img, id) > 0) {
                            int v[8] = {0};
                            int r2 = F(GetInt_t, "DijSDK_GetIntParameter")(img, id, v, 4, 0);
                            printf("   img 0x%08X r=%d %d %d %d %d\n", id, r2, v[0], v[1], v[2], v[3]);
                        }
                    }
                    for (unsigned id = 0x30000000; id < 0x30001000; ++id) {
                        if (F(Has_t, "DijSDK_HasParameter")(img, id) > 0) {
                            double v[4] = {0};
                            int r2 = F(GetDbl_t, "DijSDK_GetDoubleParameter")(img, id, v, 1);
                            printf("   img 0x%08X r=%d %g\n", id, r2, v[0]);
                        }
                    }
                    char fn[64];
                    snprintf(fn, sizeof fn, "frame_%d.bin", frameNo++);
                    size_t sz = readable(data);
                    if (sz > 1920u * 1200u * 8u)
                        sz = 1920u * 1200u * 8u;
                    FILE *f = fopen(fn, "wb");
                    if (f) {
                        fwrite(data, 1, sz, f);
                        fclose(f);
                    }
                    printf("   saved %zu bytes to %s\n", sz, fn);
                }
                F(Release_t, "DijSDK_ReleaseImage")(img);
            }

        } else if (!strncmp(cmd, "sleep:", 6)) {
            Sleep(atoi(cmd + 6));
        } else if (!strncmp(cmd, "ioctl:", 6)) {
            int code = atoi(cmd + 6);
            char out[4096];
            int outSize = sizeof out;
            mark("Ioctl %d", code);
            int rr = F(Ioctl_t, "DijSDK_Ioctl")(h, code, NULL, 0, out, &outSize);
            printf("Ioctl %d -> %d outSize=%d\n", code, rr, outSize);
        } else {
            printf("unknown command %s\n", cmd);
        }
        fflush(stdout);
    }
    mark("DijSDK_CloseCamera");
    printf("Close -> %d\n", F(Close_t, "DijSDK_CloseCamera")(h));
    mark("DijSDK_Exit");
    printf("Exit -> %d\n", F(Exit_t, "DijSDK_Exit")());
    return 0;
}
