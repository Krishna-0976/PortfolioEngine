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

struct Stock {
    std::string ticker;
    double buyPrice;
    int quantity;
};

class PortfolioMonitor {
private:
    std::vector<Stock> portfolio;
    AlertEngine& engine;
    Database& db;
    std::string apiKey;
    std::mutex mtx;
    std::unordered_map<std::string, double> priceCache;
    std::atomic<bool> running{true};
    std::thread monitorThread;

    void monitorLoop(std::function<double(const std::string&, const std::string&)> fetchPrice) {
        while (running) {
            std::cout << "\n[MONITOR] Checking prices...\n";
            std::vector<Stock> snapshot;
            { std::lock_guard<std::mutex> lock(mtx); snapshot = portfolio; }

            double totalValue = 0.0;
            for (auto& stock : snapshot) {
                double price = fetchPrice(stock.ticker, apiKey);
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (price <= 0) {
                        auto it = priceCache.find(stock.ticker);
                        if (it != priceCache.end()) price = it->second;
                        else { std::cout << "[MONITOR] " << stock.ticker << " fetch failed, skipping\n"; continue; }
                    } else {
                        priceCache[stock.ticker] = price;
                    }
                }
                double pnl = (price - stock.buyPrice) * stock.quantity;
                totalValue += price * stock.quantity;
                std::cout << stock.ticker << " | $" << price << " | P&L: $" << pnl << "\n";
                engine.checkAlerts(stock.ticker, price);
            }
            if (!snapshot.empty()) db.savePortfolioSnapshot(totalValue);
            std::cout << "[MONITOR] Next check in 30s\n";
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }

public:
    PortfolioMonitor(AlertEngine& eng, Database& database, const std::string& key)
        : engine(eng), db(database), apiKey(key) {}

    void addStock(const std::string& ticker, double buyPrice, int qty) {
        std::lock_guard<std::mutex> lock(mtx);
        portfolio.push_back({ticker, buyPrice, qty});
    }

    void removeStock(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(mtx);
        portfolio.erase(std::remove_if(portfolio.begin(), portfolio.end(),
            [&](const Stock& s) { return s.ticker == ticker; }), portfolio.end());
        priceCache.erase(ticker);
    }

    void start(std::function<double(const std::string&, const std::string&)> fetchPrice) {
        std::cout << "[MONITOR] Starting...\n";
        monitorThread = std::thread(&PortfolioMonitor::monitorLoop, this, fetchPrice);
    }

    void stop() {
        running = false;
        if (monitorThread.joinable()) monitorThread.join();
    }

    std::vector<Stock> getPortfolio() {
        std::lock_guard<std::mutex> lock(mtx);
        return portfolio;
    }

    double getCachedPrice(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = priceCache.find(ticker);
        return it != priceCache.end() ? it->second : -1.0;
    }

    ~PortfolioMonitor() { stop(); }
};