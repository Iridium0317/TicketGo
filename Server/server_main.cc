// =============================================================================
// server_main.cc  –  TicketGo Modern C++17
//
// Responsibilities:
//   1. Define the global station table (all_stations).
//      extern'd by Ticket.cc / Task.cc / Train.cc.
//   2. Build the initial Train fleet and hand ownership to Server.
//   3. Wire up a SIGINT/SIGTERM handler for graceful shutdown.
// =============================================================================
#include "Server.h"
#include "../Entity/Station.h"
#include "../Entity/Ticket.h"
#include "../Entity/Train.h"

#include <csignal>
#include <iostream>
#include <vector>

// ── Global station table ─────────────────────────────────────────────────
// Owned by this translation unit; extern'd by Train.cc, Ticket.cc, Task.cc.
std::vector<Station> all_stations;

// ── Static definitions for Station's counter ────────────────────────────
// Station.cc defines station_quantity_; declared extern in Station.h.

// ── Global Server pointer for signal handling ────────────────────────────
static Server* g_server = nullptr;

static void signalHandler(int /*sig*/) {
    std::cout << "\nShutting down…\n";
    if (g_server) g_server->stop();
}

// ── Station initialisation ────────────────────────────────────────────────
static void initStations() {
    all_stations = {
        {"北京",   0},
        {"石家庄",  50},
        {"天津",   100},
        {"济南",   200},
        {"青岛",   230},
        {"威海",   270},
        {"徐州",   500},
        {"南京",   600},
        {"苏州",   650},
        {"宿迁",   700},
        {"芜湖",   800},
        {"合肥",   900},
        {"杭州",   1000},
        {"舟山",   1200},
        {"上海",   1300},
        {"广州",   2000},
        {"深圳",   2200},
    };
}

// ── Train fleet ───────────────────────────────────────────────────────────
static std::vector<Train> createTrains() {
    // Train(capacity, station_number, train_id)
    // station_number must be in [2, all_stations.size()].
    std::vector<Train> fleet;
    fleet.emplace_back(100, 11, 0);
    fleet.emplace_back(100, 13, 1);
    fleet.emplace_back(100, 15, 2);
    fleet.emplace_back(100, 12, 3);
    return fleet;
}

// ── Entry point ───────────────────────────────────────────────────────────
int main() {
    // Initialise the global station table before any Train is constructed,
    // because the Train constructor reads all_stations.
    initStations();

    std::vector<Train> trains = createTrains();

    Server server(std::move(trains));

    g_server = &server;
    std::signal(SIGINT,  signalHandler);
    std::signal(SIGTERM, signalHandler);

    if (server.initServer() != 0) {
        std::cerr << "Failed to initialise server.\n";
        return 1;
    }

    server.run();   // blocks until stop_flag_ is set via signal

    std::cout << "Server exited cleanly.\n";
    return 0;
}
