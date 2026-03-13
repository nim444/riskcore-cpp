#pragma once
#include <string>
#include <vector>

enum class Side { LONG, SHORT };

struct Position {
    std::string ticker;
    Side side;
    int quantity;
    double entry_price;
    double current_price;
};

struct MarketData {
    std::string ticker;
    std::vector<double> daily_returns;   // 252-ish values
    double current_price;
    double volatility;                   // annualised
};

struct Greeks {
    double delta;
    double gamma;
    double vega;
    double theta;
    double implied_vol;
};

struct RiskResult {
    std::string ticker;
    Side side;
    double var_95;           // 1-day 95% historical VaR
    double sharpe_ratio;     // annualised
    Greeks greeks;
    double pnl;
};

struct PortfolioResult {
    std::vector<RiskResult> positions;
    double portfolio_var;
    double portfolio_sharpe;
    double net_exposure;
    std::vector<std::vector<double>> correlation_matrix;
    double calc_time_ms;
    std::string timestamp;
    std::string last_update_desc;  // e.g. "Portfolio VaR updated → $128,446"
};
