#pragma once
// =============================================================================
// Station.h  –  Modernised C++17
// Changes vs. legacy:
//   * char name_[50] + strcpy  →  std::string  (no buffer-overflow risk)
//   * #define macros removed
//   * const-correctness throughout
//   * station_quantity_ stays static (its definition lives in Station.cc)
// =============================================================================
#include <string>
#include <cmath>

class Station {
public:
    // station_quantity_ is the global counter; each Station increments it on
    // construction so that every instance gets a unique, sequential ID.
    static int station_quantity_;

    Station(std::string name, int distance);

    const std::string& getName()     const noexcept;
    int               getDistance() const noexcept;
    int               getStationID() const noexcept;
    int               getDistanceBetween(const Station& other) const noexcept;

private:
    int         station_id_;
    std::string name_;
    int         distance_;
};
