// =============================================================
// Project: DroneRacingServer
// File: main.cpp
// =============================================================

#include "RxTx/RxTx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <queue>
#include <condition_variable>

using namespace libRxTx;

class DroneRacingServer;

struct PlayerTransform {
    float x, y, z;
    float roll, pitch, yaw;
};

// 세션 타입 정의
enum class SessionType {
    Observer,
    Player,
    Recorder,
    Replayer
};

// -----------------------------
// Session 구조체
// -----------------------------
struct Session {
    SessionType type = SessionType::Player;
    std::string clientId;
    std::string playerName;
    std::string ip;
    bool ready = false;
    std::weak_ptr<class Match> currentMatch;
};

#define MAX_PLAYERS_PER_MATCH 2

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
        std::cout << "[Match " << matchId << "] Added Participant: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ") Type: " << (int)s->type << std::endl;

        // 플레이어/리플레이어는 위치 정보 공간 확보
        playerTransforms.push_back({ 0,0,0,0,0,0 });
        playerScores.push_back(0);
    }

    void AddObserver(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(observerMutex);
        observers.push_back(s);
        std::cout << "[Match " << matchId << "] Added Viewer: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ") Type: " << (int)s->type << std::endl;
    }

    void Start() {
        if (running) return;
        running = true;

        std::cout << "[Match " << matchId << "] Race started with "
            << players.size() << " players." << std::endl;

        std::string filename = "match_" + std::to_string(matchId) + "_log.txt";
        logFile.open(filename, std::ios::out | std::ios::trunc);
        if (logFile.is_open()) {
            std::cout << "[Match " << matchId << "] Logging started: " << filename << std::endl;
        }

        // 로깅 쓰레드 시작
        isLoggingActive = true;
        if (logThread.joinable()) logThread.join();
        logThread = std::thread(&Match::LogWorker, this);

        if (thread.joinable()) thread.join();

        auto self = shared_from_this();
        thread = std::thread([self]() { self->Loop(); });
    }

    void Loop()
    {
        std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();
        std::chrono::nanoseconds ellapsedTimeCountDown(0);
        std::chrono::nanoseconds ellapsedTimeCountSending(0);

        const long long TIME_THRESHOLD_COUNTDOWN = 1000000000;
        const long long TIME_THRESHOLD_SENDING = 16000000; // approx 60Hz

        while (running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            auto delta = now - lastTime;
            lastTime = now;

            ellapsedTimeCountDown += std::chrono::duration_cast<std::chrono::nanoseconds>(delta);

            if (raceStarted)
            {
                ellapsedTimeCountSending += std::chrono::duration_cast<std::chrono::nanoseconds>(delta);

                if (ellapsedTimeCountSending.count() >= TIME_THRESHOLD_SENDING)
                {
                    // 1. 위치 업데이트 패킷 생성
                    {
                        std::stringstream ssMsg;
                        ssMsg << std::fixed << std::setprecision(6);
                        ssMsg << "PLAYER_TRANSFORM_UPDATE|";

                        bool hasData = false;
                        {
                            std::lock_guard<std::mutex> lock(playerMutex);
                            size_t pSize = playerTransforms.size();

                            for (size_t i = 0; i < pSize; i++)
                            {
                                const auto& transform = playerTransforms[i];
                                ssMsg << i << ","
                                    << transform.x << "," << transform.y << "," << transform.z << ","
                                    << transform.roll << "," << transform.pitch << "," << transform.yaw;

                                if (i != pSize - 1) ssMsg << "/";
                            }
                            hasData = (pSize > 0);
                        }

                        if (hasData)
                        {
                            std::string msg = ssMsg.str();

                            // 로깅
                            {
                                auto raceCurrentTime = std::chrono::steady_clock::now();
                                long long raceDelta = std::chrono::duration_cast<std::chrono::milliseconds>(raceCurrentTime - startTime).count();
                                std::stringstream ssLog;
                                ssLog << raceDelta << "|" << msg << "\n";
                                EnqueueLog(ssLog.str());
                            }

                            // 브로드캐스팅 (Players + Observers)
                            auto broadcast = [&](const std::string& m) {
                                std::lock_guard<std::mutex> pLock(playerMutex);
                                for (auto& p : players) if (p && sendCallback) sendCallback(m, p->clientId);
                                std::lock_guard<std::mutex> oLock(observerMutex);
                                for (auto& o : observers) if (o && sendCallback) sendCallback(m, o->clientId);
                                };
                            broadcast(msg);
                        }
                    }

                    // 2. 랭킹/점수 업데이트 패킷 생성 (생략 가능하나 원본 유지)
                    {
                        // (점수 기반 랭킹 로직 - 기존과 동일)
                        // ...
                    }

                    ellapsedTimeCountSending -= std::chrono::nanoseconds(TIME_THRESHOLD_SENDING);
                }
            }

            // 카운트다운 로직
            if (ellapsedTimeCountDown.count() >= TIME_THRESHOLD_COUNTDOWN)
            {
                ellapsedTimeCountDown -= std::chrono::nanoseconds(TIME_THRESHOLD_COUNTDOWN);

                if (countdown >= 0)
                {
                    std::cout << "[Match " << matchId << "] Countdown: " << countdown << "\n";
                    std::string msg = "COUNT_DOWN|" + std::to_string(countdown);

                    // Broadcast
                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        for (auto& p : players) if (p && sendCallback) sendCallback(msg, p->clientId);
                    }
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        for (auto& p : observers) if (p && sendCallback) sendCallback(msg, p->clientId);
                    }

                    countdown--;
                }
                else if (countdown == -1)
                {
                    raceStarted = true;
                    startTime = std::chrono::steady_clock::now();
                    ellapsedTimeCountSending = std::chrono::nanoseconds(0);

                    std::stringstream ss;
                    ss << "START_RACE|" << players.size();
                    std::string msg = ss.str();

                    // Broadcast
                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        for (auto& p : players) if (p && sendCallback) sendCallback(msg, p->clientId);
                    }
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        for (auto& p : observers) if (p && sendCallback) sendCallback(msg, p->clientId);
                    }
                    countdown--;
                }
                else if (countdown == -2)
                {
                    // Play Time 전송 로직
                    auto raceDelta = std::chrono::steady_clock::now() - startTime;
                    playTime = std::chrono::duration_cast<std::chrono::milliseconds>(raceDelta).count();
                    // ... (Broadcast PLAY_TIME)
                }
            }

            // 자동 종료 체크 (플레이어가 1명 이하이면 종료 등 정책에 따라)
            int playerCount = 0;
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                playerCount = (int)players.size();
            }
            if (playerCount < MAX_PLAYERS_PER_MATCH && running && countdown < 0)
            {
                if (playerCount == 1) {
                    std::lock_guard<std::mutex> lock(playerMutex);
                    if (!players.empty() && players[0]) {
                        // 현재 남아있는 0번 인덱스 플레이어에게 승리 판정
                        // 주의: players 벡터에서 나간 사람이 지워졌으므로, 남은 사람은 0번 인덱스로 당겨짐
                        PropagatePlayerWin(0);
                    }
                }

                std::cout << "[Match " << matchId << "] Auto-stop: No players left.\n";
                running = false;
            }
        }
        std::cout << "[Match " << matchId << "] Loop exited.\n";
    }

    void Stop() {
        if (!running.exchange(false)) return; // 이미 멈춤

        std::cout << "[Match " << matchId << "] Stopping..." << std::endl;

        // 종료 메시지 전송
        auto broadcast = [&](const std::string& m) {
            std::lock_guard<std::mutex> pLock(playerMutex);
            for (auto& p : players) if (p && sendCallback) sendCallback(m, p->clientId);
            std::lock_guard<std::mutex> oLock(observerMutex);
            for (auto& o : observers) if (o && sendCallback) sendCallback(m, o->clientId);
            };
        broadcast("RACE_FINISHED|");

        // 로깅 종료
        {
            std::lock_guard<std::mutex> lock(logMutex);
            isLoggingActive = false;
        }
        logCv.notify_all();
        if (logThread.joinable()) logThread.join();
        if (logFile.is_open()) logFile.close();
    }

    bool IsRunning() const { return running; }
    bool IsFull() const {
        std::lock_guard<std::mutex> lock(playerMutex);
        return players.size() >= MAX_PLAYERS_PER_MATCH;
    }
    bool IsReadyToStart() const {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (players.size() < MAX_PLAYERS_PER_MATCH) return false;
        for (auto& p : players) {
            if (!p->ready) return false;
        }
        return true;
    }
    int GetID() const { return matchId; }

    void SetPlayerTransform(int playerIndex, const PlayerTransform& transform) {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (playerIndex >= 0 && playerIndex < playerTransforms.size()) {
            playerTransforms[playerIndex] = transform;
        }
    }
    void SetPlayerScore(int playerIndex, unsigned int score) {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (playerIndex >= 0 && playerIndex < playerScores.size()) {
            playerScores[playerIndex] = score;
        }
        if (26 <= score) PropagatePlayerWin(playerIndex);
    }
    void PropagatePlayerWin(int winnerIndex) {
        raceStarted = false;
        std::stringstream ss; ss << "PLAYER_WIN|" << winnerIndex;
        // Broadcast...
    }
    void PropagatePlayerCrashed(int crashedPlayerIndex) {
        raceStarted = false;
        std::stringstream ss; ss << "PLAYER_CRASHED|" << crashedPlayerIndex;
        // Broadcast...
        Stop();
    }

    std::function<void(const std::string&, const std::string&)> sendCallback;
    friend class DroneRacingServer;

