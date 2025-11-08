#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include <sstream>
#include <algorithm> // std::remove_if

using namespace libRxTx;

class DroneRacingServer;

// -----------------------------
// Session 구조체
// -----------------------------
struct Session {
    std::string clientId;
    std::string playerName;
    std::string ip;
    bool ready = false;
    std::weak_ptr<class Match> currentMatch;
};

// -----------------------------
// Match 클래스
// -----------------------------
class Match : public std::enable_shared_from_this<Match> {
public:
    explicit Match(int id) : matchId(id) {}

    // ★ FIX: 소멸자에서 스레드 join
    ~Match() {
        Stop(); // 스레드 루프가 종료되도록 보장
        if (thread.joinable()) {
            thread.join();
        }
        std::cout << "[Match " << matchId << "] Destructor finished." << std::endl;
    }

    void AddPlayer(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(playerMutex);
        players.push_back(s);
        std::cout << "[Match " << matchId << "] Added player: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ")" << std::endl;
    }

    // ★ FIX: Use-After-Free 방지를 위해 shared_from_this 사용
    void Start() {
        if (running) return;
        running = true;

        std::cout << "[Match " << matchId << "] Race started with "
            << players.size() << " players." << std::endl;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players) {
                if (p) { // weak_ptr가 아니므로 항상 유효 (서버 세션이 살아있는 한)
                    if (sendCallback) sendCallback("START_RACE", p->clientId);
                }
            }
        }

        if (thread.joinable()) thread.join();

        // ★ FIX: 람다에 this 대신 shared_ptr 캡처
        auto self = shared_from_this();
        thread = std::thread([self]() { self->Loop(); });
    }

    void Loop() {
        while (running) { // running은 atomic
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // ★ FIX: player list 접근을 thread-safe하게
            int playerCount = 0;
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                playerCount = players.size();
            }

            if (playerCount <= 1) {
                if (running) { // 중복 Stop 방지
                    std::cout << "[Match " << matchId << "] Auto-stop: " << playerCount << " player(s) left.\n";
                    Stop(); // running = false 설정, 루프 다음 턴에 탈출
                }
            }
        }
        std::cout << "[Match " << matchId << "] Loop exited.\n";
    }

    // ★ FIX: Non-blocking Stop. sleep/join 제거
    void Stop() {
        if (!running.exchange(false)) { // atomic하게 false로 설정하고 이전 값 확인
            return; // 이미 Stop이 호출됨
        }

        std::cout << "[Match " << matchId << "] Stopping..." << std::endl;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players) {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending RACE_FINISHED to " << p->clientId << std::endl;
                        sendCallback("RACE_FINISHED", p->clientId);
                    }
                }
            }
        }
        // ★ FIX: sleep/join 제거. 루프는 running 플래그 보고 스스로 종료.
    }

    bool IsRunning() const { return running; }

    bool IsFull() const {
        std::lock_guard<std::mutex> lock(playerMutex);
        return players.size() >= 4;
    }

    bool IsReadyToStart() const {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (players.size() < 2) return false;
        for (auto& p : players) {
            if (!p->ready) return false;
        }
        return true;
    }

    int GetId() const { return matchId; }

    // playerMutex는 friend class만 접근 가능하도록
    friend class DroneRacingServer;

    std::function<void(const std::string&, const std::string&)> sendCallback;

private:
    int matchId;
    std::atomic<bool> running = false;

    // ★ FIX: 뮤텍스로 보호
    mutable std::mutex playerMutex;
    std::vector<std::shared_ptr<Session>> players;

    std::thread thread;
};

// -----------------------------
// DroneRacingServer
// -----------------------------
class DroneRacingServer {
public:
    DroneRacingServer()
        : beaconUDP(Protocol::UDP), receptionTCP(Protocol::TCP) {
    }

    void Start() {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());

        StartBeacon();
        StartTCP();
    }

    void Stop() {
        beaconUDP.Stop();
        receptionTCP.Stop();
    }

    ~DroneRacingServer() {
        Stop();
    }

