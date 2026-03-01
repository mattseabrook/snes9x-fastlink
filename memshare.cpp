/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "memshare.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

namespace {

static constexpr const char *kMappingName = "Local\\Snes9xFastLink.Memory.v1";
static constexpr const char *kEventName = "Local\\Snes9xFastLink.FrameReady.v1";
static constexpr uint32_t kMagic = 0x314D4C46u; // "FLM1"
static constexpr uint32_t kVersion = 2;
static constexpr uint32_t kMaxPayload = 131072;
static constexpr uint32_t kRingSlots = 3;

#pragma pack(push, 1)
struct MemShareSlot {
    volatile LONG sequence;
    uint32_t payloadSize;
    uint64_t frameId;
    uint64_t emuQpc;
    uint64_t publishQpc;
    uint8_t payload[kMaxPayload];
};

struct MemShareHeader {
    uint32_t magic;
    uint32_t version;
    volatile LONG sequence;
    uint32_t activeSlot;
    uint32_t ringSlots;
    uint64_t qpcFrequency;
    uint64_t publishCount;
    uint64_t latestFrameId;
    uint64_t latestEmuQpc;
    uint64_t latestPublishQpc;
    MemShareSlot slots[kRingSlots];
};
#pragma pack(pop)

static HANDLE g_mapping = nullptr;
static HANDLE g_event = nullptr;
static MemShareHeader *g_view = nullptr;
static bool g_running = false;
static uint32_t g_nextSlot = 0;

} // namespace

bool MemShareStart()
{
    if (g_running)
        return true;

    g_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                   nullptr,
                                   PAGE_READWRITE,
                                   0,
                                   static_cast<DWORD>(sizeof(MemShareHeader)),
                                   kMappingName);
    if (!g_mapping)
        return false;

    g_view = reinterpret_cast<MemShareHeader *>(MapViewOfFile(g_mapping,
                                                               FILE_MAP_ALL_ACCESS,
                                                               0,
                                                               0,
                                                               sizeof(MemShareHeader)));
    if (!g_view) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }

    g_event = CreateEventA(nullptr, FALSE, FALSE, kEventName);
    if (!g_event) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
        CloseHandle(g_mapping);
        g_mapping = nullptr;
        return false;
    }

    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        std::memset(g_view, 0, sizeof(*g_view));
        g_view->magic = kMagic;
        g_view->version = kVersion;
        g_view->ringSlots = kRingSlots;
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_view->qpcFrequency = static_cast<uint64_t>(freq.QuadPart);
    } else {
        g_view->magic = kMagic;
        g_view->version = kVersion;
        if (g_view->ringSlots != kRingSlots)
            g_view->ringSlots = kRingSlots;
        if (g_view->qpcFrequency == 0) {
            LARGE_INTEGER freq;
            QueryPerformanceFrequency(&freq);
            g_view->qpcFrequency = static_cast<uint64_t>(freq.QuadPart);
        }
    }

    g_nextSlot = 0;
    g_running = true;
    return true;
}

void MemShareStop()
{
    g_running = false;

    if (g_view) {
        UnmapViewOfFile(g_view);
        g_view = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
    if (g_mapping) {
        CloseHandle(g_mapping);
        g_mapping = nullptr;
    }
}

bool MemShareIsRunning()
{
    return g_running;
}

void MemSharePublish(const uint8_t *data, size_t bytes, uint64_t frameId, uint64_t emuQpc)
{
    if (!g_running || !g_view || !data)
        return;

    if (bytes > kMaxPayload)
        bytes = kMaxPayload;

    const uint32_t slotIndex = g_nextSlot;
    g_nextSlot = (g_nextSlot + 1u) % kRingSlots;

    MemShareSlot &slot = g_view->slots[slotIndex];

    InterlockedIncrement(&slot.sequence);

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const uint64_t publishQpc = static_cast<uint64_t>(counter.QuadPart);

    slot.payloadSize = static_cast<uint32_t>(bytes);
    slot.frameId = frameId;
    slot.emuQpc = emuQpc;
    slot.publishQpc = publishQpc;
    std::memcpy(slot.payload, data, bytes);

    InterlockedIncrement(&slot.sequence);

    InterlockedIncrement(&g_view->sequence);

    g_view->activeSlot = slotIndex;
    g_view->publishCount++;
    g_view->latestFrameId = frameId;
    g_view->latestEmuQpc = emuQpc;
    g_view->latestPublishQpc = publishQpc;

    InterlockedIncrement(&g_view->sequence);

    if (g_event)
        SetEvent(g_event);
}

#else

bool MemShareStart() { return false; }
void MemShareStop() {}
bool MemShareIsRunning() { return false; }
void MemSharePublish(const uint8_t *, size_t) {}

#endif
