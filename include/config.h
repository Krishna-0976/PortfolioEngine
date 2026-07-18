#pragma once

#include <string>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <algorithm>

using namespace std;

inline string loadApiKey() {
    ifstream file("apikey.txt");
    if (file.is_open()) {
        string key;
        getline(file, key);
        // Strip any trailing whitespace/newline characters (\r from Windows line endings, etc.)
        key.erase(find_if(key.rbegin(), key.rend(), [](unsigned char ch) {
            return !isspace(ch);
        }).base(), key.end());
        if (!key.empty()) return key;
    }

    const char* envKey = getenv("FINNHUB_API_KEY");
    if (envKey && string(envKey).size() > 0) {
        return string(envKey);
    }

    cerr << "[CONFIG] ERROR: No API key found.\n"
              << "         Create a file named 'apikey.txt' next to main.exe containing your Finnhub key,\n"
              << "         OR set the FINNHUB_API_KEY environment variable before running.\n";
    return "";
}