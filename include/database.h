#pragma once
#include "sqlite3.h"
#include <iostream>
#include <vector>
#include <tuple>
#include <algorithm>
#include <mutex>
#include <chrono>
#include <functional>

class Database {
private:
    sqlite3* db;
    std::mutex mtx;

public:
    Database() {
        if (sqlite3_open("data/portfolio.db", &db) != SQLITE_OK) {
            std::cout << "Failed to open database\n";
            return;
        }

        const char* createStocksTable =
            "CREATE TABLE IF NOT EXISTS stocks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ticker TEXT,"
            "buy_price REAL,"
            "quantity INTEGER);";

        const char* createAlertHistoryTable =
            "CREATE TABLE IF NOT EXISTS alert_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ticker TEXT,"
            "condition TEXT,"
            "threshold REAL,"
            "live_price REAL,"
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

        const char* createPortfolioHistoryTable =
            "CREATE TABLE IF NOT EXISTS portfolio_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "value REAL,"
            "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);";

        const char* createActiveAlertsTable =
            "CREATE TABLE IF NOT EXISTS active_alerts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "ticker TEXT,"
            "condition TEXT,"
            "threshold REAL,"
            "UNIQUE(ticker, condition, threshold));";

        char* errMsg = nullptr;
        sqlite3_exec(db, createStocksTable,           nullptr, nullptr, &errMsg);
        sqlite3_exec(db, createAlertHistoryTable,     nullptr, nullptr, &errMsg);
        sqlite3_exec(db, createPortfolioHistoryTable, nullptr, nullptr, &errMsg);
        sqlite3_exec(db, createActiveAlertsTable,     nullptr, nullptr, &errMsg);

        if (errMsg) { std::cout << errMsg << std::endl; sqlite3_free(errMsg); }
        std::cout << "[DB] Database ready\n";
    }

    ~Database() { sqlite3_close(db); }

    // Helper: run a prepared statement, log timing, return success
    bool execPrepared(const char* sql, const std::function<void(sqlite3_stmt*)>& bindFn) {
        auto start = std::chrono::high_resolution_clock::now();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cout << "[DB] Prepare failed: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        bindFn(stmt);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        if (!ok) std::cout << "[DB] Step failed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        std::cout << "[DB] Write took " << us << " us\n";
        return ok;
    }

    // ── active_alerts ─────────────────────────────────────────────────────────

