#pragma once

#include <string>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <algorithm>

// Loads the Finnhub API key without ever hardcoding it in source.
//
// Priority order:
//   1. A local file called "apikey.txt" in the project root (same folder as main.exe).
//      This file is excluded from git via .gitignore, so it never gets committed
//      or exposed if you push this repo publicly.
//   2. The FINNHUB_API_KEY environment variable, as a fallback.
//
// To set your key: create a file named "apikey.txt" next to main.exe,
// and put ONLY your key on a single line inside it. Nothing else.
inline std::string loadApiKey() {
    std::ifstream file("apikey.txt");
    if (file.is_open()) {
        std::string key;
        std::getline(file, key);
        // Strip any trailing whitespace/newline characters (\r from Windows line endings, etc.)
        key.erase(std::find_if(key.rbegin(), key.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), key.end());
        if (!key.empty()) return key;
    }

    const char* envKey = std::getenv("FINNHUB_API_KEY");
    if (envKey && std::string(envKey).size() > 0) {
        return std::string(envKey);
    }

    std::cerr << "[CONFIG] ERROR: No API key found.\n"
              << "         Create a file named 'apikey.txt' next to main.exe containing your Finnhub key,\n"
              << "         OR set the FINNHUB_API_KEY environment variable before running.\n";
    return "";
}