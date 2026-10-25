/*
 * fc2-coop-probe: a dinput8.dll proxy for TEST COPIES of Far Cry 2 (Steam 1.03, Dunia engine).
 * Logs network, file and exit calls of Dunia.dll, runs Lua 4.0 and native commands inside the
 * game, finds the player's Havok body and exchanges its position between two copies over its own
 * UDP socket (research prototype; see README.md). MIT licence, see LICENSE.
 * Addresses (RVAs) below are for Dunia.dll SHA-256 6353936a...2765 only.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

static FILE *g_log;
static CRITICAL_SECTION g_cs;
static HMODULE g_self;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%8lu T%04lu] ", GetTickCount(), GetCurrentThreadId());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static void addr_str(const struct sockaddr *a, char *out, size_t n)
{
    if (a && a->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)a;
        snprintf(out, n, "%s:%u", inet_ntoa(s->sin_addr), ntohs(s->sin_port));
    } else {
        snprintf(out, n, "af%d", a ? a->sa_family : -1);
    }
}

static void hex_str(const char *b, int len, char *out, size_t n)
{
    int i, m = len < 16 ? len : 16;
    size_t o = 0;
    for (i = 0; i < m && o + 3 < n; i++) o += snprintf(out + o, n - o, "%02x", (unsigned char)b[i]);
    out[o] = 0;
}

#ifndef GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#endif
WINBASEAPI BOOL WINAPI GetModuleHandleExA(DWORD, LPCSTR, HMODULE *);
static const char *caller_str(void *a, char *out, size_t n)
{
    HMODULE m = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)
        && GetModuleFileNameA(m, path, MAX_PATH)) {
        char *s = strrchr(path, '\\');
        snprintf(out, n, "%s+0x%lx", s ? s + 1 : path, (unsigned long)((BYTE *)a - (BYTE *)m));
    } else {
        snprintf(out, n, "%p", a);
    }
    return out;
}
#define CALLER(buf) caller_str(__builtin_return_address(0), buf, sizeof buf)

/* originals */
static int (WINAPI *o_WSASendTo)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const struct sockaddr *, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static int (WINAPI *o_WSARecvFrom)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, struct sockaddr *, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static int (WINAPI *o_getaddrinfo)(const char *, const char *, const void *, void **);
static struct hostent *(WINAPI *o_gethostbyname)(const char *);
static int (WINAPI *o_sendto)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static int (WINAPI *o_recvfrom)(SOCKET, char *, int, int, struct sockaddr *, int *);
static int (WINAPI *o_bind)(SOCKET, const struct sockaddr *, int);
static int (WINAPI *o_connect)(SOCKET, const struct sockaddr *, int);
static VOID (WINAPI *o_ExitProcess)(UINT);
static VOID (WINAPI *o_PostQuitMessage)(int);

static int WINAPI h_sendto(SOCKET s, const char *b, int l, int f, const struct sockaddr *to, int tl)
{
    char a[40], h[40];
    int r = o_sendto(s, b, l, f, to, tl);
    addr_str(to, a, sizeof a); hex_str(b, l, h, sizeof h);
    logf_("sendto s=%u -> %s len=%d ret=%d data=%s", (unsigned)s, a, l, r, h);
    return r;
}
static int WINAPI h_recvfrom(SOCKET s, char *b, int l, int f, struct sockaddr *from, int *fl)
{
    int r = o_recvfrom(s, b, l, f, from, fl);
    if (r > 0) {
        char a[40], h[40];
        addr_str(from, a, sizeof a); hex_str(b, r, h, sizeof h);
        logf_("recvfrom s=%u <- %s len=%d data=%s", (unsigned)s, a, r, h);
    }
    return r;
}
static int WINAPI h_WSASendTo(SOCKET s, LPWSABUF b, DWORD c, LPDWORD sent, DWORD f, const struct sockaddr *to, int tl, LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    char a[40], h[40] = "", cs[96];
    int r = o_WSASendTo(s, b, c, sent, f, to, tl, o, cr);
    addr_str(to, a, sizeof a);
    if (c) hex_str(b[0].buf, (int)b[0].len, h, sizeof h);
    logf_("WSASendTo s=%u -> %s len=%lu ret=%d data=%s from %s", (unsigned)s, a, c ? b[0].len : 0, r, h, CALLER(cs));
    return r;
}
static int WINAPI h_WSARecvFrom(SOCKET s, LPWSABUF b, DWORD c, LPDWORD got, LPDWORD f, struct sockaddr *from, LPINT fl, LPWSAOVERLAPPED o, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = o_WSARecvFrom(s, b, c, got, f, from, fl, o, cr);
    if (r == 0 && got && *got) {
        char a[40], h[40];
        addr_str(from, a, sizeof a); hex_str(b[0].buf, (int)*got, h, sizeof h);
        logf_("WSARecvFrom s=%u <- %s len=%lu data=%s", (unsigned)s, a, *got, h);
    }
    return r;
}
static int WINAPI h_getaddrinfo(const char *n, const char *sv, const void *hints, void **res)
{
    char cs[96];
    int r = o_getaddrinfo(n, sv, hints, res);
    logf_("getaddrinfo(%s, %s) ret=%d from %s", n ? n : "(null)", sv ? sv : "(null)", r, CALLER(cs));
    return r;
}
static struct hostent *WINAPI h_gethostbyname(const char *n)
{
    char cs[96];
    struct hostent *r = o_gethostbyname(n);
    logf_("gethostbyname(%s) ret=%s from %s", n ? n : "(null)", r ? "ok" : "null", CALLER(cs));
    return r;
}
static int WINAPI h_bind(SOCKET s, const struct sockaddr *n, int l)
{
    char a[40];
    int r = o_bind(s, n, l);
    addr_str(n, a, sizeof a);
    logf_("bind s=%u %s ret=%d", (unsigned)s, a, r);
    return r;
}
static int WINAPI h_connect(SOCKET s, const struct sockaddr *n, int l)
{
    char a[40], cs[96];
    int r = o_connect(s, n, l);
    addr_str(n, a, sizeof a);
    logf_("connect s=%u %s ret=%d from %s", (unsigned)s, a, r, CALLER(cs));
    return r;
}
static VOID WINAPI h_ExitProcess(UINT c)
{
    logf_("ExitProcess(%u) caller=%p", c, __builtin_return_address(0));
    o_ExitProcess(c);
}
static VOID WINAPI h_PostQuitMessage(int c)
{
    logf_("PostQuitMessage(%d) caller=%p", c, __builtin_return_address(0));
    o_PostQuitMessage(c);
}

