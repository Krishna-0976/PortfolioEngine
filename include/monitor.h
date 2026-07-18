#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <algorithm>

#include "alert_engine.h"
#include "database.h"

using namespace std;

struct Stock {
    string ticker;
    double buyPrice;
    int quantity;
};

class PortfolioMonitor {
private:
    vector<Stock> portfolio;
    AlertEngine& engine;
    Database& db;
    string apiKey;
    mutex mtx;
    unordered_map<string, double> priceCache;
    atomic<bool> running{true};
    thread monitorThread;

    void monitorLoop(function<double(const string&, const string&)> fetchPrice) {
        while (running) {
            cout << "\n[MONITOR] Checking prices...\n";
            vector<Stock> snapshot;
            { lock_guard<mutex> lock(mtx); snapshot = portfolio; }

            double totalValue = 0.0;
            for (auto& stock : snapshot) {
                double price = fetchPrice(stock.ticker, apiKey);
                {
                    lock_guard<mutex> lock(mtx);
                    if (price <= 0) {
                        auto it = priceCache.find(stock.ticker);
                        if (it != priceCache.end()) price = it->second;
                        else { cout << "[MONITOR] " << stock.ticker << " fetch failed, skipping\n"; continue; }
                    } else {
                        priceCache[stock.ticker] = price;
                    }
                }
                double pnl = (price - stock.buyPrice) * stock.quantity;
                totalValue += price * stock.quantity;
                cout << stock.ticker << " | $" << price << " | P&L: $" << pnl << "\n";
                engine.checkAlerts(stock.ticker, price);
            }
            if (!snapshot.empty()) db.savePortfolioSnapshot(totalValue);
            cout << "[MONITOR] Next check in 30s\n";
            this_thread::sleep_for(chrono::seconds(30));
        }
    }

public:
    PortfolioMonitor(AlertEngine& eng, Database& database, const string& key)
        : engine(eng), db(database), apiKey(key) {}

    void addStock(const string& ticker, double buyPrice, int qty) {
        lock_guard<mutex> lock(mtx);
        portfolio.push_back({ticker, buyPrice, qty});
    }

    void removeStock(const string& ticker) {
        lock_guard<mutex> lock(mtx);
        portfolio.erase(remove_if(portfolio.begin(), portfolio.end(),
            [&](const Stock& s) { return s.ticker == ticker; }), portfolio.end());
        priceCache.erase(ticker);
    }

    void start(function<double(const string&, const string&)> fetchPrice) {
        cout << "[MONITOR] Starting...\n";
        monitorThread = thread(&PortfolioMonitor::monitorLoop, this, fetchPrice);
    }

    void stop() {
        running = false;
        if (monitorThread.joinable()) monitorThread.join();
    }

    vector<Stock> getPortfolio() {
        lock_guard<mutex> lock(mtx);
        return portfolio;
    }

    double getCachedPrice(const string& ticker) {
        lock_guard<mutex> lock(mtx);
        auto it = priceCache.find(ticker);
        return it != priceCache.end() ? it->second : -1.0;
    }

    ~PortfolioMonitor() { stop(); }
};