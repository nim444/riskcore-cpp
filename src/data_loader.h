#pragma once
#include <vector>
#include <string>
#include "models.h"

class DataLoader {
public:
    static std::vector<Position> load_positions(const std::string& positions_file);
    static std::vector<MarketData> load_market_data(const std::string& prices_file,
                                                     const std::string& returns_file,
                                                     const std::vector<Position>& positions);
};
