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
    bool operator>(const Alert& other) const { return threshold > other.threshold; }
};

class AlertEngine {
private:
    std::priority_queue<Alert, std::vector<Alert>, std::greater<Alert>> minHeap;
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
            minHeap.push(a);
            std::string key = alertKey(a.ticker, a.condition, a.threshold);
            if (firedState.find(key) == firedState.end())
                firedState[key] = false;
        }
        std::cout << "[AlertEngine] " << active.size() << " active alert(s) loaded.\n";
    }

    void addAlert(const std::string& ticker, double threshold, const std::string& condition) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            minHeap.push({ ticker, threshold, condition });
            firedState[alertKey(ticker, condition, threshold)] = false;
        }
        if (db) db->saveAlert(ticker, condition, threshold);
        std::cout << "[Alert] Set: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void removeAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        std::vector<Alert> remaining;
        while (!minHeap.empty()) {
            Alert top = minHeap.top(); minHeap.pop();
            if (!(top.ticker == ticker && top.condition == condition && top.threshold == threshold))
                remaining.push_back(top);
        }
        for (auto& a : remaining) minHeap.push(a);
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
            std::vector<Alert> remaining;
            while (!minHeap.empty()) {
                Alert top = minHeap.top(); minHeap.pop();
                if (top.ticker == ticker) {
                    std::string key = alertKey(top.ticker, top.condition, top.threshold);
                    bool met     = (top.condition == "BELOW" && livePrice <  top.threshold)
                                || (top.condition == "ABOVE" && livePrice >  top.threshold);
                    bool cleared = (top.condition == "BELOW" && livePrice >= top.threshold)
                                || (top.condition == "ABOVE" && livePrice <= top.threshold);
                    if (met && !firedState[key]) {
                        std::cout << "[ALERT] " << ticker << " " << top.condition
                                  << " $" << top.threshold << " | Live: $" << livePrice << "\n";
                        triggered.push_back({ top.condition, top.threshold, livePrice });
                        firedState[key] = true;
                    } else if (cleared) {
                        firedState[key] = false;
                    }
                }
                remaining.push_back(top);
            }
            for (auto& a : remaining) minHeap.push(a);
        }
        if (db)
            for (auto& t : triggered)
                db->saveAlertHistory(ticker, std::get<0>(t), std::get<1>(t), std::get<2>(t));
    }

    void clearAllAlerts() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!minHeap.empty()) minHeap.pop();
        firedState.clear();
        std::cout << "[AlertEngine] All alerts cleared.\n";
    }

    int size() {
        std::lock_guard<std::mutex> lock(mtx);
        return (int)minHeap.size();
    }
};