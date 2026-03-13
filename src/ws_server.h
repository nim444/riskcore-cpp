#pragma once
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include "models.h"
#include "risk_engine.h"
#include "data_loader.h"

// Simplified WebSocket server using netcat-style approach for demo
// In production, use a proper WebSocket library like uWebSockets or Beast
class WSServer {
public:
    WSServer(int port = 8080);
    ~WSServer();

    void set_data(std::vector<Position> positions, std::vector<MarketData> market_data);
    void run();
    void stop();

private:
    int port_;
    std::atomic<bool> running_;
    std::vector<Position> positions_;
    std::vector<MarketData> market_data_;
    RiskEngine engine_;
    std::thread server_thread_;

    void server_loop();
};
