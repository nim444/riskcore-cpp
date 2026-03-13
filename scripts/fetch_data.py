#!/usr/bin/env python3
"""Fetch 1 year of market data for IBM, GOOG, NVDA, MSFT, AAPL, TSLA, AMZN, META via yfinance.

Usage:
  uv run scripts/fetch_data.py
"""

import json
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import pandas as pd
import yfinance as yf

# Create data directory
data_dir = Path(__file__).parent.parent / "data"
data_dir.mkdir(exist_ok=True)

# Download 1 year of data
tickers = ["IBM", "GOOG", "NVDA", "MSFT", "AAPL", "TSLA", "AMZN", "META"]
end_date = datetime.now().date()
start_date = end_date - timedelta(days=365)

print(f"Fetching {len(tickers)} tickers from {start_date} to {end_date}...")

# Download individually to handle failures gracefully
prices_dict = {}
for ticker in tickers:
    try:
        data = yf.download(
            ticker, start=str(start_date), end=str(end_date), progress=False
        )
        if len(data) > 0:
            # Extract Close price - yfinance returns MultiIndex columns
            if isinstance(data, pd.DataFrame):
                if ("Close", ticker) in data.columns:
                    prices_dict[ticker] = data[("Close", ticker)]
                elif "Close" in data.columns:
                    prices_dict[ticker] = data["Close"]
                elif "Adj Close" in data.columns:
                    prices_dict[ticker] = data["Adj Close"]
                else:
                    # Use the first numeric column
                    prices_dict[ticker] = data.iloc[:, 0]
            print(f"✓ Downloaded {ticker} ({len(data)} days)")
        else:
            print(f"✗ No data for {ticker}")
    except Exception as e:
        print(f"✗ Failed to download {ticker}: {e}")

# Combine into single DataFrame - handle Series objects
if prices_dict:
    prices = pd.DataFrame(prices_dict)
    prices = prices.sort_index()
else:
    raise RuntimeError("Failed to download any market data")

if prices.empty:
    raise RuntimeError("Failed to download any market data")

# Save prices.csv
prices_file = data_dir / "prices.csv"
prices.to_csv(prices_file)
print(f"✓ Saved prices to {prices_file}")

# Compute daily log returns
returns = np.log(prices / prices.shift(1)).dropna()

# Save returns.csv
returns_file = data_dir / "returns.csv"
returns.to_csv(returns_file)
print(f"✓ Saved returns to {returns_file}")

# Create positions.json with entry prices from 1 year ago
prices_1yr_ago = prices.iloc[0]  # First row = 1 year ago

position_config = {
    "IBM": {"side": "LONG", "quantity": 3000},
    "GOOG": {"side": "LONG", "quantity": 1500},
    "NVDA": {"side": "SHORT", "quantity": 500},  # reduce short size
    "MSFT": {"side": "LONG", "quantity": 1200},
    "AAPL": {"side": "LONG", "quantity": 2000},
    "TSLA": {"side": "SHORT", "quantity": 800},  # reduce short size
    "AMZN": {"side": "LONG", "quantity": 1800},
    "META": {"side": "LONG", "quantity": 600},
}

positions = []
for ticker, config in position_config.items():
    if ticker not in prices.columns:
        print(f"Warning: {ticker} not in downloaded data")
        # Use a reasonable default price
        entry_price = 100.0
    else:
        entry_price = round(float(prices_1yr_ago[ticker]), 2)

    positions.append(
        {
            "ticker": ticker,
            "side": config["side"],
            "quantity": config["quantity"],
            "entry_price": entry_price,
        }
    )

positions_file = data_dir / "positions.json"
with open(positions_file, "w") as f:
    json.dump(positions, f, indent=2)
print(f"✓ Saved positions to {positions_file}")

print(f"\nSummary:")
print(f"  Prices: {len(prices)} trading days, {len(prices.columns)} tickers")
print(f"  Returns: {len(returns)} daily log returns")
print(f"  Positions: {len(positions)} portfolio holdings")
