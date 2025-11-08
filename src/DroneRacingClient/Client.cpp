#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <sstream>
#include <string>

using namespace libRxTx;

class DroneRacingClient {
public:
    DroneRacingClient()
        : beaconUDP(Protocol::UDP), controlMessagesTCP(Protocol::TCP),
        clientRunning_(true) // ★ FIX: 클라이언트 실행 플래그
    {
    }

    void StartBeaconListener() {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());

        beaconUDP.OnReceive([this](const std::string& msg, const std::string& senderIp) {
            if (!clientRunning_) return; // 종료 중이면 무시

            size_t pos = msg.find(':');
            if (pos == std::string::npos) return;

            std::string serverIp = msg.substr(pos + 1);

            // ★ FIX: 이 플래그가 재연결을 막아줌
            if (tcpConnecting_ || tcpConnected_) return;
            tcpConnecting_ = true;

            std::thread([this, serverIp]() {
                if (!controlMessagesTCP.Init()) { // 소켓 재사용
                    std::cout << "[Client] TCP Init failed" << std::endl;
                    tcpConnecting_ = false;
                    return;
                }

                controlMessagesTCP.OnReceive([this](const std::string& msg, const std::string& remoteIp) {
                    std::cout << "[TCP Received] " << msg << std::endl;

                    if (msg.rfind("CLIENT_ID:", 0) == 0) {
                        clientId_ = msg.substr(10);
                        std::cout << "[Client] Assigned ID: " << clientId_ << std::endl;

                        if (!clientId_.empty()) {
                            controlMessagesTCP.SendString("JOIN:" + clientId_);
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                            controlMessagesTCP.SendString("READY:" + clientId_);
                            std::cout << "[Client] Sent JOIN and READY for " << clientId_ << std::endl;
                        }
                    }
                    else if (msg == "START_RACE") {
                        std::cout << "[Client] Race started!" << std::endl;
                        raceStarted_ = true;
                    }
                    else if (msg == "RACE_FINISHED") {
                        std::cout << "[Client] Race finished. Shutting down..." << std::endl;
                        raceStarted_ = false;

                        // ★ FIX: 데드락 방지
                        // controlMessagesTCP.Stop(); // 여기서 호출 금지!
                        // beaconUDP.Stop();          // 여기서 호출 금지!
                        clientRunning_ = false;    // 메인 스레드에 종료 신호
                    }
                });

                // ★ FIX: 재연결 로직
                controlMessagesTCP.OnDisconnect([this](const std::string& remoteIp) {
                    std::cout << "[TCP Disconnected from " << remoteIp << "]" << std::endl;
                    tcpConnected_ = false;
                    tcpConnecting_ = false;
                    raceStarted_ = false;
                    clientId_ = "";
                    // beaconUDP가 계속 실행 중이므로, OnReceive가 다시 서버를 찾을 것임
                    });

                // TCP 연결 시도
                if (controlMessagesTCP.Connect(serverIp, 6000)) {
                    tcpConnected_ = true;
                    controlMessagesTCP.Start();
                    std::cout << "[Client] TCP connection established with " << serverIp << std::endl;

                    // ★ FIX: 비콘을 멈추지 않음! (재연결을 위해)
                    // beaconUDP.Stop();
                }
                else {
                    std::cout << "[Client] TCP connection failed to " << serverIp << std::endl;
                    // ★ FIX: 실패 시 Stop() 호출 (소켓 정리)
                    controlMessagesTCP.Close();
                }

                tcpConnecting_ = false;
                }).detach(); // 연결 스레드는 분리
            });

        beaconUDP.Start();
    }

    //private:
    RxTx beaconUDP;
    RxTx controlMessagesTCP;
    std::atomic<bool> tcpConnecting_ = false;
    std::atomic<bool> tcpConnected_ = false;
    std::atomic<bool> raceStarted_ = false;
    std::atomic<bool> clientRunning_; // ★ FIX
    std::string clientId_;
};

int main() {
    DroneRacingClient client;
    client.StartBeaconListener();

    while (client.clientRunning_) { // ★ FIX
        if (client.tcpConnected_ && client.raceStarted_)
        {
            std::stringstream ss;
            ss << "[" << client.clientId_ << "] THROTTLE_UP";
            // client.controlMessagesTCP.SendString(ss.str());
            // std::cout << ss.str() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Client exiting...\n";
}