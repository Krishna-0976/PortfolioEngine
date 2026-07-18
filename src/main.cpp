#include <iostream>
#include <string>

#define CPPHTTPLIB_OPENSSL_SUPPORT

#include "../include/httplib.h"
#include "../include/sqlite3.h"
#include "../include/json.hpp"
#include "../include/config.h"
#include "../include/alert_engine.h"
#include "../include/monitor.h"
#include "../include/server.h"
#include "../include/database.h"

using namespace std;

using json = nlohmann::json;

double fetchLivePrice(const string& ticker, const string& apiKey) {
    httplib::Client cli("https://finnhub.io");
    auto res = cli.Get(("/api/v1/quote?symbol=" + ticker + "&token=" + apiKey).c_str());
    if (res && res->status == 200)
        return json::parse(res->body)["c"].get<double>();
    return -1;
}

int main() {
    string apiKey = loadApiKey();
    if (apiKey.empty()) return 1;

    Database db;

    AlertEngine engine;
    engine.setDatabase(&db);
    engine.loadFiredStateFromHistory();
    engine.loadActiveAlertsFromDB();

    PortfolioMonitor monitor(engine, db, apiKey);
    for (const auto& stock : db.getAllStocks())
        monitor.addStock(get<0>(stock), get<1>(stock), get<2>(stock));

    monitor.start(fetchLivePrice);

    PortfolioServer server(engine, monitor, db, apiKey);
    server.start();

    return 0;
}