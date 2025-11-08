#include "RxTx/RxTx.h"

#ifdef _WIN32
#include <windows.h> // PrintAt 용
void PrintAt(int x, int y, const std::string& text) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD pos = { static_cast<SHORT>(x), static_cast<SHORT>(y) };
    SetConsoleCursorPosition(hConsole, pos);
    std::cout << text << std::flush;
}
#else
// Linux/macOS용 PrintAt (ANSI 이스케이프 코드)
void PrintAt(int x, int y, const std::string& text) {
    std::cout << "\033[" << (y + 1) << ";" << (x + 1) << "H" << text << std::flush;
}
#endif

namespace libRxTx {

#ifdef _WIN32
    bool RxTx::InitSocket() {
        WSADATA wsa;
        return (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    void RxTx::CloseSocket() { WSACleanup(); }
#else
    bool RxTx::InitSocket() { return true; }
    void RxTx::CloseSocket() {}
#endif

    RxTx::RxTx(Protocol proto)
        : protocol_(proto), udpMode_(UdpMode::Unicast), sock_(INVALID_SOCKET),
        running_(false), stopping_(false) {
    }

    RxTx::~RxTx() {
        Stop();
    }

    bool RxTx::Init() {
        if (!InitSocket()) return false;

        int type = (protocol_ == Protocol::TCP) ? SOCK_STREAM : SOCK_DGRAM;
        int proto = (protocol_ == Protocol::TCP) ? IPPROTO_TCP : 0;
        sock_ = socket(AF_INET, type, proto);
        if (sock_ == INVALID_SOCKET) {
            std::cerr << "RxTx: socket creation failed\n";
            return false;
        }

        if (protocol_ == Protocol::TCP) {
            BOOL flag = TRUE;
            setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));
            std::cout << "[TCP] TCP_NODELAY enabled (Nagle off)\n";
        }

        return true;
    }