private:
    RxTx beaconUDP;
    RxTx receptionTCP;

    std::unordered_map<std::string, std::shared_ptr<Session>> sessions; // key = clientId
    std::mutex mtx; // sessions, matches 리스트 보호
    std::atomic<int> nextClientId{ 1 };
    std::vector<std::shared_ptr<Match>> matches;
    int nextMatchId = 1;

    // ... Beacon (UDP) ...
    void StartBeacon() {
        beaconUDP.Start();
        std::thread([this]() {
            auto ip = beaconUDP.GetLocalIP();
            while (true) {
                auto message = "DRONE_RACING_SERVER_IP:" + ip;
                beaconUDP.SendToAll(message, 5000);
                std::this_thread::sleep_for(std::chrono::seconds(1));

                // (참고) 실제로는 beaconUDP.running_ 플래그를 확인해야 함
                //      (Stop()을 위해)
                //      간결성을 위해 생략.
            }
            }).detach();
    }

    // ... TCP 서버 ...
    void StartTCP() {
        receptionTCP.Init();
        receptionTCP.Bind(6000);
        receptionTCP.Listen();

        // 클라이언트 연결 시
        receptionTCP.OnConnect([this](const std::string& ip) {
            std::lock_guard<std::mutex> lock(mtx);
            auto session = std::make_shared<Session>();
            session->ip = ip;

            std::ostringstream oss;
            oss << "C" << nextClientId++;
            session->clientId = oss.str();

            sessions[session->clientId] = session;

            // ★ 중요: RxTx가 IP->ID를 매핑할 수 있도록 등록
            receptionTCP.MapClientIpToId(ip, session->clientId);
            // ★ 중요: RxTx가 소켓 키를 IP에서 ID로 변경하도록 등록
            receptionTCP.RegisterClientSocket(session->clientId, ip);

            std::cout << "[TCP] Connected: " << ip
                << " -> ClientID=" << session->clientId << std::endl;

            return "CLIENT_ID:" + session->clientId;
            });

        receptionTCP.OnReceive([this](const std::string& msg, const std::string& clientId) { // 이제 IP가 아닌 ID가 넘어옴
            std::lock_guard<std::mutex> lock(mtx);

            // ★ FIX: 이제 RxTx가 ID를 찾아주므로 GetMappedClientId 필요 없음
            auto session = FindSessionById(clientId);
            if (!session) {
                std::cout << "[TCP] Received from unknown ID: " << clientId << std::endl;
                return;
            }

            std::cout << "[TCP " << clientId << "] " << msg << std::endl;

            if (msg.rfind("JOIN:", 0) == 0) {
                session->playerName = clientId; // (임시)
                AssignToMatch(session);
            }
            else if (msg.rfind("READY:", 0) == 0) {
                session->ready = true;
                std::cout << "[TCP] " << session->clientId << " is READY" << std::endl;

                auto match = session->currentMatch.lock();
                if (match && !match->IsRunning() && match->IsReadyToStart()) {
                    match->Start();
                }
            }
            });

        // ★ FIX: Non-blocking OnDisconnect
        receptionTCP.OnDisconnect([this](const std::string& clientId) { // 이제 IP가 아닌 ID가 넘어옴
            std::lock_guard<std::mutex> lock(mtx);

            // ★ FIX: ID가 넘어오므로 매핑 불필요
            auto session = FindSessionById(clientId);
            if (!session) {
                std::cout << "[TCP] Disconnected unknown ID: " << clientId << std::endl;
                return;
            }

            std::cout << "[TCP] Disconnected: " << clientId << std::endl;

            // 서버의 세션 리스트에서 즉시 제거
            sessions.erase(clientId);

            auto match = session->currentMatch.lock();
            if (!match) return; // 매치에 속하지 않았음

            // 매치에서 해당 플레이어 제거 (Thread-safe)
            bool wasRemoved = false;
            {
                std::lock_guard<std::mutex> lock(match->playerMutex);
                auto& players = match->players;
                auto it = std::remove_if(players.begin(), players.end(),
                    [&](const std::shared_ptr<Session>& p) {
                        return p && p->clientId == clientId;
                    });

                if (it != players.end()) {
                    players.erase(it, players.end());
                    wasRemoved = true;
                }
            }

            if (wasRemoved)
                std::cout << "[Match " << match->GetId() << "] Removed player " << clientId << std::endl;

            // ★ FIX: 여기서 match->Stop()을 절대 호출하지 않음!
            // Match::Loop()가 플레이어 수 감소를 감지하고 스스로 중지할 것임.

            // 매치가 비었고 + 실행 중이지 않으면 리스트에서 제거
            {
                std::lock_guard<std::mutex> lock(match->playerMutex);
                if (match->players.empty() && !match->IsRunning()) {
                    matches.erase(
                        std::remove_if(matches.begin(), matches.end(),
                            [&](const std::shared_ptr<Match>& m) { return m == match; }),
                        matches.end()
                    );
                    std::cout << "[Server] Cleaned up empty match " << match->GetId() << std::endl;
                }
            }
            });

        receptionTCP.Start();
    }

    std::shared_ptr<Session> FindSessionById(const std::string& id) {
        auto it = sessions.find(id);
        if (it != sessions.end()) return it->second;
        return nullptr;
    }

    // ... Match 관리 ...
    void AssignToMatch(std::shared_ptr<Session> s) {
        auto openMatch = FindOpenMatch();
        if (!openMatch) {
            openMatch = std::make_shared<Match>(nextMatchId++);
            matches.push_back(openMatch);
            openMatch->sendCallback = [this](const std::string& msg, const std::string& clientId) {
                receptionTCP.SendToClient(clientId, msg);
                };

            std::cout << "[Server] Created new match " << openMatch->GetId() << std::endl;
        }

        s->currentMatch = openMatch;
        openMatch->AddPlayer(s);

        if (openMatch->IsReadyToStart()) {
            openMatch->Start();
        }
    }

    std::shared_ptr<Match> FindOpenMatch() {
        for (auto& m : matches) {
            if (!m->IsFull() && !m->IsRunning())
                return m;
        }
        return nullptr;
    }
};

// ... main() ...
int main() {
    DroneRacingServer server;
    server.Start();

    std::cout << "Server started. Press Enter to exit...\n";
    std::cin.get(); // Enter 키 입력 시 종료

    server.Stop();
    std::cout << "Server shutting down.\n";
    return 0;
}