    void saveAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "INSERT OR IGNORE INTO active_alerts (ticker, condition, threshold) VALUES (?, ?, ?);";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, threshold);
        });
    }

    void deleteActiveAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "DELETE FROM active_alerts WHERE ticker = ? AND condition = ? AND threshold = ?;";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, threshold);
        });
    }

    std::vector<std::tuple<std::string, std::string, double>> getActiveAlerts() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::tuple<std::string, std::string, double>> alerts;
        const char* sql = "SELECT ticker, condition, threshold FROM active_alerts;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                alerts.push_back({
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                    sqlite3_column_double(stmt, 2)
                });
            }
        }
        sqlite3_finalize(stmt);
        return alerts;
    }

    // ── alert_history ─────────────────────────────────────────────────────────

    void saveAlertHistory(const std::string& ticker, const std::string& condition,
                          double threshold, double livePrice) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "INSERT INTO alert_history (ticker, condition, threshold, live_price) VALUES (?, ?, ?, ?);";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, threshold);
            sqlite3_bind_double(stmt, 4, livePrice);
        });
    }

    std::vector<std::tuple<std::string, std::string, double, double, std::string>>
    getAlertHistory() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::tuple<std::string, std::string, double, double, std::string>> alerts;
        const char* sql =
            "SELECT ticker, condition, threshold, live_price, timestamp "
            "FROM alert_history ORDER BY id DESC;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                alerts.push_back({
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                    sqlite3_column_double(stmt, 2),
                    sqlite3_column_double(stmt, 3),
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))
                });
            }
        }
        sqlite3_finalize(stmt);
        return alerts;
    }

    void dismissAlertHistory(const std::string& ticker, const std::string& condition,
                              double threshold, const std::string& timestamp) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql =
            "DELETE FROM alert_history WHERE ticker = ? AND condition = ? "
            "AND threshold = ? AND timestamp = ?;";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 3, threshold);
            sqlite3_bind_text(stmt, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT);
        });
    }

    void clearActiveAlerts() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "DELETE FROM active_alerts;";
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (errMsg) { std::cout << errMsg << std::endl; sqlite3_free(errMsg); }
        std::cout << "[DB] Active alerts cleared" << std::endl;
    }

    void clearAlertHistory() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "DELETE FROM alert_history;";
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (errMsg) { std::cout << errMsg << std::endl; sqlite3_free(errMsg); }
        std::cout << "[DB] Alert history cleared\n";
    }

    void printAlertHistory() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "SELECT ticker, condition, threshold, live_price, timestamp FROM alert_history;";
        auto callback = [](void*, int, char** argv, char**) -> int {
            std::cout << "\n=============================\n";
            std::cout << "Ticker     : " << argv[0] << std::endl;
            std::cout << "Condition  : " << argv[1] << std::endl;
            std::cout << "Threshold  : $" << argv[2] << std::endl;
            std::cout << "Live Price : $" << argv[3] << std::endl;
            std::cout << "Time       : " << argv[4] << std::endl;
            std::cout << "=============================\n";
            return 0;
        };
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, callback, nullptr, &errMsg);
        if (errMsg) { std::cout << errMsg << std::endl; sqlite3_free(errMsg); }
    }

    // ── portfolio_history ─────────────────────────────────────────────────────

    void savePortfolioSnapshot(double value) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "INSERT INTO portfolio_history (value) VALUES (?);";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_double(stmt, 1, value);
        });
    }

    std::vector<std::pair<double, std::string>> getPortfolioHistory(int limit = 200) {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::pair<double, std::string>> history;
        const char* sql = "SELECT value, timestamp FROM portfolio_history ORDER BY id DESC LIMIT ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, limit);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                history.push_back({
                    sqlite3_column_double(stmt, 0),
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))
                });
            }
        }
        sqlite3_finalize(stmt);
        std::reverse(history.begin(), history.end());
        return history;
    }

    // ── stocks ────────────────────────────────────────────────────────────────

    void saveStock(const std::string& ticker, double buyPrice, int quantity) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "INSERT INTO stocks (ticker, buy_price, quantity) VALUES (?, ?, ?);";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, buyPrice);
            sqlite3_bind_int(stmt, 3, quantity);
        });
    }

    void deleteStock(const std::string& ticker) {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "DELETE FROM stocks WHERE ticker = ?;";
        execPrepared(sql, [&](sqlite3_stmt* stmt) {
            sqlite3_bind_text(stmt, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
        });
    }

    std::vector<std::tuple<std::string, double, int>> getAllStocks() {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<std::tuple<std::string, double, int>> stocks;
        const char* sql = "SELECT ticker, buy_price, quantity FROM stocks;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                stocks.push_back({
                    reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                    sqlite3_column_double(stmt, 1),
                    sqlite3_column_int(stmt, 2)
                });
            }
        }
        sqlite3_finalize(stmt);
        return stocks;
    }

    int getStockCount() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "SELECT COUNT(*) FROM stocks;";
        int count = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return count;
    }

    void clearStocks() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "DELETE FROM stocks;";
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg);
    }

    void printStocks() {
        std::lock_guard<std::mutex> lock(mtx);
        const char* sql = "SELECT * FROM stocks;";
        auto callback = [](void*, int argc, char** argv, char**) -> int {
            for (int i = 0; i < argc; i++) std::cout << (argv[i] ? argv[i] : "NULL") << " ";
            std::cout << std::endl; return 0;
        };
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, callback, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg);
    }
};