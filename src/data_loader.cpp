#include "data_loader.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::vector<Position> DataLoader::load_positions(const std::string& positions_file) {
    std::vector<Position> positions;

    std::ifstream file(positions_file);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open positions file: " + positions_file);
    }

    json data = json::parse(file);

    for (const auto& item : data) {
        Position pos;
        pos.ticker = item["ticker"].get<std::string>();
        pos.side = (item["side"].get<std::string>() == "LONG") ? Side::LONG : Side::SHORT;
        pos.quantity = item["quantity"].get<int>();
        pos.entry_price = item["entry_price"].get<double>();
        pos.current_price = 0.0;  // Will be filled by market data
        positions.push_back(pos);
    }

    return positions;
}

std::vector<MarketData> DataLoader::load_market_data(const std::string& prices_file,
                                                      const std::string& returns_file,
                                                      const std::vector<Position>& positions) {
    // Load returns
    std::map<std::string, std::vector<double>> ticker_returns;
    std::ifstream returns_stream(returns_file);
    if (!returns_stream.is_open()) {
        throw std::runtime_error("Cannot open returns file: " + returns_file);
    }

    std::string line;
    std::getline(returns_stream, line);  // Skip header

    while (std::getline(returns_stream, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string date;
        std::getline(iss, date, ',');

        for (const auto& pos : positions) {
            std::string value_str;
            std::getline(iss, value_str, ',');
            if (value_str.empty() || value_str == "nan") continue;

            double value = std::stod(value_str);
            ticker_returns[pos.ticker].push_back(value);
        }
    }
    returns_stream.close();

    // Load current prices (last row of prices file)
    std::map<std::string, double> current_prices;
    std::ifstream prices_stream(prices_file);
    if (!prices_stream.is_open()) {
        throw std::runtime_error("Cannot open prices file: " + prices_file);
    }

    std::string header;
    std::getline(prices_stream, header);  // Skip header

    std::string last_line;
    while (std::getline(prices_stream, line)) {
        if (!line.empty()) last_line = line;
    }

    if (!last_line.empty()) {
        std::istringstream iss(last_line);
        std::string date;
        std::getline(iss, date, ',');

        for (const auto& pos : positions) {
            std::string value_str;
            std::getline(iss, value_str, ',');
            if (!value_str.empty()) {
                current_prices[pos.ticker] = std::stod(value_str);
            }
        }
    }
    prices_stream.close();

    // Build MarketData vector
    std::vector<MarketData> market_data;
    for (const auto& pos : positions) {
        MarketData md;
        md.ticker = pos.ticker;
        md.daily_returns = ticker_returns[pos.ticker];
        md.current_price = current_prices[pos.ticker];

        // Compute annualised volatility from daily returns
        if (md.daily_returns.size() > 1) {
            double mean = 0.0;
            for (double r : md.daily_returns) mean += r;
            mean /= md.daily_returns.size();

            double var = 0.0;
            for (double r : md.daily_returns) var += (r - mean) * (r - mean);
            var /= (md.daily_returns.size() - 1);

            double daily_vol = std::sqrt(var);
            md.volatility = daily_vol * std::sqrt(252.0);  // Annualised
        } else {
            md.volatility = 0.0;
        }

        market_data.push_back(md);
    }

    return market_data;
}
