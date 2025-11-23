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

    ~Match() {
        Stop();
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

		sendCallback("PLAYER_INDEX|" + std::to_string(players.size() - 1), s->clientId);
    }

    void Start() {
        if (running) return;
        running = true;

        std::cout << "[Match " << matchId << "] Race started with "
            << players.size() << " players." << std::endl;

  /*      {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players) {
                if (p) {
                    if (sendCallback) sendCallback("START_RACE|", p->clientId);
                }
            }
        }*/

        if (thread.joinable()) thread.join();

        auto self = shared_from_this();
        thread = std::thread([self]() { self->Loop(); });
    }

    void Loop() {
        while (running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            if (0 <= countdown)
            {
                std::cout << "[Match " << matchId << "] Countdown: " << countdown << " seconds remaining.\n";
                {
                    std::lock_guard<std::mutex> lock(playerMutex);
                    for (auto& p : players) {
                        if (p) {
                            if (sendCallback) {
                                sendCallback("COUNT_DOWN|" + std::to_string(countdown), p->clientId);
                            }
                        }
                    }
                }
                countdown--;
				continue;
            }
            if (-1 == countdown)
            {
                startTime = std::chrono::steady_clock::now();

                std::lock_guard<std::mutex> lock(playerMutex);
                for (auto& p : players) {
                    if (p) {
                        if (sendCallback) {
                            sendCallback("START_RACE|", p->clientId);
                        }
                    }
                }
                countdown--;
            }

            if (-2 == countdown)
            {
				auto delta = std::chrono::steady_clock::now() - startTime;
                playTime = std::chrono::duration_cast<std::chrono::milliseconds>(delta).count();
                {
					std::lock_guard<std::mutex> lock(playerMutex);
                    for (auto& p : players) {
                        if (p) {
                            if (sendCallback) {
                                sendCallback("PLAY_TIME|" + std::to_string(playTime), p->clientId);
                            }
                        }
                    }
                }
            }

            int playerCount = 0;
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                playerCount = players.size();
            }

            if (playerCount <= 1) {
                if (running) {
                    std::cout << "[Match " << matchId << "] Auto-stop: " << playerCount << " player(s) left.\n";
                    Stop();
                }
            }
        }
        std::cout << "[Match " << matchId << "] Loop exited.\n";
    }

    void Stop() {
        if (!running.exchange(false)) {
            return;
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

    int GetID() const { return matchId; }

    friend class DroneRacingServer;

    std::function<void(const std::string&, const std::string&)> sendCallback;

private:
    int matchId;
    std::atomic<bool> running = false;
	int countdown = 5;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    long long playTime;

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

    std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
    std::mutex mtx;
    std::atomic<int> nextClientId{ 1 };
    std::vector<std::shared_ptr<Match>> matches;
    int nextMatchId = 1;

    void StartBeacon() {
        beaconUDP.Start();
        std::thread([this]() {
            auto ip = beaconUDP.GetLocalIP();
            while (true) {
                auto message = "DRONE_RACING_SERVER_IP:" + ip;
                beaconUDP.SendToAll(message, 5000);
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            }).detach();
    }

    void StartTCP() {
        receptionTCP.Init();
        receptionTCP.Bind(6000);
        receptionTCP.Listen();

        receptionTCP.OnConnect([this](const std::string& ip) {
            std::lock_guard<std::mutex> lock(mtx);
            auto session = std::make_shared<Session>();
            session->ip = ip;

            std::ostringstream oss;
            oss << "C" << nextClientId++;
            session->clientId = oss.str();

            sessions[session->clientId] = session;

            receptionTCP.MapClientIpToId(ip, session->clientId);
            receptionTCP.RegisterClientSocket(session->clientId, ip);

            std::cout << "[TCP] Connected: " << ip
                << " -> ClientID=" << session->clientId << std::endl;

            return "CLIENT_ID|" + session->clientId;
            });

        receptionTCP.OnReceive([this](const std::string& msg, const std::string& clientId) {
            std::lock_guard<std::mutex> lock(mtx);

            auto session = FindSessionById(clientId);
            if (!session) {
                std::cout << "[TCP] Received from unknown ID: " << clientId << std::endl;
                return;
            }

            std::cout << "[TCP " << clientId << "] " << msg << std::endl;

            if (msg.rfind("JOIN", 0) == 0) {
                session->playerName = clientId;
                AssignToMatch(session);

                std::stringstream ss;
				ss << "JOINED_MATCH|" << session->currentMatch.lock()->GetID();
				receptionTCP.SendToClient(clientId, ss.str());
            }
            else if (msg.rfind("READY|", 0) == 0) {
                session->ready = true;
                std::cout << "[TCP] " << session->clientId << " is READY" << std::endl;

                auto match = session->currentMatch.lock();
                if (match && !match->IsRunning() && match->IsReadyToStart()) {
                    match->Start();
                }
            }
            });

        receptionTCP.OnDisconnect([this](const std::string& clientId) {
            std::lock_guard<std::mutex> lock(mtx);

            auto session = FindSessionById(clientId);
            if (!session) {
                std::cout << "[TCP] Disconnected unknown ID: " << clientId << std::endl;
                return;
            }

            std::cout << "[TCP] Disconnected: " << clientId << std::endl;

            sessions.erase(clientId);

            auto match = session->currentMatch.lock();
            if (!match) return;

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
                std::cout << "[Match " << match->GetID() << "] Removed player " << clientId << std::endl;

            // FIX: 여기서 match->Stop()을 절대 호출하지 않음!
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
                    std::cout << "[Server] Cleaned up empty match " << match->GetID() << std::endl;
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

    void AssignToMatch(std::shared_ptr<Session> s) {
        auto openMatch = FindOpenMatch();
        if (!openMatch) {
            openMatch = std::make_shared<Match>(nextMatchId++);
            matches.push_back(openMatch);
            openMatch->sendCallback = [this](const std::string& msg, const std::string& clientId) {
                receptionTCP.SendToClient(clientId, msg);
                };

            std::cout << "[Server] Created new match " << openMatch->GetID() << std::endl;
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

int main() {
    DroneRacingServer server;
    server.Start();

    std::cout << "Server started. Press Enter to exit...\n";
    std::cin.get();

    server.Stop();
    std::cout << "Server shutting down.\n";
    return 0;
}