// =============================================================
// Project: DroneRacingClient_Recording
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
#include <queue>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <functional> // Added for std::function

using namespace libRxTx;

struct ReplayFrame {
    long long timestamp;
    int playerIndex;
    float x, y, z;
    float roll, pitch, yaw;
};

class RecorderClient {
public:
    RecorderClient() : beaconUDP(Protocol::UDP), controlMessagesTCP(Protocol::TCP), clientRunning_(true), isRecording_(false) {}

    ~RecorderClient()
    {
        // 1. Stop logic first
        clientRunning_ = false;

        // 2. Stop Networking
        beaconUDP.Stop();
        controlMessagesTCP.Stop();

        // 3. Stop Recording Thread
        StopRecording();

        // 4. Join Connection Thread
        if (connectionThread_.joinable())
        {
            connectionThread_.join();
        }
    }

    void Start() { StartBeaconListener(); }
    bool IsRunning() const { return clientRunning_; }

private:
    RxTx beaconUDP;
    RxTx controlMessagesTCP;
    std::atomic<bool> tcpConnecting_ = false;
    std::atomic<bool> tcpConnected_ = false;
    std::atomic<bool> clientRunning_;

    // Thread management for connection
    std::thread connectionThread_;

    std::thread recordThread_;
    std::queue<ReplayFrame> recordQueue_;
    std::mutex recordMutex_;
    std::condition_variable recordCv_;
    std::atomic<bool> isRecording_;
    std::ofstream recordFile_;

    // Mutex for time operations (localtime is not thread-safe)
    std::mutex timeMutex_;

    void StartBeaconListener() {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());

        beaconUDP.OnReceive([this](const std::string& msg, const std::string&) {
            if (!clientRunning_ || tcpConnecting_ || tcpConnected_) return;

            size_t pos = msg.find(':');
            if (pos == std::string::npos) return;

            std::string serverIp = msg.substr(pos + 1);

            // Prevent multiple connection attempts
            if (!tcpConnecting_.exchange(true))
            {
                // [Fix] Do not use detach(). Manage thread lifecycle.
                if (connectionThread_.joinable()) connectionThread_.join();
                connectionThread_ = std::thread([this, serverIp]() { ConnectToServer(serverIp); });
            }
            });

        beaconUDP.Start();
        std::cout << "[Recorder] Waiting for Server Beacon..." << std::endl;
    }

    void ConnectToServer(const std::string& serverIp) {
        if (!controlMessagesTCP.Init()) { tcpConnecting_ = false; return; }

        controlMessagesTCP.OnReceive([this](const std::string& msg, const std::string&) {
            // [Note] In a real environment, packet buffering (sticky packet handling) is required here as well.
            if (msg.rfind("PLAYER_TRANSFORM_UPDATE|", 0) == 0) {
                ParseAndEnqueue(msg.substr(24));
            }
            else if (msg.rfind("CLIENT_ID|", 0) == 0) {
                controlMessagesTCP.SendString("JOIN|recorder"); // Connect as Recorder type
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                controlMessagesTCP.SendString("READY|");
                std::cout << "[Recorder] Connected and ready." << std::endl;
            }
            else if (msg.rfind("START_RACE|", 0) == 0) {
                std::cout << "[Recorder] Race Started. Recording..." << std::endl;
                StartRecording();
            }
            else if (msg.rfind("RACE_FINISHED|", 0) == 0) {
                std::cout << "[Recorder] Race Finished. Saved." << std::endl;
                StopRecording();
                // Optional: Keep running to record next match? 
                // Currently set to exit app after one race.
                clientRunning_ = false;
            }
            });

        controlMessagesTCP.OnDisconnect([this](const std::string&) {
            tcpConnected_ = false;
            StopRecording();
            clientRunning_ = false;
            });

        if (controlMessagesTCP.Connect(serverIp, 6000)) {
            tcpConnected_ = true;
            // Stop UDP beacon once connected to save resources
            beaconUDP.Stop();
            controlMessagesTCP.Start();
        }
        else {
            controlMessagesTCP.Close();
            tcpConnecting_ = false; // Allow retry if connection failed
        }
    }

    void StartRecording() {
        if (isRecording_) return;

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;

        // [Fix] Thread-safe time formatting
        {
            std::lock_guard<std::mutex> lock(timeMutex_);
#ifdef _MSC_VER
            struct tm buf;
            localtime_s(&buf, &in_time_t);
            ss << std::put_time(&buf, "Replay_%Y%m%d_%H%M%S.csv");
#else
            ss << std::put_time(std::localtime(&in_time_t), "Replay_%Y%m%d_%H%M%S.csv");
#endif
        }

        recordFile_.open(ss.str(), std::ios::out | std::ios::trunc);
        if (recordFile_.is_open()) recordFile_ << "Timestamp,PlayerIndex,X,Y,Z,Roll,Pitch,Yaw\n";

        isRecording_ = true;
        if (recordThread_.joinable()) recordThread_.join();
        recordThread_ = std::thread(&RecorderClient::RecordWorker, this);
    }

    void StopRecording() {
        if (!isRecording_.exchange(false)) return; // Use exchange for atomic check-and-set

        recordCv_.notify_all();
        if (recordThread_.joinable()) recordThread_.join();
        if (recordFile_.is_open()) recordFile_.close();
    }

    void ParseAndEnqueue(const std::string& payload) {
        if (!isRecording_) return;
        long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::vector<ReplayFrame> frames;
        std::stringstream ss(payload);
        std::string segment;

        // Parsing "ID,X,Y,Z,R,P,Y/ID,X,Y,Z..."
        while (std::getline(ss, segment, '/')) {
            std::stringstream ssData(segment);
            std::string val;
            std::vector<std::string> values;
            while (std::getline(ssData, val, ',')) values.push_back(val);

            if (values.size() >= 7) {
                ReplayFrame f;
                f.timestamp = now;
                try {
                    f.playerIndex = std::stoi(values[0]);
                    f.x = std::stof(values[1]); f.y = std::stof(values[2]); f.z = std::stof(values[3]);
                    f.roll = std::stof(values[4]); f.pitch = std::stof(values[5]); f.yaw = std::stof(values[6]);
                    frames.push_back(f);
                }
                catch (...) {
                    // Ignore parsing errors
                }
            }
        }

        if (!frames.empty())
        {
            std::lock_guard<std::mutex> lock(recordMutex_);
            for (const auto& f : frames) recordQueue_.push(f);
            recordCv_.notify_one();
        }
    }

    void RecordWorker() {
        while (true) {
            std::unique_lock<std::mutex> lock(recordMutex_);
            recordCv_.wait(lock, [this]() { return !recordQueue_.empty() || !isRecording_; });

            if (recordQueue_.empty() && !isRecording_) break;

            std::queue<ReplayFrame> temp;
            temp.swap(recordQueue_);
            lock.unlock();

            if (recordFile_.is_open()) {
                while (!temp.empty()) {
                    const auto& f = temp.front();
                    recordFile_ << f.timestamp << "," << f.playerIndex << ","
                        << f.x << "," << f.y << "," << f.z << ","
                        << f.roll << "," << f.pitch << "," << f.yaw << "\n";
                    temp.pop();
                }
            }
        }
    }
};

int main() {
    printf("[DroneRacingClient_Recording]\n");

    RecorderClient client;
    client.Start();

    // Simple main loop
    while (client.IsRunning()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}