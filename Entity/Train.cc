#include "Train.h"

#include <algorithm>    // std::sample
#include <numeric>      // std::iota
#include <random>       // std::mt19937, std::random_device
#include <iostream>
#include <set>
#include <vector>

extern std::vector<Station> all_stations;

int Train::train_quantity_ = 0;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Train::Train(int capacity, int station_number, int train_id)
    : capacity_(capacity)
    , station_number_(station_number)
    , train_id_(train_id)
{
    const int total = static_cast<int>(all_stations.size());

    // ── Sanity / boundary checks ──────────────────────────────────────────
    // Legacy bug: if station_number > total, the expression
    //   (all_stations.size() - station_number) wraps around to a huge
    //   unsigned value, causing an infinite while-loop.
    if (station_number_ < 2) {
        throw std::invalid_argument(
            "Train: station_number must be >= 2, got " +
            std::to_string(station_number_));
    }
    if (station_number_ > total) {
        throw std::invalid_argument(
            "Train: station_number (" + std::to_string(station_number_) +
            ") exceeds all_stations.size() (" + std::to_string(total) + ')');
    }

    // ── Seat-availability arrays ──────────────────────────────────────────
    // each_available_seats_[i] = available seats on segment i → i+1
    each_available_seats_.assign(station_number_ - 1, capacity_);

    // max_sold_seats_[i] = selling cap for the segment closest to the
    // train's middle (mirrors legacy logic, prevents over-booking).
    max_sold_seats_.reserve(station_number_ - 1);
    for (int i = 0; i < station_number_ - 1; ++i) {
        int denom = (station_number_ - (i + 1)) * (i + 1);
        max_sold_seats_.push_back(capacity_ * station_number_ / denom);
    }

    // ── Station selection (C++17 std::sample) ─────────────────────────────
    // Legacy bug: rand() % (all_stations.size() + 1) could generate the
    // out-of-range index `all_stations.size()`, which never matched any real
    // station in the for-loop, making the not_passed set fill with garbage
    // and causing wrong station counts or an infinite loop.
    //
    // Fix: build a [0, total-1] index vector and use std::sample to select
    // exactly `to_skip` unique indices to skip.  Guaranteed O(n) and no
    // infinite loop possible.
    std::mt19937 rng(std::random_device{}());

    const int to_skip = total - station_number_;
    std::vector<int> all_indices(total);
    std::iota(all_indices.begin(), all_indices.end(), 0);

    std::set<int> skip_set;
    if (to_skip > 0) {
        std::vector<int> skipped;
        skipped.reserve(to_skip);
        std::sample(all_indices.begin(), all_indices.end(),
                    std::back_inserter(skipped),
                    to_skip, rng);
        skip_set.insert(skipped.begin(), skipped.end());
    }

    std::cout << "Train " << train_id_ << ": ";
    for (int i = 0; i < total; ++i) {
        if (!skip_set.count(i)) {
            stations_.push_back(all_stations[i]);
            std::cout << all_stations[i].getName() << ' ';
        }
    }
    std::cout << '\n';

    ++train_quantity_;
}

// ---------------------------------------------------------------------------
// Private helper – no locking; caller must hold at least a shared lock.
// ---------------------------------------------------------------------------
bool Train::isEnoughTicketsNoLock(int from_station, int to_station) const noexcept {
    if (to_station <= from_station
        || to_station  >= station_number_
        || from_station < 0) {
        return false;
    }

    // Per-segment selling cap (mirrors legacy formula)
    const int min_s = (from_station > station_number_ - 2 - to_station)
                      ? to_station - 1
                      : from_station;
    if (max_sold_seats_[min_s] <= 0) return false;

    for (int i = from_station; i < to_station; ++i) {
        if (each_available_seats_[i] <= 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Public read operations – shared_lock
// ---------------------------------------------------------------------------
bool Train::isEnoughTickets(int from_station, int to_station) const {
    std::shared_lock lock(rw_mutex_);
    return isEnoughTicketsNoLock(from_station, to_station);
}

int Train::getTicketNumber(int from_station, int to_station) const {
    std::shared_lock lock(rw_mutex_);

    if (to_station <= from_station
        || to_station  >= station_number_
        || from_station < 0) {
        return 0;
    }

    const int min_s = (from_station > station_number_ - 2 - to_station)
                      ? to_station - 1
                      : from_station;
    if (max_sold_seats_[min_s] <= 0) return 0;

    int min_avail = capacity_ + 1;
    for (int i = from_station; i < to_station; ++i) {
        if (each_available_seats_[i] <= 0) return 0;
        if (each_available_seats_[i] < min_avail)
            min_avail = each_available_seats_[i];
    }

    return std::min(min_avail, max_sold_seats_[min_s]);
}

// ---------------------------------------------------------------------------
// Public write operations – unique_lock
// ---------------------------------------------------------------------------
bool Train::buyTicket(int from_station, int to_station) {
    std::unique_lock lock(rw_mutex_);

    // Re-check availability inside the write lock (no TOCTOU race).
    if (!isEnoughTicketsNoLock(from_station, to_station)) return false;

    for (int i = from_station; i < to_station; ++i)
        --each_available_seats_[i];

    const int min_s = (from_station > station_number_ - 2 - to_station)
                      ? to_station - 1
                      : from_station;
    --max_sold_seats_[min_s];
    return true;
}

void Train::refundTicket(int from_station, int to_station) {
    std::unique_lock lock(rw_mutex_);

    for (int i = from_station; i < to_station; ++i)
        ++each_available_seats_[i];

    const int min_s = (from_station > station_number_ - 2 - to_station)
                      ? to_station - 1
                      : from_station;
    ++max_sold_seats_[min_s];
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int Train::getTrainID()       const noexcept { return train_id_;       }
int Train::getStationNumber() const noexcept { return station_number_; }

int Train::getAbsoluteStationID(int relative_id) const {
    // Bounds-check to prevent segfault on bad client input.
    if (relative_id < 0 || relative_id >= static_cast<int>(stations_.size())) {
        throw std::out_of_range(
            "Train::getAbsoluteStationID: relative_id " +
            std::to_string(relative_id) + " out of range");
    }
    return stations_[relative_id].getStationID();
}