static BOOL (WINAPI *o_PeekMessageA)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *o_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI h_PeekMessageA(LPMSG, HWND, UINT, UINT, UINT);
static BOOL WINAPI h_PeekMessageW(LPMSG, HWND, UINT, UINT, UINT);

static HANDLE (WINAPI *o_CreateFileA)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE (WINAPI *o_CreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static HANDLE WINAPI h_CreateFileA(LPCSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    HANDLE h = o_CreateFileA(n, a, sh, sa, cd, fl, t);
    if (n && (strstr(n, "av") || strstr(n, "Saved") || strstr(n, "Documents") || strstr(n, "Profile"))) logf_("CreateFileA %s acc=%lx disp=%lu -> %s", n, a, cd, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}
static HANDLE WINAPI h_CreateFileW(LPCWSTR n, DWORD a, DWORD sh, LPSECURITY_ATTRIBUTES sa, DWORD cd, DWORD fl, HANDLE t)
{
    HANDLE h = o_CreateFileW(n, a, sh, sa, cd, fl, t);
    if (n && (wcsstr(n, L"av") || wcsstr(n, L"Saved") || wcsstr(n, L"Documents") || wcsstr(n, L"Profile"))) logf_("CreateFileW %ls acc=%lx disp=%lu -> %s", n, a, cd, h == INVALID_HANDLE_VALUE ? "FAIL" : "ok");
    return h;
}

typedef struct { const char *dll, *name; void **orig; void *hook; } Target;
static Target g_targets[] = {
    { "ws2_32.dll", "sendto", (void **)&o_sendto, h_sendto },
    { "ws2_32.dll", "recvfrom", (void **)&o_recvfrom, h_recvfrom },
    { "ws2_32.dll", "bind", (void **)&o_bind, h_bind },
    { "ws2_32.dll", "connect", (void **)&o_connect, h_connect },
    { "ws2_32.dll", "WSASendTo", (void **)&o_WSASendTo, h_WSASendTo },
    { "ws2_32.dll", "WSARecvFrom", (void **)&o_WSARecvFrom, h_WSARecvFrom },
    { "ws2_32.dll", "getaddrinfo", (void **)&o_getaddrinfo, h_getaddrinfo },
    { "ws2_32.dll", "gethostbyname", (void **)&o_gethostbyname, h_gethostbyname },
    { "user32.dll", "PeekMessageA", (void **)&o_PeekMessageA, h_PeekMessageA },
    { "user32.dll", "PeekMessageW", (void **)&o_PeekMessageW, h_PeekMessageW },
    { "kernel32.dll", "CreateFileA", (void **)&o_CreateFileA, h_CreateFileA },
    { "kernel32.dll", "CreateFileW", (void **)&o_CreateFileW, h_CreateFileW },
    { "kernel32.dll", "ExitProcess", (void **)&o_ExitProcess, h_ExitProcess },
    { "user32.dll", "PostQuitMessage", (void **)&o_PostQuitMessage, h_PostQuitMessage },
};
#define NT (sizeof g_targets / sizeof g_targets[0])

static int patch_iat(HMODULE m)
{
    BYTE *base = (BYTE *)m;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd->VirtualAddress);
    void *want[NT];
    size_t i;
    int patched = 0;
    for (i = 0; i < NT; i++) {
        HMODULE h = GetModuleHandleA(g_targets[i].dll);
        if (!h) h = LoadLibraryA(g_targets[i].dll);
        want[i] = h ? (void *)GetProcAddress(h, g_targets[i].name) : NULL;
        *g_targets[i].orig = want[i];
    }
    for (; d->Name; d++) {
        void **t = (void **)(base + d->FirstThunk);
        for (; *t; t++) {
            for (i = 0; i < NT; i++) {
                if (want[i] && *t == want[i]) {
                    DWORD old;
                    VirtualProtect(t, sizeof(void *), PAGE_READWRITE, &old);
                    *t = g_targets[i].hook;
                    VirtualProtect(t, sizeof(void *), old, &old);
                    logf_("hooked %s via IAT of %s", g_targets[i].name, (char *)(base + d->Name));
                    patched++;
                }
            }
        }
    }
    return patched;
}

/* ---- Lua 4.0 access (Dunia.dll, RVAs from the Steam v1.03 build) ---- */
#define RVA_GETTOP    0x2aa9c0
#define RVA_SETTOP    0x2aa9e0
#define RVA_DOBUFFER  0x2ab600
#define RVA_CHECKLSTR 0x2d24b0
#define RVA_ALERT     0x2c3500
#define RVA_GETGLOBAL 0x2aaf90

static BYTE *g_base;
static int (__cdecl *o_gettop)(void *);
static int (__cdecl *lua_settop_)(void *, int);
static int (__cdecl *lua_dobuffer_)(void *, const char *, size_t, const char *);
static const char *(__cdecl *lua_checklstr_)(void *, int, size_t *);
static void (__cdecl *lua_getglobal_)(void *, const char *);

#define MAXL 8
static struct { void *L; unsigned long thread; unsigned long hits; } g_states[MAXL];
static int g_nstates;
static char g_cmdpath[MAX_PATH];
static volatile LONG g_busy;
static DWORD g_lastpoll;

static void *make_detour(BYTE *target, int stolen, void *hook, int keep_original)
{
    DWORD old;
    BYTE *tramp = NULL;
    if (keep_original) {
        tramp = (BYTE *)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(tramp, target, stolen);
        tramp[stolen] = 0xE9;
        *(DWORD *)(tramp + stolen + 1) = (DWORD)(target + stolen) - (DWORD)(tramp + stolen + 5);
    }
    VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD *)(target + 1) = (DWORD)hook - (DWORD)(target + 5);
    VirtualProtect(target, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    return tramp;
}

static int __cdecl h_alert(void *L)
{
    const char *s = lua_checklstr_(L, 1, NULL);
    logf_("LUA: %s", s ? s : "(nil)");
    return 0;
}

static void run_cmd(void *L)
{
    FILE *f = fopen(g_cmdpath, "rb");
    long n;
    char *buf;
    int top, st;
    if (!f) return;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(n + 1);
    if (fread(buf, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    buf[n] = 0;
    remove(g_cmdpath);
    top = o_gettop(L);
    lua_dobuffer_(L, "__out = ''", 10, "coop_pre");
    st = lua_dobuffer_(L, buf, (size_t)n, "coop_cmd");
    lua_getglobal_(L, "__out");
    {
        size_t ol = 0;
        const char *o = lua_checklstr_(L, o_gettop(L), &ol);
        logf_("cmd executed (%ld bytes) status=%d out(%u)=%s", n, st, (unsigned)ol, o ? o : "(null)");
    }
    lua_settop_(L, top);
    free(buf);
}


static int lua_eval(const char *code, char *out, size_t on)
{
    void *L;
    int top, st;
    size_t ol = 0;
    const char *o;
    out[0] = 0;
    if (g_nstates == 0) return -1;
    L = g_states[0].L;
    top = o_gettop(L);
    lua_dobuffer_(L, "__out = ''", 10, "coop_pre");
    st = lua_dobuffer_(L, code, strlen(code), "coop_eval");
    lua_getglobal_(L, "__out");
    o = lua_checklstr_(L, o_gettop(L), &ol);
    if (o) { strncpy(out, o, on - 1); out[on - 1] = 0; }
    lua_settop_(L, top);
    return st;
}


/* ---- native peek commands (file coop_native.txt, run on the main thread) ---- */
#define RVA_ENTMGR_PTR 0x1644e80
#define RVA_GETENTITY  0x55820
typedef void *(__attribute__((thiscall)) *getent_t)(void *mgr, void **out, unsigned lo, unsigned hi);
#define RVA_BODY_VTABLE 0xE67658
static BYTE *g_body, *g_bodymat;
static BYTE **g_av_body_p;
static int g_walk;
static float g_walk_c[3], g_walk_r, g_walk_secs, g_walk_speed;
static DWORD g_walk_t0;
static int g_net_cfg;  /* 1 = start requested, 2 = running */
static unsigned g_net_lport, g_net_pport;
static char g_net_pip[32];
static unsigned long long g_net_id;
static char g_natpath[MAX_PATH];
static int g_hold;
static unsigned long long g_hold_id;
static float g_hold_pos[3];
static unsigned long g_hold_ticks;

static void hexdump_(const BYTE *p, int len)
{
    int i, j;
    for (i = 0; i < len; i += 16) {
        char line[256];
        int o = snprintf(line, sizeof line, "%08lx:", (unsigned long)(p + i));
        for (j = 0; j < 16 && i + j + 4 <= len; j += 4) {
            unsigned v = *(const unsigned *)(p + i + j);
            float f = *(const float *)(p + i + j);
            if (f > -1e6f && f < 1e6f && (f > 1e-3f || f < -1e-3f)) o += snprintf(line + o, sizeof line - o, " %08x(%.3f)", v, f);
            else o += snprintf(line + o, sizeof line - o, " %08x", v);
        }
        logf_("%s", line);
    }
}


static int safe_read(const void *addr, void *buf, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, buf, n, &got) && got == n;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char mod[96];
    caller_str(ep->ExceptionRecord->ExceptionAddress, mod, sizeof mod);
    logf_("CRASH: code %08lx at %s (access address %p)", (unsigned long)ep->ExceptionRecord->ExceptionCode, mod,
          ep->ExceptionRecord->NumberParameters > 1 ? (void *)ep->ExceptionRecord->ExceptionInformation[1] : NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}
WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG, PVECTORED_EXCEPTION_HANDLER);
static LONG WINAPI veh_filter(EXCEPTION_POINTERS *ep)
{
    static int n;
    if (ep->ExceptionRecord->ExceptionCode == 0xC0000005 && n < 30) {
        char mod[96];
        n++;
        caller_str(ep->ExceptionRecord->ExceptionAddress, mod, sizeof mod);
        logf_("ACCESS VIOLATION #%d at %s, %s address %p", n, mod, ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
              (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static BYTE *entity_obj(unsigned long long id, void **holder_out);
/* exclude: a body to skip (the local player's when binding the avatar: both stand on the same spot
 * after loading the same save, so the nearest body would otherwise be the player's own) */
static int find_body(unsigned long long id, BYTE **body_out, BYTE **mat_out, const BYTE *exclude)
{
    void *holder = NULL;
    BYTE *obj = entity_obj(id, &holder);
    float ex, ey, ez, best = 1.0f;
    MEMORY_BASIC_INFORMATION mbi;
    BYTE *p = NULL;
    int found = 0;
    unsigned vt = (unsigned)(g_base + RVA_BODY_VTABLE);
    if (!obj) { logf_("findbody %llu: no entity", id); return 0; }
    ex = *(float *)(obj + 0x60); ey = *(float *)(obj + 0x64); ez = *(float *)(obj + 0x68);
    if (holder) (*(int *)((BYTE *)holder + 8))--;
    *body_out = NULL; *mat_out = NULL;
    {
        static BYTE chunk[0x10000 + 0x40];
        while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mbi.Protect & PAGE_READWRITE) && mbi.RegionSize < 0x20000000) {
                BYTE *base = (BYTE *)mbi.BaseAddress, *rend = base + mbi.RegionSize;
                BYTE *c;
                for (c = base; c < rend; c += 0x10000) {
                    size_t n = (size_t)(rend - c);
                    size_t k;
                    if (n > sizeof chunk) n = sizeof chunk;
                    if (!safe_read(c, chunk, n)) continue;
                    for (k = 0; k + 4 <= n && k < 0x10000; k += 4) {
                        if (*(unsigned *)(chunk + k) == vt && k + 0x1C <= n && c + k != exclude) {
                            BYTE *m = *(BYTE **)(chunk + k + 0x18);
                            float f[3];
                            if (safe_read(m + 0x30, f, sizeof f)) {
                                float d = fabsf(f[0] - ex) + fabsf(f[1] - ey) + fabsf(f[2] - ez);
                                if (d < 1.0f) {
                                    logf_("findbody: body %p matrix %p pos %.3f %.3f %.3f (dist %.3f)", c + k, m, f[0], f[1], f[2], d);
                                    found++;
                                    if (d < best) { best = d; *body_out = c + k; *mat_out = m; }
                                }
                            }
                        }
                    }
                }
            }
            p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
        }
    }
    logf_("findbody done: %d match(es) near entity %.3f %.3f %.3f, nearest dist %.3f", found, ex, ey, ez, *body_out ? best : -1.0f);
    return *body_out != NULL;
}
static void native_cmd(const char *ln)
{
    unsigned long long id;
    unsigned long a, pv;
    float fx, fy, fz;
    int n;
    if (sscanf(ln, "peek %lx %d", &a, &n) == 2) {
        if (IsBadReadPtr((void *)a, n)) logf_("peek %lx: unreadable", a);
        else hexdump_((const BYTE *)a, n);
    } else if (sscanf(ln, "ent %llu %d", &id, &n) == 2) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        void *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("ent: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16)) { logf_("ent %llu: no holder", id); return; }
        obj = *(void **)((BYTE *)holder + 0xC);
        logf_("ent %llu: holder=%p obj=%p refs=%d", id, holder, obj, *(int *)((BYTE *)holder + 8));
        if (obj && !IsBadReadPtr(obj, n)) { logf_("vtable=%p", *(void **)obj); hexdump_((const BYTE *)obj, n); }
    } else if (sscanf(ln, "pos %llu", &id) == 1) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        BYTE *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("pos: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16) || !*(void **)((BYTE *)holder + 0xC)) { logf_("pos %llu: no entity", id); return; }
        obj = *(BYTE **)((BYTE *)holder + 0xC);
        logf_("pos %llu: x=%.3f y=%.3f z=%.3f dir=(%.3f,%.3f)", id, *(float *)(obj + 0x60), *(float *)(obj + 0x64), *(float *)(obj + 0x68), *(float *)(obj + 0x30), *(float *)(obj + 0x34));
    } else if (sscanf(ln, "setpos %llu %f %f %f", &id, &fx, &fy, &fz) == 4) {
        void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
        void *holder = NULL;
        BYTE *obj;
        if (IsBadReadPtr(pm, 4) || !*pm) { logf_("setpos: entity manager null"); return; }
        ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
        if (!holder || IsBadReadPtr(holder, 16) || !*(void **)((BYTE *)holder + 0xC)) { logf_("setpos %llu: no entity", id); return; }
        obj = *(BYTE **)((BYTE *)holder + 0xC);
        *(float *)(obj + 0x60) = fx; *(float *)(obj + 0x64) = fy; *(float *)(obj + 0x68) = fz;
        logf_("setpos %llu: wrote %.3f %.3f %.3f", id, fx, fy, fz);
    } else if (sscanf(ln, "netstart %u %31s %u %llu", &g_net_lport, g_net_pip, &g_net_pport, &g_net_id) == 4) {
        g_net_cfg = 1;
        logf_("netstart requested: local %u peer %s:%u entity %llu", g_net_lport, g_net_pip, g_net_pport, g_net_id);
    } else if (strncmp(ln, "netstop", 7) == 0) {
        g_net_cfg = 0;
        logf_("netstop");
    } else if (sscanf(ln, "walk %f %f %f", &fx, &fy, &fz) == 3) {
        if (!g_bodymat || IsBadReadPtr(g_bodymat, 0x40)) { logf_("walk: no cached body"); return; }
        g_walk_c[0] = *(float *)(g_bodymat + 0x30); g_walk_c[1] = *(float *)(g_bodymat + 0x34); g_walk_c[2] = *(float *)(g_bodymat + 0x38);
        g_walk_r = fx; g_walk_secs = fy; g_walk_speed = fz; g_walk_t0 = GetTickCount(); g_walk = 1;
        logf_("walk: radius %.2f for %.1f s at %.2f m/s around %.2f %.2f %.2f (start on circle edge)", fx, fy, fz, g_walk_c[0], g_walk_c[1], g_walk_c[2]);
    } else if (sscanf(ln, "bodyvel %f %f %f %lx", &fx, &fy, &fz, &a) == 4) {
        if (!g_body || IsBadWritePtr(g_body + a, 12)) { logf_("bodyvel: bad body/offset"); return; }
        *(float *)(g_body + a) = fx; *(float *)(g_body + a + 4) = fy; *(float *)(g_body + a + 8) = fz;
        logf_("bodyvel: wrote %.2f %.2f %.2f at body+0x%lx", fx, fy, fz, a);
    } else if (sscanf(ln, "avvel %f %f %f %lx", &fx, &fy, &fz, &a) == 4) {
        if (!g_av_body_p || !*g_av_body_p || IsBadWritePtr(*g_av_body_p + a, 12)) { logf_("avvel: no avatar body"); return; }
        *(float *)(*g_av_body_p + a) = fx; *(float *)(*g_av_body_p + a + 4) = fy; *(float *)(*g_av_body_p + a + 8) = fz;
        logf_("avvel: wrote %.2f %.2f %.2f at avatar body+0x%lx", fx, fy, fz, a);
    } else if (sscanf(ln, "findbody %llu", &id) == 1) {
        find_body(id, &g_body, &g_bodymat, NULL);
    } else if (strncmp(ln, "posb", 4) == 0) {
        if (!g_bodymat || IsBadReadPtr(g_bodymat, 0x40) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) logf_("posb: no valid cached body");
        else logf_("posb: %.3f %.3f %.3f", *(float *)(g_bodymat + 0x30), *(float *)(g_bodymat + 0x34), *(float *)(g_bodymat + 0x38));
    } else if (sscanf(ln, "tpb %f %f %f", &fx, &fy, &fz) == 3) {
        if (!g_bodymat || IsBadWritePtr(g_bodymat, 0x40) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) logf_("tpb: no valid cached body");
        else {
            *(float *)(g_bodymat + 0x30) = fx; *(float *)(g_bodymat + 0x34) = fy; *(float *)(g_bodymat + 0x38) = fz;
            logf_("tpb: wrote %.3f %.3f %.3f", fx, fy, fz);
        }
    } else if (strncmp(ln, "follow ", 7) == 0 || strncmp(ln, "tp ", 3) == 0) {
        int tp = ln[0] == 't';
        char *c = (char *)ln + (tp ? 3 : 7);
        void *holder = NULL;
        BYTE *obj, *cur;
        unsigned long long eid = strtoull(c, &c, 10);
        float v[3] = { 0, 0, 0 };
        int nv = 0, ok = 1;
        unsigned long offs[12];
        int no = 0, k;
        if (tp) { for (nv = 0; nv < 3; nv++) v[nv] = strtof(c, &c); }
        while (*c && no < 12) { char *e; unsigned long o = strtoul(c, &e, 16); if (e == c) break; offs[no++] = o; c = e; }
        obj = entity_obj(eid, &holder);
        cur = obj;
        for (k = 0; k < no && cur && ok; k++) {
            if (IsBadReadPtr(cur + offs[k], 4)) { ok = 0; break; }
            cur = *(BYTE **)(cur + offs[k]);
        }
        if (!cur || !ok || IsBadReadPtr(cur, 0x40)) logf_("%s %llu: chain broken at step %d", tp ? "tp" : "follow", eid, k);
        else if (tp) {
            *(float *)(cur + 0x30) = v[0]; *(float *)(cur + 0x34) = v[1]; *(float *)(cur + 0x38) = v[2];
            logf_("tp %llu: wrote %.3f %.3f %.3f at %p+0x30", eid, v[0], v[1], v[2], cur);
        } else {
            float *f = (float *)(cur + 0x30);
            logf_("follow %llu: end %p pos %.3f %.3f %.3f (entity %.3f %.3f %.3f)", eid, cur, f[0], f[1], f[2], *(float *)(obj + 0x60), *(float *)(obj + 0x64), *(float *)(obj + 0x68));
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
    } else if (sscanf(ln, "scan %f %f", &fx, &fy) == 2) {
        MEMORY_BASIC_INFORMATION mbi;
        BYTE *p = NULL;
        int found = 0;
        logf_("scan for x=%.3f y=%.3f", fx, fy);
        while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && mbi.RegionSize < 0x20000000) {
                BYTE *q = (BYTE *)mbi.BaseAddress;
                BYTE *end = q + mbi.RegionSize - 24;
                for (; q < end; q += 4) {
                    if (fabsf(*(float *)q - fx) < 0.01f) {
                        int k;
                        for (k = 4; k <= 16; k += 4) {
                            if (fabsf(*(float *)(q + k) - fy) < 0.01f) {
                                logf_("SCAN %p y@+%d z=%.3f %s", q, k, *(float *)(q + 8), (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ? "rw" : "ro");
                                found++;
                                break;
                            }
                        }
                    }
                }
            }
            p = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
            if (found > 2000) break;
        }
        logf_("scan done, %d hits", found);
    } else if (sscanf(ln, "pokef %lx %f", &a, &fx) == 2) {
        if (IsBadWritePtr((void *)a, 4)) logf_("pokef %lx: not writable", a);
        else { *(float *)a = fx; logf_("pokef %lx = %.3f", a, fx); }
    } else if (sscanf(ln, "hold %llu %f %f %f", &id, &fx, &fy, &fz) == 4) {
        g_hold_id = id; g_hold_pos[0] = fx; g_hold_pos[1] = fy; g_hold_pos[2] = fz; g_hold_ticks = 0; g_hold = 1;
        logf_("hold on: %llu -> %.3f %.3f %.3f", id, fx, fy, fz);
    } else if (strncmp(ln, "unhold", 6) == 0) {
        g_hold = 0;
        logf_("hold off after %lu ticks", g_hold_ticks);
    } else if (sscanf(ln, "poke %lx %lx", &a, &pv) == 2) {
        if (IsBadWritePtr((void *)a, 4)) logf_("poke %lx: not writable", a);
        else { *(unsigned long *)a = pv; logf_("poke %lx = %08lx", a, pv); }
    } else logf_("native: unknown command: %s", ln);
}

static void run_native(void)
{
    FILE *f = fopen(g_natpath, "rb");
    char ln[256];
    if (!f) return;
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (ln[0]) native_cmd(ln);
    }
    fclose(f);
    remove(g_natpath);
    logf_("native commands done");
}


/* ---- per-frame position hold (test) ---- */
static BYTE *entity_obj(unsigned long long id, void **holder_out)
{
    void **pm = (void **)(g_base + RVA_ENTMGR_PTR);
    void *holder = NULL;
    if (IsBadReadPtr(pm, 4) || !*pm) return NULL;
    ((getent_t)(g_base + RVA_GETENTITY))(*pm, &holder, (unsigned)id, (unsigned)(id >> 32));
    if (!holder || IsBadReadPtr(holder, 16)) return NULL;
    *holder_out = holder;
    return *(BYTE **)((BYTE *)holder + 0xC);
}

static void hold_tick(void)
{
    void *holder = NULL;
    BYTE *obj;
    if (!g_hold || g_nstates == 0 || GetCurrentThreadId() != g_states[0].thread) return;
    obj = entity_obj(g_hold_id, &holder);
    if (obj) {
        float *p = (float *)(obj + 0x60);
        g_hold_ticks++;
        if (g_hold_ticks <= 10 || g_hold_ticks % 120 == 0)
            logf_("hold tick %lu: engine left %.3f %.3f %.3f", g_hold_ticks, p[0], p[1], p[2]);
        p[0] = g_hold_pos[0]; p[1] = g_hold_pos[1]; p[2] = g_hold_pos[2];
    }
    if (holder) (*(int *)((BYTE *)holder + 8))--;
}

/* ---- own UDP transport: position exchange between two copies ---- */
#pragma pack(push, 1)
typedef struct { unsigned magic, seq; float pos[3]; float dir[2]; } NetPkt;
#pragma pack(pop)
#define NET_MAGIC 0x43324346u
static SOCKET g_ns = INVALID_SOCKET;
static struct sockaddr_in g_peer;
static DWORD g_net_lastsend, g_net_lastlog, g_net_lastfind, g_net_lastrx;
static unsigned g_net_seq, g_net_tx, g_net_rx;
static NetPkt g_remote;
static unsigned long long g_av_id;
static BYTE *g_av_body, *g_av_mat;
static void av_init_ptr(void) { g_av_body_p = &g_av_body; }
static float g_av_cur[3], g_av_dir[2];
static int g_av_have;
static DWORD g_av_lastspawn, g_av_lastfind;

static int av_valid(void)
{
    return g_av_mat && g_av_body != g_body && !IsBadWritePtr(g_av_mat, 0x40) && !IsBadReadPtr(g_av_body, 4) && *(unsigned *)g_av_body == (unsigned)(g_base + RVA_BODY_VTABLE);
}

static void av_remove(void)
{
    char code[128], out[64];
    if (g_av_id) {
        snprintf(code, sizeof code, "RemoveEntity('%llu') __out = 'removed'", g_av_id);
        lua_eval(code, out, sizeof out);
        logf_("avatar %llu removed: %s", g_av_id, out);
    }
    g_av_id = 0; g_av_body = g_av_mat = NULL; g_av_have = 0;
}

static void av_tick(DWORD now)
{
    av_init_ptr();
    if (g_net_rx == 0 || now - g_net_lastrx > 3000) return;
    if (!g_av_id) {
        char code[256], out[64];
        unsigned long long id;
        if (now - g_av_lastspawn < 3000) return;
        g_av_lastspawn = now;
        snprintf(code, sizeof code, "__out = tostring(SpawnEntityFromArchetype(GetEntityName(GetLocalPlayerId()), %.3f, %.3f, %.3f, 0, 0, 0))",
                 g_remote.pos[0], g_remote.pos[1], g_remote.pos[2]);
        lua_eval(code, out, sizeof out);
        id = strtoull(out, NULL, 10);
        if (id == 0 || id == 0xFFFFFFFFFFFFFFFFull) { logf_("avatar spawn failed: %s", out); return; }
        g_av_id = id;
        logf_("avatar spawned: %llu", id);
        snprintf(code, sizeof code, "CDominoManager_GetInstance():SendRegisteredEventToEntity('%llu', 'CFCXCountersComponent_SetInvincible') __out = 'ok'", id);
        lua_eval(code, out, sizeof out);
        logf_("avatar set invincible: status %s", out);
        return;
    }
    if (!av_valid()) {
        if (!g_body) return;  /* need our own body first, so it can be excluded */
        if (now - g_av_lastfind < 1000) return;
        g_av_lastfind = now;
        if (find_body(g_av_id, &g_av_body, &g_av_mat, g_body)) { g_av_have = 0; logf_("avatar body bound: %p (local body %p)", g_av_body, g_body); }
        return;
    }
    {
        float *cur = g_av_cur, *tg = g_remote.pos;
        float d = fabsf(tg[0] - cur[0]) + fabsf(tg[1] - cur[1]) + fabsf(tg[2] - cur[2]);
        if (!g_av_have || d > 6.0f) { memcpy(cur, tg, sizeof g_av_cur); g_av_have = 1; }
        else { cur[0] += (tg[0] - cur[0]) * 0.25f; cur[1] += (tg[1] - cur[1]) * 0.25f; cur[2] += (tg[2] - cur[2]) * 0.25f; }
        *(float *)(g_av_mat + 0x30) = cur[0]; *(float *)(g_av_mat + 0x34) = cur[1]; *(float *)(g_av_mat + 0x38) = cur[2];
    }
    /* heading: the body capsule never rotates and the avatar has no controller, so the entity matrix
     * keeps whatever rotation is written into it (yaw only, rows (d0,d1,0) (-d1,d0,0) (0,0,1),
     * the same layout as the player's) */
    {
        float dx = g_remote.dir[0], dy = g_remote.dir[1], n = sqrtf(dx * dx + dy * dy);
        void *holder = NULL;
        BYTE *obj;
        if (n < 0.5f) return;
        dx /= n; dy /= n;
        if (!g_av_have || g_av_dir[0] * dx + g_av_dir[1] * dy < -0.9f) { g_av_dir[0] = dx; g_av_dir[1] = dy; }
        else {
            g_av_dir[0] += (dx - g_av_dir[0]) * 0.25f; g_av_dir[1] += (dy - g_av_dir[1]) * 0.25f;
            n = sqrtf(g_av_dir[0] * g_av_dir[0] + g_av_dir[1] * g_av_dir[1]);
            if (n < 1e-3f) { g_av_dir[0] = dx; g_av_dir[1] = dy; } else { g_av_dir[0] /= n; g_av_dir[1] /= n; }
        }
        obj = entity_obj(g_av_id, &holder);
        if (obj && !IsBadWritePtr(obj + 0x30, 0x30)) {
            float *r = (float *)(obj + 0x30);
            r[0] = g_av_dir[0]; r[1] = g_av_dir[1]; r[2] = 0;
            r[4] = -g_av_dir[1]; r[5] = g_av_dir[0]; r[6] = 0;
            r[8] = 0; r[9] = 0; r[10] = 1;
        }
        if (holder) (*(int *)((BYTE *)holder + 8))--;
    }
}

static void net_tick(void)
{
    DWORD now;
    if (g_net_cfg == 0) {
        if (g_av_id) av_remove();
        if (g_ns != INVALID_SOCKET) { closesocket(g_ns); g_ns = INVALID_SOCKET; logf_("net socket closed (tx=%u rx=%u)", g_net_tx, g_net_rx); }
        return;
    }
    if (g_nstates == 0 || GetCurrentThreadId() != g_states[0].thread) return;
    now = GetTickCount();
    if (g_net_cfg == 1) {
        WSADATA wd;
        struct sockaddr_in a;
        u_long nb = 1;
        WSAStartup(MAKEWORD(2, 2), &wd);
        g_ns = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((unsigned short)g_net_lport);
        if (bind(g_ns, (struct sockaddr *)&a, sizeof a) != 0) { logf_("net: bind %u failed (%d)", g_net_lport, WSAGetLastError()); closesocket(g_ns); g_ns = INVALID_SOCKET; g_net_cfg = 0; return; }
        ioctlsocket(g_ns, FIONBIO, &nb);
        memset(&g_peer, 0, sizeof g_peer);
        g_peer.sin_family = AF_INET; g_peer.sin_addr.s_addr = inet_addr(g_net_pip); g_peer.sin_port = htons((unsigned short)g_net_pport);
        g_net_seq = g_net_tx = g_net_rx = 0; g_net_lastsend = g_net_lastlog = now; g_net_lastrx = 0;
        memset(&g_remote, 0, sizeof g_remote);
        g_net_cfg = 2;
        logf_("net: socket up, local %u -> peer %s:%u", g_net_lport, g_net_pip, g_net_pport);
    }
    if (g_walk && g_bodymat && !IsBadWritePtr(g_bodymat, 0x40)) {
        float t = (GetTickCount() - g_walk_t0) / 1000.0f;
        if (t > g_walk_secs) { g_walk = 0; logf_("walk finished"); }
        else {
            float ang = g_walk_speed * t / g_walk_r;
            *(float *)(g_bodymat + 0x30) = g_walk_c[0] + g_walk_r * (cosf(ang) - 1.0f);
            *(float *)(g_bodymat + 0x34) = g_walk_c[1] + g_walk_r * sinf(ang);
            *(float *)(g_bodymat + 0x38) = g_walk_c[2];
        }
    }
    /* make sure we know our own body */
    if (!g_bodymat || IsBadReadPtr(g_body, 4) || *(unsigned *)g_body != (unsigned)(g_base + RVA_BODY_VTABLE)) {
        if (now - g_net_lastfind > 3000) {
            char c[64];
            g_net_lastfind = now;
            snprintf(c, sizeof c, "findbody %llu", g_net_id);
            native_cmd(c);
        }
    }
    /* receive everything pending */
    for (;;) {
        NetPkt p;
        int r = recvfrom(g_ns, (char *)&p, sizeof p, 0, NULL, NULL);
        if (r != (int)sizeof p) break;
        if (p.magic != NET_MAGIC) continue;
        g_remote = p; g_net_rx++; g_net_lastrx = now;
    }
    av_tick(now);
    /* send at 20 Hz */
    if (g_bodymat && now - g_net_lastsend >= 50) {
        NetPkt p;
        void *holder = NULL;
        BYTE *obj = entity_obj(g_net_id, &holder);
        g_net_lastsend = now;
        p.magic = NET_MAGIC; p.seq = ++g_net_seq;
        memcpy(p.pos, g_bodymat + 0x30, sizeof p.pos);
        p.dir[0] = obj ? *(float *)(obj + 0x30) : 0; p.dir[1] = obj ? *(float *)(obj + 0x34) : 0;
        if (holder) (*(int *)((BYTE *)holder + 8))--;
        sendto(g_ns, (char *)&p, sizeof p, 0, (struct sockaddr *)&g_peer, sizeof g_peer);
        g_net_tx++;
    }
    if (now - g_net_lastlog >= 1000) {
        g_net_lastlog = now;
        logf_("net: tx=%u rx=%u local=(%.2f %.2f %.2f) remote=(%.2f %.2f %.2f) seq=%u age=%lums", g_net_tx, g_net_rx,
              g_bodymat ? *(float *)(g_bodymat + 0x30) : 0, g_bodymat ? *(float *)(g_bodymat + 0x34) : 0, g_bodymat ? *(float *)(g_bodymat + 0x38) : 0,
              g_remote.pos[0], g_remote.pos[1], g_remote.pos[2], g_remote.seq, g_net_lastrx ? (unsigned long)(now - g_net_lastrx) : 0UL);
    }
}

static void pump_main(void)
{
    FILE *f;
    if (g_nstates == 0 || g_busy || GetCurrentThreadId() != g_states[0].thread) return;
    if (GetTickCount() - g_lastpoll < 250) return;
    g_lastpoll = GetTickCount();
    f = fopen(g_natpath, "rb");
    if (f) { fclose(f); g_busy = 1; run_native(); g_busy = 0; return; }
    f = fopen(g_cmdpath, "rb");
    if (!f) return;
    fclose(f);
    g_busy = 1;
    run_cmd(g_states[0].L);
    g_busy = 0;
}
static BOOL WINAPI h_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT c) { hold_tick(); net_tick(); pump_main(); return o_PeekMessageA(m, h, a, b, c); }
static BOOL WINAPI h_PeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT c) { hold_tick(); net_tick(); pump_main(); return o_PeekMessageW(m, h, a, b, c); }

