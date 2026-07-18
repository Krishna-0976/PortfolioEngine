#pragma once

#include <queue>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <iostream>
#include <mutex>
#include "database.h"

using namespace std;

struct Alert {
    string ticker;
    double threshold;
    string condition; // "ABOVE" or "BELOW"
};

enum AlertState : char { ARMED = 0, FIRED = 1, REMOVED = 2 };

struct HeapEntry {
    double threshold;
    size_t idx; // index into TickerAlerts::alerts / state
};

using Heap = priority_queue<HeapEntry, vector<HeapEntry>,
                                  function<bool(const HeapEntry&, const HeapEntry&)>>;

class AlertEngine {
private:
    struct TickerAlerts {
        vector<Alert> alerts;
        vector<char>  state;

        Heap armedAbove{ [](const HeapEntry& a, const HeapEntry& b) { return a.threshold > b.threshold; } }; // min-heap
        Heap armedBelow{ [](const HeapEntry& a, const HeapEntry& b) { return a.threshold < b.threshold; } }; // max-heap
        Heap firedAbove{ [](const HeapEntry& a, const HeapEntry& b) { return a.threshold < b.threshold; } }; // max-heap
        Heap firedBelow{ [](const HeapEntry& a, const HeapEntry& b) { return a.threshold > b.threshold; } }; // min-heap
    };

    unordered_map<string, TickerAlerts> byTicker;
    unordered_set<string> pendingFiredKeys; // from history, before alerts are (re)loaded
    Database* db = nullptr;
    mutex mtx;

    static string alertKey(const string& ticker, const string& condition, double threshold) {
        return ticker + "_" + condition + "_" + to_string((int)(threshold * 100));
    }

    void insertAlert(const string& ticker, double threshold, const string& condition, bool startFired) {
        TickerAlerts& t = byTicker[ticker];
        size_t idx = t.alerts.size();
        t.alerts.push_back({ ticker, threshold, condition });
        t.state.push_back(startFired ? FIRED : ARMED);

        HeapEntry entry{ threshold, idx };
        if (condition == "ABOVE") {
            if (startFired) t.firedAbove.push(entry);
            else            t.armedAbove.push(entry);
        } else {
            if (startFired) t.firedBelow.push(entry);
            else            t.armedBelow.push(entry);
        }
    }

public:
    void setDatabase(Database* database) {
        lock_guard<mutex> lock(mtx);
        db = database;
    }

    void loadFiredStateFromHistory() {
        if (!db) return;
        auto history = db->getAlertHistory();
        lock_guard<mutex> lock(mtx);
        for (auto& row : history)
            pendingFiredKeys.insert(alertKey(get<0>(row), get<1>(row), get<2>(row)));
        cout << "[AlertEngine] " << history.size() << " fired states loaded.\n";
    }

    void loadActiveAlertsFromDB() {
        if (!db) return;
        auto active = db->getActiveAlerts();
        lock_guard<mutex> lock(mtx);
        for (auto& row : active) {
            string ticker    = get<0>(row);
            string condition = get<1>(row);
            double threshold      = get<2>(row);
            bool startFired = pendingFiredKeys.count(alertKey(ticker, condition, threshold)) > 0;
            insertAlert(ticker, threshold, condition, startFired);
        }
        cout << "[AlertEngine] " << active.size() << " active alert(s) loaded.\n";
    }

    void addAlert(const string& ticker, double threshold, const string& condition) {
        {
            lock_guard<mutex> lock(mtx);
            insertAlert(ticker, threshold, condition, false);
        }
        if (db) db->saveAlert(ticker, condition, threshold);
        cout << "[Alert] Set: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void removeAlert(const string& ticker, const string& condition, double threshold) {
        lock_guard<mutex> lock(mtx);
        auto it = byTicker.find(ticker);
        if (it != byTicker.end()) {
            auto& t = it->second;
            for (size_t i = 0; i < t.alerts.size(); ++i) {
                if (t.state[i] != REMOVED && t.alerts[i].condition == condition && t.alerts[i].threshold == threshold) {
                    t.state[i] = REMOVED;
                    break;
                }
            }
        }
        cout << "[Alert] Removed: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void resetAlert(const string& ticker, const string& condition, double threshold) {
        lock_guard<mutex> lock(mtx);
        auto it = byTicker.find(ticker);
        if (it != byTicker.end()) {
            auto& t = it->second;
            for (size_t i = 0; i < t.alerts.size(); ++i) {
                if (t.state[i] == FIRED && t.alerts[i].condition == condition && t.alerts[i].threshold == threshold) {
                    t.state[i] = ARMED;
                    HeapEntry entry{ threshold, i };
                    if (condition == "ABOVE") t.armedAbove.push(entry);
                    else                      t.armedBelow.push(entry);
                    break;
                }
            }
        }
        cout << "[Alert] Reset: " << ticker << " " << condition << " $" << threshold << "\n";
    }

    void checkAlerts(const string& ticker, double livePrice) {
        vector<tuple<string, double, double>> triggered;
        {
            lock_guard<mutex> lock(mtx);
            auto it = byTicker.find(ticker);
            if (it == byTicker.end()) return;
            auto& t = it->second;

            while (!t.armedAbove.empty()) {
                HeapEntry top = t.armedAbove.top();
                if (t.state[top.idx] != ARMED) { t.armedAbove.pop(); continue; }
                if (livePrice > top.threshold) {
                    t.armedAbove.pop();
                    t.state[top.idx] = FIRED;
                    t.firedAbove.push(top);
                    triggered.push_back({ "ABOVE", top.threshold, livePrice });
                    cout << "[ALERT] " << ticker << " ABOVE $" << top.threshold
                              << " | Live: $" << livePrice << "\n";
                } else break;
            }
            while (!t.firedAbove.empty()) {
                HeapEntry top = t.firedAbove.top();
                if (t.state[top.idx] != FIRED) { t.firedAbove.pop(); continue; }
                if (livePrice <= top.threshold) {
                    t.firedAbove.pop();
                    t.state[top.idx] = ARMED;
                    t.armedAbove.push(top);
                } else break;
            }

            while (!t.armedBelow.empty()) {
                HeapEntry top = t.armedBelow.top();
                if (t.state[top.idx] != ARMED) { t.armedBelow.pop(); continue; }
                if (livePrice < top.threshold) {
                    t.armedBelow.pop();
                    t.state[top.idx] = FIRED;
                    t.firedBelow.push(top);
                    triggered.push_back({ "BELOW", top.threshold, livePrice });
                    cout << "[ALERT] " << ticker << " BELOW $" << top.threshold
                              << " | Live: $" << livePrice << "\n";
                } else break;
            }
            while (!t.firedBelow.empty()) {
                HeapEntry top = t.firedBelow.top();
                if (t.state[top.idx] != FIRED) { t.firedBelow.pop(); continue; }
                if (livePrice >= top.threshold) {
                    t.firedBelow.pop();
                    t.state[top.idx] = ARMED;
                    t.armedBelow.push(top);
                } else break;
            }
        }
        if (db)
            for (auto& tr : triggered)
                db->saveAlertHistory(ticker, get<0>(tr), get<1>(tr), get<2>(tr));
    }

    void clearAllAlerts() {
        lock_guard<mutex> lock(mtx);
        byTicker.clear();
        pendingFiredKeys.clear();
        cout << "[AlertEngine] All alerts cleared.\n";
    }

    int size() {
        lock_guard<mutex> lock(mtx);
        int total = 0;
        for (auto& pair : byTicker)
            for (char s : pair.second.state)
                if (s != REMOVED) total++;
        return total;
    }
};