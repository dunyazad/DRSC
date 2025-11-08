#pragma once
#include <RxTx/RxTxCommon.h>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
// Windows 타입/매크로 호환
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define BOOL bool
#define TRUE true
#define closesocket close
#define WSAGetLastError() errno
#define SD_BOTH SHUT_RDWR
#endif

RXTX_API void PrintAt(int x, int y, const std::string& text);

namespace libRxTx {

    enum class Protocol { UDP, TCP };
    enum class UdpMode { Unicast, Broadcast, Multicast };

    struct RXTX_API Outgoing {
        std::string msg;
        std::string host;
        int port;
    };

    class RXTX_API RxTx {
    public:
        explicit RxTx(Protocol proto);
        ~RxTx();

        bool Init();
        bool Bind(int port, const std::string& localIp = "");
        bool Listen(int backlog = 5);
        bool Connect(const std::string& host, int port);
        void Start();
        void Stop();
        void Close();

        void ForceDisconnectClient(const std::string& clientId);

        void SetMode(UdpMode mode);
        void SendPacket(const std::string& msg, const std::string& host, int port);
        void SendToAll(const std::string& msg, int port);
        void SendString(const std::string& msg);
        void SendToClient(const std::string& id, const std::string& msg);
        void Broadcast(const std::string& msg);

        void JoinMulticastGroup(const std::string& groupAddr);
        void LeaveMulticastGroup(const std::string& groupAddr);
        std::string GetLocalIP();
        void RegisterClientSocket(const std::string& clientId, const std::string& ip);
        void MapClientIpToId(const std::string& ip, const std::string& id);
        std::string GetMappedClientId(const std::string& ip);

        // 콜백 등록 함수들
        void OnReceive(std::function<void(const std::string&, const std::string&)> cb);
        void OnConnect(std::function<std::string(const std::string&)> cb);
        void OnDisconnect(std::function<void(const std::string&)> cb);

    private:
        bool InitSocket();
        void CloseSocket();
        void RunReceiver();
        void RunSender();

    private:
        Protocol protocol_;
        UdpMode udpMode_;
        SOCKET sock_;
        sockaddr_in addr_{};
        std::atomic<bool> running_;
        std::atomic<bool> stopping_{ false }; // ★ 수정: static bool 대신
        std::thread recvThread_;
        std::thread sendThread_;

        std::mutex sendMutex_;
        std::condition_variable sendCond_;
        std::queue<Outgoing> sendQueue_;

        std::function<void(const std::string&, const std::string&)> recvCallback_;
        std::function<std::string(const std::string&)> connectCallback_;
        std::function<void(const std::string&)> disconnectCallback_;

        std::string localInterfaceIp_;

        // TCP 전용
        std::string connectedHost_;
        std::unordered_map<std::string, SOCKET> clientSockets_;
        std::unordered_map<std::string, std::string> ipToId_;
        std::mutex clientMutex_; // clientSockets_와 ipToId_ 보호
        std::vector<std::thread> clientThreads_;
        std::mutex clientThreadsMutex_; // clientThreads_ 보호
    };

} // namespace libRxTx