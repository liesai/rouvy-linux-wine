#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define BRIDGE_PORT 28765
#define PACKET_MAX 4096

extern IMAGE_DOS_HEADER __ImageBase;

typedef void (__cdecl *notify_cb)(void *sender);
typedef void (__cdecl *info_cb)(void *sender, uint64_t address, uint64_t timestamp,
                                int8_t rssi, const wchar_t *name,
                                int32_t packet_type, uint8_t flags);
typedef void (__cdecl *uuid_cb)(void *sender, uint64_t address, uint64_t timestamp,
                                int8_t rssi, const GUID *uuid);
typedef void (__cdecl *manf_cb)(void *sender, uint64_t address, uint64_t timestamp,
                                int8_t rssi, uint16_t company_id,
                                uint16_t data_size, void *payload);

typedef struct watcher {
    info_cb info;
    notify_cb started;
    notify_cb stopped;
    uuid_cb uuid;
    manf_cb manufacturer;
    volatile LONG active;
    SOCKET socket;
    HANDLE thread;
} watcher;

typedef void (__cdecl *gatt_event_cb)(void *sender, int error);
typedef void (__cdecl *gatt_changed_cb)(void *sender, uint8_t handle, void *value, uint32_t value_len);

typedef struct gatt_client {
    gatt_event_cb connected;
    gatt_event_cb disconnected;
    gatt_changed_cb changed;
    int state;
    uint64_t address;
    struct gatt_client *next;
} gatt_client;

/* The managed CPPBridge structs all declare [StructLayout(Pack = 1)]. */
#pragma pack(push, 1)
typedef struct gatt_uuid {
    BOOL is_short_uuid;
    uint16_t short_uuid;
    GUID long_uuid;
} gatt_uuid;

typedef struct gatt_service {
    gatt_uuid uuid;
    uint16_t handle;
} gatt_service;

typedef struct gatt_services {
    uint8_t count;
    gatt_service services[255];
} gatt_services;

typedef struct gatt_characteristic {
    uint16_t service_handle;
    gatt_uuid uuid;
    uint16_t handle;
    uint16_t value_handle;
    BOOL is_broadcastable;
    BOOL is_readable;
    BOOL is_writable;
    BOOL is_writable_without_response;
    BOOL is_signed_writable;
    BOOL is_notifiable;
    BOOL is_indicatable;
    BOOL has_extended_properties;
} gatt_characteristic;

typedef struct gatt_characteristics {
    uint8_t count;
    gatt_characteristic chars[255];
} gatt_characteristics;
#pragma pack(pop)

static gatt_client *clients;
static SRWLOCK clients_lock = SRWLOCK_INIT;
static volatile LONG notification_count;

__declspec(dllexport) int __cdecl WCLWatcherStop(watcher *w);

static HMODULE original;

static void log_line(const char *message)
{
    FILE *f = fopen("Z:\\tmp\\rouvy-ble-dll.log", "a");
    if (!f) return;
    fprintf(f, "%lu %s\n", GetTickCount(), message);
    fclose(f);
}

static void log_fmt(const char *format, ...)
{
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    log_line(message);
}

static FARPROC original_proc(const char *name)
{
    wchar_t path[MAX_PATH];
    wchar_t *slash;
    if (!original) {
        if (!GetModuleFileNameW((HMODULE)&__ImageBase, path, MAX_PATH)) return NULL;
        slash = wcsrchr(path, L'\\');
        if (!slash) return NULL;
        wcscpy(slash + 1, L"WclBlePluginCPP.original.dll");
        original = LoadLibraryW(path);
        if (!original) log_line("failed to load original WCL DLL");
    }
    return original ? GetProcAddress(original, name) : NULL;
}

static int parse_guid(const char *s, GUID *g)
{
    unsigned int d1, d2, d3, d4[8];
    int n = sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &d1, &d2, &d3, &d4[0], &d4[1], &d4[2], &d4[3],
        &d4[4], &d4[5], &d4[6], &d4[7]);
    if (n != 11) return 0;
    g->Data1 = d1;
    g->Data2 = (uint16_t)d2;
    g->Data3 = (uint16_t)d3;
    for (int i = 0; i < 8; ++i) g->Data4[i] = (uint8_t)d4[i];
    return 1;
}

static uint16_t hex_payload(const char *hex, uint8_t *out, uint16_t capacity)
{
    uint16_t count = 0;
    while (hex[0] && hex[1] && count < capacity) {
        unsigned int byte;
        if (sscanf(hex, "%2x", &byte) != 1) break;
        out[count++] = (uint8_t)byte;
        hex += 2;
    }
    return count;
}

