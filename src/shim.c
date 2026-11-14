/* icue-msi-argb: proxy for MysticLight_SDK_x64.dll used by the iCUE MSI plugin (CueMsiPlugin.dll 2.2.0).
   Lets iCUE drive the JARGB_V2 headers of MSI 800-series boards (LED MCU 0DB0:0076): one colour per header (fast)
   or per-LED through the plugin's Lighting Channel. Config: C:\ProgramData\icue-msi-shim\config.txt
   Controller writes are restricted to: feature 0x50 (store-to-flash byte always 0), feature 0x51 (areas 4/0..2, 8/0),
   output report 0x01 with commands 0x82/0x84 on ports 0-3. Nothing else is ever sent. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <oleauto.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
typedef unsigned long long U;
typedef int (*FN)(U,U,U,U,U,U,U,U);
typedef struct { ULONG Size; USHORT VendorID, ProductID, VersionNumber; } HATTR;
__declspec(dllimport) void __stdcall HidD_GetHidGuid(GUID*);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetAttributes(HANDLE, HATTR*);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetSerialNumberString(HANDLE, PVOID, ULONG);
__declspec(dllimport) BOOLEAN __stdcall HidD_GetFeature(HANDLE, PVOID, ULONG);
__declspec(dllimport) BOOLEAN __stdcall HidD_SetFeature(HANDLE, PVOID, ULONG);
static HMODULE g_real, g_self; static CRITICAL_SECTION g_cs, g_dcs; static volatile LONG g_lines; static int g_init;
#define DIRW L"C:\\ProgramData\\icue-msi-shim"
static void logw(const wchar_t* fmt, ...) {
  if (g_lines > 6000) return; InterlockedIncrement(&g_lines);
  wchar_t buf[2048]; va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 2047, fmt, ap); buf[2047]=0; va_end(ap);
  SYSTEMTIME st; GetLocalTime(&st); EnterCriticalSection(&g_cs);
  FILE* f = _wfopen(DIRW L"\\shim.log", L"a, ccs=UTF-8");
  if (f) { fwprintf(f, L"%02d:%02d:%02d.%03d [%lu] %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), buf); fclose(f); }
  LeaveCriticalSection(&g_cs); }
static int readable(U p, SIZE_T n) { MEMORY_BASIC_INFORMATION m; if (p < 0x10000) return 0; if (!VirtualQuery((void*)p, &m, sizeof m)) return 0; if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD))) return 0; return ((U)m.BaseAddress + m.RegionSize) >= p + n; }
/* ---------- config ---------- */
typedef struct { wchar_t name[64]; BYTE hi, lo; int n, cl, cfirst, rev; DWORD ch[240]; int want; int have, mode, dirty; DWORD lastc, lastpx[240]; ULONGLONG frame_t, refresh_at, nonuni_t; } ZONE; static int g_ncl;
static ZONE g_z[8]; static int g_nz; static wchar_t g_x[64][64]; static int g_nx, g_debug; static wchar_t g_dev[96] = L"MSI Motherboard"; static int g_direct_enabled = 1;
static int g_keysplit = 0; static wchar_t g_board[16] = L"7E51";
static void load_cfg(void) { g_nz = 0; g_nx = 0; FILE* f = _wfopen(DIRW L"\\config.txt", L"r, ccs=UTF-8"); if (!f) { logw(L"config: not found"); return; } wchar_t ln[256];
  while (fgetws(ln, 255, f)) { size_t l = wcslen(ln); while (l && (ln[l-1]==L'\n'||ln[l-1]==L'\r')) ln[--l]=0;
    if (!wcsncmp(ln, L"keysplit=", 9)) g_keysplit = _wtoi(ln+9); else if (!wcsncmp(ln, L"board=", 6)) { wcsncpy(g_board, ln+6, 15); g_board[15] = 0; }
    if (!wcsncmp(ln, L"dev=", 4)) wcsncpy(g_dev, ln+4, 95);
    else if (!wcsncmp(ln, L"direct=", 7)) g_direct_enabled = _wtoi(ln+7); else if (!wcsncmp(ln, L"debug=", 6)) g_debug = _wtoi(ln+6); else if (!wcsncmp(ln, L"led=", 4)) { if (g_nx < 64) wcsncpy(g_x[g_nx++], ln+4, 63); }
    else if ((!wcsncmp(ln, L"zone=", 5) || !wcsncmp(ln, L"channel=", 8)) && g_nz < 8) { int isch = ln[0] == L'c'; wchar_t* nm = ln + (isch ? 8 : 5); wchar_t* p1 = wcschr(nm, L'|'); if (!p1) continue; *p1 = 0; int hi=0, lo=0, n=0, cl=0, rev=0; int got = swscanf(p1+1, L"%d|%d|%d|%d|%d", &hi, &lo, &n, &cl, &rev); if (got < 3) continue; if (!isch) cl = 0; else if (got < 4) cl = n;
      if (!((hi==4 && lo>=0 && lo<=2) || (hi==8 && lo==0)) || n < 1 || n > 240 || (isch && (cl < 1 || cl > 240 || g_ncl + cl > 480))) { logw(L"config: '%ls' rejected (area %d/%d n=%d cl=%d)", nm, hi, lo, n, cl); continue; }
      ZONE* z = &g_z[g_nz++]; memset(z, 0, sizeof *z); wcsncpy(z->name, nm, 63); z->hi=(BYTE)hi; z->lo=(BYTE)lo; z->n=n; z->cl=cl; z->rev=(isch && rev) ? 1 : 0; z->cfirst=g_ncl; g_ncl += cl; logw(L"config: %ls '%ls' area %d/%d leds=%d channel-leds=%d first=%d", isch ? L"channel" : L"zone", nm, hi, lo, n, cl, z->cfirst); } }
  fclose(f); logw(L"config: %d zones, direct=%d, dev='%ls'", g_nz, g_direct_enabled, g_dev); }
