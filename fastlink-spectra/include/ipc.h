#pragma once

#include <cstdint>
#include <vector>

struct MemorySnapshot {
    std::vector<uint8_t> bytes;
    uint64_t timestampQpc = 0;
    bool valid = false;
};

class SharedMemoryClient {
public:
    SharedMemoryClient();
    ~SharedMemoryClient();

    bool Open();
    void Close();
    bool IsOpen() const;
    bool ReadLatest(MemorySnapshot &snapshot);

private:
    void *mapping_;
    void *view_;
    void *event_;
};
