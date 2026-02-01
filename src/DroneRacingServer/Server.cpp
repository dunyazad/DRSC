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
#include <functional>

using namespace libRxTx;

class DroneRacingServer;

struct PlayerTransform
{
    float x, y, z;
    float roll, pitch, yaw;
};

// Session Type Definition
enum class SessionType
{
    Observer,
    Player,
    Recorder,
    Replayer
};

// -----------------------------
// Session Structure
// -----------------------------
struct Session
{
    SessionType type = SessionType::Player;
    std::string clientId;
    std::string playerName;
    std::string ip;
    bool ready = false;
    std::weak_ptr<class Match> currentMatch;
};

#define MAX_PLAYERS_PER_MATCH 2

// -----------------------------
// Match Class
// -----------------------------
class Match : public std::enable_shared_from_this<Match>
{
public:
    explicit Match(int id) : matchId(id)
    {
    }

    ~Match()
    {
        Stop();

        if (thread.joinable())
        {
            thread.join();
        }

        {
            std::lock_guard<std::mutex> lock(logMutex);
            isLoggingActive = false;
        }
        logCv.notify_all();

        if (logThread.joinable())
        {
            logThread.join();
        }

        if (logFile.is_open())
        {
            logFile.close();
        }

        std::cout << "[Match " << matchId << "] Destructor finished." << std::endl;
    }

