#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#include "rest_client.h"

#pragma comment(lib, "winhttp.lib")

RestClient::RestClient() : host_(L"127.0.0.1"), port_(9000), path_(L"/") {}
RestClient::~RestClient() = default;

void RestClient::SetEndpoint(const std::string &host, uint16_t port, const std::wstring &path)
{
    host_ = std::wstring(host.begin(), host.end());
    port_ = port;
    path_ = path;
}

bool RestClient::Fetch(std::vector<uint8_t> &outBytes)
{
    outBytes.clear();

    HINTERNET session = WinHttpOpen(L"FastLinkSpectra/0.1",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
    if (!session)
        return false;

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host_.c_str(), port_, 0);
    if (!connect)
        goto cleanup;

    HINTERNET request = WinHttpOpenRequest(connect,
                                           L"GET",
                                           path_.c_str(),
                                           nullptr,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           0);
    if (!request)
        goto cleanup_connect;

    if (!WinHttpSetTimeouts(request, 200, 200, 250, 250)) {
        goto cleanup_request;
    }

    if (!WinHttpSendRequest(request,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)) {
        goto cleanup_request;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        goto cleanup_request;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
            break;
        if (available == 0) {
            ok = !outBytes.empty();
            break;
        }

        const size_t oldSize = outBytes.size();
        outBytes.resize(oldSize + available);

        DWORD read = 0;
        if (!WinHttpReadData(request, outBytes.data() + oldSize, available, &read))
            break;
        outBytes.resize(oldSize + read);
    }

cleanup_request:
    WinHttpCloseHandle(request);
cleanup_connect:
    WinHttpCloseHandle(connect);
cleanup:
    WinHttpCloseHandle(session);
    return ok;
}
