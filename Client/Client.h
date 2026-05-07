#pragma once
// =============================================================================
// Client.h  –  Modernised C++17
//
// Changes vs. legacy:
//   * #define macros  →  inline constexpr
//   * sprintf / char toSend[MAX_BUFFER]  →  std::string / snprintf-free
//     formatting using std::to_string and operator+
//   * scanf stays (stdio interaction) but input validation is added
//   * buffer_ exposed as std::string to avoid stack-buffer overflows
//   * All methods that don't mutate state are const-qualified
// =============================================================================
#include <string>

inline constexpr int  CLIENT_PORT   = 8888;
inline constexpr int  CLIENT_BUFFER = 2048;

class Client {
public:
    // Connects to 127.0.0.1:CLIENT_PORT.
    // Returns 0 on success, 1 on failure.
    int  init();

    // Main REPL loop.
    int  core();

private:
    // ── Network helpers ──────────────────────────────────────────────────
    // Sends `content` to the server and prints the response.
    // Returns 0 on success, non-zero on error.
    int  sendRequest(const std::string& content);

    // ── UI helpers ───────────────────────────────────────────────────────
    void welcomePrinter() const;
    int  operations()     const;   // returns user's menu choice

    int  showAllTrains();
    int  buyTicketByTrain();
    int  checkAvailableSeats();
    int  checkMyTickets();
    int  refund();
    int  checkIncome();

    // ── Members ──────────────────────────────────────────────────────────
    int         sock_{-1};
    std::string recv_buf_;   // reused across calls; avoids stack VLA
};