/* In-memory patch of CueMsiPlugin.dll 2.2.0 (file on disk is never modified). Each site is checked byte-for-byte before writing; any mismatch => nothing is patched. */
/* keysplit: LEDs 1-100 -> keys 0x20001.., 101-150 -> 0x30001.., 151-160 -> 0x10001.. (the three key groups iCUE 5.52 accepts from plugins). Trampoline allocated within +-2GB of the plugin. */
static int patch_keysplit(BYTE* base) {
  static const BYTE tr[] = { 0x41,0x81,0xFC,0x64,0,0,0, 0x72,0x1B, 0x41,0x81,0xFC,0x96,0,0,0, 0x72,0x09, 0x41,0x81,0xC4,0x6B,0xFF,0x00,0x00, 0xEB,0x10,
    0x41,0x81,0xC4,0x9D,0xFF,0x02,0x00, 0xEB,0x07, 0x41,0x81,0xC4,0x01,0x00,0x02,0x00, 0x44,0x89,0xE0, 0xC1,0xE8,0x10, 0x89,0x85,0x54,0x03,0x00,0x00, 0xC3 };
  BYTE* t = NULL; for (ULONG_PTR a = ((ULONG_PTR)base & ~(ULONG_PTR)0xFFFF) - 0x10000; a > (ULONG_PTR)base - 0x70000000 && !t; a -= 0x10000) t = (BYTE*)VirtualAlloc((void*)a, 4096, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!t) { logw(L"patch: no trampoline memory near plugin"); return 0; }
  /* colour path: plugin turns key into LED index with key-0x20001 (rva 35D6); map the three groups back to 0..159 */
  static const BYTE tr2[] = { 0x41,0x89,0xF8, 0x41,0x81,0xF8,0x01,0x00,0x03,0x00, 0x73,0x19, 0x41,0x81,0xF8,0x01,0x00,0x02,0x00, 0x73,0x08,
    0x41,0x81,0xC0,0x95,0x00,0xFF,0xFF, 0xC3, 0x41,0x81,0xC0,0xFF,0xFF,0xFD,0xFF, 0xC3, 0x41,0x81,0xC0,0x63,0x00,0xFD,0xFF, 0xC3 };
  static const BYTE o4[7] = { 0x44,0x8D,0x87,0xFF,0xFF,0xFD,0xFF };
  if (memcmp(base + 0x35D6, o4, 7)) { logw(L"patch: colour site (rva 035D6) does not match; keysplit skipped"); return 0; }
  memcpy(t, tr, sizeof tr); memcpy(t + 0x80, tr2, sizeof tr2); FlushInstructionCache(GetCurrentProcess(), t, 0x100);
  LONGLONG rel = (LONGLONG)(t - (base + 0xCCA0 + 5)); if (rel > 0x7FFFFFFFLL || rel < -0x80000000LL) { logw(L"patch: trampoline out of range"); return 0; }
  BYTE c1[7] = { 0xE8,0,0,0,0, 0x66,0x90 }; LONG r32 = (LONG)rel; memcpy(c1+1, &r32, 4);
  static const BYTE c2[10] = { 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90 };
  static const BYTE c3[10] = { 0x48,0x8B,0x8D,0x50,0x03,0x00,0x00, 0x48,0x89,0x08 };
  LONGLONG rel2 = (LONGLONG)((t + 0x80) - (base + 0x35D6 + 5)); BYTE c4[7] = { 0xE8,0,0,0,0, 0x66,0x90 }; LONG r2 = (LONG)rel2; memcpy(c4+1, &r2, 4);
  DWORD old2; if (!VirtualProtect(base + 0x3000, 0x1000, PAGE_EXECUTE_READWRITE, &old2)) { logw(L"patch: VirtualProtect(colour) failed %lu", GetLastError()); return 0; }
  memcpy(base + 0x35D6, c4, 7); { DWORD tt; VirtualProtect(base + 0x3000, 0x1000, old2, &tt); FlushInstructionCache(GetCurrentProcess(), base + 0x3000, 0x1000); }
  memcpy(base + 0xCCEC, c2, 10); memcpy(base + 0xCDF7, c3, 10); memcpy(base + 0xCCA0, c1, 7);
  logw(L"patch: keysplit on (trampoline %p): LEDs 1-100 -> 0x20001+, 101-150 -> 0x30001+, 151-160 -> 0x10001+", t); return 1; }
