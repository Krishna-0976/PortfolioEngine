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

    bool operator>(const Alert& other) const {
        return threshold > other.threshold;
    }
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
        auto history = db->getAlertHistory(); // Database has its own lock; call before locking ours
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& row : history) {
            std::string key = alertKey(std::get<0>(row), std::get<1>(row), std::get<2>(row));
            firedState[key] = true;
        }
        std::cout << "[AlertEngine] Loaded " << history.size() << " fired states from DB." << std::endl;
    }

    void loadActiveAlertsFromDB() {
        if (!db) return;
        auto active = db->getActiveAlerts(); // fetch outside our lock to avoid nested locking
        std::lock_guard<std::mutex> lock(mtx);
        for (auto& row : active) {
            Alert a;
            a.ticker    = std::get<0>(row);
            a.condition = std::get<1>(row);
            a.threshold = std::get<2>(row);
            minHeap.push(a);
            std::string key = alertKey(a.ticker, a.condition, a.threshold);
            if (firedState.find(key) == firedState.end())
                firedState[key] = false;
        }
        std::cout << "[AlertEngine] Reloaded " << active.size() << " active alert(s) from DB.\n";
    }

    void addAlert(const std::string& ticker, double threshold, const std::string& condition) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            Alert a;
            a.ticker    = ticker;
            a.threshold = threshold;
            a.condition = condition;
            minHeap.push(a);
            firedState[alertKey(ticker, condition, threshold)] = false;
        }
        if (db) db->saveAlert(ticker, condition, threshold); // Database locks itself
        std::cout << "Alert set: " << ticker << " " << condition << " $" << threshold << std::endl;
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
        std::cout << "[AlertEngine] Removed alert: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void resetAlert(const std::string& ticker, const std::string& condition, double threshold) {
        std::lock_guard<std::mutex> lock(mtx);
        firedState[alertKey(ticker, condition, threshold)] = false;
        std::cout << "[AlertEngine] Reset alert: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void checkAlerts(const std::string& ticker, double livePrice) {
        std::vector<std::pair<std::string, double>> toRecord; // {condition+threshold info deferred to after unlock}
        std::vector<std::tuple<std::string, double, double>> triggered; // condition, threshold, livePrice

        {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<Alert> remaining;
            while (!minHeap.empty()) {
                Alert top = minHeap.top(); minHeap.pop();
                if (top.ticker == ticker) {
                    std::string key = alertKey(top.ticker, top.condition, top.threshold);
                    bool conditionMet     = (top.condition == "BELOW" && livePrice <  top.threshold)
                                         || (top.condition == "ABOVE" && livePrice >  top.threshold);
                    bool conditionCleared = (top.condition == "BELOW" && livePrice >= top.threshold)
                                         || (top.condition == "ABOVE" && livePrice <= top.threshold);
                    if (conditionMet && !firedState[key]) {
                        std::cout << "ALERT TRIGGERED: " << ticker << " " << top.condition
                                  << " $" << top.threshold << " | Live: $" << livePrice << std::endl;
                        triggered.push_back({top.condition, top.threshold, livePrice});
                        firedState[key] = true;
                    } else if (conditionCleared) {
                        firedState[key] = false;
                    }
                }
                remaining.push_back(top);
            }
            for (auto& a : remaining) minHeap.push(a);
        }

        // Call into Database (which locks itself) only after releasing our own lock,
        // to avoid holding two locks / risking deadlock across classes.
        if (db) {
            for (auto& t : triggered)
                db->saveAlertHistory(ticker, std::get<0>(t), std::get<1>(t), std::get<2>(t));
        }
    }

    void clearAllAlerts() {
        std::lock_guard<std::mutex> lock(mtx);
        while (!minHeap.empty()) minHeap.pop();
        firedState.clear();
        std::cout << "[AlertEngine] All alerts cleared." << std::endl;
    }

    int size() {
        std::lock_guard<std::mutex> lock(mtx);
        return (int)minHeap.size();
    }
};