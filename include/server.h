#pragma once

#include "httplib.h"
#include "alert_engine.h"
#include "monitor.h"
#include "json.hpp"
#include <string>
#include <atomic>

using json = nlohmann::json;

class PortfolioServer {
private:
    httplib::Server svr;
    AlertEngine&      engine;
    PortfolioMonitor& monitor;
    Database&         db;
    std::string       apiKey;

    // Tracks how often incoming HTTP requests are served from the in-memory
    // price cache vs. requiring a live Finnhub API call. This is the real,
    // measured number behind the "reduced API calls" resume claim.
    std::atomic<long long> totalPriceRequests{0};
    std::atomic<long long> cacheServedRequests{0};

public:
    PortfolioServer(AlertEngine& eng, PortfolioMonitor& mon, Database& database, const std::string& key)
        : engine(eng), monitor(mon), db(database), apiKey(key) {}

    void start() {
        svr.set_mount_point("/", "./frontend");

        svr.Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin",  "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            res.status = 204;
        });

        svr.Get("/price", [this](const httplib::Request& req, httplib::Response& res) {
            std::string ticker = req.has_param("ticker") ? req.get_param_value("ticker") : "";
            double price = -1;
            if (!ticker.empty()) {
                totalPriceRequests++;
                price = monitor.getCachedPrice(ticker);
                if (price > 0) {
                    cacheServedRequests++; // served from memory, no Finnhub call made
                } else {
                    httplib::Client cli("https://finnhub.io");
                    std::string path = "/api/v1/quote?symbol=" + ticker + "&token=" + apiKey;
                    auto r = cli.Get(path.c_str());
                    if (r && r->status == 200) {
                        auto data = json::parse(r->body);
                        price = data["c"].get<double>();
                    }
                }
            }
            json result = { {"ticker", ticker}, {"price", price} };
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        svr.Get("/portfolio", [this](const httplib::Request&, httplib::Response& res) {
            json result = json::array();
            for (auto& s : monitor.getPortfolio()) {
                totalPriceRequests++;
                double livePrice = monitor.getCachedPrice(s.ticker);
                if (livePrice > 0) cacheServedRequests++; // served from memory
                double pnl = livePrice > 0 ? (livePrice - s.buyPrice) * s.quantity : 0.0;
                result.push_back({
                    {"ticker", s.ticker}, {"buyPrice", s.buyPrice},
                    {"quantity", s.quantity}, {"livePrice", livePrice}, {"pnl", pnl}
                });
            }
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        // New: real-time view of cache effectiveness. Hit this while load-testing
        // /price or /portfolio to get your actual "% API calls avoided" number.
        svr.Get("/cachestats", [this](const httplib::Request&, httplib::Response& res) {
            long long total  = totalPriceRequests.load();
            long long served = cacheServedRequests.load();
            double hitRate = total > 0 ? (100.0 * (double)served / (double)total) : 0.0;
            json result = {
                {"totalRequests", total},
                {"cacheServedRequests", served},
                {"cacheHitRatePercent", hitRate}
            };
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        svr.Post("/portfolio/delete", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker = body["ticker"];
            monitor.removeStock(ticker);
            db.deleteStock(ticker);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        svr.Get("/summary", [this](const httplib::Request&, httplib::Response& res) {
            auto stocks = monitor.getPortfolio();
            double totalValue = 0, totalPnL = 0, bestPnL = -1e18, worstPnL = 1e18;
            std::string best = "N/A", worst = "N/A";
            for (auto& s : stocks) {
                double livePrice = monitor.getCachedPrice(s.ticker);
                if (livePrice <= 0) continue;
                double pnl = (livePrice - s.buyPrice) * s.quantity;
                totalValue += livePrice * s.quantity;
                totalPnL   += pnl;
                if (pnl > bestPnL)  { bestPnL  = pnl; best  = s.ticker; }
                if (pnl < worstPnL) { worstPnL = pnl; worst = s.ticker; }
            }
            json result = {
                {"stockCount", stocks.size()}, {"portfolioValue", totalValue},
                {"totalPnL", totalPnL}, {"bestPerformer", best}, {"worstPerformer", worst}
            };
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        svr.Post("/stock", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker = body["ticker"];
            double buyPrice    = body["buyPrice"];
            int quantity       = body["quantity"];
            monitor.addStock(ticker, buyPrice, quantity);
            db.saveStock(ticker, buyPrice, quantity);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // Add alert
        svr.Post("/alert", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker    = body["ticker"];
            std::string condition = body["condition"];
            double threshold      = body["threshold"];
            engine.addAlert(ticker, threshold, condition);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // Delete active alert
        svr.Post("/alert/delete", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker    = body["ticker"];
            std::string condition = body["condition"];
            double threshold      = body["threshold"];
            engine.removeAlert(ticker, condition, threshold);
            db.deleteActiveAlert(ticker, condition, threshold);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // Reset alert so it can fire again
        svr.Post("/alert/reset", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker    = body["ticker"];
            std::string condition = body["condition"];
            double threshold      = body["threshold"];
            engine.resetAlert(ticker, condition, threshold);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // Dismiss one triggered alert from history
        svr.Post("/alert/dismiss", [this](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body);
            std::string ticker    = body["ticker"];
            std::string condition = body["condition"];
            double threshold      = body["threshold"];
            std::string timestamp = body["timestamp"];
            db.dismissAlertHistory(ticker, condition, threshold, timestamp);
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // List active alerts
        svr.Get("/activealerts", [this](const httplib::Request&, httplib::Response& res) {
            json result = json::array();
            for (auto& a : db.getActiveAlerts()) {
                result.push_back({
                    {"ticker",    std::get<0>(a)},
                    {"condition", std::get<1>(a)},
                    {"threshold", std::get<2>(a)}
                });
            }
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        // Triggered alert history
        svr.Get("/alerts", [this](const httplib::Request&, httplib::Response& res) {
            json result = json::array();
            for (auto& a : db.getAlertHistory()) {
                result.push_back({
                    {"ticker",    std::get<0>(a)},
                    {"condition", std::get<1>(a)},
                    {"threshold", std::get<2>(a)},
                    {"livePrice", std::get<3>(a)},
                    {"timestamp", std::get<4>(a)}
                });
            }
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        // Portfolio performance history
        svr.Get("/history", [this](const httplib::Request& req, httplib::Response& res) {
            int limit = 200;
            if (req.has_param("limit")) try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
            json result = json::array();
            for (auto& p : db.getPortfolioHistory(limit))
                result.push_back({ {"value", p.first}, {"timestamp", p.second} });
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content(result.dump(), "application/json");
        });

        // Clear all active alerts
        svr.Get("/clearactivealerts", [this](const httplib::Request&, httplib::Response& res) {
            db.clearActiveAlerts();
            engine.clearAllAlerts();
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"cleared\"}", "application/json");
        });

        // Clear triggered alert history
        svr.Get("/clearalerts", [this](const httplib::Request&, httplib::Response& res) {
            db.clearAlertHistory();
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_content("{\"status\":\"cleared\"}", "application/json");
        });

        std::cout << "[SERVER] Running at http://localhost:8080" << std::endl;
        svr.listen("0.0.0.0", 8080);
    }
};