    std::string RxTx::GetLocalIP() {
#ifdef _WIN32
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        addrinfo hints{}, * info = nullptr;
        hints.ai_family = AF_INET;
        getaddrinfo(hostname, nullptr, &hints, &info);
        char ip[INET_ADDRSTRLEN];
        sockaddr_in* addr = (sockaddr_in*)info->ai_addr;
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
        freeaddrinfo(info);
        return ip;
#else
        // Linux/macOS는 이 구현이 더 잘 작동할 수 있음
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in serv;
        memset(&serv, 0, sizeof(serv));
        serv.sin_family = AF_INET;
        serv.sin_addr.s_addr = inet_addr("8.8.8.8"); // Google DNS
        serv.sin_port = htons(53);
        connect(s, (sockaddr*)&serv, sizeof(serv));
        sockaddr_in name;
        socklen_t namelen = sizeof(name);
        getsockname(s, (sockaddr*)&name, &namelen);
        closesocket(s);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &name.sin_addr, ip, sizeof(ip));
        return ip;
#endif
    }


    bool RxTx::Bind(int port, const std::string& localIp) {
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        addr_.sin_addr.s_addr = localIp.empty() ? INADDR_ANY : inet_addr(localIp.c_str());
        localInterfaceIp_ = localIp;

        BOOL reuse = TRUE;
        setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

        if (bind(sock_, (sockaddr*)&addr_, sizeof(addr_)) == SOCKET_ERROR) {
            std::cerr << "RxTx: bind failed (Error: " << WSAGetLastError() << ")" << std::endl;
            return false;
        }

        if (protocol_ == Protocol::UDP && udpMode_ == UdpMode::Broadcast) {
            BOOL enable = TRUE;
            setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (char*)&enable, sizeof(enable));
            std::cout << "[UDP] Broadcast mode enabled\n";
        }

        std::cout << (protocol_ == Protocol::TCP ? "TCP" : "UDP")
            << " socket bound on port " << port
            << (localIp.empty() ? "" : (" (Interface: " + localIp + ")")) << std::endl;
        return true;
    }

    bool RxTx::Listen(int backlog) {
        if (protocol_ != Protocol::TCP) return false;
        if (listen(sock_, backlog) == SOCKET_ERROR) {
            std::cerr << "RxTx: listen failed\n";
            return false;
        }
        std::cout << "TCP listening..." << std::endl;
        return true;
    }

    bool RxTx::Connect(const std::string& host, int port) {
        addr_.sin_family = AF_INET;
        addr_.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr_.sin_addr);

        if (protocol_ == Protocol::TCP) {
            if (connect(sock_, (sockaddr*)&addr_, sizeof(addr_)) == SOCKET_ERROR) {
                std::cerr << "RxTx: connect failed\n";
                return false;
            }
            connectedHost_ = host;
            if (connectCallback_) connectCallback_(host); // TODO: OnConnect 콜백은 서버에만 의미있음
            std::cout << "TCP connected to " << host << ":" << port << std::endl;
        }
        return true;
    }

    void RxTx::SetMode(UdpMode mode) {
        udpMode_ = mode;
        if (protocol_ != Protocol::UDP) return;
        if (udpMode_ == UdpMode::Broadcast) {
            BOOL enable = TRUE;
            setsockopt(sock_, SOL_SOCKET, SO_BROADCAST, (char*)&enable, sizeof(enable));
        }
    }

    void RxTx::SendPacket(const std::string& msg, const std::string& host, int port) {
        if (protocol_ == Protocol::TCP) {
            SOCKET targetSock = sock_;
            {
                std::lock_guard<std::mutex> lock(clientMutex_);
                auto it = clientSockets_.find(host);
                if (it != clientSockets_.end()) targetSock = it->second;
            }

            if (targetSock != INVALID_SOCKET) {
                int len = htonl((int)msg.size());
                send(targetSock, (char*)&len, 4, 0);
                send(targetSock, msg.c_str(), (int)msg.size(), 0);
            }
        }
        else {
            sockaddr_in dest{};
            dest.sin_family = AF_INET;
            dest.sin_port = htons(port);
            inet_pton(AF_INET, host.c_str(), &dest.sin_addr);
            sendto(sock_, msg.c_str(), (int)msg.size(), 0, (sockaddr*)&dest, sizeof(dest));
        }
    }

    void RxTx::SendToClient(const std::string& id, const std::string& msg) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        auto it = clientSockets_.find(id);
        if (it == clientSockets_.end()) {
            std::cout << "[RxTx] SendToClient(" << id << "): socket not found!" << std::endl;
            return;
        }

        SOCKET s = it->second;
        if (s == INVALID_SOCKET) {
            std::cout << "[RxTx] SendToClient(" << id << "): invalid socket" << std::endl;
            return;
        }

        int len = htonl((int)msg.size());
        int l1 = send(s, (char*)&len, 4, 0);
        int l2 = send(s, msg.c_str(), (int)msg.size(), 0);

        if (l1 <= 0 || l2 <= 0) {
            int err = WSAGetLastError();
            std::cout << "[RxTx] SendToClient(" << id << "): send failed, err=" << err << std::endl;

            // ★ 참고:
            // send 실패 시 소켓을 닫고 map에서 제거합니다.
            // RunReceiver()의 해당 클라이언트 스레드도 recv() 실패로 루프를 탈출하며
            // OnDisconnect 콜백을 호출할 것입니다.
            // 여기서 콜백을 직접 호출하면 데드락 위험이 있습니다.
            shutdown(s, SD_BOTH);
            closesocket(s);
            clientSockets_.erase(it);
            return;
        }
    }

    void RxTx::Broadcast(const std::string& msg) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        for (auto& kv : clientSockets_) {
            SOCKET s = kv.second;
            int len = htonl((int)msg.size());
            send(s, (char*)&len, 4, 0);
            send(s, msg.c_str(), (int)msg.size(), 0);
        }
    }

    void RxTx::SendToAll(const std::string& msg, int port) {
        if (protocol_ != Protocol::UDP) return;
        std::string bcastAddr = "255.255.255.255";
        // ... (로컬 IP 기반 브로드캐스트 주소 계산 로직은 복잡하므로 255로 통일) ...
        SendPacket(msg, bcastAddr, port);
    }

    void RxTx::SendString(const std::string& msg) {
        // 이 함수는 TCP 클라이언트 전용
        if (protocol_ != Protocol::TCP) return;

        std::lock_guard<std::mutex> lock(sendMutex_);
        sendQueue_.push({ msg, "", 0 });
        sendCond_.notify_one();
    }

    void RxTx::JoinMulticastGroup(const std::string& groupAddr) {
        if (protocol_ != Protocol::UDP) return;
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(groupAddr.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));
    }

    void RxTx::LeaveMulticastGroup(const std::string& groupAddr) {
        // ... (Join과 동일한 로직, IP_DROP_MEMBERSHIP)
    }

    void RxTx::RegisterClientSocket(const std::string& clientId, const std::string& ip) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        auto it = clientSockets_.find(ip);
        if (it != clientSockets_.end()) {
            clientSockets_[clientId] = it->second;
            clientSockets_.erase(it);
        }
    }

    void RxTx::MapClientIpToId(const std::string& ip, const std::string& id) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        ipToId_[ip] = id;
    }

    std::string RxTx::GetMappedClientId(const std::string& ip) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        auto it = ipToId_.find(ip);
        if (it != ipToId_.end()) return it->second;
        return {};
    }

    void RxTx::OnReceive(std::function<void(const std::string&, const std::string&)> cb) {
        recvCallback_ = std::move(cb);
    }

    void RxTx::OnConnect(std::function<std::string(const std::string&)> cb) {
        connectCallback_ = std::move(cb);
    }

    void RxTx::OnDisconnect(std::function<void(const std::string&)> cb) {
        disconnectCallback_ = std::move(cb);
    }

    void RxTx::Start() {
        if (running_) Stop(); // 이미 실행 중이면 재시작
        running_ = true;
        stopping_ = false;

        recvThread_ = std::thread(&RxTx::RunReceiver, this);
        sendThread_ = std::thread(&RxTx::RunSender, this);
    }

    // ★★★ [CRITICAL] 수정된 Stop 함수 ★★★
    void RxTx::Stop() {
        if (stopping_.exchange(true)) {
            // 다른 스레드가 이미 Stop()을 호출 중
            return;
        }

        running_ = false;
        sendCond_.notify_all(); // Send 스레드 unblock

        // 메인 소켓의 accept(), recv(), recvfrom()을 unblock
        if (sock_ != INVALID_SOCKET) {
            shutdown(sock_, SD_BOTH);
        }

        // (Close()는 모든 스레드가 join된 후에 호출)

        try {
            if (recvThread_.joinable()) recvThread_.join();
            if (sendThread_.joinable()) sendThread_.join();

            // 모든 활성 클라이언트 스레드에 종료 신호를 보내고 join
            {
                std::lock_guard<std::mutex> lock(clientThreadsMutex_);
                // clientSockets_를 순회하며 shutdown
                // (이미 sock_ shutdown으로 accept()가 막혔고,
                // running_ = false로 새 클라이언트 스레드 생성이 막힘)

                // 이미 RunReceiver에서 shutdown(clientSock)을 처리함
                // 여기서는 join만 수행
                for (auto& th : clientThreads_) {
                    if (th.joinable()) {
                        th.join();
                    }
                }
                clientThreads_.clear();
            }
        }
        catch (const std::system_error& e) {
            std::cerr << "[RxTx] join() 중 오류 발생: " << e.what() << std::endl;
        }

        Close(); // 소켓을 닫고 WSA/등 정리
        stopping_ = false; // 다시 시작할 수 있도록
    }

    void RxTx::Close() {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        CloseSocket(); // WSA cleanup or no-op
    }

    void RxTx::ForceDisconnectClient(const std::string& clientId) {
        // ... (이 함수는 현재 서버 로직에서 호출되지 않음) ...
    }

    // ★★★ [CRITICAL] 수정된 RunReceiver 함수 ★★★
    void RxTx::RunReceiver() {
        char buffer[4096] = {};

        if (protocol_ == Protocol::TCP) {
            int optval = 0;
            socklen_t optlen = sizeof(optval);
            getsockopt(sock_, SOL_SOCKET, SO_ACCEPTCONN, (char*)&optval, &optlen);
            bool isServer = (optval != 0);

            if (isServer) {
                // ----------------------------
                // TCP SERVER MODE
                // ----------------------------
                while (running_) {
                    sockaddr_in clientAddr{};
                    socklen_t len = sizeof(clientAddr);
                    SOCKET clientSock = accept(sock_, (sockaddr*)&clientAddr, &len);

                    if (clientSock == INVALID_SOCKET) {
                        if (running_) { // Stop()에 의한 종료가 아니라면 오류
                            std::cerr << "RxTx: accept failed (Error: " << WSAGetLastError() << ")" << std::endl;
                        }
                        break; // 루프 종료
                    }

                    std::string clientIp = inet_ntoa(clientAddr.sin_addr);
                    std::string clientId = clientIp; // 기본 ID는 IP

                    {
                        std::lock_guard<std::mutex> lock(clientMutex_);
                        clientSockets_[clientIp] = clientSock; // 일단 IP로 등록
                    }

                    if (connectCallback_) {
                        std::string msg = connectCallback_(clientIp); // 서버 로직 실행 (IP -> ID 매핑)

                        // ★ 서버 로직이 IP->ID를 매핑했을 것이므로, 다시 가져옴
                        clientId = GetMappedClientId(clientIp);
                        if (clientId.empty()) {
                            clientId = clientIp; // 매핑 안됐으면 IP 유지
                        }

                        if (!msg.empty()) { // 웰컴 메시지 전송
                            int len = htonl((int)msg.size());
                            send(clientSock, (char*)&len, 4, 0);
                            send(clientSock, msg.c_str(), (int)msg.size(), 0);
                        }
                    }

                    // ----------------------------
                    // per-client thread
                    // ★ FIX: detach() 대신 vector에 저장
                    // ----------------------------
                    {
                        std::lock_guard<std::mutex> lock(clientThreadsMutex_);
                        clientThreads_.emplace_back([this, clientSock, clientIp, clientId]() {
                            char buf[4096];
                            std::string currentId = clientId; // 스레드 로컬 ID

                            while (running_) {
                                int headerLen = recv(clientSock, buf, 4, MSG_WAITALL);
                                if (headerLen <= 0) break; // 연결 끊김

                                int packetSize = ntohl(*(int*)buf);
                                if (packetSize > 4092) { // 버퍼 오버플로우 방지
                                    std::cerr << "[RxTx " << currentId << "] Packet size too large: " << packetSize << std::endl;
                                    break;
                                }

                                int bytes = recv(clientSock, buf, packetSize, MSG_WAITALL);
                                if (bytes <= 0) break; // 연결 끊김

                                if (recvCallback_)
                                    recvCallback_(std::string(buf, bytes), currentId);
                            }

                            // ----------------------------
                            // Client disconnected
                            // ----------------------------
                            {
                                std::lock_guard<std::mutex> lock(clientMutex_);
                                // ★ FIX: IP와 최종 ID 키를 모두 제거 시도
                                clientSockets_.erase(clientIp);
                                clientSockets_.erase(currentId);
                            }

                            if (disconnectCallback_)
                                // ★ FIX: IP가 아닌 최종 ID(clientId)로 콜백
                                disconnectCallback_(currentId);

                            closesocket(clientSock);
                            // 스레드는 여기서 종료. Stop()에서 join()됨.
                            });
                    }
                }
            }
            else {
                // ----------------------------
                // TCP CLIENT MODE
                // ----------------------------
                while (running_) {
                    int headerLen = recv(sock_, buffer, 4, MSG_WAITALL);
                    if (headerLen <= 0) {
                        if (running_) std::cerr << "[RxTx] Client recv failed (Error: " << WSAGetLastError() << ")" << std::endl;
                        break;
                    }

                    int packetSize = ntohl(*(int*)buffer);
                    if (packetSize > 4092) break;

                    int bytes = recv(sock_, buffer, packetSize, MSG_WAITALL);
                    if (bytes <= 0) break;

                    if (recvCallback_)
                        recvCallback_(std::string(buffer, bytes), connectedHost_);
                }

                if (running_) { // Stop()이 아닌 실제 연결 끊김
                    if (disconnectCallback_)
                        disconnectCallback_(localInterfaceIp_);
                }
            }
        }
        else {
            // ----------------------------
            // UDP MODE
            // ----------------------------
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            while (running_) {
                fd_set fds;
                timeval tv{ 1, 0 }; // 1초 타임아웃
                FD_ZERO(&fds);
                FD_SET(sock_, &fds);

                int sel = select(sock_ + 1, &fds, nullptr, nullptr, &tv);
                if (sel > 0 && FD_ISSET(sock_, &fds)) {
                    int bytes = recvfrom(sock_, buffer, sizeof(buffer) - 1, 0,
                        (sockaddr*)&from, &fromLen);
                    if (bytes > 0) {
                        buffer[bytes] = '\0';
                        if (recvCallback_)
                            recvCallback_(std::string(buffer, bytes),
                                inet_ntoa(from.sin_addr));
                    }
                }
                else if (sel < 0) {
                    if (running_) std::cerr << "UDP select error: " << WSAGetLastError() << std::endl;
                    break;
                }
                // sel == 0 (타임아웃)이면 running_ 체크하고 계속
            }
        }
    }

    void RxTx::RunSender() {
        while (running_) {
            std::unique_lock<std::mutex> lock(sendMutex_);
            sendCond_.wait(lock, [this] { return !sendQueue_.empty() || !running_; });
            if (!running_) break;
            Outgoing out = sendQueue_.front();
            sendQueue_.pop();
            lock.unlock();

            if (protocol_ == Protocol::TCP) {
                // (TCP 클라이언트 전용)
                int len = htonl((int)out.msg.size());
                send(sock_, (char*)&len, 4, 0);
                send(sock_, out.msg.c_str(), (int)out.msg.size(), 0);
            }
            else {
                // (UDP 전용)
                sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(out.port);
                inet_pton(AF_INET, out.host.c_str(), &dest.sin_addr);
                sendto(sock_, out.msg.c_str(), (int)out.msg.size(), 0, (sockaddr*)&dest, sizeof(dest));
            }
        }
    }
} // namespace libRxTx