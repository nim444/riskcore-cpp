#include "ws_server.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>

using json = nlohmann::json;

static json portfolio_result_to_json(const PortfolioResult& result) {
    json j;
    j["timestamp"] = result.timestamp;
    j["calc_time_ms"] = result.calc_time_ms;
    j["portfolio_var"] = result.portfolio_var;
    j["portfolio_sharpe"] = result.portfolio_sharpe;
    j["net_exposure"] = result.net_exposure;

    json positions = json::array();
    for (const auto& pos : result.positions) {
        json p;
        p["ticker"] = pos.ticker;
        p["side"] = (pos.side == Side::LONG) ? "LONG" : "SHORT";
        p["var_95"] = pos.var_95;
        p["sharpe"] = pos.sharpe_ratio;
        p["pnl"] = pos.pnl;

        json greeks;
        greeks["delta"] = pos.greeks.delta;
        greeks["gamma"] = pos.greeks.gamma;
        greeks["vega"] = pos.greeks.vega;
        greeks["theta"] = pos.greeks.theta;
        greeks["iv"] = pos.greeks.implied_vol;
        p["greeks"] = greeks;

        positions.push_back(p);
    }
    j["positions"] = positions;
    j["correlation"] = result.correlation_matrix;

    return j;
}

WSServer::WSServer(int port) : port_(port), running_(false) {}

WSServer::~WSServer() { stop(); }

void WSServer::set_data(std::vector<Position> positions,
                        std::vector<MarketData> market_data) {
    positions_ = positions;
    market_data_ = market_data;
}

void WSServer::server_loop() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Failed to create socket\n";
        return;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt failed\n";
        close(server_fd);
        return;
    }

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Bind failed\n";
        close(server_fd);
        return;
    }

    if (listen(server_fd, 1) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return;
    }

    std::cout << "WebSocket-like server running on ws://localhost:" << port_
              << "/stream\n";

    while (running_) {
        // Simple demo: compute and print every 2 seconds
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (!positions_.empty()) {
            PortfolioResult result = engine_.compute(positions_, market_data_);
            json data = portfolio_result_to_json(result);
            std::cout << "Updated: " << data.dump() << "\n";
        }
    }

    close(server_fd);
}

void WSServer::run() {
    running_ = true;
    server_thread_ = std::thread(&WSServer::server_loop, this);
    server_thread_.join();
}

void WSServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}
