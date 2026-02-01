// =============================================================
// Project: DroneRacingClient
// File: main.cpp
// =============================================================

#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <sstream>
#include <string>

using namespace libRxTx;

class DroneRacingClient
{
public:
    DroneRacingClient()
        : beaconUDP(Protocol::UDP), controlMessagesTCP(Protocol::TCP),
        clientRunning_(true)
    {
    }

    void StartBeaconListener()
    {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());

        beaconUDP.OnReceive([this](const std::string& msg, const std::string& senderIp)
            {
                // Ignore if shutting down
                if (!clientRunning_)
                {
                    return;
                }

                size_t pos = msg.find(':');
                if (pos == std::string::npos)
                {
                    return;
                }

                std::string serverIp = msg.substr(pos + 1);

                if (tcpConnecting_ || tcpConnected_)
                {
                    return;
                }
                tcpConnecting_ = true;

                std::thread([this, serverIp]()
                    {
                        // Reuse socket check
                        if (!controlMessagesTCP.Init())
                        {
                            std::cout << "[Client] TCP Init failed" << std::endl;
                            tcpConnecting_ = false;
                            return;
                        }

                        controlMessagesTCP.OnReceive([this](const std::string& msg, const std::string& remoteIp)
                            {
                                std::cout << "[TCP Received] " << msg << std::endl;

                                if (msg.rfind("CLIENT_ID|", 0) == 0)
                                {
                                    clientId_ = msg.substr(10);
                                    std::cout << "[Client] Assigned ID: " << clientId_ << std::endl;

                                    if (!clientId_.empty())
                                    {
                                        controlMessagesTCP.SendString("JOIN|" + clientId_);
                                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                                        controlMessagesTCP.SendString("READY|" + clientId_);
                                        std::cout << "[Client] Sent JOIN and READY for " << clientId_ << std::endl;
                                    }
                                }
                                else if (msg == "START_RACE|")
                                {
                                    std::cout << "[Client] Race started!" << std::endl;
                                    raceStarted_ = true;
                                }
                                else if (msg == "RACE_FINISHED|")
                                {
                                    std::cout << "[Client] Race finished. Shutting down..." << std::endl;
                                    raceStarted_ = false;

                                    // FIX: Prevent Deadlock
                                    // Do not call controlMessagesTCP.Stop() here!
                                    // Do not call beaconUDP.Stop() here!

                                    // Signal termination to the main thread
                                    clientRunning_ = false;
                                }
                            });

                        controlMessagesTCP.OnDisconnect([this](const std::string& remoteIp)
                            {
                                std::cout << "[TCP Disconnected from " << remoteIp << "]" << std::endl;
                                tcpConnected_ = false;
                                tcpConnecting_ = false;
                                raceStarted_ = false;
                                clientId_ = "";
                            });

                        if (controlMessagesTCP.Connect(serverIp, 6000))
                        {
                            tcpConnected_ = true;
                            controlMessagesTCP.Start();
                            std::cout << "[Client] TCP connection established with " << serverIp << std::endl;

                            // beaconUDP.Stop();
                        }
                        else
                        {
                            std::cout << "[Client] TCP connection failed to " << serverIp << std::endl;
                            controlMessagesTCP.Close();
                        }

                        tcpConnecting_ = false;
                    }).detach();
            });

        beaconUDP.Start();
    }

    // Public members are used directly in main()
public:
    RxTx beaconUDP;
    RxTx controlMessagesTCP;
    std::atomic<bool> tcpConnecting_ = false;
    std::atomic<bool> tcpConnected_ = false;
    std::atomic<bool> raceStarted_ = false;
    std::atomic<bool> clientRunning_;
    std::string clientId_;
};

int main()
{
    printf("[Dummy Player]\n");

    DroneRacingClient client;
    client.StartBeaconListener();

    while (client.clientRunning_)
    {
        if (client.tcpConnected_ && client.raceStarted_)
        {
            std::stringstream ss;
            ss << "[" << client.clientId_ << "] THROTTLE_UP";
            client.controlMessagesTCP.SendString(ss.str());
            // std::cout << ss.str() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Client exiting...\n";
}