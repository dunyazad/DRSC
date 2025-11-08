#pragma once

// =====================================================
//  RxTxCommon.h
//  - Platform abstraction
//  - DLL export/import definition
//  - Socket utilities
// =====================================================

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#ifdef RXTX_EXPORTS
#define RXTX_API __declspec(dllexport)
#elif defined(RXTX_IMPORTS)
#define RXTX_API __declspec(dllimport)
#else
#define RXTX_API
#endif
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define RXTX_API
#endif

#include <iostream>
#include <string>

// ÇÃ·§Æû ÃÊ±âÈ­ À¯Æ¿
namespace libRxTx {

    inline bool InitSocket() {
#ifdef _WIN32
        WSADATA wsa;
        return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#else
        return true;
#endif
    }

    inline void CloseSocket() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

} // namespace libRxTx
