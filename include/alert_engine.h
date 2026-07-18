#pragma once

#include <queue>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <mutex>
#include "database.h"

struct Alert {
    std::string ticker;
    double threshold;
    std::string condition;
};

class AlertEngine {
private:
    std::unordered_map<std::string, std::vector<Alert>> alertsByTicker;
    Database* db = nullptr;
    std::unordered_map<std::string, bool> firedState;
    std::mutex mtx;

    std::string alertKey(const std::string& ticker, const std::string& condition, double threshold) {
        return ticker + "_" + condition + "_" + std::to_string((int)(threshold * 100));
    }

public:
    void setDatabase(Database* database) {
        std::lock_guard<std::mutex> lock(mtx);
        db = database;
    }

    void loadFiredStateFromHistory() {
        if (!db) return;
        auto history = db->getAlertHistory();
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& row : history)
            firedState[alertKey(std::get<0>(row), std::get<1>(row), std::get<2>(row))] = true;
        std::cout << "[AlertEngine] " << history.size() << " fired states loaded.\n";
    }

    void loadActiveAlertsFromDB() {
        if (!db) return;
        auto active = db->getActiveAlerts();
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& row : active) {
            Alert a{ std::get<0>(row), std::get<2>(row), std::get<1>(row) };
            alertsByTicker[a.ticker].push_back(a);
            std::string key = alertKey(a.ticker, a.condition, a.threshold);
            if (firedState.find(key) == firedState.end())
                firedState[key] = false;
        }
        std::cout << "[AlertEngine] " << active.size() << " active alert(s) loaded.\n";
    }

    void addAlert(const std::string& ticker, double threshold, const std::string& condition) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            alertsByTicker[ticker].push_back({ ticker, threshold, condition });
            firedState[alertKey(ticker, condition, threshold)] = false;
        }
        if (db) db->saveAlert(ticker, condition, threshold);
        std::cout << "[Alert] Set: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void removeAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = alertsByTicker.find(ticker);
        if (it != alertsByTicker.end()) {
            auto& vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const Alert& a) {
                return a.condition == condition && a.threshold == threshold;
            }), vec.end());
        }
        firedState.erase(alertKey(ticker, condition, threshold));
        std::cout << "[Alert] Removed: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void resetAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        firedState[alertKey(ticker, condition, threshold)] = false;
        std::cout << "[Alert] Reset: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void checkAlerts(const std::string& ticker, double livePrice) {
        std::vector<std::tuple<std::string, double, double>> triggered;
        {
            std::lock_guard<std::mutex> lock(mtx);
            auto it = alertsByTicker.find(ticker);
            if (it == alertsByTicker.end()) return;

            for (auto& a : it->second) {
                std::string key = alertKey(a.ticker, a.condition, a.threshold);
                bool met     = (a.condition == "BELOW" && livePrice <  a.threshold)
                            || (a.condition == "ABOVE" && livePrice >  a.threshold);
                bool cleared = (a.condition == "BELOW" && livePrice >= a.threshold)
                            || (a.condition == "ABOVE" && livePrice <= a.threshold);
                if (met && !firedState[key]) {
                    std::cout << "[ALERT] " << ticker << " " << a.condition
                              << " $" << a.threshold << " | Live: $" << livePrice << "\n";
                    triggered.push_back({ a.condition, a.threshold, livePrice });
                    firedState[key] = true;
                } else if (cleared) {
                    firedState[key] = false;
                }
            }
        }
        if (db)
            for (auto& t : triggered)
                db->saveAlertHistory(ticker, std::get<0>(t), std::get<1>(t), std::get<2>(t));
    }

    void clearAllAlerts() {
        std::lock_guard<std::mutex> lock(mtx);
        alertsByTicker.clear();
        firedState.clear();
        std::cout << "[AlertEngine] All alerts cleared.\n";
    }

    int size() {
        std::lock_guard<std::mutex> lock(mtx);
        int total = 0;
        for (auto& pair : alertsByTicker) total += (int)pair.second.size();
        return total;
    }
};