static int rpc_call(const char *command, char *response, size_t capacity)
{
    WSADATA data;
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in address;
    size_t used = 0;
    int ok = 0;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) goto done;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(28766);
    if (connect(s, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) goto done;
    if (send(s, command, (int)strlen(command), 0) <= 0 || send(s, "\n", 1, 0) <= 0) goto done;
    shutdown(s, SD_SEND);
    while (used + 1 < capacity) {
        int got = recv(s, response + used, (int)(capacity - used - 1), 0);
        if (got <= 0) break;
        used += got;
    }
    response[used] = 0;
    ok = used >= 2 && !strncmp(response, "OK", 2);
done:
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
    return ok;
}

static void uuid_to_gatt(const char *text, gatt_uuid *out)
{
    static const char suffix[] = "-0000-1000-8000-00805f9b34fb";
    memset(out, 0, sizeof(*out));
    if (strlen(text) == 36 && !strncmp(text, "0000", 4) && !strcmp(text + 8, suffix)) {
        unsigned int short_uuid = 0;
        sscanf(text + 4, "%4x", &short_uuid);
        out->is_short_uuid = TRUE;
        out->short_uuid = (uint16_t)short_uuid;
    }
    parse_guid(text, &out->long_uuid);
}

static void notify_characteristic(uint64_t address, uint8_t handle, void *value, uint32_t length)
{
    int matched = 0;
    AcquireSRWLockShared(&clients_lock);
    for (gatt_client *client = clients; client; client = client->next) {
        if (client->address == address && client->changed) {
            ++matched;
            client->changed(client, handle, value, length);
        }
    }
    ReleaseSRWLockShared(&clients_lock);
    LONG count = InterlockedIncrement(&notification_count);
    if (count <= 20 || count % 100 == 0)
        log_fmt("GATT notify #%ld address=%012llX handle=%u length=%u callbacks=%d",
                count, (unsigned long long)address, handle, length, matched);
}

static DWORD WINAPI receiver_thread(void *opaque)
{
    watcher *w = opaque;
    char packet[PACKET_MAX];
    log_line("receiver started");
    while (InterlockedCompareExchange(&w->active, 0, 0)) {
        int size = recv(w->socket, packet, sizeof(packet) - 1, 0);
        if (size <= 0) break;
        packet[size] = 0;

        char *save = NULL;
        char *kind = strtok_s(packet, "\t\r\n", &save);
        char *address_s = strtok_s(NULL, "\t\r\n", &save);
        if (kind && kind[0] == 'N') {
            char *handle_s = strtok_s(NULL, "\t\r\n", &save);
            char *payload_s = strtok_s(NULL, "\t\r\n", &save);
            uint8_t payload[1024];
            uint16_t payload_size = payload_s ? hex_payload(payload_s, payload, sizeof(payload)) : 0;
            if (address_s && handle_s)
                notify_characteristic(_strtoui64(address_s, NULL, 16),
                                      (uint8_t)strtoul(handle_s, NULL, 10),
                                      payload, payload_size);
            continue;
        }
        char *timestamp_s = strtok_s(NULL, "\t\r\n", &save);
        char *rssi_s = strtok_s(NULL, "\t\r\n", &save);
        if (!kind || !address_s || !timestamp_s || !rssi_s) continue;

        uint64_t address = _strtoui64(address_s, NULL, 16);
        uint64_t timestamp = _strtoui64(timestamp_s, NULL, 10);
        int8_t rssi = (int8_t)strtol(rssi_s, NULL, 10);

        if (kind[0] == 'D' && w->info) {
            char *name = strtok_s(NULL, "\t\r\n", &save);
            char *type_s = strtok_s(NULL, "\t\r\n", &save);
            char *flags_s = strtok_s(NULL, "\t\r\n", &save);
            wchar_t wide_name[256] = L"";
            if (name && strcmp(name, "-") != 0)
                MultiByteToWideChar(CP_UTF8, 0, name, -1, wide_name, 256);
            w->info(w, address, timestamp, rssi, wide_name,
                    type_s ? strtol(type_s, NULL, 10) : 0,
                    flags_s ? (uint8_t)strtoul(flags_s, NULL, 10) : 6);
        } else if (kind[0] == 'U' && w->uuid) {
            char *uuid_s = strtok_s(NULL, "\t\r\n", &save);
            GUID uuid;
            if (uuid_s && parse_guid(uuid_s, &uuid))
                w->uuid(w, address, timestamp, rssi, &uuid);
        } else if (kind[0] == 'M' && w->manufacturer) {
            char *company_s = strtok_s(NULL, "\t\r\n", &save);
            char *payload_s = strtok_s(NULL, "\t\r\n", &save);
            uint8_t payload[1024];
            uint16_t payload_size = payload_s ? hex_payload(payload_s, payload, sizeof(payload)) : 0;
            w->manufacturer(w, address, timestamp, rssi,
                            company_s ? (uint16_t)strtoul(company_s, NULL, 10) : 0,
                            payload_size, payload);
        }
    }
    log_line("receiver stopped");
    return 0;
}

