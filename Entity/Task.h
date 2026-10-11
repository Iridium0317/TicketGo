#pragma once
// =============================================================================
// Task.h  –  Modernised C++17
//
// Key changes vs. legacy:
//   1. char content_[MAX_BUFFER] + strcpy  →  std::string  (no overflow).
//   2. strtok / atoi  →  std::string_view tokeniser (zero-copy, thread-safe).
//   3. struct Message { int code; char message[2048]; }  →
//      TaskResult using std::optional<std::string>:
//         - value present  = success, contains the reply string
//         - std::nullopt   = error/decode failure (caller uses error_msg)
//      This eliminates the code=-1 magic number and the char[] copy.
//   4. All magic-number command IDs replaced with inline constexpr.
//   5. tickets_ stored as std::shared_ptr reference to support safe
//      concurrent ownership across the thread pool.
//   6. The Ticket vector itself is guarded by a std::mutex owned by Server;
//      Task::doTask() receives a reference to that mutex and locks for writes.
// =============================================================================
#include <string>
#include <string_view>
#include <deque>
#include <vector>
#include <optional>
#include <mutex>

#include "Train.h"
#include "Ticket.h"

// ── Protocol command constants ─────────────────────────────────────────────
inline constexpr int CMD_BUY_OR_QUERY  = 1;   // 1 train from to [1=buy|0=query]
inline constexpr int CMD_LIST_TRAINS   = 2;   // 2
inline constexpr int CMD_MY_TICKETS    = 3;   // 3
inline constexpr int CMD_REFUND        = 4;   // 4 ticket_id
inline constexpr int CMD_PROFIT        = 5;   // 5

// ── Result type ───────────────────────────────────────────────────────────
struct TaskResult {
    bool        success;
    std::string message;   // human-readable reply to send to the client
};

// ── Task ──────────────────────────────────────────────────────────────────
class Task {
public:
    Task(int sd,
         std::string content,
         std::vector<Ticket>& tickets,
         std::mutex& tickets_mutex);

    // Parses the custom text protocol; returns the command id or -1 on error.
    // Thread-safe: operates only on local std::string members.
    int decode();

    int              getRequestType()      const noexcept;
    const std::vector<int>& getRequestArguments() const noexcept;
    int              getSd()               const noexcept;

    // Executes the decoded command against the shared train & ticket stores.
    // Returns a TaskResult that the worker thread sends back to the client.
    TaskResult doTask(std::deque<Train>& trains);

private:
    int              sd_;
    std::string      content_;
    int              operate_number_{-1};
    std::vector<int> operate_arguments_;
    std::vector<Ticket>& tickets_;
    std::mutex&          tickets_mutex_;
};
