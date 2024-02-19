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

        std::string responseContent = "Hello, World!";
        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + std::to_string(responseContent.length()) + "\r\n\r\n";

#ifdef _WIN32
        send(client_fd, headers.c_str(), headers.length(), 0);
        send(client_fd, responseContent.c_str(), responseContent.length(), 0);
        closesocket(client_fd);
#else
        write(client_fd, headers.c_str(), headers.size());
        write(client_fd, responseContent.c_str(), responseContent.length());
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