    void AddPlayer(std::shared_ptr<Session> s)
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        players.push_back(s);
        std::cout << "[Match " << matchId << "] Added Participant: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ") Type: " << (int)s->type << std::endl;

        playerTransforms.push_back({ 0,0,0,0,0,0 });
        playerScores.push_back(0);
    }

    void AddObserver(std::shared_ptr<Session> s)
    {
        std::lock_guard<std::mutex> lock(observerMutex);
        observers.push_back(s);
        std::cout << "[Match " << matchId << "] Added Viewer: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ") Type: " << (int)s->type << std::endl;
    }

    void RemoveSession(const std::string& clientId)
    {
        bool shouldStop = false;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            int removeIndex = -1;
            for (size_t i = 0; i < players.size(); ++i)
            {
                if (players[i]->clientId == clientId)
                {
                    removeIndex = (int)i;
                    break;
                }
            }

            if (removeIndex != -1)
            {
                players.erase(players.begin() + removeIndex);
                if (removeIndex < (int)playerTransforms.size())
                {
                    playerTransforms.erase(playerTransforms.begin() + removeIndex);
                }
                if (removeIndex < (int)playerScores.size())
                {
                    playerScores.erase(playerScores.begin() + removeIndex);
                }

                std::cout << "[Match " << matchId << "] Removed Player: " << clientId << std::endl;

                // [Fix] Check win condition
                if (raceStarted && players.size() == 1)
                {
                    std::cout << "[Match " << matchId << "] Only one player remaining. Declaring winner." << std::endl;
                    PropagatePlayerWin(0);
                    shouldStop = true; // Mark to stop later
                }
            }
        } // playerMutex is released here

        {
            std::lock_guard<std::mutex> lock(observerMutex);
            auto it = std::remove_if(observers.begin(), observers.end(),
                [&](const std::shared_ptr<Session>& s) { return s->clientId == clientId; });
            if (it != observers.end())
            {
                observers.erase(it, observers.end());
                std::cout << "[Match " << matchId << "] Removed Observer: " << clientId << std::endl;
            }
        }

        // Call Stop() outside the lock to prevent Deadlock
        if (shouldStop)
        {
            Stop();
        }
    }

    void Start()
    {
        if (running)
        {
            return;
        }

        // [Fix] Reset state for restart
        running = true;
        raceStarted = false;
        countdown = 5;

        std::cout << "[Match " << matchId << "] Race started with "
            << players.size() << " players." << std::endl;

        // [Fix] Add timestamp to filename to prevent overwriting
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ssFilename;
        ssFilename << "match_" << matchId << "_"
            << std::put_time(std::localtime(&now_time), "%Y%m%d_%H%M%S")
            << "_log.txt";

        std::string filename = ssFilename.str();

        logFile.open(filename, std::ios::out | std::ios::trunc);
        if (logFile.is_open())
        {
            std::cout << "[Match " << matchId << "] Logging started: " << filename << std::endl;
        }

        isLoggingActive = true;
        if (logThread.joinable())
        {
            logThread.join();
        }
        logThread = std::thread(&Match::LogWorker, this);

        if (thread.joinable())
        {
            thread.join();
        }

        auto self = shared_from_this();
        thread = std::thread([self]() { self->Loop(); });
    }

    void Loop()
    {
        std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();
        std::chrono::nanoseconds ellapsedTimeCountDown(0);
        std::chrono::nanoseconds ellapsedTimeCountSending(0);

        const long long TIME_THRESHOLD_COUNTDOWN = 1000000000;
        const long long TIME_THRESHOLD_SENDING = 16000000;

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

                                if (i != pSize - 1)
                                {
                                    ssMsg << "/";
                                }
                            }
                            hasData = (pSize > 0);
                        }

                        if (hasData)
                        {
                            std::string msg = ssMsg.str();
                            {
                                auto raceCurrentTime = std::chrono::steady_clock::now();
                                long long raceDelta = std::chrono::duration_cast<std::chrono::milliseconds>(raceCurrentTime - startTime).count();
                                std::stringstream ssLog;
                                ssLog << raceDelta << "|" << msg << "\n";
                                EnqueueLog(ssLog.str());
                            }

                            auto broadcast = [&](const std::string& m)
                                {
                                    std::lock_guard<std::mutex> pLock(playerMutex);
                                    for (auto& p : players)
                                    {
                                        if (p && sendCallback) sendCallback(m, p->clientId);
                                    }
                                    std::lock_guard<std::mutex> oLock(observerMutex);
                                    for (auto& o : observers)
                                    {
                                        if (o && sendCallback) sendCallback(m, o->clientId);
                                    }
                                };
                            broadcast(msg);
                        }
                    }
                    ellapsedTimeCountSending -= std::chrono::nanoseconds(TIME_THRESHOLD_SENDING);
                }
            }

            if (ellapsedTimeCountDown.count() >= TIME_THRESHOLD_COUNTDOWN)
            {
                ellapsedTimeCountDown -= std::chrono::nanoseconds(TIME_THRESHOLD_COUNTDOWN);
                if (countdown >= 0)
                {
                    std::string msg = "COUNT_DOWN|" + std::to_string(countdown);
                    auto broadcast = [&](const std::string& m)
                        {
                            std::lock_guard<std::mutex> pLock(playerMutex);
                            for (auto& p : players)
                            {
                                if (p && sendCallback) sendCallback(m, p->clientId);
                            }
                            std::lock_guard<std::mutex> oLock(observerMutex);
                            for (auto& o : observers)
                            {
                                if (o && sendCallback) sendCallback(m, o->clientId);
                            }
                        };
                    broadcast(msg);
                    countdown--;
                }
                else if (countdown == -1)
                {
                    raceStarted = true;
                    startTime = std::chrono::steady_clock::now();
                    ellapsedTimeCountSending = std::chrono::nanoseconds(0);

                    std::stringstream ss;
                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        ss << "START_RACE|" << players.size();
                    }
                    std::string msg = ss.str();
                    auto broadcast = [&](const std::string& m)
                        {
                            std::lock_guard<std::mutex> pLock(playerMutex);
                            for (auto& p : players)
                            {
                                if (p && sendCallback) sendCallback(m, p->clientId);
                            }
                            std::lock_guard<std::mutex> oLock(observerMutex);
                            for (auto& o : observers)
                            {
                                if (o && sendCallback) sendCallback(m, o->clientId);
                            }
                        };
                    broadcast(msg);

                    countdown--;
                }
            }

            int playerCount = 0;
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                playerCount = (int)players.size();
            }

            // [Fix] Only stop if NO players are left.
            // Previously: (playerCount < MAX_PLAYERS_PER_MATCH) caused stop on single disconnect.
            if (playerCount == 0 && running && countdown < 0)
            {
                std::cout << "[Match " << matchId << "] Auto-stop: No players left.\n";
                running = false;
            }
        }
        std::cout << "[Match " << matchId << "] Loop exited.\n";
    }

    void Stop()
    {
        if (!running.exchange(false))
        {
            return;
        }

        std::cout << "[Match " << matchId << "] Stopping..." << std::endl;

        auto broadcast = [&](const std::string& m)
            {
                std::lock_guard<std::mutex> pLock(playerMutex);
                for (auto& p : players)
                {
                    if (p && sendCallback) sendCallback(m, p->clientId);
                }
                std::lock_guard<std::mutex> oLock(observerMutex);
                for (auto& o : observers)
                {
                    if (o && sendCallback) sendCallback(m, o->clientId);
                }
            };
        broadcast("RACE_FINISHED|");

        {
            std::lock_guard<std::mutex> lock(logMutex);
            isLoggingActive = false;
        }
        logCv.notify_all();

        if (logThread.joinable())
        {
            logThread.join();
        }
        if (logFile.is_open())
        {
            logFile.close();
        }
    }

    bool IsRunning() const
    {
        return running;
    }

    bool IsFull() const
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        return players.size() >= MAX_PLAYERS_PER_MATCH;
    }

    bool IsReadyToStart() const
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (players.size() < MAX_PLAYERS_PER_MATCH)
        {
            return false;
        }

        for (auto& p : players)
        {
            if (!p->ready)
            {
                return false;
            }
        }
        return true;
    }

    int GetID() const
    {
        return matchId;
    }

    int GetPlayerIndex(const std::shared_ptr<Session>& targetSession)
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        for (size_t i = 0; i < players.size(); ++i)
        {
            if (players[i] == targetSession)
            {
                return (int)i;
            }
        }
        return -1;
    }

    void SetPlayerTransform(int playerIndex, const PlayerTransform& transform)
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (playerIndex >= 0 && playerIndex < (int)playerTransforms.size())
        {
            playerTransforms[playerIndex] = transform;
        }
    }

    void SetPlayerScore(int playerIndex, unsigned int score)
    {
        bool shouldStop = false;
        {
            std::lock_guard<std::mutex> lock(playerMutex);
            if (playerIndex >= 0 && playerIndex < (int)playerScores.size())
            {
                playerScores[playerIndex] = score;
            }
            if (26 <= score)
            {
                PropagatePlayerWin(playerIndex);
                shouldStop = true; // Mark to stop later
            }
        } // Mutex is released here

        // Call Stop() outside the lock to prevent Deadlock
        if (shouldStop)
        {
            Stop();
        }
    }

    void PropagatePlayerWin(int winnerIndex)
    {
        raceStarted = false;
        std::stringstream ss;
        ss << "PLAYER_WIN|" << winnerIndex;
        std::string msg = ss.str();

        // [Fix] REMOVED lock_guard here.
        // The caller already holds the lock, so we can iterate safely.

        // 1. Send to Players
        for (auto& p : players)
        {
            if (p && sendCallback) sendCallback(msg, p->clientId);
        }

        // 2. Send to Observers (Observer lock is separate, so we lock it here)
        {
            std::lock_guard<std::mutex> oLock(observerMutex);
            for (auto& o : observers)
            {
                if (o && sendCallback) sendCallback(msg, o->clientId);
            }
        }

        // [Fix] REMOVED Stop() call here.
        // Stop() must be called by the caller AFTER releasing the mutex.
    }

    void PropagatePlayerCrashed(int crashedPlayerIndex)
    {
        raceStarted = false;
        std::stringstream ss;
        ss << "PLAYER_CRASHED|" << crashedPlayerIndex;
        std::string msg = ss.str();

        std::lock_guard<std::mutex> pLock(playerMutex);
        for (auto& p : players)
        {
            if (p && sendCallback) sendCallback(msg, p->clientId);
        }
        std::lock_guard<std::mutex> oLock(observerMutex);
        for (auto& o : observers)
        {
            if (o && sendCallback) sendCallback(msg, o->clientId);
        }

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
    std::vector<std::shared_ptr<Session>> players;

    mutable std::mutex observerMutex;
    std::vector<std::shared_ptr<Session>> observers;

    std::thread thread;
    std::vector<PlayerTransform> playerTransforms;
    std::vector<unsigned int> playerScores;

    std::ofstream logFile;
    std::thread logThread;
    std::queue<std::string> logQueue;
    std::mutex logMutex;
    std::condition_variable logCv;
    bool isLoggingActive = false;

    void EnqueueLog(const std::string& message)
    {
        {
            std::lock_guard<std::mutex> lock(logMutex);
            if (!isLoggingActive)
            {
                return;
            }
            logQueue.push(message);
        }
        logCv.notify_one();
    }

    void LogWorker()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(logMutex);
            logCv.wait(lock, [this]() { return !logQueue.empty() || !isLoggingActive; });
            if (logQueue.empty() && !isLoggingActive)
            {
                break;
            }

            std::queue<std::string> tempQueue;
            tempQueue.swap(logQueue);
            lock.unlock();

            if (logFile.is_open())
            {
                while (!tempQueue.empty())
                {
                    logFile << tempQueue.front();
                    tempQueue.pop();
                }
            }
        }
    }
};

