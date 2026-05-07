#include "Station.h"

// Static member definition – one translation unit only.
int Station::station_quantity_ = 0;

Station::Station(std::string name, int distance)
    : name_(std::move(name))
    , distance_(distance)
    , station_id_(station_quantity_++)   // assign then increment
{}

const std::string& Station::getName() const noexcept     { return name_;     }
int               Station::getDistance() const noexcept  { return distance_; }
int               Station::getStationID() const noexcept { return station_id_; }

int Station::getDistanceBetween(const Station& other) const noexcept {
    return std::abs(distance_ - other.distance_);
}
