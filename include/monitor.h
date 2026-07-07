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

// Holds a cached price plus when it was fetched, so we can decide
// whether it's still "fresh" enough to skip a live API call.
struct CacheEntry {
    double price;
    std::chrono::steady_clock::time_point timestamp;
};

class PortfolioMonitor {
private:
    std::vector<Stock> portfolio;
    AlertEngine& engine;
    Database& db;
    std::string apiKey;
    std::mutex portfolioMutex;
    std::unordered_map<std::string, CacheEntry> priceCache;
    std::atomic<bool> running{true};
    std::thread monitorThread;

    // Cache TTL: if a price was fetched within this window, reuse it instead
    // of hitting Finnhub again within the monitor loop itself.
    // Note: with a 30s poll interval and this TTL, this loop's own cache reuse
    // will rarely/never fire — that's expected. The cache that actually matters
    // for the resume claim is read via getCachedPrice() from server.h, where
    // concurrent HTTP requests are served from memory (see /cachestats).
    static constexpr int CACHE_TTL_SECONDS = 10;

    void monitorLoop(std::function<double(const std::string&, const std::string&)> fetchPrice) {
        while (running) {
            std::cout << "\n[MONITOR] Checking prices..." << std::endl;
            std::vector<Stock> snapshot;
            {
                std::lock_guard<std::mutex> lock(portfolioMutex);
                snapshot = portfolio;
            }
            double totalValue = 0.0;
            auto now = std::chrono::steady_clock::now();

            for (auto& stock : snapshot) {
                double livePrice = -1;
                bool usedCache = false;

                {
                    std::lock_guard<std::mutex> lock(portfolioMutex);
                    auto it = priceCache.find(stock.ticker);
                    if (it != priceCache.end()) {
                        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                            now - it->second.timestamp).count();
                        if (age < CACHE_TTL_SECONDS) {
                            livePrice = it->second.price;
                            usedCache = true;
                        }
                    }
                }

                if (usedCache) {
                    std::cout << "[MONITOR] " << stock.ticker << " served from cache ($"
                              << livePrice << ", age < " << CACHE_TTL_SECONDS << "s)" << std::endl;
                } else {
                    livePrice = fetchPrice(stock.ticker, apiKey);
                    if (livePrice <= 0) {
                        // Live fetch failed — fall back to last known price if we have one
                        std::lock_guard<std::mutex> lock(portfolioMutex);
                        auto it = priceCache.find(stock.ticker);
                        if (it != priceCache.end()) {
                            livePrice = it->second.price;
                            std::cout << "[MONITOR] " << stock.ticker
                                      << " fetch failed — using stale cached $" << livePrice << std::endl;
                        } else {
                            std::cout << "[MONITOR] " << stock.ticker
                                      << " fetch failed — no cache yet, skipping" << std::endl;
                            continue;
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(portfolioMutex);
                        priceCache[stock.ticker] = {livePrice, now};
                    }
                }

                double pnl = (livePrice - stock.buyPrice) * stock.quantity;
                totalValue += livePrice * stock.quantity;
                std::cout << stock.ticker << " | Live: $" << livePrice << " | P&L: $" << pnl << std::endl;
                engine.checkAlerts(stock.ticker, livePrice);
            }

            if (!snapshot.empty()) db.savePortfolioSnapshot(totalValue);
            std::cout << "[MONITOR] Next check in 30 seconds..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }

public:
    PortfolioMonitor(AlertEngine& eng, Database& database, const std::string& key)
        : engine(eng), db(database), apiKey(key) {}

    void addStock(const std::string& ticker, double buyPrice, int qty) {
        std::lock_guard<std::mutex> lock(portfolioMutex);
        portfolio.push_back({ticker, buyPrice, qty});
    }

    void removeStock(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(portfolioMutex);
        portfolio.erase(
            std::remove_if(portfolio.begin(), portfolio.end(),
                [&ticker](const Stock& s) { return s.ticker == ticker; }),
            portfolio.end()
        );
        priceCache.erase(ticker);
        std::cout << "[MONITOR] Removed stock: " << ticker << std::endl;
    }

    void start(std::function<double(const std::string&, const std::string&)> fetchPrice) {
        std::cout << "[MONITOR] Starting background monitor thread..." << std::endl;
        monitorThread = std::thread(&PortfolioMonitor::monitorLoop, this, fetchPrice);
    }

    void stop() {
        running = false;
        if (monitorThread.joinable()) monitorThread.join();
        std::cout << "[MONITOR] Stopped." << std::endl;
    }

    std::vector<Stock> getPortfolio() {
        std::lock_guard<std::mutex> lock(portfolioMutex);
        return portfolio;
    }

    double getCachedPrice(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(portfolioMutex);
        auto it = priceCache.find(ticker);
        return (it != priceCache.end()) ? it->second.price : -1.0;
    }

    ~PortfolioMonitor() { stop(); }
};