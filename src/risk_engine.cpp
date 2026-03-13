#include "risk_engine.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <chrono>
#include <sstream>
#include <iomanip>

// Standard normal CDF using erfc
double norm_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

// Standard normal PDF
double norm_pdf(double x) {
    return std::exp(-0.5 * x * x) / std::sqrt(2.0 * M_PI);
}

PortfolioResult RiskEngine::compute(std::vector<Position>& positions,
                                     std::vector<MarketData>& market_data) {
    auto start = std::chrono::high_resolution_clock::now();

    PortfolioResult result;

    // Set timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t));
    result.timestamp = std::string(buf);

    // Update current prices in positions
    for (auto& pos : positions) {
        for (const auto& md : market_data) {
            if (pos.ticker == md.ticker) {
                pos.current_price = md.current_price;
                break;
            }
        }
    }

    // Compute individual position risk
    for (auto& pos : positions) {
        RiskResult risk;
        risk.ticker = pos.ticker;
        risk.side = pos.side;

        // Find market data for this ticker
        MarketData* md = nullptr;
        for (auto& m : market_data) {
            if (m.ticker == pos.ticker) {
                md = &m;
                break;
            }
        }

        if (!md) {
            continue;
        }

        // Notional value
        double notional = pos.quantity * pos.current_price;
        if (pos.side == Side::SHORT) notional = -notional;

        // Historical VaR (95%)
        risk.var_95 = compute_historical_var(md->daily_returns, notional);

        // Greeks
        risk.greeks = compute_greeks(pos.current_price, md->volatility,
                                      pos.current_price, pos.side);

        // Sharpe ratio
        risk.sharpe_ratio = compute_sharpe(md->daily_returns);

        // P&L
        double entry_notional = pos.quantity * pos.entry_price;
        if (pos.side == Side::SHORT) {
            // For short: profit when price goes down
            risk.pnl = entry_notional - notional;
        } else {
            risk.pnl = notional - entry_notional;
        }

        result.positions.push_back(risk);
    }

    // Correlation matrix
    result.correlation_matrix = compute_correlation_matrix(market_data);

    // Portfolio VaR and net exposure
    result.portfolio_var = compute_portfolio_var(positions, market_data);

    // Portfolio Sharpe (weighted by notional)
    double total_notional = 0.0;

    // First pass: compute total absolute notional
    std::vector<double> notionals;
    for (const auto& pos : positions) {
        double notional = pos.quantity * pos.current_price;
        if (pos.side == Side::SHORT) notional = -notional;
        notionals.push_back(notional);
        total_notional += std::abs(notional);
    }

    // Second pass: compute weighted return
    double weighted_return = 0.0;
    for (size_t i = 0; i < positions.size(); ++i) {
        MarketData* md = nullptr;
        for (auto& m : market_data) {
            if (m.ticker == positions[i].ticker) {
                md = &m;
                break;
            }
        }
        if (!md) continue;

        double weight = notionals[i] / (total_notional + 1e-9);  // Correct: divide by total, not self
        if (!md->daily_returns.empty()) {
            double mean_return = 0.0;
            for (double r : md->daily_returns) mean_return += r;
            mean_return /= md->daily_returns.size();
            weighted_return += weight * mean_return;
        }
    }

    if (total_notional > 0) {
        double annualised_return = weighted_return * 252.0;
        // Estimate portfolio vol from correlation
        double portfolio_var_frac = result.portfolio_var / total_notional;
        double daily_vol = portfolio_var_frac / 1.645;  // Reverse from 95% VaR
        double portfolio_vol = daily_vol * std::sqrt(252.0);  // Annualize
        result.portfolio_sharpe = (annualised_return - 0.045) / (portfolio_vol + 1e-9);
    } else {
        result.portfolio_sharpe = 0.0;
    }

    // Net exposure (absolute sum of notionals)
    result.net_exposure = 0.0;
    for (const auto& pos : positions) {
        double notional = pos.quantity * pos.current_price;
        result.net_exposure += std::abs(notional);
    }

    auto end = std::chrono::high_resolution_clock::now();
    result.calc_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Generate descriptive update message
    std::ostringstream desc;
    if (!result.positions.empty()) {
        // Pick the position with highest absolute Greeks change
        int max_greek_idx = 0;
        double max_delta = std::abs(result.positions[0].greeks.delta);
        for (size_t i = 1; i < result.positions.size(); ++i) {
            double delta = std::abs(result.positions[i].greeks.delta);
            if (delta > max_delta) {
                max_delta = delta;
                max_greek_idx = i;
            }
        }
        const auto& top_pos = result.positions[max_greek_idx];
        desc << top_pos.ticker << " Greeks recalculated (δ=" << std::fixed << std::setprecision(3)
             << top_pos.greeks.delta << ", Sharpe=" << std::setprecision(2) << top_pos.sharpe_ratio << ")";
    }
    result.last_update_desc = desc.str();

    return result;
}

double RiskEngine::compute_historical_var(const std::vector<double>& daily_returns,
                                           double notional,
                                           double confidence) {
    if (daily_returns.empty()) return 0.0;

    // Compute P&L from returns
    std::vector<double> pnl;
    for (double r : daily_returns) {
        pnl.push_back(notional * r);
    }

    // Sort P&L (worst losses first)
    std::sort(pnl.begin(), pnl.end());

    // Get percentile (5th percentile for 95% confidence)
    int idx = static_cast<int>((1.0 - confidence) * pnl.size());
    idx = std::max(0, std::min(idx, static_cast<int>(pnl.size()) - 1));

    return std::abs(pnl[idx]);
}

