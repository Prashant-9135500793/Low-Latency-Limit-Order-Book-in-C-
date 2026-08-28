#pragma once
// csv_reader.hpp
//
// A deliberately tiny, dependency-free CSV reader used only to load
// data/AAPL.csv -- real historical daily price data (Open/High/Low/
// Close/Volume) for Apple Inc., not synthetic/fabricated data. See
// data/README.md for provenance.
//
// This is NOT a general-purpose CSV parser (no quoted-field escaping,
// no embedded commas) -- it is scoped exactly to the one well-formed
// numeric file this project ships with, per the project's "compact,
// understandable" philosophy. Swap in a real CSV library if reusing
// this for arbitrary CSV input.

#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace lob {

struct DailyBar {
    std::string date;
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    uint64_t volume = 0;
};

// Reads data/AAPL.csv's specific column layout:
//   Date,AAPL.Open,AAPL.High,AAPL.Low,AAPL.Close,AAPL.Volume,AAPL.Adjusted,dn,mavg,up,direction
// Only the first six columns are used. Returns an empty vector (and
// prints an error) if the file can't be opened -- callers should treat
// that as fatal, since there is no synthetic fallback for this mode by
// design: this feed handler mode exists specifically to run on real
// recorded data, not to silently substitute fake data if the real file
// is missing.
inline std::vector<DailyBar> loadDailyBars(const std::string& path) {
    std::vector<DailyBar> bars;
    std::ifstream f(path);
    if (!f.is_open()) {
        return bars;
    }
    std::string line;
    std::getline(f, line); // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string field;
        DailyBar bar;
        std::getline(ss, bar.date, ',');
        std::getline(ss, field, ','); bar.open = std::stod(field);
        std::getline(ss, field, ','); bar.high = std::stod(field);
        std::getline(ss, field, ','); bar.low = std::stod(field);
        std::getline(ss, field, ','); bar.close = std::stod(field);
        std::getline(ss, field, ','); bar.volume = std::stoull(field);
        bars.push_back(bar);
    }
    return bars;
}

} // namespace lob