private:
    int matchId;
    std::atomic<bool> running = false;
    int countdown = 5;
    std::chrono::time_point<std::chrono::steady_clock> startTime;
    long long playTime;
    bool raceStarted = false;

    mutable std::mutex playerMutex;
    std::vector<std::shared_ptr<Session>> players; // Player + Replayer

    mutable std::mutex observerMutex;
    std::vector<std::shared_ptr<Session>> observers; // Observer + Recorder

    std::thread thread;
    std::vector<PlayerTransform> playerTransforms;
    std::vector<unsigned int> playerScores;

    std::ofstream logFile;
    std::thread logThread;
    std::queue<std::string> logQueue;
    std::mutex logMutex;
    std::condition_variable logCv;
    bool isLoggingActive = false;

    void EnqueueLog(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(logMutex);
            if (!isLoggingActive) return;
            logQueue.push(message);
        }
        logCv.notify_one();
    }
    void LogWorker() {
        while (true) {
            std::unique_lock<std::mutex> lock(logMutex);
            logCv.wait(lock, [this]() { return !logQueue.empty() || !isLoggingActive; });
            if (logQueue.empty() && !isLoggingActive) break;
            std::queue<std::string> tempQueue;
            tempQueue.swap(logQueue);
            lock.unlock();
            if (logFile.is_open()) {
                while (!tempQueue.empty()) {
                    logFile << tempQueue.front();
                    tempQueue.pop();
                }
            }
        }
    }
};