// -----------------------------
// DroneRacingServer (Revised)
// -----------------------------
class DroneRacingServer
{
public:
    DroneRacingServer() : beaconUDP(Protocol::UDP), receptionTCP(Protocol::TCP)
    {
    }

    void Start()
    {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());
        StartBeacon();
        StartTCP();
    }

    void Stop()
    {
        // 1. Stop Beacon Thread first
        beaconRunning = false;
        if (beaconThread.joinable())
        {
            beaconThread.join();
        }
        beaconUDP.Stop(); // Stop socket after thread finishes

        // 2. Stop TCP
        receptionTCP.Stop();
    }

    ~DroneRacingServer()
    {
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

    // Added: Members for safe thread management
    std::thread beaconThread;
    std::atomic<bool> beaconRunning{ false };

    void StartBeacon()
    {
        beaconUDP.Start();
        beaconRunning = true;

        // [Fix] Store thread in member variable instead of detaching
        beaconThread = std::thread([this]()
            {
                auto ip = beaconUDP.GetLocalIP();
                while (beaconRunning)
                {
                    beaconUDP.SendToAll("DRONE_RACING_SERVER_IP:" + ip, 5000);

                    // Sleep with check to allow faster shutdown
                    for (int i = 0; i < 10; ++i)
                    {
                        if (!beaconRunning)
                        {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            });
    }

    void StartTCP()
    {
        receptionTCP.Init();
        receptionTCP.Bind(6000);
        receptionTCP.Listen();

        receptionTCP.OnConnect([this](const std::string& ip)
            {
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

        receptionTCP.OnReceive([this](const std::string& msg, const std::string& clientId)
            {
                std::lock_guard<std::mutex> lock(mtx);
                auto session = FindSessionById(clientId);
                if (!session)
                {
                    return;
                }

                if (msg.rfind("JOIN|", 0) == 0)
                {
                    session->playerName = clientId;
                    std::istringstream iss(msg.substr(5));
                    std::string typeStr;
                    iss >> typeStr;
                    std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::tolower);

                    if (typeStr == "player")
                    {
                        session->type = SessionType::Player;
						printf("Player connected: %s\n", clientId.c_str());
                    }
                    else if (typeStr == "observer")
                    {
                        session->type = SessionType::Observer;
						printf("Observer connected: %s\n", clientId.c_str());
                    }
                    else if (typeStr == "replayer")
                    {
                        session->type = SessionType::Replayer;
						printf("Replayer connected: %s\n", clientId.c_str());
                    }
                    else if (typeStr == "recorder")
                    {
                        session->type = SessionType::Recorder;
						printf("Recorder connected: %s\n", clientId.c_str());
                    }
                    else if (typeStr == "bot")
                    {
                        session->type = SessionType::Player;
						printf("Bot connected: %s\n", clientId.c_str());
                    }
                    else
                    {
                        session->type = SessionType::Player;
						printf("Unknown type, defaulting to Player: %s\n", clientId.c_str());
                    }

                    AssignToMatch(session);

                    auto match = session->currentMatch.lock();
                    if (match)
                    {
                        std::stringstream ss;
                        ss << "JOINED_MATCH|" << match->GetID();
                        receptionTCP.SendToClient(clientId, ss.str());

                        if (session->type == SessionType::Player || session->type == SessionType::Replayer)
                        {
                            int idx = match->GetPlayerIndex(session);
                            std::stringstream ssIdx;
                            ssIdx << "PLAYER_INDEX|" << idx;
                            receptionTCP.SendToClient(clientId, ssIdx.str());
                        }
                    }
                }
                else if (msg.rfind("READY|", 0) == 0)
                {
                    session->ready = true;
                    std::cout << "[TCP] " << session->clientId << " is READY" << std::endl;
                    auto match = session->currentMatch.lock();
                    if (match && !match->IsRunning() && match->IsReadyToStart())
                    {
                        match->Start();
                    }
                }
                else if (msg.rfind("PLAYER_TRANSFORM|", 0) == 0)
                {
                    auto match = session->currentMatch.lock();
                    if (match)
                    {
                        int realIndex = match->GetPlayerIndex(session);
                        if (realIndex != -1)
                        {
                            std::istringstream iss(msg.substr(17));
                            int clientSentIndex = -1;
                            PlayerTransform transform;
                            char sep;
                            iss >> clientSentIndex >> sep
                                >> transform.x >> sep >> transform.y >> sep >> transform.z >> sep
                                >> transform.roll >> sep >> transform.pitch >> sep >> transform.yaw;

                            match->SetPlayerTransform(realIndex, transform);
                        }
                    }
                }
            });

        receptionTCP.OnDisconnect([this](const std::string& clientId)
            {
                std::lock_guard<std::mutex> lock(mtx);
                auto session = FindSessionById(clientId);
                if (!session)
                {
                    return;
                }

                sessions.erase(clientId);
                auto match = session->currentMatch.lock();
                if (!match)
                {
                    return;
                }

                match->RemoveSession(clientId);

                bool isEmpty = false;
                if (!match->IsRunning())
                {
                    std::lock_guard<std::mutex> pLock(match->playerMutex);
                    std::lock_guard<std::mutex> oLock(match->observerMutex);
                    if (match->players.empty() && match->observers.empty())
                    {
                        isEmpty = true;
                    }
                }

                if (isEmpty)
                {
                    matches.erase(std::remove_if(matches.begin(), matches.end(),
                        [&](auto& m) { return m == match; }), matches.end());
                    std::cout << "[Server] Cleaned up match " << match->GetID() << std::endl;
                }
            });

        receptionTCP.Start();
    }

    std::shared_ptr<Session> FindSessionById(const std::string& id)
    {
        auto it = sessions.find(id);
        return (it != sessions.end()) ? it->second : nullptr;
    }

    void AssignToMatch(std::shared_ptr<Session> s)
    {
        if (s->type == SessionType::Player || s->type == SessionType::Replayer)
        {
            auto openMatch = FindOpenMatch();
            if (!openMatch)
            {
                openMatch = std::make_shared<Match>(nextMatchId++);
                matches.push_back(openMatch);
                openMatch->sendCallback = [this](const std::string& msg, const std::string& cid) { receptionTCP.SendToClient(cid, msg); };
                std::cout << "[Server] Created Match " << openMatch->GetID() << " (P)" << std::endl;
            }
            s->currentMatch = openMatch;
            openMatch->AddPlayer(s);
            if (openMatch->IsReadyToStart())
            {
                openMatch->Start();
            }
        }
        else
        {
            auto firstMatch = FindFirstMatch();
            if (!firstMatch)
            {
                firstMatch = std::make_shared<Match>(nextMatchId++);
                matches.push_back(firstMatch);
                firstMatch->sendCallback = [this](const std::string& msg, const std::string& cid) { receptionTCP.SendToClient(cid, msg); };
                std::cout << "[Server] Created Match " << firstMatch->GetID() << " (O)" << std::endl;
            }
            s->currentMatch = firstMatch;
            firstMatch->AddObserver(s);
        }
    }

    std::shared_ptr<Match> FindOpenMatch()
    {
        for (auto& m : matches)
        {
            if (!m->IsFull() && !m->IsRunning()) return m;
        }
        return nullptr;
    }

    std::shared_ptr<Match> FindFirstMatch()
    {
        return matches.empty() ? nullptr : matches.front();
    }
};

int main()
{
    printf("[Server]\n");

    DroneRacingServer server;
    server.Start();
    std::cout << "Server started. Press Enter to exit...\n";
    std::cin.get();
    server.Stop();
    return 0;
}