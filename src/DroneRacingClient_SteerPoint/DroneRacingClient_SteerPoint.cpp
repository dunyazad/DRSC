// =============================================================
// Project: DroneRacingClient_Hybrid_Offset
// File: main.cpp
// =============================================================

// [Fix] Windows min/max macro conflict prevention (Must be at the top)
#define NOMINMAX

#include "RxTx/RxTx.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

using namespace libRxTx;

// Interpolation Mode Enum
enum class InterpolationMode
{
    Linear,
    CatmullRom
};

// Structure to define a target point
struct SteerPoint
{
    float x, y, z;
    float roll, pitch, yaw;
    float durationToNext; // Time in seconds to reach the next point
};

class SteerPointClient
{
public:
    SteerPointClient()
        : beaconUDP(Protocol::UDP), controlMessagesTCP(Protocol::TCP),
        clientRunning_(true), raceStarted_(false), mode_(InterpolationMode::Linear),
        offsetX_(0.0f), offsetY_(0.0f), offsetZ_(0.0f)
    {
    }

    void SetInterpolationMode(InterpolationMode mode)
    {
        mode_ = mode;
    }

    // [New] Set a common offset for all steer points
    void SetCommonOffset(float x, float y, float z)
    {
        offsetX_ = x;
        offsetY_ = y;
        offsetZ_ = z;
    }

    void AddSteerPoint(float x, float y, float z, float r, float p, float y_angle, float duration)
    {
        SteerPoint sp;
        // [New] Apply offset
        sp.x = x + offsetX_;
        sp.y = y + offsetY_;
        sp.z = z + offsetZ_;

        sp.roll = r;
        sp.pitch = p;
        sp.yaw = y_angle;
        sp.durationToNext = duration;

        waypoints_.push_back(sp);
    }

    void StartNetwork()
    {
        beaconUDP.Init();
        beaconUDP.SetMode(UdpMode::Broadcast);
        beaconUDP.Bind(5000, beaconUDP.GetLocalIP());

        beaconUDP.OnReceive([this](const std::string& msg, const std::string&)
            {
                if (!clientRunning_ || tcpConnecting_ || tcpConnected_)
                {
                    return;
                }

                size_t pos = msg.find(':');
                if (pos == std::string::npos)
                {
                    return;
                }

                std::string serverIp = msg.substr(pos + 1);
                tcpConnecting_ = true;
                std::thread([this, serverIp]() { ConnectAndListen(serverIp); }).detach();
            });

        beaconUDP.Start();
        std::cout << "[SteerClient] Waiting for Server Beacon..." << std::endl;
    }