typedef struct { DWORD rva; BYTE orig[10]; int len; int off; } PSITE;
static int patch_plugin(void) {
  if (!g_keysplit) return 0;
  HMODULE m = GetModuleHandleW(L"CueMsiPlugin.dll"); if (!m) { logw(L"patch: CueMsiPlugin.dll not loaded"); return 0; }
  static const PSITE s[3] = {
    { 0xCCA0, {0x41,0x81,0xC4,0x01,0x00,0x02,0x00}, 7, 3 },                 /* add r12d,20001h  (LED key)   */
    { 0xCCEC, {0xC7,0x85,0x54,0x03,0x00,0x00,0x02,0x00,0x00,0x00}, 10, 6 }, /* mov [rbp+354h],2 (LED group) */
    { 0xCDF7, {0x44,0x89,0x20,0xC7,0x40,0x04,0x02,0x00,0x00,0x00}, 10, 6 } };/* mov [rax+4],2    (LED group) */
  BYTE* base = (BYTE*)m; for (int i = 0; i < 3; i++) if (memcmp(base + s[i].rva, s[i].orig, s[i].len)) { logw(L"patch: plugin is not CueMsiPlugin 2.2.0 (site %05X differs); keysplit off, iCUE will use at most 100 LEDs", s[i].rva); return 0; }
  DWORD old; if (!VirtualProtect(base + 0xCC00, 0x400, PAGE_EXECUTE_READWRITE, &old)) { logw(L"patch: VirtualProtect failed %lu", GetLastError()); return 0; }
  int ok = patch_keysplit(base);
  DWORD t; VirtualProtect(base + 0xCC00, 0x400, old, &t); FlushInstructionCache(GetCurrentProcess(), base + 0xCC00, 0x400); return ok; }
/* ---------- direct HID ---------- */
static HANDLE g_thr, g_stop; static CRITICAL_SECTION g_zcs; static LONG st_calls, st_sends, st_changes; static double st_sum, st_max; static ULONGLONG st_t0; static DWORD WINAPI worker(LPVOID p); static HANDLE g_hf = INVALID_HANDLE_VALUE, g_hio = INVALID_HANDLE_VALUE; static BYTE g_orig[290]; static int g_on, g_failed, g_ever; static ULONGLONG g_lastcheck; static LONG g_frames, g_reprep;
static int find_path(wchar_t* out, int cch) { GUID g; HidD_GetHidGuid(&g); HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT|DIGCF_DEVICEINTERFACE); if (di == INVALID_HANDLE_VALUE) return 0; int found = 0;
  SP_DEVICE_INTERFACE_DATA id; id.cbSize = sizeof id; for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(di, NULL, &g, i, &id); i++) { BYTE buf[1024]; SP_DEVICE_INTERFACE_DETAIL_DATA_W* dd = (void*)buf; dd->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(di, &id, dd, sizeof buf, NULL, NULL)) continue; HANDLE h = CreateFileW(dd->DevicePath, 0, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL); if (h == INVALID_HANDLE_VALUE) continue;
    HATTR a; a.Size = sizeof a; if (HidD_GetAttributes(h, &a) && a.VendorID == 0x0DB0 && a.ProductID == 0x0076) { wcsncpy(out, dd->DevicePath, cch-1); out[cch-1]=0; found = 1; } CloseHandle(h); }
  SetupDiDestroyDeviceInfoList(di); return found; }
