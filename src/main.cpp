#include <iostream>
#include <string>
#include <vector>
#include "models.h"
#include "data_loader.h"
#include "risk_engine.h"
#include "ws_server.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [--run|--serve|--version]\n"
              << "  --run     Compute risk once and exit (default)\n"
              << "  --serve   Start WebSocket server on ws://localhost:8080\n"
              << "  --version Print version\n";
}

json portfolio_result_to_json(const PortfolioResult& result) {
    json j;
    j["timestamp"] = result.timestamp;
    j["calc_time_ms"] = result.calc_time_ms;
    j["portfolio_var"] = result.portfolio_var;
    j["portfolio_sharpe"] = result.portfolio_sharpe;
    j["net_exposure"] = result.net_exposure;
    j["last_update_desc"] = result.last_update_desc;

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

int main(int argc, char* argv[]) {
    std::string mode = "run";

    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--run") {
            mode = "run";
        } else if (arg == "--serve") {
            mode = "serve";
        } else if (arg == "--version") {
            std::cout << "riskcore v0.1.0\n";
            return 0;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    try {
        std::cout << "Loading market data..." << std::endl;
        DataLoader loader;
        auto positions = loader.load_positions("data/positions.json");
        auto market_data =
            loader.load_market_data("data/prices.csv", "data/returns.csv", positions);

        std::cout << "Loaded " << positions.size() << " positions and "
                  << market_data.size() << " market data series\n";

        if (mode == "run") {
            std::cout << "Computing risk metrics..." << std::endl;
            RiskEngine engine;
            PortfolioResult result = engine.compute(positions, market_data);

            json output = portfolio_result_to_json(result);
            std::cout << output.dump(2) << std::endl;
        } else if (mode == "serve") {
            std::cout << "Starting WebSocket server..." << std::endl;
            WSServer server(8080);
            server.set_data(positions, market_data);
            server.run();
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
