#pragma once
#include <vector>
#include "models.h"

class RiskEngine {
public:
    PortfolioResult compute(std::vector<Position>& positions,
                            std::vector<MarketData>& market_data);

private:
    // Helper functions
    double compute_historical_var(const std::vector<double>& daily_returns,
                                   double notional,
                                   double confidence = 0.95);
    Greeks compute_greeks(double spot, double volatility, double current_price,
                         Side side);
    double compute_sharpe(const std::vector<double>& daily_returns);
    std::vector<std::vector<double>> compute_correlation_matrix(
        const std::vector<MarketData>& market_data);
    double compute_portfolio_var(const std::vector<Position>& positions,
                                 const std::vector<MarketData>& market_data);
};
