#include <iostream>
#include <string>
#include <cstdlib>
#include <csignal>
#include <atomic>

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "../include/httplib.h"
#include "../include/sqlite3.h"
#include "../include/json.hpp"
#include "../include/alert_engine.h"
#include "../include/monitor.h"
#include "../include/server.h"
#include "../include/database.h"
#include "../include/config.h"

using json = nlohmann::json;

double fetchLivePrice(const std::string& ticker, const std::string& apiKey) {
    httplib::Client cli("https://finnhub.io");
    std::string path = "/api/v1/quote?symbol=" + ticker + "&token=" + apiKey;
    auto res = cli.Get(path.c_str());
    if (res && res->status == 200) {
        auto data = json::parse(res->body);
        return data["c"].get<double>();
    }
    return -1;
}

// Global pointers so the signal handler can reach them and shut down cleanly.
// (Signal handlers can't take arguments, so this is the standard C++ pattern.)
PortfolioMonitor* g_monitor = nullptr;
std::atomic<bool> g_shuttingDown{false};

void handleSigint(int) {
    if (g_shuttingDown) return; // avoid double-handling if Ctrl+C hit twice
    g_shuttingDown = true;
    std::cout << "\n[MAIN] Shutdown signal received, stopping monitor thread..." << std::endl;
    if (g_monitor) g_monitor->stop();
    std::cout << "[MAIN] Clean shutdown complete. Exiting." << std::endl;
    std::exit(0);
}

int main() {
    // Load the Finnhub API key from apikey.txt (gitignored) or FINNHUB_API_KEY env var.
    // See config.h for details — the key is never hardcoded in source, so this repo
    // is safe to push publicly without exposing your credentials.
    std::string apiKey = loadApiKey();
    if (apiKey.empty()) {
        return 1; // loadApiKey() already printed a clear error message
    }

    Database db;
    std::cout << "Stocks in DB: " << db.getStockCount() << std::endl;

    AlertEngine engine;
    engine.setDatabase(&db);

    // Mark previously fired alerts so they don't re-trigger on restart
    engine.loadFiredStateFromHistory();

    // Reload alerts the user set via the UI
    engine.loadActiveAlertsFromDB();

    PortfolioMonitor monitor(engine, db, apiKey);
    g_monitor = &monitor; // let the signal handler reach it

    // Catch Ctrl+C so the monitor thread joins cleanly instead of being killed mid-write
    std::signal(SIGINT, handleSigint);

    auto stocks = db.getAllStocks();
    std::cout << "Loaded stocks: " << stocks.size() << std::endl;
    for (const auto& stock : stocks)
        monitor.addStock(std::get<0>(stock), std::get<1>(stock), std::get<2>(stock));

    monitor.start(fetchLivePrice);

    PortfolioServer server(engine, monitor, db, apiKey);
    server.start(); // blocks here; Ctrl+C is handled by handleSigint above

    return 0;
}