__declspec(dllexport) void *__cdecl WCLWatcherCreate(info_cb info, notify_cb started,
    notify_cb stopped, uuid_cb uuid, manf_cb manufacturer)
{
    watcher *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->info = info;
    w->started = started;
    w->stopped = stopped;
    w->uuid = uuid;
    w->manufacturer = manufacturer;
    w->socket = INVALID_SOCKET;
    log_line("watcher created");
    return w;
}

__declspec(dllexport) void __cdecl WCLWatcherDestroy(watcher *w)
{
    if (!w) return;
    WCLWatcherStop(w);
    free(w);
    log_line("watcher destroyed");
}

__declspec(dllexport) BOOL __cdecl WCLWatcherGetActive(watcher *w)
{
    return w && InterlockedCompareExchange(&w->active, 0, 0);
}

__declspec(dllexport) int __cdecl WCLWatcherStart(watcher *w, void *radio)
{
    WSADATA data;
    struct sockaddr_in address;
    (void)radio;
    if (!w) return -1;
    if (InterlockedCompareExchange(&w->active, 1, 0)) return 0;
    if (WSAStartup(MAKEWORD(2, 2), &data)) goto fail;
    w->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (w->socket == INVALID_SOCKET) goto fail;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(BRIDGE_PORT);
    if (bind(w->socket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) goto fail;
    w->thread = CreateThread(NULL, 0, receiver_thread, w, 0, NULL);
    if (!w->thread) goto fail;
    if (w->started) w->started(w);
    log_line("watcher started successfully");
    return 0;
fail:
    log_line("watcher start failed");
    if (w->socket != INVALID_SOCKET) closesocket(w->socket);
    w->socket = INVALID_SOCKET;
    InterlockedExchange(&w->active, 0);
    WSACleanup();
    return -1;
}

__declspec(dllexport) int __cdecl WCLWatcherStop(watcher *w)
{
    if (!w || !InterlockedExchange(&w->active, 0)) return 0;
    if (w->socket != INVALID_SOCKET) {
        closesocket(w->socket);
        w->socket = INVALID_SOCKET;
    }
    if (w->thread) {
        WaitForSingleObject(w->thread, 3000);
        CloseHandle(w->thread);
        w->thread = NULL;
    }
    WSACleanup();
    if (w->stopped) w->stopped(w);
    return 0;
}

#define FORWARD0(ret, name, fallback) \
    __declspec(dllexport) ret __cdecl name(void) { \
        typedef ret (__cdecl *fn_t)(void); fn_t fn = (fn_t)original_proc(#name); \
        return fn ? fn() : (fallback); }
#define FORWARD1(ret, name, t1, fallback) \
    __declspec(dllexport) ret __cdecl name(t1 a1) { \
        typedef ret (__cdecl *fn_t)(t1); fn_t fn = (fn_t)original_proc(#name); \
        return fn ? fn(a1) : (fallback); }
#define FORWARD2(ret, name, t1, t2, fallback) \
    __declspec(dllexport) ret __cdecl name(t1 a1, t2 a2) { \
        typedef ret (__cdecl *fn_t)(t1,t2); fn_t fn = (fn_t)original_proc(#name); \
        return fn ? fn(a1,a2) : (fallback); }
#define FORWARD3(ret, name, t1, t2, t3, fallback) \
    __declspec(dllexport) ret __cdecl name(t1 a1, t2 a2, t3 a3) { \
        typedef ret (__cdecl *fn_t)(t1,t2,t3); fn_t fn = (fn_t)original_proc(#name); \
        return fn ? fn(a1,a2,a3) : (fallback); }
#define FORWARD4(ret, name, t1, t2, t3, t4, fallback) \
    __declspec(dllexport) ret __cdecl name(t1 a1, t2 a2, t3 a3, t4 a4) { \
        typedef ret (__cdecl *fn_t)(t1,t2,t3,t4); fn_t fn = (fn_t)original_proc(#name); \
        return fn ? fn(a1,a2,a3,a4) : (fallback); }

FORWARD0(uint32_t, WCLFlushApc, 0)
FORWARD0(void, WCLSetApcSync, (void)0)
FORWARD3(uint32_t, WCLWait, void **, uint32_t, uint32_t, 0)

FORWARD3(void *, WCLManagerCreate, void *, void *, void *, NULL)
FORWARD1(void, WCLManagerDestroy, void *, (void)0)
FORWARD1(int, WCLManagerOpen, void *, -1)
FORWARD1(int, WCLManagerClose, void *, -1)
FORWARD1(BOOL, WCLManagerGetActive, void *, FALSE)
FORWARD1(void *, WCLManagerGetRadio, void *, NULL)

__declspec(dllexport) void *__cdecl WCLGattClientCreate(gatt_event_cb connected,
    gatt_event_cb disconnected, gatt_changed_cb changed)
{
    gatt_client *client = calloc(1, sizeof(*client));
    if (!client) return NULL;
    client->connected = connected;
    client->disconnected = disconnected;
    client->changed = changed;
    AcquireSRWLockExclusive(&clients_lock);
    client->next = clients;
    clients = client;
    ReleaseSRWLockExclusive(&clients_lock);
    return client;
}

__declspec(dllexport) void __cdecl WCLGattClientDestroy(gatt_client *client)
{
    if (!client) return;
    AcquireSRWLockExclusive(&clients_lock);
    gatt_client **link = &clients;
    while (*link && *link != client) link = &(*link)->next;
    if (*link) *link = client->next;
    ReleaseSRWLockExclusive(&clients_lock);
    free(client);
}

__declspec(dllexport) int __cdecl WCLGattClientConnect(gatt_client *client, void *radio, uint64_t address)
{
    char command[128], response[1024];
    (void)radio;
    if (!client) return 0x30001;
    client->state = 2;
    client->address = address;
    snprintf(command, sizeof(command), "CONNECT %012llX", (unsigned long long)address);
    if (!rpc_call(command, response, sizeof(response))) {
        client->state = 0;
        log_line(response[0] ? response : "GATT connect RPC failed");
        return 0x5105c;
    }
    client->state = 3;
    if (client->connected) client->connected(client, 0);
    return 0;
}

__declspec(dllexport) int __cdecl WCLGattClientDisconnect(gatt_client *client)
{
    char command[128], response[1024];
    if (!client) return 0x30001;
    snprintf(command, sizeof(command), "DISCONNECT %012llX", (unsigned long long)client->address);
    rpc_call(command, response, sizeof(response));
    client->state = 0;
    if (client->disconnected) client->disconnected(client, 0);
    return 0;
}

__declspec(dllexport) void __cdecl WCLGattClientFreeMem(void *memory)
{
    if (memory) CoTaskMemFree(memory);
}

__declspec(dllexport) int __cdecl WCLGattClientGetServices(gatt_client *client, gatt_services *services)
{
    char command[128], response[16384];
    if (!client || !services) return 0x30001;
    snprintf(command, sizeof(command), "SERVICES %012llX", (unsigned long long)client->address);
    if (!rpc_call(command, response, sizeof(response))) return 0x5105c;
    services->count = 0;
    char *data = response + 2;
    while (*data == ' ') ++data;
    char *save = NULL;
    for (char *entry = strtok_s(data, ";\r\n", &save); entry && services->count < 255;
         entry = strtok_s(NULL, ";\r\n", &save)) {
        char uuid_text[64]; unsigned int handle;
        if (sscanf(entry, "%63[^,],%u", uuid_text, &handle) != 2) continue;
        gatt_service *service = &services->services[services->count++];
        memset(service, 0, sizeof(*service));
        uuid_to_gatt(uuid_text, &service->uuid);
        service->handle = (uint16_t)handle;
    }
    log_fmt("GATT services address=%012llX count=%u ptr=%p", (unsigned long long)client->address,
            services->count, (void *)services->services);
    return 0;
}

__declspec(dllexport) int __cdecl WCLGattClientGetCharacteristics(gatt_client *client,
    gatt_service *service, gatt_characteristics *chars)
{
    char command[128], response[32768];
    if (!client || !service || !chars) return 0x30001;
    snprintf(command, sizeof(command), "CHARS %012llX %u",
             (unsigned long long)client->address, service->handle);
    if (!rpc_call(command, response, sizeof(response))) return 0x5105c;
    chars->count = 0;
    char *data = response + 2;
    while (*data == ' ') ++data;
    char *save = NULL;
    for (char *entry = strtok_s(data, ";\r\n", &save); entry && chars->count < 255;
         entry = strtok_s(NULL, ";\r\n", &save)) {
        char uuid_text[64]; unsigned int service_handle, handle, value_handle, flags;
        if (sscanf(entry, "%63[^,],%u,%u,%u,%u", uuid_text, &service_handle,
                   &handle, &value_handle, &flags) != 5) continue;
        gatt_characteristic *characteristic = &chars->chars[chars->count++];
        memset(characteristic, 0, sizeof(*characteristic));
        characteristic->service_handle = (uint16_t)service_handle;
        uuid_to_gatt(uuid_text, &characteristic->uuid);
        characteristic->handle = (uint16_t)handle;
        characteristic->value_handle = (uint16_t)value_handle;
        characteristic->is_broadcastable = !!(flags & (1u << 0));
        characteristic->is_readable = !!(flags & (1u << 1));
        characteristic->is_writable = !!(flags & (1u << 2));
        characteristic->is_writable_without_response = !!(flags & (1u << 3));
        characteristic->is_signed_writable = !!(flags & (1u << 4));
        characteristic->is_notifiable = !!(flags & (1u << 5));
        characteristic->is_indicatable = !!(flags & (1u << 6));
        characteristic->has_extended_properties = !!(flags & (1u << 7));
    }
    log_fmt("GATT chars address=%012llX service=%u count=%u ptr=%p",
            (unsigned long long)client->address, service->handle, chars->count, (void *)chars->chars);
    return 0;
}

__declspec(dllexport) int __cdecl WCLGattClientGetState(gatt_client *client)
{
    return client ? client->state : 0;
}

__declspec(dllexport) int __cdecl WCLGattClientReadCharacteristicValue(gatt_client *client,
    gatt_characteristic *characteristic, void **value, uint32_t *value_len)
{
    char command[128], response[16384];
    if (!client || !characteristic || !value || !value_len) return 0x30001;
    snprintf(command, sizeof(command), "READ %012llX %u",
             (unsigned long long)client->address, characteristic->value_handle);
    if (!rpc_call(command, response, sizeof(response))) return 0x5105c;
    char *hex = response + 2;
    while (*hex == ' ') ++hex;
    size_t max = strlen(hex) / 2;
    uint8_t *memory = CoTaskMemAlloc(max ? max : 1);
    if (!memory) return 0x30001;
    *value_len = hex_payload(hex, memory, (uint16_t)(max > 65535 ? 65535 : max));
    *value = memory;
    log_fmt("GATT read address=%012llX handle=%u length=%u",
            (unsigned long long)client->address, characteristic->value_handle, *value_len);
    return 0;
}

__declspec(dllexport) int __cdecl WCLGattClientSubscribeCharacteristic(gatt_client *client,
    gatt_characteristic *characteristic)
{
    char command[128], response[1024];
    snprintf(command, sizeof(command), "SUB %012llX %u",
             (unsigned long long)client->address, characteristic->value_handle);
    int result = rpc_call(command, response, sizeof(response)) ? 0 : 0x5105c;
    log_fmt("GATT subscribe address=%012llX handle=%u result=%d",
            (unsigned long long)client->address, characteristic->value_handle, result);
    return result;
}

__declspec(dllexport) int __cdecl WCLGattClientUnsubscribeCharacteristic(gatt_client *client,
    gatt_characteristic *characteristic)
{
    char command[128], response[1024];
    snprintf(command, sizeof(command), "UNSUB %012llX %u",
             (unsigned long long)client->address, characteristic->value_handle);
    return rpc_call(command, response, sizeof(response)) ? 0 : 0x5105c;
}

__declspec(dllexport) int __cdecl WCLGattClientWriteCharacteristicValue(gatt_client *client,
    gatt_characteristic *characteristic, void *value, uint32_t value_len)
{
    char command[8192], response[1024];
    int used = snprintf(command, sizeof(command), "WRITE %012llX %u ",
                        (unsigned long long)client->address, characteristic->value_handle);
    uint8_t *bytes = value;
    for (uint32_t i = 0; i < value_len && used + 2 < (int)sizeof(command); ++i)
        used += snprintf(command + used, sizeof(command) - used, "%02x", bytes[i]);
    int ok = rpc_call(command, response, sizeof(response));
    log_fmt("GATT write address=%012llX handle=%u length=%u command=%.256s result=%s response=%.256s",
            (unsigned long long)client->address, characteristic->value_handle,
            value_len, command, ok ? "OK" : "ERR", response);
    return ok ? 0 : 0x5105c;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    if (reason == DLL_PROCESS_DETACH && original) FreeLibrary(original);
    return TRUE;
}
