#include <windows.h>
#include <stdint.h>

/*
 * ROUVY's Windows ZeroConf plug-in uses DnsServiceBrowse(), which is still a
 * stub in Wine 11.  The vendor DLL starts a worker that later lets a C++
 * exception escape when that call fails, terminating the whole Unity process.
 *
 * Network discovery is optional (it is used for LAN/Wahoo discovery).  This
 * replacement keeps the managed wrapper's native handle valid, reports that
 * the adapter is disabled, and makes every other operation a no-op.  BLE and
 * ANT are implemented by separate ROUVY plug-ins and remain untouched.
 */

typedef struct DnsZeroConfAdapterShim {
    uint32_t magic;
} DnsZeroConfAdapterShim;

static DnsZeroConfAdapterShim adapter = { 0x52565A43u }; /* "RVZC" */

__declspec(dllexport) void *CreateDnsZeroConfAdapter(void)
{
    return &adapter;
}

__declspec(dllexport) void DestroyDnsZeroConfAdapter(void *instance)
{
    (void)instance;
}

__declspec(dllexport) BOOL DnsZeroConfAdapter_IsEnabled(void *instance)
{
    (void)instance;
    return FALSE;
}

__declspec(dllexport) void DnsZeroConfAdapter_SetDiscoveryCallback(
    void *instance, void *callback)
{
    (void)instance;
    (void)callback;
}

__declspec(dllexport) void DnsZeroConfAdapter_SetEventCallback(
    void *instance, void *callback)
{
    (void)instance;
    (void)callback;
}

__declspec(dllexport) void DnsZeroConfAdapter_StartScan(
    void *instance, const char *service_type)
{
    (void)instance;
    (void)service_type;
}

__declspec(dllexport) void DnsZeroConfAdapter_StopScan(void *instance)
{
    (void)instance;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
