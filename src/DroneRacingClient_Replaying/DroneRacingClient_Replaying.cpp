// =============================================================
// Project: DroneRacingClient_Replaying
// File: main.cpp
// =============================================================

#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

using namespace libRxTx;

struct ReplayFrame {
    long long timestamp;
    int originalIndex;
    float x, y, z;
    float roll, pitch, yaw;
};

class ReplayClient {
public:
    ReplayClient() : beaconUDP(Protocol::UDP), controlMessagesTCP(Protocol::TCP), clientRunning_(true), raceStarted_(false) {}

    bool LoadReplayFile(const std::string& filename, int targetCsvIndex = 0) {
        std::ifstream file(filename);
        if (!file.is_open()) { std::cerr << "Failed to open: " << filename << "\n"; return false; }
        frames_.clear();
        std::string line;
        std::getline(file, line); // Skip Header
        while (std::getline(file, line)) {
            std::stringstream ss(line);
            std::string val;
            std::vector<std::string> row;
            while (std::getline(ss, val, ',')) row.push_back(val);
            if (row.size() >= 8) {
                int pIndex = std::stoi(row[1]);
                if (pIndex == targetCsvIndex) {
                    ReplayFrame f;
                    f.timestamp = std::stoll(row[0]);
                    f.originalIndex = pIndex;
                    f.x = std::stof(row[2]); f.y = std::stof(row[3]); f.z = std::stof(row[4]);
                    f.roll = std::stof(row[5]); f.pitch = std::stof(row[6]); f.yaw = std::stof(row[7]);
                    frames_.push_back(f);
                }
            }
        }
        file.close();
        std::sort(frames_.begin(), frames_.end(), [](const ReplayFrame& a, const ReplayFrame& b) { return a.timestamp < b.timestamp; });
        return !frames_.empty();
    }

    void StartNetwork() {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());
        beaconUDP.OnReceive([this](const std::string& msg, const std::string&) {
            if (!clientRunning_ || tcpConnecting_ || tcpConnected_) return;
            size_t pos = msg.find(':');
            if (pos == std::string::npos) return;
            std::string serverIp = msg.substr(pos + 1);
            tcpConnecting_ = true;
            std::thread([this, serverIp]() { ConnectAndListen(serverIp); }).detach();
            });
        beaconUDP.Start();
        std::cout << "[Replayer] Waiting for Server Beacon..." << std::endl;
    }

    void UpdateLoop() {
        while (clientRunning_) {
            if (tcpConnected_ && raceStarted_ && myLiveIndex_ != -1 && !frames_.empty()) {
                if (!playbackInitialized_) {
                    playbackStartTime_ = std::chrono::steady_clock::now();
                    recordingStartTime_ = frames_[0].timestamp;
                    currentFrameIdx_ = 0;
                    playbackInitialized_ = true;
                    std::cout << "[Replayer] Streaming started..." << std::endl;
                }
                auto now = std::chrono::steady_clock::now();
                long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - playbackStartTime_).count();

                while (currentFrameIdx_ < frames_.size()) {
                    long long frameRelativeTime = frames_[currentFrameIdx_].timestamp - recordingStartTime_;
                    if (frameRelativeTime <= elapsedMs) {
                        SendFrame(frames_[currentFrameIdx_]);
                        currentFrameIdx_++;
                    }
                    else { break; }
                }
            }
            else { playbackInitialized_ = false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    RxTx beaconUDP;
    RxTx controlMessagesTCP;
    std::atomic<bool> tcpConnecting_ = false;
    std::atomic<bool> tcpConnected_ = false;
    std::atomic<bool> raceStarted_;
    std::atomic<bool> clientRunning_;
    int myLiveIndex_ = -1;

    std::vector<ReplayFrame> frames_;
    size_t currentFrameIdx_ = 0;
    bool playbackInitialized_ = false;
    std::chrono::steady_clock::time_point playbackStartTime_;
    long long recordingStartTime_ = 0;

    void ConnectAndListen(const std::string& serverIp) {
        if (!controlMessagesTCP.Init()) { tcpConnecting_ = false; return; }
        controlMessagesTCP.OnReceive([this](const std::string& msg, const std::string&) {
            if (msg.rfind("CLIENT_ID|", 0) == 0) {
                controlMessagesTCP.SendString("JOIN|replayer");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                controlMessagesTCP.SendString("READY|");
                std::cout << "[Replayer] Joined as Bot." << std::endl;
            }
            else if (msg.rfind("PLAYER_INDEX|", 0) == 0) {
                myLiveIndex_ = std::stoi(msg.substr(13));
                std::cout << "[Replayer] Assigned Index: " << myLiveIndex_ << std::endl;
            }
            else if (msg.rfind("START_RACE|", 0) == 0) { raceStarted_ = true; }
            else if (msg.rfind("RACE_FINISHED|", 0) == 0) { raceStarted_ = false; clientRunning_ = false; }
            });

        if (controlMessagesTCP.Connect(serverIp, 6000)) { tcpConnected_ = true; controlMessagesTCP.Start(); }
        else { controlMessagesTCP.Close(); }
        tcpConnecting_ = false;
    }

    void SendFrame(const ReplayFrame& f) {
        std::stringstream ss;
        ss << "PLAYER_TRANSFORM|" << myLiveIndex_ << "/"
            << f.x << "/" << f.y << "/" << f.z << "/"
            << f.roll << "/" << f.pitch << "/" << f.yaw;
        controlMessagesTCP.SendString(ss.str());
    }
};

int main() {
    printf("[DroneRacingClient_Replaying]\n");

    ReplayClient client;
    std::string filename;
    std::cout << "Replay File (e.g. Replay_2025xxx.csv): ";
    std::cin >> filename;
    if (!client.LoadReplayFile(filename, 0)) return -1; // 0번 플레이어 데이터 로드

    client.StartNetwork();
    client.UpdateLoop();
    return 0;
}