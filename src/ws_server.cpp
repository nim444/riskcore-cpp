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
#include <openssl/sha.h>
#include <sstream>

using json = nlohmann::json;

// Base64 encode for WebSocket handshake
std::string base64_encode(const unsigned char* buf, unsigned int bufLen) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    int val = 0;
    int valb = 0;
    for (unsigned int i = 0; i < bufLen; ++i) {
        val = (val << 8) + buf[i];
        valb += 8;
        while (valb >= 6) {
            valb -= 6;
            ret.push_back(base64_chars[(val >> valb) & 0x3F]);
        }
    }
    if (valb > 0) ret.push_back(base64_chars[(val << (6 - valb)) & 0x3F]);
    while (ret.size() % 4) ret.push_back('=');
    return ret;
}

// SHA1 hash for WebSocket key
std::string sha1_hash(const std::string& input) {
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA_CTX sha1;
    SHA1_Init(&sha1);
    SHA1_Update(&sha1, input.c_str(), input.length());
    SHA1_Final(hash, &sha1);
    return base64_encode(hash, SHA_DIGEST_LENGTH);
}

static json portfolio_result_to_json(const PortfolioResult& result, const std::vector<Position>& positions_data) {
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

        // Find quantity and price from original positions
        for (const auto& orig_pos : positions_data) {
            if (orig_pos.ticker == pos.ticker) {
                p["quantity"] = orig_pos.quantity;
                p["price"] = orig_pos.current_price;
                break;
            }
        }

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

    if (listen(server_fd, 5) < 0) {
        std::cerr << "Listen failed\n";
        close(server_fd);
        return;
    }

    std::cout << "WebSocket server running on ws://localhost:" << port_ << "/stream\n";
    running_ = true;

    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Accept connection with timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int activity = select(server_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (activity <= 0) continue;

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        // Read WebSocket upgrade request
        char buffer[4096] = {0};
        ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            close(client_fd);
            continue;
        }

        std::string request(buffer);

        // Check if request is to /stream path
        if (request.find("GET /stream HTTP") == std::string::npos) {
            close(client_fd);
            continue;
        }

        std::string key;
        size_t key_pos = request.find("Sec-WebSocket-Key: ");
        if (key_pos != std::string::npos) {
            key_pos += 19;
            size_t key_end = request.find("\r\n", key_pos);
            key = request.substr(key_pos, key_end - key_pos);
        }

        // Send WebSocket upgrade response
        if (!key.empty()) {
            std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
            std::string accept_key = sha1_hash(key + magic);

            std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                   "Upgrade: websocket\r\n"
                                   "Connection: Upgrade\r\n"
                                   "Sec-WebSocket-Accept: " + accept_key + "\r\n\r\n";
            send(client_fd, response.c_str(), response.length(), 0);

            // Send data every 2 seconds
            auto last_send = std::chrono::steady_clock::now();
            while (running_) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_send).count() >= 2000) {
                    if (!positions_.empty()) {
                        PortfolioResult result = engine_.compute(positions_, market_data_);
                        json data = portfolio_result_to_json(result, positions_);
                        std::string json_str = data.dump();

                        // WebSocket frame: 0x81 (FIN + TEXT), payload length, payload
                        unsigned char frame[50000];
                        int frame_size = 0;
                        frame[frame_size++] = 0x81;  // FIN + TEXT

                        if (json_str.length() < 126) {
                            frame[frame_size++] = json_str.length();
                        } else if (json_str.length() < 65536) {
                            frame[frame_size++] = 126;
                            frame[frame_size++] = (json_str.length() >> 8) & 0xFF;
                            frame[frame_size++] = json_str.length() & 0xFF;
                        } else {
                            frame[frame_size++] = 127;
                            for (int i = 7; i >= 0; --i) {
                                frame[frame_size++] = (json_str.length() >> (i * 8)) & 0xFF;
                            }
                        }

                        std::memcpy(&frame[frame_size], json_str.c_str(), json_str.length());
                        frame_size += json_str.length();
                        send(client_fd, frame, frame_size, 0);
                    }
                    last_send = now;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        close(client_fd);
    }

    close(server_fd);
}

void WSServer::run() {
    server_thread_ = std::thread(&WSServer::server_loop, this);
    server_thread_.join();
}

void WSServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}
