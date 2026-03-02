#pragma once

#include <cstdint>
#include <string>
#include <vector>

class RestClient {
public:
    RestClient();
    ~RestClient();

    void SetEndpoint(const std::string &host, uint16_t port, const std::wstring &path);
    bool Fetch(std::vector<uint8_t> &outBytes);

private:
    std::wstring host_;
    uint16_t port_;
    std::wstring path_;
};