    void UpdateLoop()
    {
        // 60 FPS transmission rate
        const std::chrono::milliseconds frameDuration(16);

        while (clientRunning_)
        {
            if (tcpConnected_ && raceStarted_ && myLiveIndex_ != -1 && waypoints_.size() >= 2)
            {
                if (!pathInitialized_)
                {
                    currentSegmentIndex_ = 0;
                    segmentStartTime_ = std::chrono::steady_clock::now();
                    pathInitialized_ = true;
                    std::cout << "[SteerClient] Path execution started..." << std::endl;
                }

                // Check bounds
                if (currentSegmentIndex_ >= waypoints_.size() - 1)
                {
                    // Loop path
                    currentSegmentIndex_ = 0;
                    segmentStartTime_ = std::chrono::steady_clock::now();
                }

                // Prepare indices
                // P1: Start of segment, P2: End of segment
                // P0: Previous (for Spline), P3: Next Next (for Spline)
                int idx0 = std::max(0, (int)currentSegmentIndex_ - 1);
                int idx1 = (int)currentSegmentIndex_;
                int idx2 = std::min((int)waypoints_.size() - 1, (int)currentSegmentIndex_ + 1);
                int idx3 = std::min((int)waypoints_.size() - 1, (int)currentSegmentIndex_ + 2);

                SteerPoint& p0 = waypoints_[idx0];
                SteerPoint& p1 = waypoints_[idx1];
                SteerPoint& p2 = waypoints_[idx2];
                SteerPoint& p3 = waypoints_[idx3];

                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<float> elapsed = now - segmentStartTime_;

                // Use P1's duration for the segment between P1 and P2
                float t = elapsed.count() / p1.durationToNext;

                if (t >= 1.0f)
                {
                    // Move to next segment
                    currentSegmentIndex_++;
                    segmentStartTime_ = now;
                    t = 0.0f;
                }

                float curX, curY, curZ;

                // Branch based on selected mode
                if (mode_ == InterpolationMode::CatmullRom)
                {
                    // Spline Interpolation (Smooth)
                    curX = CatmullRom(p0.x, p1.x, p2.x, p3.x, t);
                    curY = CatmullRom(p0.y, p1.y, p2.y, p3.y, t);
                    curZ = CatmullRom(p0.z, p1.z, p2.z, p3.z, t);
                }
                else
                {
                    // Linear Interpolation (Straight lines)
                    curX = Lerp(p1.x, p2.x, t);
                    curY = Lerp(p1.y, p2.y, t);
                    curZ = Lerp(p1.z, p2.z, t);
                }

                // Calculate Rotation using Linear Interpolation (Safer for angles)
                float curRoll = LerpAngle(p1.roll, p2.roll, t);
                float curPitch = LerpAngle(p1.pitch, p2.pitch, t);
                float curYaw = LerpAngle(p1.yaw, p2.yaw, t);

                SendTransform(curX, curY, curZ, curRoll, curPitch, curYaw);

                std::this_thread::sleep_for(frameDuration);
            }
            else
            {
                pathInitialized_ = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

private:
    RxTx beaconUDP;
    RxTx controlMessagesTCP;
    std::atomic<bool> tcpConnecting_ = false;
    std::atomic<bool> tcpConnected_ = false;
    std::atomic<bool> raceStarted_;
    std::atomic<bool> clientRunning_;
    InterpolationMode mode_;
    int myLiveIndex_ = -1;

    // [New] Common offsets
    float offsetX_;
    float offsetY_;
    float offsetZ_;

    std::vector<SteerPoint> waypoints_;
    size_t currentSegmentIndex_ = 0;
    bool pathInitialized_ = false;
    std::chrono::steady_clock::time_point segmentStartTime_;

    // Linear Interpolation
    float Lerp(float a, float b, float t)
    {
        return a + (b - a) * t;
    }

    // Catmull-Rom Spline Interpolation
    float CatmullRom(float p0, float p1, float p2, float p3, float t)
    {
        float t2 = t * t;
        float t3 = t2 * t;

        return 0.5f * ((2.0f * p1) +
            (-p0 + p2) * t +
            (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    // Angle Interpolation (Shortest Path)
    float LerpAngle(float a, float b, float t)
    {
        float diff = b - a;
        // Normalize to -180 ~ 180
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        return a + diff * t;
    }

    void ConnectAndListen(const std::string& serverIp)
    {
        if (!controlMessagesTCP.Init())
        {
            tcpConnecting_ = false;
            return;
        }

        controlMessagesTCP.OnReceive([this](const std::string& msg, const std::string&)
            {
                if (msg.rfind("CLIENT_ID|", 0) == 0)
                {
                    controlMessagesTCP.SendString("JOIN|bot");
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    controlMessagesTCP.SendString("READY|");
                    std::cout << "[SteerClient] Joined as Bot." << std::endl;
                }
                else if (msg.rfind("PLAYER_INDEX|", 0) == 0)
                {
                    myLiveIndex_ = std::stoi(msg.substr(13));
                    std::cout << "[SteerClient] Assigned Index: " << myLiveIndex_ << std::endl;
                }
                else if (msg.rfind("START_RACE|", 0) == 0)
                {
                    raceStarted_ = true;
                }
                else if (msg.rfind("RACE_FINISHED|", 0) == 0)
                {
                    raceStarted_ = false;
                    clientRunning_ = false;
                }
            });

        if (controlMessagesTCP.Connect(serverIp, 6000))
        {
            tcpConnected_ = true;
            controlMessagesTCP.Start();
        }
        else
        {
            controlMessagesTCP.Close();
        }
        tcpConnecting_ = false;
    }

    void SendTransform(float x, float y, float z, float r, float p, float y_angle)
    {
        std::stringstream ss;
        ss << "PLAYER_TRANSFORM|" << myLiveIndex_ << "/"
            << x << "/" << y << "/" << z << "/"
            << r << "/" << p << "/" << y_angle;
        controlMessagesTCP.SendString(ss.str());
    }
};

int main()
{
    printf("[DroneRacingClient_Hybrid_Offset]\n");

    SteerPointClient client;

    // 1. Set Interpolation Mode
    client.SetInterpolationMode(InterpolationMode::Linear);
    std::cout << "Mode set to: Linear\n";

    // 2. [New] Set Common Offset (Change these values to shift the whole path)
    // Example: Shift X by +100.0, Y by -50.0
    client.SetCommonOffset(54510.000000, -38330.000000, 71960.000000);
    
    // 3. Add Steer Points (Offsets will be applied automatically)
    client.AddSteerPoint(698.93f, 91.03f, 1135.99f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(698.93f, 3161.53f, -736.54f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-460.96f, 7621.39f, -212.66f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-617.56f, 9570.83f, -1050.26f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-767.41f, 10450.48f, -1994.24f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-1364.13f, 12616.62f, -749.84f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-2625.31f, 16162.89f, -583.77f, 0.00f, 0.00f, 49.38f + 90.0f, 4.0f);
    client.AddSteerPoint(-13584.35f, 25788.88f, -613.09f, 0.00f, 0.00f, 50.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-14689.46f, 31486.05f, 477.32f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-15524.97f, 33719.77f, 263.55f, 0.00f, 0.00f, 40.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-18197.67f, 37145.18f, 263.55f, 0.00f, 0.00f, -20.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-15899.01f, 40270.93f, 297.74f, 0.00f, 0.00f, -90.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-13540.04f, 39645.58f, 263.55f, 0.00f, 0.00f, -130.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-10119.59f, 35176.82f, 287.92f, 0.00f, 0.00f, -130.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-6679.64f, 33533.63f, 267.39f, 0.00f, 0.00f, -30.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-6679.64f, 35376.04f, 283.70f, 0.00f, 0.00f, 50.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-11031.61f, 39647.61f, 90.44f, 0.00f, 0.00f, 40.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-11982.24f, 41543.23f, 8.38f, 0.00f, 0.00f, 30.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-13537.65f, 43750.35f, -77.21f, 0.00f, 0.00f, 10.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-13584.51f, 48166.01f, -115.96f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-13584.51f, 48851.13f, -122.18f, 0.00f, 0.00f, 0.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-13468.18f, 49752.07f, -126.33f, 0.00f, 0.00f, -10.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-12118.40f, 54281.07f, -260.65f, 0.00f, 0.00f, -20.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-10700.00f, 55246.00f, 147.29f, 0.00f, 0.00f, -30.00f + 90.0f, 4.0f);
    client.AddSteerPoint(-8897.00f, 56495.00f, 1356.20f, 0.00f, 0.00f, -40.00f + 90.0f, 4.0f);

    client.StartNetwork();
    client.UpdateLoop();
    
    return 0;
}