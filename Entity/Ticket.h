#pragma once
// =============================================================================
// Ticket.h  –  Modernised C++17
// Changes vs. legacy:
//   * ticket_quantity_ / total_income_  →  std::atomic<int>  (safe across
//     worker threads without an extra lock on every construction/refund)
//   * getFromStationName / getToStationName  →  return const std::string&
//     (zero-copy, no dangling char* from a local Station)
//   * All accessor methods are const-qualified
// =============================================================================
#include <atomic>
#include <string>
#include "Station.h"

class Ticket {
public:
    // Shared counters – defined in Ticket.cc.
    static std::atomic<int> ticket_quantity_;
    static std::atomic<int> total_income_;

    // absolute station IDs into the global all_stations vector
    Ticket(int user_id, int train_id, int from_station_abs, int to_station_abs);

    int               getUserID()           const noexcept;
    int               getTicketID()         const noexcept;
    int               getPrice()            const noexcept;
    int               getTrainID()          const noexcept;
    const std::string& getFromStationName() const noexcept;
    const std::string& getToStationName()   const noexcept;
    bool              getIsRefund()         const noexcept;

    void setRefunded();
    void setRelativeStation(int from, int to) noexcept;
    int  getRelativeFrom() const noexcept;
    int  getRelativeTo()   const noexcept;

private:
    int  user_id_;
    int  ticket_id_;
    int  price_;
    int  train_id_;
    int  from_station_;        // absolute index into all_stations
    int  to_station_;          // absolute index into all_stations
    int  relative_from_{0};    // relative index within the train
    int  relative_to_{0};
    bool is_refund_{false};
};
