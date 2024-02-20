/*****************************************************************************\
	 Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
				This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/* memserve.cpp
	Cross-Platform Socket Server for SNES READ-ONLY Memory Access
    Presents the CMemory struct, and it's member functions, as JSON over HTTP
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <err.h>
#endif

#include "snes9x.h"
#include "memmap.h"

#include "memserve.h"

void MemServe()
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed with error: %d\n", WSAGetLastError());
        exit(1);
    }
#endif

    int one = 1;
    int client_fd;
    struct sockaddr_in svr_addr, cli_addr;
    socklen_t sin_len = sizeof(cli_addr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
#ifdef _WIN32
        fprintf(stderr, "Can't open socket with error: %d\n", WSAGetLastError());
#else
        fprintf(stderr, "Can't open socket\n");
#endif
        exit(1);
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)) < 0) {
#ifdef _WIN32
        fprintf(stderr, "Can't set SO_REUSEADDR with error: %d\n", WSAGetLastError());
#else
        fprintf(stderr, "Can't set SO_REUSEADDR\n");
#endif
        exit(1);
    }

    int port = Settings.MemServePort;
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = INADDR_ANY;
    svr_addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&svr_addr, sizeof(svr_addr)) == -1) {
#ifdef _WIN32
        closesocket(sock);
        fprintf(stderr, "Can't bind with error: %d\n", WSAGetLastError());
#else
        fprintf(stderr, "Can't bind\n");
#endif
        exit(1);
    }

    listen(sock, 5);

#ifdef _WIN32
    int bytesReceived;
    int bytesSent;
#else
	ssize_t bytesReceived;
    ssize_t bytesSent;
#endif
    std::vector<char> buffer(8192);
    std::string request;

    while (true) {
        client_fd = accept(sock, (struct sockaddr*)&cli_addr, &sin_len);

        if (client_fd == -1) {
#ifdef _WIN32
            fprintf(stderr, "Can't accept with error: %d\n", WSAGetLastError());
#else
            fprintf(stderr, "Can't accept\n");
#endif
            continue;
        }

        do {
            bytesReceived = recv(client_fd, buffer.data(), buffer.size(), 0);

            if (bytesReceived > 0) {
                request.append(buffer.data(), bytesReceived);
            }
            else if (bytesReceived == 0) {
                break;
            }
            else {
#ifdef _WIN32
                closesocket(client_fd);
#else
                close(client_fd);
#endif

                return;
            }
        } while (request.find("\r\n\r\n") == std::string::npos);

        const char* ramData = reinterpret_cast<const char*>(Memory.RAM);
        size_t ramSize = sizeof(Memory.RAM);

        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: " + std::to_string(ramSize) + "\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n";

#ifdef _WIN32
        bytesSent = send(client_fd, headers.c_str(), headers.length(), 0);
        if (bytesSent == SOCKET_ERROR) {
            fprintf(stderr, "Socket error: %d\n", WSAGetLastError());
            closesocket(client_fd);
#else
        bytesSent = write(client_fd, headers.c_str(), headers.size());
        if (bytesSent < 0) {
            fprintf(stderr, "Socket error\n");
            close(client_fd);
#endif
            return;
        }

        if (static_cast<size_t>(bytesSent) != headers.length()) {
#ifdef _WIN32
            fprintf(stderr, "Not all Header data was sent: %d\n", WSAGetLastError());
            closesocket(client_fd);
#else
            fprintf(stderr, "Not all Header data was sent\n");
            close(client_fd);
#endif
            return;
        }

        size_t totalSent = 0;

        while (totalSent < ramSize) {
#ifdef _WIN32
            bytesSent = send(client_fd, ramData + totalSent, ramSize - totalSent, 0);
            if (bytesSent == SOCKET_ERROR) {
                fprintf(stderr, "Not all data was sent: %d\n", WSAGetLastError());
                closesocket(client_fd);
#else
            bytesSent = send(client_fd, ramData + totalSent, ramSize - totalSent, MSG_NOSIGNAL);
            if (bytesSent < 0) {
                fprintf(stderr, "Not all data was sent\n");
                close(client_fd);
#endif
                return;
            }

            totalSent += bytesSent;
        }

#ifdef _WIN32
        shutdown(client_fd, SD_SEND);
        closesocket(client_fd);
#else
        shutdown(client_fd, SHUT_WR);
        close(client_fd);
#endif

        // Potentially remove this after adjust the while loop itself- TODO
        if (!Settings.MemoryServe) {
            break;
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif
}