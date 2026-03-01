#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "ipc.h"

namespace {

static constexpr const char *kMappingName = "Local\\Snes9xFastLink.Memory.v1";
static constexpr const char *kEventName = "Local\\Snes9xFastLink.FrameReady.v1";
static constexpr uint32_t kMagic = 0x314D4C46u;
static constexpr uint32_t kVersion = 1;
static constexpr uint32_t kMaxPayload = 131072;

#pragma pack(push, 1)
struct SharedBlock {
    uint32_t magic;
    uint32_t version;
    volatile LONG sequence;
    uint32_t payloadSize;
    uint64_t qpc;
    uint8_t payload[kMaxPayload];
};
#pragma pack(pop)

} // namespace

SharedMemoryClient::SharedMemoryClient() : mapping_(nullptr), view_(nullptr), event_(nullptr) {}

SharedMemoryClient::~SharedMemoryClient()
{
    Close();
}

bool SharedMemoryClient::Open()
{
    if (IsOpen())
        return true;

    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, kMappingName);
    if (!mapping)
        return false;

    void *view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedBlock));
    if (!view) {
        CloseHandle(mapping);
        return false;
    }

    HANDLE evt = OpenEventA(SYNCHRONIZE, FALSE, kEventName);

    mapping_ = mapping;
    view_ = view;
    event_ = evt;
    return true;
}

void SharedMemoryClient::Close()
{
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (event_) {
        CloseHandle(reinterpret_cast<HANDLE>(event_));
        event_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(reinterpret_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
}

bool SharedMemoryClient::IsOpen() const
{
    return view_ != nullptr;
}

bool SharedMemoryClient::ReadLatest(MemorySnapshot &snapshot)
{
    if (!IsOpen())
        return false;

    SharedBlock *block = reinterpret_cast<SharedBlock *>(view_);
    if (block->magic != kMagic || block->version != kVersion)
        return false;

    if (event_) {
        WaitForSingleObject(reinterpret_cast<HANDLE>(event_), 1);
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        LONG s1 = block->sequence;
        if (s1 & 1)
            continue;

        const uint32_t size = std::min(block->payloadSize, kMaxPayload);
        snapshot.bytes.resize(size);
        std::memcpy(snapshot.bytes.data(), block->payload, size);
        snapshot.timestampQpc = block->qpc;

        LONG s2 = block->sequence;
        if (s1 == s2 && !(s2 & 1)) {
            snapshot.valid = true;
            return true;
        }
    }

    return false;
}
