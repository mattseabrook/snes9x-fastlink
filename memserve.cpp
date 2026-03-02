/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/* memserve.cpp
    Minimal HTTP server streaming SNES RAM as application/octet-stream.
    - Non-blocking accept with select() for graceful shutdown
    - Zero-copy RAM send (direct pointer to Memory.RAM)
    - No heap allocations in hot path
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET socket_t;
#define CLOSE_SOCKET closesocket
#define SOCKET_VALID(s) ((s) != INVALID_SOCKET)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <sys/select.h>
#include <cerrno>
typedef int socket_t;
#define CLOSE_SOCKET close
#define SOCKET_VALID(s) ((s) >= 0)
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#endif

#include <cstdio>
#include <cstring>
#include <atomic>

#include "snes9x.h"
#include "memmap.h"
#include "memserve.h"

// Shutdown signal - set by main thread to request stop
static std::atomic<bool> g_shutdownRequested{false};
static socket_t g_listenSocket = INVALID_SOCKET;

// Pre-computed HTTP response header (Content-Length is fixed: 128KB = 131072)
static constexpr char HTTP_HEADER[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Content-Length: 131072\r\n"
    "Connection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "\r\n";
static constexpr size_t HTTP_HEADER_LEN = sizeof(HTTP_HEADER) - 1;

void MemServeStop()
{
    g_shutdownRequested.store(true, std::memory_order_release);
    // Close listen socket to unblock select()
    if (SOCKET_VALID(g_listenSocket)) {
        CLOSE_SOCKET(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
}

void MemServe()
{
    g_shutdownRequested.store(false, std::memory_order_release);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[MemServe] WSAStartup failed: %d\n", WSAGetLastError());
        return;
    }
#endif

    g_listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!SOCKET_VALID(g_listenSocket)) {
        fprintf(stderr, "[MemServe] socket() failed\n");
        goto cleanup;
    }

    {
        int one = 1;
        setsockopt(g_listenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    }

    {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
        addr.sin_port = htons(static_cast<uint16_t>(Settings.MemServePort));

        if (bind(g_listenSocket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[MemServe] bind() failed on port %d\n", Settings.MemServePort);
            goto cleanup;
        }
    }

    if (listen(g_listenSocket, 4) == SOCKET_ERROR) {
        fprintf(stderr, "[MemServe] listen() failed\n");
        goto cleanup;
    }

    fprintf(stderr, "[MemServe] Listening on 127.0.0.1:%d\n", Settings.MemServePort);

    // Main accept loop with select() for interruptibility
    while (!g_shutdownRequested.load(std::memory_order_acquire)) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(g_listenSocket, &readfds);

        struct timeval tv{0, 100000}; // 100ms timeout
        int sel = select(static_cast<int>(g_listenSocket) + 1, &readfds, nullptr, nullptr, &tv);

        if (sel == SOCKET_ERROR) {
            if (g_shutdownRequested.load(std::memory_order_acquire)) break;
            continue;
        }
        if (sel == 0) continue; // timeout, recheck shutdown flag

        socket_t client = accept(g_listenSocket, nullptr, nullptr);
        if (!SOCKET_VALID(client)) continue;

        // Drain request (we don't parse it, just wait for end of headers)
        {
            char buf[512];
            int total = 0;
            while (total < 4096) {
                int n = recv(client, buf, sizeof(buf), 0);
                if (n <= 0) break;
                total += n;
                // Check for end of HTTP headers
                if (total >= 4 && memcmp(buf + (n > 4 ? n - 4 : 0), "\r\n\r\n", 4) == 0) break;
            }
        }

        // Send header (stack buffer, no allocation)
        send(client, HTTP_HEADER, static_cast<int>(HTTP_HEADER_LEN), 0);

        // Zero-copy send of RAM directly
        const char* ramPtr = reinterpret_cast<const char*>(Memory.RAM);
        size_t remaining = sizeof(Memory.RAM);
        while (remaining > 0) {
            int chunk = (remaining > 65536) ? 65536 : static_cast<int>(remaining);
#ifdef _WIN32
            int sent = send(client, ramPtr, chunk, 0);
#else
            int sent = send(client, ramPtr, chunk, MSG_NOSIGNAL);
#endif
            if (sent <= 0) break;
            ramPtr += sent;
            remaining -= sent;
        }

#ifdef _WIN32
        shutdown(client, SD_SEND);
#else
        shutdown(client, SHUT_WR);
#endif
        CLOSE_SOCKET(client);
    }

cleanup:
    if (SOCKET_VALID(g_listenSocket)) {
        CLOSE_SOCKET(g_listenSocket);
        g_listenSocket = INVALID_SOCKET;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    fprintf(stderr, "[MemServe] Stopped\n");
}