static int hid_cmd(BYTE cmd, BYTE port, BYTE val, BYTE* reply) { if ((cmd != 0x82 && cmd != 0x84) || port > 3) return 0; BYTE o[64]; memset(o, 0, 64); o[0]=0x01; o[1]=cmd; o[6]=port; if (cmd == 0x84) o[7] = val ? 1 : 0;
  OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); DWORD n = 0; int ok = 0;
  if (!WriteFile(g_hio, o, 64, &n, &ov) && GetLastError() != ERROR_IO_PENDING) { CloseHandle(ov.hEvent); return 0; }
  if (WaitForSingleObject(ov.hEvent, 400) != WAIT_OBJECT_0) { CancelIo(g_hio); CloseHandle(ov.hEvent); return 0; }
  ULONGLONG end = GetTickCount64() + 700; while (GetTickCount64() < end) { BYTE b[64]; memset(b, 0, 64); ResetEvent(ov.hEvent); n = 0;
    if (!ReadFile(g_hio, b, 64, &n, &ov) && GetLastError() != ERROR_IO_PENDING) break; if (WaitForSingleObject(ov.hEvent, 700) != WAIT_OBJECT_0) { CancelIo(g_hio); break; }
    if (b[0] == 0x01 && b[1] == 0x5A && b[6] == port) { if (reply) memcpy(reply, b, 64); ok = 1; break; } }
  CloseHandle(ov.hEvent); return ok; }
