#include "RxTx/RxTx.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace libRxTx;

class DroneRacingServer;

struct PlayerTransform {
    float x, y, z;
    float roll, pitch, yaw;
};

enum class SessionType {
    Observer,
    Player
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

        //sendCallback("PLAYER_INDEX|" + std::to_string(players.size() - 1), s->clientId);

		playerTransforms.push_back({ 0,0,0,0,0,0 }); // 초기화된
		playerScores.push_back(0);
    }

    void AddObserver(std::shared_ptr<Session> s) {
        std::lock_guard<std::mutex> lock(observerMutex);
        observers.push_back(s);
        std::cout << "[Match " << matchId << "] Added observer: "
            << (s->playerName.empty() ? "(unknown)" : s->playerName)
            << " (" << s->clientId << ")" << std::endl;

        //sendCallback("PLAYER_INDEX|" + std::to_string(players.size() - 1), s->clientId);
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

    void Loop()
    {
        std::chrono::steady_clock::time_point lastTime = std::chrono::steady_clock::now();
        std::chrono::nanoseconds ellapsedTimeCountDown(0);
        std::chrono::nanoseconds ellapsedTimeCountSending(0);

        // Constants for time comparison (Nanoseconds)
        // 1 Second = 1,000,000,000 ns
        const long long TIME_THRESHOLD_COUNTDOWN = 1000000000;
        // 16 ms = 16,000,000 ns (approx 60 Hz)
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

                // Check if accumulated time exceeds 16ms
                if (ellapsedTimeCountSending.count() >= TIME_THRESHOLD_SENDING)
                {
                    {
                        std::string msg = "PLAYER_TRANSFORM_UPDATE|";
                        bool hasData = false;

                        {
                            std::lock_guard<std::mutex> lock(playerMutex);
                            size_t pSize = playerTransforms.size();

                            for (size_t i = 0; i < pSize; i++)
                            {
                                const auto& transform = playerTransforms[i];

                                msg += std::to_string(i) + ","
                                    + std::to_string(transform.x) + ","
                                    + std::to_string(transform.y) + ","
                                    + std::to_string(transform.z) + ","
                                    + std::to_string(transform.roll) + ","
                                    + std::to_string(transform.pitch) + ","
                                    + std::to_string(transform.yaw);

                                if (i != pSize - 1)
                                {
                                    msg += "/";
                                }
                            }
                            hasData = (pSize > 0);
                        }

                        if (hasData)
                        {
                            {
                                std::lock_guard<std::mutex> lock(playerMutex);
                                for (auto& p : players)
                                {
                                    if (p && sendCallback)
                                    {
                                        sendCallback(msg, p->clientId);
                                    }
                                }
                            }

                            {
                                std::lock_guard<std::mutex> lock(observerMutex);
                                for (auto& p : observers)
                                {
                                    if (p && sendCallback)
                                    {
                                        sendCallback(msg, p->clientId);
                                    }
                                }
                            }
                        }
                    }
                    {
                        std::string msg = "PLAYER_RANKING_UPDATE|";
                        bool hasData = false;

                        std::vector<std::tuple<unsigned int, unsigned int>> scores;
                        {
                            std::lock_guard<std::mutex> lock(playerMutex);
                            size_t pSize = playerScores.size();

                            for (size_t i = 0; i < pSize; i++)
                                scores.emplace_back(i, playerScores[i]);

                            std::sort(scores.begin(), scores.end(),
                                [](const auto& a, const auto& b) {
                                    return std::get<1>(a) > std::get<1>(b);
                                });

                            std::vector<unsigned int> scorePerPlayerIndex(pSize, 0);

                            unsigned int currentRank = 1;

                            for (size_t i = 0; i < scores.size(); ++i)
                            {
                                const auto& [playerIndex, score] = scores[i];

                                if (i > 0)
                                {
                                    const auto& [prevPlayerIndex, prevScore] = scores[i - 1];

                                    if (score == prevScore)
                                    {
                                        scorePerPlayerIndex[playerIndex] = scorePerPlayerIndex[prevPlayerIndex];
                                    }
                                    else
                                    {
                                        scorePerPlayerIndex[playerIndex] = currentRank;
                                    }
                                }
                                else
                                {
                                    scorePerPlayerIndex[playerIndex] = currentRank;
                                }

                                currentRank++;
                            }

                            scores.clear();
                            for (size_t i = 0; i < pSize; i++)
                                scores.emplace_back(i, scorePerPlayerIndex[i]);
                        }

                        {
                            std::lock_guard<std::mutex> lock(playerMutex);
                            size_t pSize = playerScores.size();
                            for (size_t i = 0; i < pSize; i++)
                            {
                                const auto& [playerIndex, rank] = scores[i];
                                msg += std::to_string(playerIndex) + ","
                                    + std::to_string(rank);
                                if (i != pSize - 1)
                                {
                                    msg += "/";
                                }
							}
							hasData = (pSize > 0);
                        }

                        if (hasData)
                        {
                            {
                                std::lock_guard<std::mutex> lock(playerMutex);
                                for (auto& p : players)
                                {
                                    if (p && sendCallback)
                                    {
                                        sendCallback(msg, p->clientId);
                                    }
                                }
                            }
                            {
                                //std::lock_guard<std::mutex> lock(observerMutex);
                                //for (auto& p : observers)
                                //{
                                //    if (p && sendCallback)
                                //    {
                                //        sendCallback(msg, p->clientId);
                                //    }
                                //}
                            }
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
                    std::cout << "[Match " << matchId << "] Countdown: " << countdown << " seconds remaining.\n";

                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        for (auto& p : players)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback("COUNT_DOWN|" + std::to_string(countdown), p->clientId);
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        for (auto& p : observers)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback("COUNT_DOWN|" + std::to_string(countdown), p->clientId);
                            }
                        }
                    }
                    countdown--;
                    continue;
                }
                else if (countdown == -1)
                {
                    raceStarted = true;
                    startTime = std::chrono::steady_clock::now();

                    ellapsedTimeCountSending = std::chrono::nanoseconds(0);

					std::stringstream ss;
					ss << "START_RACE|" << players.size();

                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        for (auto& p : players)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback(ss.str(), p->clientId);
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        for (auto& p : observers)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback(ss.str(), p->clientId);
                            }
                        }
                    }
                    countdown--;
                }
                else if (countdown == -2)
                {
                    auto raceDelta = std::chrono::steady_clock::now() - startTime;
                    playTime = std::chrono::duration_cast<std::chrono::milliseconds>(raceDelta).count();

                    {
                        std::lock_guard<std::mutex> lock(playerMutex);
                        for (auto& p : players)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback("PLAY_TIME|" + std::to_string(playTime), p->clientId);
                            }
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(observerMutex);
                        for (auto& p : observers)
                        {
                            if (p && sendCallback)
                            {
                                sendCallback("PLAY_TIME|" + std::to_string(playTime), p->clientId);
                            }
                        }
                    }
                }
            }

            int playerCount = 0;
            {
                std::lock_guard<std::mutex> lock(playerMutex);
                playerCount = (int)players.size();
            }

            if (playerCount <= 1)
            {
                if (running && countdown < 0)
                {
                    std::cout << "[Match " << matchId << "] Auto-stop: " << playerCount << " player(s) left.\n";
                    // Stop(); 
                    running = false;
                }
            }
        }
        std::cout << "[Match " << matchId << "] Loop exited.\n";
    }

    void Stop() {
        std::cout << "[Match " << matchId << "] Stopping..." << std::endl;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players) {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending RACE_FINISHED to " << p->clientId << std::endl;
                        sendCallback("RACE_FINISHED|", p->clientId);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(observerMutex);
            for (auto& p : observers) {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending RACE_FINISHED to " << p->clientId << std::endl;
                        sendCallback("RACE_FINISHED|", p->clientId);
                    }
                }
            }
        }

        if (!running.exchange(false)) {
            return;
        }
    }

    bool IsRunning() const { return running; }

    bool IsFull() const {
        std::lock_guard<std::mutex> lock(playerMutex);
        return players.size() >= 2;
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

    void SetPlayerTransform(int playerIndex, const PlayerTransform& transform)
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (playerIndex >= 0 && playerIndex < playerTransforms.size())
        {
            playerTransforms[playerIndex] = transform;
        }
	}

    void SetPlayerScore(int playerIndex, unsigned int score)
    {
        std::lock_guard<std::mutex> lock(playerMutex);
        if (playerIndex >= 0 && playerIndex < playerScores.size())
        {
            playerScores[playerIndex] = score;
        }

        if (26 <= score)
        {
			PropagatePlayerWin(playerIndex);
        }
    }

    void PropagatePlayerWin(int winnerIndex)
    {
        raceStarted = false;

        std::stringstream ss;
        ss << "PLAYER_WIN|" << winnerIndex;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players)
            {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending PLAYER_WIN(" << winnerIndex
                            << ") to " << p->clientId << std::endl;
                        sendCallback(ss.str(), p->clientId);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(observerMutex);
            for (auto& p : observers)
            {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending PLAYER_WIN(" << winnerIndex
                            << ") to " << p->clientId << std::endl;
                        sendCallback(ss.str(), p->clientId);
                    }
                }
            }
        }
    }

    void PropagatePlayerCrashed(int crashedPlayerIndex)
    {
        raceStarted = false;

		std::stringstream ss;
		ss << "PLAYER_CRASHED|" << crashedPlayerIndex;

        {
            std::lock_guard<std::mutex> lock(playerMutex);
            for (auto& p : players)
            {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending PLAYER_CRASHED(" << crashedPlayerIndex
                            << ") to " << p->clientId << std::endl;
                        sendCallback(ss.str(), p->clientId);
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(observerMutex);
            for (auto& p : observers)
            {
                if (p) {
                    if (sendCallback) {
                        std::cout << "[Match " << matchId << "] Sending PLAYER_CRASHED(" << crashedPlayerIndex
                            << ") to " << p->clientId << std::endl;
                        sendCallback(ss.str(), p->clientId);
                    }
                }
            }
        }
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

            std::cout << "[TCP] Connected: " << ip << " -> ClientID=" << session->clientId << std::endl;

            return "CLIENT_ID|" + session->clientId;
            });

        receptionTCP.OnReceive([this](const std::string& msg, const std::string& clientId) {
            std::lock_guard<std::mutex> lock(mtx);

            auto session = FindSessionById(clientId);
            if (!session) {
                std::cout << "[TCP] Received from unknown ID: " << clientId << std::endl;
                return;
            }

            //std::cout << "[TCP " << clientId << "] " << msg << std::endl;

            if (msg.rfind("JOIN|", 0) == 0) {
                session->playerName = clientId;

				auto remaining = msg.substr(5);

                std::istringstream iss(remaining);
				std::string typeStr;
                iss >> typeStr;
                std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::tolower);
                if (typeStr == "observer")
                {
                    session->type = SessionType::Observer;
                }
                else
                {
                    session->type = SessionType::Player;
                }
                AssignToMatch(session);

                {
                    std::stringstream ss;
                    ss << "JOINED_MATCH|" << session->currentMatch.lock()->GetID();
                    receptionTCP.SendToClient(clientId, ss.str());
                }

                {
                    std::stringstream ss;
					ss << "PLAYER_INDEX|" << session->currentMatch.lock()->players.size() - 1;
                    receptionTCP.SendToClient(clientId, ss.str());
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
            else if (msg.rfind("PLAYER_TRANSFORM|", 0) == 0)
            {
                auto match = session->currentMatch.lock();
                if (match)
                {
                    // 1. 헤더("PLAYER_TRANSFORM|") 길이인 17글자 이후부터 파싱 시작
                    std::istringstream iss(msg.substr(17));

                    int playerIndex = -1;
                    PlayerTransform transform;
                    char sep; // 구분자(_, /, | 등)를 흡수할 임시 변수

                    iss >> playerIndex >> sep
                        >> transform.x >> sep
                        >> transform.y >> sep
                        >> transform.z >> sep
                        >> transform.roll >> sep
                        >> transform.pitch >> sep
                        >> transform.yaw;

                    // 2. 스트림 파싱
                    // 포맷 가정: Index(구분자)X(구분자)Y(구분자)Z(구분자)Roll(구분자)Pitch(구분자)Yaw
                    // 예: "0/100.5_200.5_300.5/0.0_90.0_0.0"
                    {
                        // 3. 데이터 업데이트
                        match->SetPlayerTransform(playerIndex, transform);

                        // 디버깅이 필요한 경우 아래 주석 해제
                        /*
                        std::cout << "[Match " << match->GetID() << "] Player " << playerIndex
                                  << " Transform Updated: "
                                  << transform.x << ", " << transform.y << ", " << transform.z
                                  << std::endl;
                        */
                    }
                }
            }
            else if (msg.rfind("PLAYER_SCORE|", 0) == 0)
            {
                auto match = session->currentMatch.lock();
                if (match)
                {
                    std::istringstream iss(msg.substr(13));
                    int playerIndex = -1;
                    unsigned int score = 0;
                    char sep; // 구분자(_, /, | 등)를 흡수할 임시 변수
                    iss >> playerIndex >> sep >> score;
                    {
                        match->SetPlayerScore(playerIndex, score);
                        // 디버깅이 필요한 경우 아래 주석 해제
                        /*
                        std::cout << "[Match " << match->GetID() << "] Player " << playerIndex
                                  << " Score Updated: " << score
                                  << std::endl;
                        */
					}
                }
            }
            else if (msg.rfind("PLAYER_CRASHED|", 0) == 0)
            {
                auto match = session->currentMatch.lock();
                if (match)
                {
                    std::istringstream iss(msg.substr(15));
                    int playerIndex = -1;
                    char sep; // 구분자(_, /, | 등)를 흡수할 임시 변수
                    iss >> playerIndex;
					match->PropagatePlayerCrashed(playerIndex);
                    {
                        std::cout << "[Match " << match->GetID() << "] Player " << playerIndex
                                  << " has crashed."
                                  << std::endl;
					}
                    
                    match->Stop();
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
                {
                    std::lock_guard<std::mutex> lock(match->observerMutex);
                    auto& observers = match->observers;
                    auto it = std::remove_if(observers.begin(), observers.end(),
                        [&](const std::shared_ptr<Session>& p) {
                            return p && p->clientId == clientId;
                        });

                    if (it != observers.end()) {
                        observers.erase(it, observers.end());
                        wasRemoved = true;
                    }
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

					//match->Stop();

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
        if (SessionType::Player == s->type)
        {
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
        else if(SessionType::Observer == s->type)
        {
            auto firstMatch = FindFirstMatch();
            if (nullptr == firstMatch)
            {
                firstMatch = std::make_shared<Match>(nextMatchId++);
                matches.push_back(firstMatch);
                firstMatch->sendCallback = [this](const std::string& msg, const std::string& clientId) {
                    receptionTCP.SendToClient(clientId, msg);
                    };

                std::cout << "[Server] Created new match " << firstMatch->GetID() << std::endl;
            }
            
            s->currentMatch = firstMatch;

            firstMatch->AddObserver(s);
		}
    }

    std::shared_ptr<Match> FindOpenMatch() {
        for (auto& m : matches) {
            if (!m->IsFull() && !m->IsRunning())
                return m;
        }
        return nullptr;
    }

    std::shared_ptr<Match> FindFirstMatch() {
        if(matches.empty())
			return nullptr;
        else
			return matches.front();
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