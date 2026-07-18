# 📈 Portfolio Engine

A real-time portfolio monitoring and alerting backend, built entirely in **C++17** from the ground up — no framework, no boilerplate generator. It ingests live market data on a background thread, caches it safely across concurrent requests, evaluates per-stock price alerts with correct fire-once/reset semantics, persists everything to SQLite so nothing is lost on restart, and serves it all through a hand-written REST API to a live dashboard.

This isn't a wrapper around someone else's finance SDK — the HTTP server, the SQLite access layer, the alert evaluation engine, the caching layer, and the price-polling thread are all built from scratch on top of `httplib`, `sqlite3`, and `nlohmann::json` as the only third-party pieces.

---

## 🧠 How It Actually Works

The system is built from five cooperating pieces, each with a clear responsibility:

**1. `Database` (`database.h`)** — the only thing that ever talks to SQLite. Owns four tables: `stocks`, `active_alerts`, `alert_history`, and `portfolio_history`. Every read and write goes through a single mutex-guarded `exec()` helper, which also times every write with `std::chrono` so the engine has a real measured average write latency instead of a guessed number.

**2. `AlertEngine` (`alert_engine.h`)** — owns all alert logic. Alerts are stored per-ticker (a map from ticker → list of that ticker's alerts), so evaluating one stock's price only ever touches that stock's own alerts, not the whole portfolio's. Each alert tracks a `fired` boolean: crossing the threshold fires it once, crossing back clears it, crossing again re-fires it. Every fire gets written to `alert_history` in SQLite.

**3. `PortfolioMonitor` (`monitor.h`)** — runs on its own background thread. Every 30 seconds, it walks the current portfolio, fetches a live price for each ticker (via a `std::function` callback, so the actual data source is swappable), updates an in-memory price cache, calls into `AlertEngine::checkAlerts()` for each stock, and writes a portfolio-value snapshot to `portfolio_history`.

**4. `PortfolioServer` (`server.h`)** — a REST API built on `httplib`, exposing endpoints for adding/removing stocks, managing alerts, and reading back history, cache stats, and DB stats. Read endpoints like `/price` and `/portfolio` are served from the in-memory cache the monitor thread maintains, so a burst of dashboard requests doesn't turn into a burst of external API calls.

**5. `main.cpp`** — wires all four pieces together at startup: opens the database, reloads every stock, every active alert, and every fired-alert state from disk, then starts the monitor thread and the HTTP server.

### What happens on a fresh start
1. `Database` opens `data/portfolio.db`, creating the four tables if they don't exist yet.
2. `AlertEngine` reloads every previously fired alert's state, and every currently active alert, from SQLite — so alerts don't reset or re-fire spuriously just because the process restarted.
3. Every stock previously saved is added back into `PortfolioMonitor`.
4. The monitor thread starts polling prices immediately, and the HTTP server starts listening on port 8080.

### What happens every 30 seconds
For each stock in the portfolio: fetch its live price → update the cache → compute P&L → check that ticker's alerts against the new price → if a snapshot needs saving, write total portfolio value to `portfolio_history`.

### What happens on an incoming HTTP request
`/portfolio` and `/price` never make a live network call themselves — they read whatever the monitor thread most recently cached. This is what the `/cachestats` endpoint measures: the ratio of requests served from cache versus falling through to a live Finnhub call.

---

## 🎯 Project Objectives

- Continuously track live prices for an entire portfolio without redundant external API calls.
- Evaluate configurable price-threshold alerts per stock, with correct fire-once/reset semantics.
- Persist portfolio, alerts, and history to SQLite so restarting the engine never loses state.
- Serve everything through a hand-built REST API consumable by a live dashboard.
- Expose real, measured performance numbers instead of estimated ones.

---

## 🚀 Key Features

### ⚡ Background Price Monitor
A dedicated monitor thread polls live prices on a fixed cycle, computes per-stock profit/loss and total portfolio value, and feeds every price update into the alert engine and a persistent snapshot history — completely decoupled from the HTTP server, so a slow API response never blocks price monitoring.

### 🔒 Thread-Safe Price Caching
Live prices are cached in memory behind a mutex. Concurrent HTTP requests are served from this cache instead of each triggering its own external API call.

### 🎯 Per-Ticker Alert Engine
Alerts are stored and evaluated per ticker rather than in one shared structure scanned in full on every check, so checking a single stock's price only touches that stock's own alerts.

### 💾 Persistent, Restart-Safe State
Stocks, active alerts, and triggered-alert history are all written to SQLite and fully reloaded on startup.

### 📊 Measured, Not Estimated, Performance Stats
- `GET /cachestats` — total price requests vs. how many were served from the in-memory cache, with a live hit-rate percentage.
- `GET /dbstats` — real average SQLite write latency, measured with `std::chrono` around every single write.

---

## 📂 Project Structure

```
PortfolioEngine/
├── include/
│   ├── alert_engine.h    # Per-ticker alert storage and evaluation
│   ├── database.h        # SQLite persistence + write-latency timing
│   ├── monitor.h         # Background price-polling thread
│   ├── server.h          # REST API (httplib)
│   ├── config.h          # API key loading (apikey.txt or env var)
│   ├── sqlite3.h / sqlite3.c
│   ├── httplib.h
│   └── json.hpp
├── src/
│   └── main.cpp
├── frontend/              # Dashboard served at "/"
├── data/                  # SQLite database lives here (gitignored)
├── apikey.txt             # Your Finnhub key, one line (gitignored)
├── build.bat
└── README.md
```

---

## ▶️ Setup & Run

### 1. Get a free API key
Sign up at [finnhub.io](https://finnhub.io) and copy your API key from the dashboard.

### 2. Create your key file
In the project root, create a file named exactly `apikey.txt` containing only your key on a single line — no quotes, no extra text. This file is gitignored, so it's never committed.

### 3. Install dependencies
- A C++17 compiler (this project builds with MSVC via the Visual Studio Developer Command Prompt, or MinGW/g++).
- OpenSSL development libraries (required by `httplib` for HTTPS calls to Finnhub).

### 4. Build
```bash
build.bat
```
This compiles `main.cpp` and `sqlite3.c` and links against OpenSSL.

### 5. Run
```bash
main.exe
```
On success, you'll see:
```
[DB] Ready
[AlertEngine] N fired states loaded.
[AlertEngine] N active alert(s) loaded.
[MONITOR] Starting...
[SERVER] Running at http://localhost:8080
```

### 6. Open the dashboard
Visit `http://localhost:8080` in your browser to add stocks, set alerts, and watch live P&L update every 30 seconds.

---

## 🔌 API Endpoints

| Endpoint             | Method | Description                                |
| --------------------- | ------ | -------------------------------------------- |
| `/stock`              | POST   | Add a stock to the portfolio                |
| `/portfolio/delete`   | POST   | Remove a stock                              |
| `/portfolio`          | GET    | Current holdings with live price and P&L    |
| `/summary`            | GET    | Portfolio-level summary stats               |
| `/price?ticker=`      | GET    | Cached/live price for a single ticker       |
| `/alert`              | POST   | Set a price alert                           |
| `/alert/delete`       | POST   | Remove an active alert                      |
| `/alert/reset`        | POST   | Reset an alert so it can fire again         |
| `/activealerts`       | GET    | List currently active alerts                |
| `/alerts`             | GET    | Triggered alert history                     |
| `/alert/dismiss`      | POST   | Dismiss one entry from triggered history    |
| `/history?limit=`     | GET    | Portfolio value over time                   |
| `/clearactivealerts`  | GET    | Clear all active alerts                     |
| `/clearalerts`        | GET    | Clear triggered alert history               |
| `/cachestats`         | GET    | Real cache hit-rate stats                   |
| `/dbstats`            | GET    | Real average SQLite write latency           |

---

## 🔬 Measured Results (representative run)

| Metric                                | Result                                       |
| --------------------------------------- | ----------------------------------------------- |
| Average SQLite write latency            | ~19 ms (measured live via `/dbstats`)          |
| Cache hit rate under repeated polling   | Query `/cachestats` for your own live number   |
| Alerts scoped per-ticker                | Verified — checking one ticker never touches another's alerts |
| State survives restart                  | Verified — stocks, active alerts, and alert history all reload correctly |

Numbers above are from an actual run against a live portfolio — check your own `/dbstats` and `/cachestats` after the engine's been running a while, since both improve as more data accumulates.

---

## 📝 Notes & Known Limitations

- Prices are sourced from Finnhub's free tier, which covers real-time US equities. Indian exchanges (NSE/BSE) aren't included in the free tier — confirmed directly against the API (`"You don't have access to this resource"` on NSE-suffixed tickers). `fetchLivePrice()` is passed into the monitor as a `std::function`, so swapping in a different data source is a contained change if that's ever needed.
- Never commit `apikey.txt` — it's already excluded via `.gitignore`.
- US markets (and the prices this engine fetches) don't move on weekends/holidays — expect identical prices across consecutive monitor cycles outside trading hours.

---

## 🎯 Learning Outcomes

This project explores concepts found in trading dashboards, monitoring systems, and alerting platforms: background threading decoupled from request handling, thread-safe in-memory caching, REST API design from scratch, persistent state management with SQLite, and building real instrumentation that reports measured numbers instead of assumed ones.

---

👨‍💻 **Author**

**Krishna Parmar**
B.Tech ICT Student, Dhirubhai Ambani University
