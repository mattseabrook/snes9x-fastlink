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
    MessageBox(NULL, L"Memory Server is running", L"Debug/Test", MB_OK);
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
        err(1, "can't open socket");
#endif
        exit(1);
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)) < 0) {
#ifdef _WIN32
        fprintf(stderr, "Can't set SO_REUSEADDR with error: %d\n", WSAGetLastError());
#else
        err(1, "Can't set SO_REUSEADDR");
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
        close(sock);
        err(1, "Can't bind");
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
            perror("Can't accept");
#endif
            continue;
        }

        json j = ramToJson(Memory.RAM, sizeof(Memory.RAM) / sizeof(Memory.RAM[0]));
        std::string jsonResponse = j.dump(2);

        std::string httpResponse = "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/json\r\n\r\n" +
            jsonResponse + "\r\n";

#ifdef _WIN32
        send(client_fd, httpResponse.c_str(), sizeof(httpResponse) - 1, 0); // -1:'\0'
        closesocket(client_fd);
#else
        write(client_fd, httpResponse.c_str(), sizeof(httpResponse) - 1); // -1:'\0'
        close(client_fd);
#endif
        if (!Settings.MemoryServe) {
			break;
		}
    }

#ifdef _WIN32
    WSACleanup();
#endif
}   