static int __cdecl h_gettop(void *L)
{
    unsigned long tid = GetCurrentThreadId();
    int i, best = -1;
    for (i = 0; i < g_nstates; i++) if (g_states[i].L == L) break;
    if (i == g_nstates && g_nstates < MAXL) {
        g_states[i].L = L; g_states[i].thread = tid; g_states[i].hits = 0; g_nstates++;
        logf_("new lua_State %p first seen on thread %lu", L, tid);
    }
    if (i < MAXL) g_states[i].hits++;
    return o_gettop(L);
}

static void install_lua(HMODULE m)
{
    char *s;
    g_base = (BYTE *)m;
    lua_settop_ = (void *)(g_base + RVA_SETTOP);
    lua_dobuffer_ = (void *)(g_base + RVA_DOBUFFER);
    lua_checklstr_ = (void *)(g_base + RVA_CHECKLSTR);
    lua_getglobal_ = (void *)(g_base + RVA_GETGLOBAL);
    GetModuleFileNameA(g_self, g_cmdpath, MAX_PATH);
    s = strrchr(g_cmdpath, '\\');
    strcpy(s + 1, "coop_cmd.lua");
    strcpy(g_natpath, g_cmdpath);
    strcpy(strrchr(g_natpath, '\\') + 1, "coop_native.txt");
    o_gettop = (void *)make_detour(g_base + RVA_GETTOP, 7, h_gettop, 1);
    make_detour(g_base + RVA_ALERT, 6, h_alert, 0);
    logf_("lua hooks installed (gettop, _ALERT); command file %s", g_cmdpath);
}