Greeks RiskEngine::compute_greeks(double spot, double volatility, [[maybe_unused]] double current_price,
                                   Side side) {
    Greeks greeks;
    greeks.implied_vol = volatility * 100.0;  // As percentage

    if (volatility < 1e-9) {
        greeks.delta = (side == Side::LONG) ? 0.5 : -0.5;
        greeks.gamma = 0.0;
        greeks.vega = 0.0;
        greeks.theta = 0.0;
        return greeks;
    }

    // Black-Scholes parameters
    double S = spot;
    double K = spot;  // ATM
    double r = 0.045;
    double T = 0.25;  // 3 months
    double sigma = volatility;

    // d1, d2
    double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T) / (sigma * std::sqrt(T));
    [[maybe_unused]] double d2 = d1 - sigma * std::sqrt(T);

    double N_d1 = norm_cdf(d1);
    double N_prime_d1 = norm_pdf(d1);

    // Delta (for long call)
    greeks.delta = N_d1;
    if (side == Side::SHORT) greeks.delta = -greeks.delta;

    // Gamma
    greeks.gamma = N_prime_d1 / (S * sigma * std::sqrt(T));

    // Vega (per 1% change in volatility)
    greeks.vega = S * N_prime_d1 * std::sqrt(T) / 100.0;

    // Theta (per day)
    greeks.theta = -(S * N_prime_d1 * sigma) / (2.0 * std::sqrt(T)) / 365.0;

    return greeks;
}

double RiskEngine::compute_sharpe(const std::vector<double>& daily_returns) {
    if (daily_returns.empty()) return 0.0;

    double mean = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0) /
                  daily_returns.size();
    double var = 0.0;
    for (double r : daily_returns) {
        var += (r - mean) * (r - mean);
    }
    var /= (daily_returns.size() - 1);

    double std_dev = std::sqrt(var);
    if (std_dev < 1e-9) return 0.0;

    double annualised_return = mean * 252.0;
    double annualised_vol = std_dev * std::sqrt(252.0);

    return (annualised_return - 0.045) / annualised_vol;
}

std::vector<std::vector<double>>
RiskEngine::compute_correlation_matrix(const std::vector<MarketData>& market_data) {
    int n = market_data.size();
    std::vector<std::vector<double>> corr(n, std::vector<double>(n, 0.0));

    // Compute means
    std::vector<double> means(n);
    for (int i = 0; i < n; ++i) {
        if (!market_data[i].daily_returns.empty()) {
            means[i] = std::accumulate(market_data[i].daily_returns.begin(),
                                      market_data[i].daily_returns.end(), 0.0) /
                       market_data[i].daily_returns.size();
        }
    }

    // Compute correlation
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (market_data[i].daily_returns.size() != market_data[j].daily_returns.size()) {
                corr[i][j] = 0.0;
                continue;
            }

            int size = market_data[i].daily_returns.size();
            if (size == 0) {
                corr[i][j] = 0.0;
                continue;
            }

            double cov = 0.0;
            double var_i = 0.0;
            double var_j = 0.0;

            for (int k = 0; k < size; ++k) {
                double di = market_data[i].daily_returns[k] - means[i];
                double dj = market_data[j].daily_returns[k] - means[j];
                cov += di * dj;
                var_i += di * di;
                var_j += dj * dj;
            }

            cov /= (size - 1);
            var_i /= (size - 1);
            var_j /= (size - 1);

            double std_i = std::sqrt(var_i);
            double std_j = std::sqrt(var_j);

            if (std_i > 1e-9 && std_j > 1e-9) {
                corr[i][j] = cov / (std_i * std_j);
            } else {
                corr[i][j] = (i == j) ? 1.0 : 0.0;
            }
        }
    }

    return corr;
}

double RiskEngine::compute_portfolio_var(const std::vector<Position>& positions,
                                         const std::vector<MarketData>& market_data) {
    // Compute covariance matrix from daily returns
    int n = market_data.size();
    if (n == 0) return 0.0;

    // Compute means
    std::vector<double> means(n);
    int size = 0;
    for (int i = 0; i < n; ++i) {
        if (!market_data[i].daily_returns.empty()) {
            size = market_data[i].daily_returns.size();
            means[i] = std::accumulate(market_data[i].daily_returns.begin(),
                                      market_data[i].daily_returns.end(), 0.0) /
                       size;
        }
    }

    if (size == 0) return 0.0;

    // Covariance matrix
    std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            if (market_data[i].daily_returns.empty() || market_data[j].daily_returns.empty()) {
                cov[i][j] = (i == j) ? 1.0 : 0.0;
                continue;
            }
            for (int k = 0; k < size && k < static_cast<int>(market_data[i].daily_returns.size()) &&
                             k < static_cast<int>(market_data[j].daily_returns.size()); ++k) {
                double di = market_data[i].daily_returns[k] - means[i];
                double dj = market_data[j].daily_returns[k] - means[j];
                cov[i][j] += di * dj;
            }
            cov[i][j] /= (size - 1);
        }
    }

    // Compute total notional and weights
    double total_notional = 0.0;
    std::vector<double> notionals(n);
    for (const auto& pos : positions) {
        for (int i = 0; i < n; ++i) {
            if (pos.ticker == market_data[i].ticker) {
                notionals[i] = pos.quantity * pos.current_price;
                if (pos.side == Side::SHORT) notionals[i] = -notionals[i];
                total_notional += std::abs(notionals[i]);
                break;
            }
        }
    }

    if (total_notional < 1e-9) return 0.0;

    // Compute portfolio variance: w^T * Sigma * w
    double port_var_frac = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            port_var_frac += (notionals[i] / total_notional) * cov[i][j] *
                             (notionals[j] / total_notional);
        }
    }

    double port_vol = std::sqrt(std::abs(port_var_frac));
    double portfolio_var = port_vol * 1.645 * total_notional;  // 95% confidence

    return portfolio_var;
}