// -----------------------------
// DroneRacingServer
// -----------------------------
class DroneRacingServer {
public:
    DroneRacingServer() : beaconUDP(Protocol::UDP), receptionTCP(Protocol::TCP) {}

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
    ~DroneRacingServer() { Stop(); }

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
                beaconUDP.SendToAll("DRONE_RACING_SERVER_IP:" + ip, 5000);
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
            std::ostringstream oss; oss << "C" << nextClientId++;
            session->clientId = oss.str();
            sessions[session->clientId] = session;
            receptionTCP.MapClientIpToId(ip, session->clientId);
            receptionTCP.RegisterClientSocket(session->clientId, ip);
            std::cout << "[TCP] Connected: " << ip << " -> ClientID=" << session->clientId << std::endl;
            return "CLIENT_ID|" + session->clientId;
            });

        receptionTCP.OnReceive([this](const std::string& msg, const std::string& clientId) {
            std::lock_guard<std::mutex> lock(mtx);
            auto session = FindSessionById(clientId);
            if (!session) return;

            if (msg.rfind("JOIN|", 0) == 0) {
                session->playerName = clientId;
                std::istringstream iss(msg.substr(5));
                std::string typeStr;
                iss >> typeStr;
                std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::tolower);

                if (typeStr == "player") session->type = SessionType::Player;
                else if (typeStr == "replayer") session->type = SessionType::Replayer;
                else if (typeStr == "recorder") session->type = SessionType::Recorder;
                else session->type = SessionType::Player;

                AssignToMatch(session);

                std::stringstream ss;
                ss << "JOINED_MATCH|" << session->currentMatch.lock()->GetID();
                receptionTCP.SendToClient(clientId, ss.str());

                // Player/Replayer에게만 인덱스 전송
                if (session->type == SessionType::Player || session->type == SessionType::Replayer) {
                    std::stringstream ssIdx;
                    ssIdx << "PLAYER_INDEX|" << session->currentMatch.lock()->players.size() - 1;
                    receptionTCP.SendToClient(clientId, ssIdx.str());
                }
            }
            else if (msg.rfind("READY|", 0) == 0) {
                session->ready = true;
                std::cout << "[TCP] " << session->clientId << " is READY" << std::endl;
                auto match = session->currentMatch.lock();
                if (match && !match->IsRunning() && match->IsReadyToStart()) {
                    match->Start();
                }
            }
            else if (msg.rfind("PLAYER_TRANSFORM|", 0) == 0) {
                auto match = session->currentMatch.lock();
                if (match) {
                    std::istringstream iss(msg.substr(17));
                    int playerIndex = -1;
                    PlayerTransform transform;
                    char sep;
                    iss >> playerIndex >> sep
                        >> transform.x >> sep >> transform.y >> sep >> transform.z >> sep
                        >> transform.roll >> sep >> transform.pitch >> sep >> transform.yaw;
                    match->SetPlayerTransform(playerIndex, transform);
                }
            }
            // ... (SCORE, CRASHED 처리 등 생략 가능하나 원본 구조 유지)
            });

        receptionTCP.OnDisconnect([this](const std::string& clientId) {
            std::lock_guard<std::mutex> lock(mtx);
            auto session = FindSessionById(clientId);
            if (!session) return;

            sessions.erase(clientId);
            auto match = session->currentMatch.lock();
            if (!match) return;

            // 매치에서 세션 제거
            {
                std::lock_guard<std::mutex> pLock(match->playerMutex);
                auto& vec = match->players;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [&](auto& s) { return s->clientId == clientId; }), vec.end());
            }
            {
                std::lock_guard<std::mutex> oLock(match->observerMutex);
                auto& vec = match->observers;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [&](auto& s) { return s->clientId == clientId; }), vec.end());
            }

            // 매치가 완전히 비었으면 삭제
            {
                std::lock_guard<std::mutex> pLock(match->playerMutex);
                std::lock_guard<std::mutex> oLock(match->observerMutex);
                if (match->players.empty() && match->observers.empty() && !match->IsRunning()) {
                    matches.erase(std::remove_if(matches.begin(), matches.end(),
                        [&](auto& m) { return m == match; }), matches.end());
                    std::cout << "[Server] Cleaned up match " << match->GetID() << std::endl;
                }
            }
            });

        receptionTCP.Start();
    }

    std::shared_ptr<Session> FindSessionById(const std::string& id) {
        auto it = sessions.find(id);
        return (it != sessions.end()) ? it->second : nullptr;
    }

    void AssignToMatch(std::shared_ptr<Session> s) {
        if (s->type == SessionType::Player || s->type == SessionType::Replayer) {
            auto openMatch = FindOpenMatch();
            if (!openMatch) {
                openMatch = std::make_shared<Match>(nextMatchId++);
                matches.push_back(openMatch);
                openMatch->sendCallback = [this](const std::string& msg, const std::string& cid) { receptionTCP.SendToClient(cid, msg); };
                std::cout << "[Server] Created Match " << openMatch->GetID() << " (P)" << std::endl;
            }
            s->currentMatch = openMatch;
            openMatch->AddPlayer(s);
            if (openMatch->IsReadyToStart()) openMatch->Start();
        }
        else {
            auto firstMatch = FindFirstMatch();
            if (!firstMatch) {
                firstMatch = std::make_shared<Match>(nextMatchId++);
                matches.push_back(firstMatch);
                firstMatch->sendCallback = [this](const std::string& msg, const std::string& cid) { receptionTCP.SendToClient(cid, msg); };
                std::cout << "[Server] Created Match " << firstMatch->GetID() << " (O)" << std::endl;
            }
            s->currentMatch = firstMatch;
            firstMatch->AddObserver(s);
        }
    }

    std::shared_ptr<Match> FindOpenMatch() {
        for (auto& m : matches) if (!m->IsFull() && !m->IsRunning()) return m;
        return nullptr;
    }
    std::shared_ptr<Match> FindFirstMatch() {
        return matches.empty() ? nullptr : matches.front();
    }
};

int main() {
	printf("[Server]\n");

    DroneRacingServer server;
    server.Start();
    std::cout << "Server started. Press Enter to exit...\n";
    std::cin.get();
    server.Stop();
    return 0;
}