static DWORD WINAPI watcher(LPVOID p)
{
    int tries;
    (void)p;
    for (tries = 0; tries < 1200; tries++) {
        HMODULE m = GetModuleHandleA("Dunia.dll");
        if (m) {
            logf_("Dunia.dll at %p, patching IAT", (void *)m);
            logf_("patched %d IAT entries", patch_iat(m));
            install_lua(m);
            return 0;
        }
        Sleep(50);
    }
    logf_("Dunia.dll never appeared");
    return 0;
}

/* real dinput8 */
static FARPROC g_real;
__declspec(naked) HRESULT WINAPI DirectInput8Create(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, LPUNKNOWN e)
{
    __asm__ __volatile__("jmp *%0" : : "m"(g_real));
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], sys[MAX_PATH];
        HMODULE r;
        g_self = inst;
        SetUnhandledExceptionFilter(crash_filter);
        AddVectoredExceptionHandler(1, veh_filter);
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameA(inst, path, MAX_PATH);
        strcpy(strrchr(path, '\\') + 1, "coop_probe.log");
        g_log = fopen(path, "a");
        logf_("probe loaded, cmdline: %s", GetCommandLineA());
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dinput8.dll");
        r = LoadLibraryA(sys);
        g_real = r ? GetProcAddress(r, "DirectInput8Create") : NULL;
        CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    }
    return TRUE;
}
