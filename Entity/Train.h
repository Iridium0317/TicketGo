#pragma once
// =============================================================================
// Train.h  –  Modernised C++17
//
// Key changes vs. legacy:
//   1. std::shared_mutex replaces std::mutex → true read-write separation.
//      - Queries (getTicketNumber, isEnoughTickets) → std::shared_lock  (read)
//      - Mutations (buyTicket, refundTicket)        → std::unique_lock  (write)
//   2. rand() removed entirely; constructor uses std::mt19937 +
//      std::uniform_int_distribution via std::sample (C++17) for O(n) station
//      selection with guaranteed termination.
//   3. Constructor validates station_number to prevent:
//        a) Underflow when station_number > all_stations.size()
//        b) The off-by-one rand() % (size+1) bug that could insert an
//           out-of-range sentinel into the skip-set, preventing the while-loop
//           from ever terminating.
//   4. Private isEnoughTicketsNoLock() avoids recursive locking: buyTicket()
//      acquires unique_lock then calls the lock-free helper internally.
// =============================================================================
#include <vector>
#include <shared_mutex>
#include <stdexcept>
#include "Station.h"

class Train {
public:
    static int train_quantity_;

    // Throws std::invalid_argument if station_number is out of range.
    Train(int capacity, int station_number, int train_id);

    // Write operations – acquire unique_lock internally.
    bool buyTicket   (int from_station, int to_station);
    void refundTicket(int from_station, int to_station);

    // Read operations – acquire shared_lock internally.
    bool isEnoughTickets(int from_station, int to_station) const;
    int  getTicketNumber(int from_station, int to_station) const;

    int getTrainID()              const noexcept;
    int getStationNumber()        const noexcept;
    int getAbsoluteStationID(int relative_id) const;

private:
    // Assumes the caller already holds at least a shared lock.
    bool isEnoughTicketsNoLock(int from_station, int to_station) const noexcept;

    const int train_id_;
    const int capacity_;
    const int station_number_;

    std::vector<Station> stations_;
    std::vector<int>     each_available_seats_;   // seats[i] → seg i→i+1
    std::vector<int>     max_sold_seats_;          // per-segment selling cap

    // mutable so that const read methods can still acquire the shared_lock.
    mutable std::shared_mutex rw_mutex_;
};
