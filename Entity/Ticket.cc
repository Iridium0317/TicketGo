#include "Ticket.h"
#include <vector>

// The global station table is owned by server_main.cc.
extern std::vector<Station> all_stations;

// Static member definitions.
std::atomic<int> Ticket::ticket_quantity_{0};
std::atomic<int> Ticket::total_income_{0};

Ticket::Ticket(int user_id, int train_id, int from_station_abs, int to_station_abs)
    : user_id_(user_id)
    , train_id_(train_id)
    , from_station_(from_station_abs)
    , to_station_(to_station_abs)
{
    // Price = half the geographic distance between the two absolute stations.
    price_     = all_stations[from_station_].getDistanceBetween(all_stations[to_station_]) / 2;
    ticket_id_ = ticket_quantity_.fetch_add(1, std::memory_order_relaxed);
    total_income_.fetch_add(price_, std::memory_order_relaxed);
}

int Ticket::getUserID()   const noexcept { return user_id_;   }
int Ticket::getTicketID() const noexcept { return ticket_id_; }
int Ticket::getPrice()    const noexcept { return price_;      }
int Ticket::getTrainID()  const noexcept { return train_id_;   }

const std::string& Ticket::getFromStationName() const noexcept {
    return all_stations[from_station_].getName();
}
const std::string& Ticket::getToStationName() const noexcept {
    return all_stations[to_station_].getName();
}

bool Ticket::getIsRefund() const noexcept { return is_refund_; }

void Ticket::setRefunded() {
    is_refund_ = true;
    total_income_.fetch_sub(price_, std::memory_order_relaxed);
}

void Ticket::setRelativeStation(int from, int to) noexcept {
    relative_from_ = from;
    relative_to_   = to;
}

int Ticket::getRelativeFrom() const noexcept { return relative_from_; }
int Ticket::getRelativeTo()   const noexcept { return relative_to_;   }
