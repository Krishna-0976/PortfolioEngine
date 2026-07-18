#pragma once
#include "sqlite3.h"
#include <iostream>
#include <vector>
#include <tuple>
#include <algorithm>
#include <mutex>
#include <functional>
#include <chrono>
#include <atomic>

using namespace std;

class Database {
private:
    sqlite3* db;
    mutex mtx;
    atomic<long long> totalWriteMicros{0};
    atomic<long long> writeCount{0};

    bool exec(const char* sql, const function<void(sqlite3_stmt*)>& bind) {
        auto start = chrono::steady_clock::now();

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        bind(stmt);
        bool ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);

        auto end = chrono::steady_clock::now();
        auto micros = chrono::duration_cast<chrono::microseconds>(end - start).count();
        totalWriteMicros += micros;
        writeCount++;

        return ok;
    }

public:
    Database() {
        if (sqlite3_open("data/portfolio.db", &db) != SQLITE_OK) {
            cout << "[DB] Failed to open database\n"; return;
        }
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS stocks ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, buy_price REAL, quantity INTEGER);",
            nullptr, nullptr, nullptr);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS alert_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, condition TEXT, "
            "threshold REAL, live_price REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);",
            nullptr, nullptr, nullptr);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS portfolio_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, value REAL, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP);",
            nullptr, nullptr, nullptr);
        sqlite3_exec(db,
            "CREATE TABLE IF NOT EXISTS active_alerts ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, ticker TEXT, condition TEXT, threshold REAL, "
            "UNIQUE(ticker, condition, threshold));",
            nullptr, nullptr, nullptr);
        cout << "[DB] Ready\n";
    }

    ~Database() { sqlite3_close(db); }

    double getAverageWriteLatencyMs() {
        long long count = writeCount.load();
        if (count == 0) return 0.0;
        double avgMicros = (double)totalWriteMicros.load() / (double)count;
        return avgMicros / 1000.0;
    }

    long long getWriteCount() {
        return writeCount.load();
    }

    void saveAlert(const string& ticker, const string& condition, double threshold) {
        lock_guard<mutex> lock(mtx);
        exec("INSERT OR IGNORE INTO active_alerts (ticker, condition, threshold) VALUES (?,?,?);",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(s, 3, threshold);
            });
    }

    void deleteActiveAlert(const string& ticker, const string& condition, double threshold) {
        lock_guard<mutex> lock(mtx);
        exec("DELETE FROM active_alerts WHERE ticker=? AND condition=? AND threshold=?;",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(s, 3, threshold);
            });
    }

    void clearActiveAlerts() {
        lock_guard<mutex> lock(mtx);
        sqlite3_exec(db, "DELETE FROM active_alerts;", nullptr, nullptr, nullptr);
    }

    vector<tuple<string, string, double>> getActiveAlerts() {
        lock_guard<mutex> lock(mtx);
        vector<tuple<string, string, double>> out;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT ticker, condition, threshold FROM active_alerts;", -1, &s, nullptr) == SQLITE_OK)
            while (sqlite3_step(s) == SQLITE_ROW)
                out.push_back({ (const char*)sqlite3_column_text(s,0),
                                (const char*)sqlite3_column_text(s,1),
                                sqlite3_column_double(s,2) });
        sqlite3_finalize(s);
        return out;
    }

    void saveAlertHistory(const string& ticker, const string& condition,
                          double threshold, double livePrice) {
        lock_guard<mutex> lock(mtx);
        exec("INSERT INTO alert_history (ticker, condition, threshold, live_price) VALUES (?,?,?,?);",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(s, 3, threshold);
                sqlite3_bind_double(s, 4, livePrice);
            });
    }

    void dismissAlertHistory(const string& ticker, const string& condition,
                              double threshold, const string& timestamp) {
        lock_guard<mutex> lock(mtx);
        exec("DELETE FROM alert_history WHERE ticker=? AND condition=? AND threshold=? AND timestamp=?;",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(s, 2, condition.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(s, 3, threshold);
                sqlite3_bind_text(s, 4, timestamp.c_str(), -1, SQLITE_TRANSIENT);
            });
    }

    void clearAlertHistory() {
        lock_guard<mutex> lock(mtx);
        sqlite3_exec(db, "DELETE FROM alert_history;", nullptr, nullptr, nullptr);
        cout << "[DB] Alert history cleared\n";
    }

    vector<tuple<string, string, double, double, string>> getAlertHistory() {
        lock_guard<mutex> lock(mtx);
        vector<tuple<string, string, double, double, string>> out;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT ticker, condition, threshold, live_price, timestamp "
            "FROM alert_history ORDER BY id DESC;", -1, &s, nullptr) == SQLITE_OK)
            while (sqlite3_step(s) == SQLITE_ROW)
                out.push_back({ (const char*)sqlite3_column_text(s,0),
                                (const char*)sqlite3_column_text(s,1),
                                sqlite3_column_double(s,2),
                                sqlite3_column_double(s,3),
                                (const char*)sqlite3_column_text(s,4) });
        sqlite3_finalize(s);
        return out;
    }

    void savePortfolioSnapshot(double value) {
        lock_guard<mutex> lock(mtx);
        exec("INSERT INTO portfolio_history (value) VALUES (?);",
            [&](sqlite3_stmt* s) { sqlite3_bind_double(s, 1, value); });
    }

    vector<pair<double, string>> getPortfolioHistory(int limit = 200) {
        lock_guard<mutex> lock(mtx);
        vector<pair<double, string>> out;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db,
            "SELECT value, timestamp FROM portfolio_history ORDER BY id DESC LIMIT ?;",
            -1, &s, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(s, 1, limit);
            while (sqlite3_step(s) == SQLITE_ROW)
                out.push_back({ sqlite3_column_double(s,0), (const char*)sqlite3_column_text(s,1) });
        }
        sqlite3_finalize(s);
        reverse(out.begin(), out.end());
        return out;
    }

    void saveStock(const string& ticker, double buyPrice, int quantity) {
        lock_guard<mutex> lock(mtx);
        exec("INSERT INTO stocks (ticker, buy_price, quantity) VALUES (?,?,?);",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(s, 2, buyPrice);
                sqlite3_bind_int(s, 3, quantity);
            });
    }

    void deleteStock(const string& ticker) {
        lock_guard<mutex> lock(mtx);
        exec("DELETE FROM stocks WHERE ticker=?;",
            [&](sqlite3_stmt* s) { sqlite3_bind_text(s, 1, ticker.c_str(), -1, SQLITE_TRANSIENT); });
    }

    vector<tuple<string, double, int>> getAllStocks() {
        lock_guard<mutex> lock(mtx);
        vector<tuple<string, double, int>> out;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT ticker, buy_price, quantity FROM stocks;", -1, &s, nullptr) == SQLITE_OK)
            while (sqlite3_step(s) == SQLITE_ROW)
                out.push_back({ (const char*)sqlite3_column_text(s,0),
                                sqlite3_column_double(s,1),
                                sqlite3_column_int(s,2) });
        sqlite3_finalize(s);
        return out;
    }

    int getStockCount() {
        lock_guard<mutex> lock(mtx);
        int count = 0;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM stocks;", -1, &s, nullptr) == SQLITE_OK)
            if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
        return count;
    }
};