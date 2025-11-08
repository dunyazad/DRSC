#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>

using namespace libRxTx;

int main() {
    // UDP Receiver
    RxTx udp(Protocol::UDP);
    udp.Init();
    udp.SetMode(UdpMode::Broadcast);
    udp.Bind(5000); // 자동 NIC 선택
    udp.OnReceive([](const std::string& msg, const std::string& ip) {
        std::cout << "[UDP Received from " << ip << "] " << msg << std::endl;
        });
    udp.Start();

    // UDP Sender
    RxTx sender(Protocol::UDP);
    sender.Init();
    sender.SetMode(UdpMode::Broadcast);
    sender.Bind(0); // 자동 포트 선택
    sender.Start();
    sender.SendToAll("Hello from SendToAll!", 5000);

    // TCP Server
    RxTx server(Protocol::TCP);
    server.Init();
    server.Bind(9000);
    server.Listen();

    // OnConnect는 std::string 반환해야 함
    server.OnConnect([](const std::string& ip) -> std::string {
        std::cout << "[TCP] Connect: " << ip << std::endl;
        // 서버는 클라이언트에 초기 메시지 보낼 수도 있음
        return "WELCOME:" + ip;
        });

    server.OnReceive([](const std::string& msg, const std::string& ip) {
        std::cout << "[TCP] Received from " << ip << ": " << msg << std::endl;
        });

    server.OnDisconnect([](const std::string& ip) {
        std::cout << "[TCP] Disconnect: " << ip << std::endl;
        });

    server.Start();

    // TCP Client
    RxTx client(Protocol::TCP);
    client.Init();
    client.Connect("127.0.0.1", 9000);

    client.OnReceive([](const std::string& msg, const std::string& ip) {
        std::cout << "[TCP-Client] Received from server: " << msg << std::endl;
        });

    client.Start();

    // UDP broadcast test
    udp.SendPacket("Hello from SendPacket!", "255.255.255.255", 5000);

    std::this_thread::sleep_for(std::chrono::seconds(3));

    udp.Stop();
    sender.Stop();
    server.Stop();
    client.Stop();

    return 0;
}
