#!/usr/bin/env python3
import yfinance as yf
from datetime import datetime, timedelta

end_date = datetime.now().date()
start_date = end_date - timedelta(days=365)

print("Downloading GOOG...")
data = yf.download('GOOG', start=str(start_date), end=str(end_date), progress=False)
print(f'Type: {type(data)}')
print(f'Shape: {data.shape}')
print(f'Columns: {data.columns.tolist() if hasattr(data, "columns") else "N/A"}')
print(f'Index name: {data.index.name}')
print('First 3 rows:')
print(data.head(3))
print('\nLast 3 rows:')
print(data.tail(3))