static int set_board(const BYTE* src) { BYTE p[290]; memcpy(p, src, 290); p[0] = 0x50; p[289] = 0; /* never store */ return HidD_SetFeature(g_hf, p, 290) ? 1 : 0; }
static HANDLE take_mutex(void) { HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\Access_SMBUS.HTP.Method"); if (!m) return NULL; DWORD r = WaitForSingleObject(m, 150); if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) { CloseHandle(m); return NULL; } return m; }
static void drop_mutex(HANDLE m) { if (m) { ReleaseMutex(m); CloseHandle(m); } }
static void direct_close(void) { if (g_hf != INVALID_HANDLE_VALUE) CloseHandle(g_hf); if (g_hio != INVALID_HANDLE_VALUE) CloseHandle(g_hio); g_hf = g_hio = INVALID_HANDLE_VALUE; }
static int direct_begin(void) { wchar_t path[512]; if (!g_direct_enabled || !g_nz) return 0; if (!find_path(path, 512)) { logw(L"direct: controller 0DB0:0076 not found"); return 0; }
  g_hf = CreateFileW(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  g_hio = CreateFileW(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if (g_hf == INVALID_HANDLE_VALUE || g_hio == INVALID_HANDLE_VALUE) { logw(L"direct: open failed %lu", GetLastError()); direct_close(); return 0; }
  wchar_t sn[64]; memset(sn, 0, sizeof sn); if (!HidD_GetSerialNumberString(g_hf, sn, sizeof sn - 2) || (g_board[0] && _wcsnicmp(sn, g_board, wcslen(g_board)))) { logw(L"direct: controller serial '%ls' does not start with board='%ls'; direct mode off", sn, g_board); direct_close(); return 0; }
  HANDLE m = take_mutex(); BYTE b[290]; memset(b, 0, 290); b[0] = 0x50; if (!HidD_GetFeature(g_hf, b, 290)) { logw(L"direct: GET 0x50 failed %lu", GetLastError()); drop_mutex(m); direct_close(); return 0; }
  if (b[1] == 9) { FILE* f = _wfopen(DIRW L"\\board_state.bin", L"rb"); BYTE s[290]; int got = f ? (int)fread(s, 1, 290, f) : 0; if (f) fclose(f); if (got == 290 && s[0] == 0x50 && s[1] != 9) { memcpy(b, s, 290); logw(L"direct: board already in Advanced; using saved state file as original"); } else logw(L"direct: board already in Advanced and no saved state; original will be Advanced"); }
  else { FILE* f = _wfopen(DIRW L"\\board_state.bin", L"wb"); if (f) { fwrite(b, 1, 290, f); fclose(f); } }
  memcpy(g_orig, b, 290);
  for (BYTE port = 0; port < 4; port++) { BYTE r[64]; int d = hid_cmd(0x82, port, 0, r); int e = hid_cmd(0x84, port, 0, NULL); logw(L"direct: port %d gen2 detect=%d strips=%d, disable=%d", port, d, d ? r[7] : -1, e); if (!e) { logw(L"direct: gen2 disable not acknowledged; aborting direct mode"); for (BYTE q = 0; q <= port; q++) hid_cmd(0x84, q, 1, NULL); drop_mutex(m); direct_close(); return 0; } }
  int ok = 1; drop_mutex(m); logw(L"direct: gen2 off; board-packet mode (0x50 colours, store=0), mutex=%d", m != NULL); if (!ok) { for (BYTE q = 0; q < 4; q++) hid_cmd(0x84, q, 1, NULL); direct_close(); return 0; }
  for (int i = 0; i < g_nz; i++) { g_z[i].have = 0; g_z[i].want = 0; g_z[i].mode = 0; g_z[i].dirty = 0; } g_lastcheck = GetTickCount64(); g_on = 1; g_ever = 1; g_stop = CreateEventW(NULL, TRUE, FALSE, NULL); g_thr = CreateThread(NULL, 0, worker, NULL, 0, NULL); logw(L"direct: ON (worker=%p)", g_thr); return 1; }
static void direct_end(const wchar_t* why) { if (!g_on) return; if (g_thr) { SetEvent(g_stop); WaitForSingleObject(g_thr, 3000); CloseHandle(g_thr); CloseHandle(g_stop); g_thr = NULL; } HANDLE m = take_mutex(); int e = 0; for (BYTE q = 0; q < 4; q++) e += hid_cmd(0x84, q, 1, NULL); int ok = set_board(g_orig); drop_mutex(m); direct_close(); g_on = 0; logw(L"direct: OFF (%ls) gen2 re-enabled on %d ports, restore=%d, frames sent=%ld, re-preps=%ld", why, e, ok, g_frames, g_reprep); }
static int send_px(ZONE* z, const DWORD* px) { BYTE f[727]; memset(f, 0, 727); if (!((z->hi==4 && z->lo<=2) || (z->hi==8 && z->lo==0)) || z->n < 1 || z->n > 240) return 0; f[0]=0x51; f[1]=0x09; f[2]=z->hi; f[3]=z->lo; f[6]=(BYTE)z->n; for (int i = 0; i < z->n; i++) { f[7+3*i]=(BYTE)(px[i]>>16); f[8+3*i]=(BYTE)(px[i]>>8); f[9+3*i]=(BYTE)px[i]; }
  if (HidD_SetFeature(g_hf, f, 727)) { InterlockedIncrement(&g_frames); return 1; } if (g_frames < 5) logw(L"direct: frame to %02X/%02X failed %lu", z->hi, z->lo, GetLastError()); return 0; }
static void dbg_frame(BSTR* names, LONG n, U* a) { static LONG lastn = -1; static ULONGLONG lastt; ULONGLONG now = GetTickCount64();
  if (n != lastn) { lastn = n; wchar_t o[1900]; o[0] = 0; for (LONG k = 0; k < n && wcslen(o) < 1700; k++) { wcsncat(o, names[k] ? names[k] : L"<null>", 60); wcscat(o, L" | "); } logw(L"dbg: SetLedColorsSync n=%ld a5=%llx a6=%llx a7=%llx names: %ls", n, a[5], a[6], a[7], o); }
  if (now - lastt > 4000) { lastt = now; wchar_t o[1900]; o[0] = 0; for (LONG k = 0; k < n && k < 60; k++) { wchar_t t[24]; swprintf(t, 24, L"%02X%02X%02X ", (BYTE)((DWORD*)a[2])[k], (BYTE)((DWORD*)a[3])[k], (BYTE)((DWORD*)a[4])[k]); wcscat(o, t); } logw(L"dbg: colours[%ld]: %ls", n, o); } }
static void direct_frame(U* a) { __try { if (!readable(a[1],8) || !readable(a[2],4) || !readable(a[3],4) || !readable(a[4],4)) return; SAFEARRAY* sa = *(SAFEARRAY**)a[1]; if (!sa || !sa->pvData) return; LONG n = (LONG)sa->rgsabound[0].cElements; if (n > 512) n = 512; BSTR* names = (BSTR*)sa->pvData; if (g_debug) dbg_frame(names, n, a);
    EnterCriticalSection(&g_zcs); for (LONG k = 0; k < n; k++) { if (!names[k]) continue; DWORD c = ((((DWORD*)a[2])[k] & 255) << 16) | ((((DWORD*)a[3])[k] & 255) << 8) | (((DWORD*)a[4])[k] & 255);
      if (g_ncl && !wcsncmp(names[k], L"JCORSAIR ", 9)) { int idx = _wtoi(names[k] + 9) - 1; for (int i = 0; i < g_nz; i++) if (g_z[i].cl && idx >= g_z[i].cfirst && idx < g_z[i].cfirst + g_z[i].cl) { g_z[i].ch[idx - g_z[i].cfirst] = c; g_z[i].want = 1; break; } continue; }
      for (int i = 0; i < g_nz; i++) if (!g_z[i].cl && !wcscmp(g_z[i].name, names[k])) { g_z[i].ch[0] = c; g_z[i].want = 1; break; } } LeaveCriticalSection(&g_zcs); InterlockedIncrement(&st_calls);
  } __except(1) { logw(L"direct: exception in frame"); } }
/* channel points -> physical LEDs: more points than LEDs => average every point that falls on the LED (no dead points); fewer => centred sampling */
static DWORD map_px(const DWORD* c, int cl, int n, int k) { if (cl == n) return c[k];
  if (cl < n) { int j = ((2*k + 1) * cl) / (2*n); return c[j < cl ? j : cl - 1]; }
  int s = (k * cl) / n, e = ((k + 1) * cl + n - 1) / n; if (e > cl) e = cl; if (e <= s) e = s + 1; unsigned r = 0, g = 0, b = 0;
  for (int j = s; j < e; j++) { r += (c[j] >> 16) & 255; g += (c[j] >> 8) & 255; b += c[j] & 255; } int m = e - s; return ((r / m) << 16) | ((g / m) << 8) | (b / m); }
static DWORD WINAPI worker(LPVOID p) { LARGE_INTEGER fq, t0, t1; QueryPerformanceFrequency(&fq); st_t0 = GetTickCount64(); st_calls = st_sends = st_changes = 0; st_sum = st_max = 0; ULONGLONG lastsend = 0; static DWORD ch[8][240], px[8][240]; int rr = 0; LONG st_fr = 0;
  while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) { ULONGLONG now = GetTickCount64(); int want[8], nm[8]; int changed = 0, any = 0, did = 0;
    EnterCriticalSection(&g_zcs); for (int i = 0; i < g_nz; i++) { want[i] = g_z[i].want; if (want[i]) memcpy(ch[i], g_z[i].ch, sizeof(DWORD) * (g_z[i].cl ? g_z[i].cl : 1)); } LeaveCriticalSection(&g_zcs);
    for (int i = 0; i < g_nz; i++) if (want[i]) { ZONE* z = &g_z[i]; int uni = 1; any = 1; for (int k = 0; k < z->n; k++) { px[i][k] = z->cl ? map_px(ch[i], z->cl, z->n, z->rev ? z->n - 1 - k : k) : ch[i][0]; if (px[i][k] != px[i][0]) uni = 0; }
      if (!uni) z->nonuni_t = now; nm[i] = (!uni || (z->have && z->mode == 9 && now - z->nonuni_t < 1000)) ? 9 : 2; /* one colour => fast 0x50 path; per-LED => 0x51 frames; stay per-LED 1 s after the last non-uniform frame */
      if (!z->have || nm[i] != z->mode || (nm[i] == 2 && px[i][0] != z->lastc)) changed = 1; }
    if (any && (changed || now - lastsend > 2000)) { BYTE b[290]; memcpy(b, g_orig, 290);
      for (int i = 0; i < g_nz; i++) if (want[i]) { ZONE* z = &g_z[i]; int idx = (z->hi == 8) ? 3 : z->lo; int o = 1 + 16*idx; if (z->cl) b[o+15] = (BYTE)z->n; /* Cycle_Num = strip length, as MSI does */
        if (nm[i] == 2) { b[o] = 2; b[o+1] = (BYTE)(px[i][0] >> 16); b[o+2] = (BYTE)(px[i][0] >> 8); b[o+3] = (BYTE)px[i][0]; b[o+14] = (BYTE)((g_orig[o+14] & 0x43) | 0x20 | (5 << 2)); }
        else { b[o] = 9; b[o+14] = (BYTE)((g_orig[o+14] & 0x63) | (5 << 2)); } }
      QueryPerformanceCounter(&t0); int ok = set_board(b); QueryPerformanceCounter(&t1); double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
      if (ok) { for (int i = 0; i < g_nz; i++) if (want[i]) { ZONE* z = &g_z[i]; if (nm[i] == 9 && (!z->have || z->mode != 9)) { z->dirty = 1; z->refresh_at = now + 400; } z->mode = nm[i]; z->lastc = px[i][0]; z->have = 1; } InterlockedIncrement(&g_frames); } else if (g_frames < 5) logw(L"direct: board packet failed %lu", GetLastError());
      st_sum += ms; if (ms > st_max) st_max = ms; st_sends++; if (changed) st_changes++; lastsend = now; did = 1; }
    for (int q = 0; q < g_nz; q++) { int i = (rr + q) % g_nz; ZONE* z = &g_z[i]; if (!want[i] || !z->have || z->mode != 9) continue; int due = z->refresh_at && now >= z->refresh_at;
      if (z->dirty || due || memcmp(px[i], z->lastpx, sizeof(DWORD) * z->n) || now - z->frame_t > 10000) { if (send_px(z, px[i])) { memcpy(z->lastpx, px[i], sizeof(DWORD) * z->n); z->dirty = 0; if (due) z->refresh_at = 0; z->frame_t = now; st_fr++; st_changes++; } else Sleep(20); rr = i + 1; did = 1; break; } }
    if (!did) Sleep(2);
    if (now - st_t0 >= 5000) { double s = (double)(now - st_t0) / 1000.0; if (g_debug && st_changes > 3) logw(L"stats: plugin frames %.1f/s, board packets %.1f/s (avg %.1f ms, max %.1f ms), per-LED frames %.1f/s", st_calls / s, st_sends / s, st_sends ? st_sum / st_sends : 0.0, st_max, st_fr / s); st_calls = st_sends = st_changes = 0; st_fr = 0; st_sum = st_max = 0; st_t0 = now; } }
  return 0; }
/* ---------- SDK proxy ---------- */
static void ensure(void) { if (g_init) return; EnterCriticalSection(&g_cs); if (!g_init) { wchar_t p[MAX_PATH]; GetModuleFileNameW(g_self, p, MAX_PATH); wchar_t* s = wcsrchr(p, L'\\'); if (s) wcscpy(s+1, L"MysticLight_SDK_x64_real.dll"); g_real = LoadLibraryW(p); g_init = 1; LeaveCriticalSection(&g_cs); logw(L"real SDK => %p", g_real); load_cfg(); patch_plugin(); return; } LeaveCriticalSection(&g_cs); }
static void add_style(U pp) { __try { SAFEARRAY* sa = *(SAFEARRAY**)pp; if (!sa) return; LONG lb=0, ub=-1; SafeArrayGetLBound(sa,1,&lb); SafeArrayGetUBound(sa,1,&ub); LONG n = ub-lb+1; SAFEARRAY* ns = SafeArrayCreateVector(VT_BSTR, 0, n+1); BSTR* s; BSTR* d; SafeArrayAccessData(sa,(void**)&s); SafeArrayAccessData(ns,(void**)&d); for (LONG i=0;i<n;i++) d[i] = SysAllocString(s[i]); d[n] = SysAllocString(L"Direct All Sync"); SafeArrayUnaccessData(sa); SafeArrayUnaccessData(ns); *(SAFEARRAY**)pp = ns; } __except(1) {} }
static void set_zone_names(U pp) { __try { int nzn = 0, k = 0; for (int i=0;i<g_nz;i++) if (!g_z[i].cl) nzn++; SAFEARRAY* ns = SafeArrayCreateVector(VT_BSTR, 0, g_ncl + nzn + g_nx); BSTR* d; SafeArrayAccessData(ns,(void**)&d); for (int i=0;i<g_ncl;i++) { wchar_t t[24]; swprintf(t, 24, L"JCORSAIR %03d", i+1); d[k++] = SysAllocString(t); } for (int i=0;i<g_nz;i++) if (!g_z[i].cl) d[k++] = SysAllocString(g_z[i].name); for (int i=0;i<g_nx;i++) d[k++] = SysAllocString(g_x[i]); SafeArrayUnaccessData(ns); *(SAFEARRAY**)pp = ns; } __except(1) {} }
static void set_dev_name(U pp) { __try { SAFEARRAY* ns = SafeArrayCreateVector(VT_BSTR, 0, 1); BSTR* d; SafeArrayAccessData(ns,(void**)&d); d[0] = SysAllocString(g_dev); SafeArrayUnaccessData(ns); *(SAFEARRAY**)pp = ns; } __except(1) {} }
static DWORD g_lr = 999, g_lg, g_lb; static ULONGLONG g_lt; static int g_steady;
static void sdk_fallback(U* a) { __try { typedef int (*FS)(BSTR, DWORD, BSTR); typedef int (*FC)(BSTR, DWORD, DWORD, DWORD, DWORD); if (!readable(a[2],4)) return; DWORD r = ((DWORD*)a[2])[0] & 255, g = ((DWORD*)a[3])[0] & 255, b = ((DWORD*)a[4])[0] & 255; if (r == g_lr && g == g_lg && b == g_lb) return; ULONGLONG now = GetTickCount64(); if (now - g_lt < 250) return;
    BSTR type = SysAllocString(L"MSI_MB"); if (!g_steady) { BSTR st = SysAllocString(L"Steady"); ((FS)GetProcAddress(g_real, "MLAPI_SetLedStyle"))(type, 0, st); SysFreeString(st); g_steady = 1; } ((FC)GetProcAddress(g_real, "MLAPI_SetLedColor"))(type, 0, r, g, b); SysFreeString(type); g_lr=r; g_lg=g; g_lb=b; g_lt=GetTickCount64(); } __except(1) {} }
static int call(const char* name, U* a) { ensure(); if (!g_real) return -3;
  if (!strcmp(name, "MLAPI_SetLedColorsSync")) { EnterCriticalSection(&g_dcs); if (!g_on && !g_failed) { if (!direct_begin()) { g_failed = 1; logw(L"direct: unavailable, falling back to SDK path"); } } if (g_on) direct_frame(a); else sdk_fallback(a); LeaveCriticalSection(&g_dcs); return 0; }
  if (!strcmp(name, "MLAPI_SetLedStyle") && readable(a[2],4)) { const wchar_t* st = (const wchar_t*)a[2]; if (!wcscmp(st, L"Direct All Sync")) { logw(L"plugin: SetLedStyle Direct All Sync (faked ok)"); return 0; }
    if (!wcscmp(st, L"Default")) { EnterCriticalSection(&g_dcs); int was = g_on; direct_end(L"plugin released control"); g_failed = 0; LeaveCriticalSection(&g_dcs); if (was || g_ever) return 0; } }
  if (!strcmp(name, "MLAPI_Release")) { EnterCriticalSection(&g_dcs); direct_end(L"MLAPI_Release"); LeaveCriticalSection(&g_dcs); }
  if (!strncmp(name, "MLAPI_Set", 9) && g_direct_enabled && g_nz) { logw(L"blocked (not forwarded): %hs(%llx, %llx, %llx, %llx, %llx, %llx) a0='%ls'", name, a[0], a[1], a[2], a[3], a[4], a[5], readable(a[0],2) ? (wchar_t*)a[0] : L""); return 0; }
  FN f = (FN)GetProcAddress(g_real, name); if (!f) return -3; int r = f(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
  if (r == 0) { if (!strcmp(name, "MLAPI_GetLedInfo")) add_style(a[3]); else if (!strcmp(name, "MLAPI_GetLedName") && g_nz) set_zone_names(a[1]); else if (!strcmp(name, "MLAPI_GetDeviceName")) set_dev_name(a[1]); }
  if (strncmp(name, "MLAPI_Get", 9)) logw(L"sdk: %hs => %d", name, r); return r; }
#define P(name) __declspec(dllexport) int name(U a,U b,U c,U d,U e,U f,U g,U h){ U v[8]={a,b,c,d,e,f,g,h}; return call(#name, v); }
P(MLAPI_Initialize) P(MLAPI_Release) P(MLAPI_GetDeviceInfo) P(MLAPI_GetDeviceName) P(MLAPI_GetDeviceNameEx) P(MLAPI_GetErrorMessage) P(MLAPI_GetLedName) P(MLAPI_GetLedInfo) P(MLAPI_GetLedColor) P(MLAPI_GetLedStyle) P(MLAPI_GetLedMaxBright) P(MLAPI_GetLedBright) P(MLAPI_GetLedMaxSpeed) P(MLAPI_GetLedSpeed) P(MLAPI_SetLedColor) P(MLAPI_SetLedStyle) P(MLAPI_SetLedBright) P(MLAPI_SetLedSpeed) P(MLAPI_SetLedColorEx) P(MLAPI_SetLedColors) P(MLAPI_SetLedColorSync) P(MLAPI_SetLedColorsSync) P(MLAPI_MysticLightControlNotify)
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { if (reason == DLL_PROCESS_ATTACH) { g_self = h; InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_dcs); InitializeCriticalSection(&g_zcs); DisableThreadLibraryCalls(h); CreateDirectoryW(DIRW, NULL); { WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExW(DIRW L"\\shim.log", GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 1048576)) MoveFileExW(DIRW L"\\shim.log", DIRW L"\\shim.old.log", MOVEFILE_REPLACE_EXISTING); } logw(L"==== icue-msi-argb 1.0 attached"); } return TRUE; }


