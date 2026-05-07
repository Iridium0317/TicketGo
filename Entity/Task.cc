#include "Task.h"

#include <charconv>    // std::from_chars  (C++17, no locale, no heap)
#include <sstream>
#include <iostream>
#include <vector>

extern std::vector<Station> all_stations;

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
Task::Task(int sd,
           std::string content,
           std::vector<Ticket>& tickets,
           std::mutex& tickets_mutex)
    : sd_(sd)
    , content_(std::move(content))
    , tickets_(tickets)
    , tickets_mutex_(tickets_mutex)
{}

// ---------------------------------------------------------------------------
// decode()
//
// Parses the whitespace-delimited protocol string using std::string_view so
// that no heap allocation or copy occurs during tokenisation.
//
// Protocol format: "<cmd> [arg0] [arg1] ..."
//   cmd 1: "1 <train> <from> <to> <0|1>"
//   cmd 2: "2"
//   cmd 3: "3"
//   cmd 4: "4 <ticket_id>"
//   cmd 5: "5"
// ---------------------------------------------------------------------------
int Task::decode() {
    operate_arguments_.clear();
    operate_number_ = -1;

    std::string_view sv(content_);
    // Strip leading / trailing whitespace and any \r\n from the read() call.
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\r' || sv.front() == '\n'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back()  == ' ' || sv.back()  == '\r' || sv.back()  == '\n'))
        sv.remove_suffix(1);

    if (sv.empty()) return -1;

    bool first = true;
    while (!sv.empty()) {
        // Find the next token boundary.
        const auto space_pos = sv.find(' ');
        std::string_view token = sv.substr(0, space_pos);

        if (!token.empty()) {
            int value{};
            // std::from_chars is locale-independent, bounds-safe, and does
            // not require a null-terminated string.
            const auto [ptr, ec] = std::from_chars(token.data(),
                                                   token.data() + token.size(),
                                                   value);
            if (ec != std::errc{}) return -1;  // non-integer token → bad input

            if (first) {
                operate_number_ = value;
                first = false;
            } else {
                operate_arguments_.push_back(value);
            }
        }

        if (space_pos == std::string_view::npos) break;
        sv.remove_prefix(space_pos + 1);
    }

    return operate_number_;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------
int Task::getRequestType()  const noexcept { return operate_number_; }
const std::vector<int>& Task::getRequestArguments() const noexcept {
    return operate_arguments_;
}
int Task::getSd() const noexcept { return sd_; }

// ---------------------------------------------------------------------------
// Helper: build a separator line
// ---------------------------------------------------------------------------
static const std::string kSep = "----------------------------------------\n";

// ---------------------------------------------------------------------------
// doTask()
// ---------------------------------------------------------------------------
TaskResult Task::doTask(std::vector<Train>& trains) {
    if (decode() == -1) {
        return {false, "操作失败，请检查您的输入，并重试。\n"};
    }

    const auto& args = operate_arguments_;
    std::string msg;
    msg.reserve(512);

    switch (operate_number_) {

    // ── cmd 1: buy or query ──────────────────────────────────────────────
    // Protocol: "1 <train_idx> <from_rel> <to_rel> <1=buy|0=query>"
    case CMD_BUY_OR_QUERY: {
        if (args.size() < 4) {
            return {false, "操作失败，请检查您的输入，并重试。\n"};
        }
        const int train_idx = args[0];
        const int from_rel  = args[1];
        const int to_rel    = args[2];
        const int do_buy    = args[3];

        if (train_idx < 0 || train_idx >= static_cast<int>(trains.size())) {
            return {false, "车次编号无效，请重试。\n"};
        }

        Train& train = trains[train_idx];

        msg += kSep;
        msg += "您选择的车票当前剩余：";
        msg += std::to_string(train.getTicketNumber(from_rel, to_rel));
        msg += "\n";
        msg += kSep;

        if (do_buy == 0) {
            // Query-only – no write needed.
            return {true, std::move(msg)};
        }

        // Attempt purchase: buyTicket() acquires unique_lock internally.
        if (!train.buyTicket(from_rel, to_rel)) {
            return {false, "操作失败，余票不足或站点无效，请重试。\n"};
        }

        // Create the ticket record – guard the shared tickets vector.
        const int abs_from = train.getAbsoluteStationID(from_rel);
        const int abs_to   = train.getAbsoluteStationID(to_rel);

        std::unique_lock<std::mutex> tlock(tickets_mutex_);
        Ticket& newTicket = tickets_.emplace_back(sd_, train.getTrainID(),
                                                  abs_from, abs_to);
        newTicket.setRelativeStation(from_rel, to_rel);
        // Take a snapshot of the values we need before releasing the lock.
        const int  tid   = newTicket.getTicketID();
        const int  trid  = newTicket.getTrainID();
        const auto from_name = newTicket.getFromStationName();
        const auto to_name   = newTicket.getToStationName();
        const int  price = newTicket.getPrice();
        tlock.unlock();

        msg += "预定成功！您的订单如下：\n";
        msg += kSep;
        msg += "订单号：";   msg += std::to_string(tid);   msg += "\n";
        msg += "车次：";     msg += std::to_string(trid);  msg += "\n";
        msg += "站点：";     msg += from_name; msg += " ---> "; msg += to_name; msg += "\n";
        msg += "售价： ¥";  msg += std::to_string(price); msg += "\n";
        msg += kSep;
        return {true, std::move(msg)};
    }

    // ── cmd 2: list all trains ───────────────────────────────────────────
    case CMD_LIST_TRAINS: {
        for (int i = 0; i < static_cast<int>(trains.size()); ++i) {
            msg += '['; msg += std::to_string(i); msg += "] ";
            for (int j = 0; j < trains[i].getStationNumber(); ++j) {
                msg += '('; msg += std::to_string(j); msg += ')';
                msg += all_stations[trains[i].getAbsoluteStationID(j)].getName();
                if (j != trains[i].getStationNumber() - 1)
                    msg += " -> ";
            }
            msg += '\n'; msg += kSep;
        }
        return {true, std::move(msg)};
    }

    // ── cmd 3: my tickets ────────────────────────────────────────────────
    case CMD_MY_TICKETS: {
        msg += kSep;
        bool found = false;

        std::unique_lock<std::mutex> tlock(tickets_mutex_);
        for (const auto& t : tickets_) {
            if (t.getUserID() != sd_) continue;
            found = true;
            msg += '['; msg += std::to_string(t.getTicketID()); msg += "]  ";
            msg += "车次：";   msg += std::to_string(t.getTrainID());  msg += ",  ";
            msg += "站点：";   msg += t.getFromStationName(); msg += " ---> ";
                               msg += t.getToStationName();   msg += ",  ";
            msg += "售价： ¥"; msg += std::to_string(t.getPrice());   msg += ",  ";
            msg += "是否退票： "; msg += (t.getIsRefund() ? "是" : "否"); msg += "\n";
        }
        tlock.unlock();

        if (!found) msg += "您当前没有订单！\n";
        return {true, std::move(msg)};
    }

    // ── cmd 4: refund ────────────────────────────────────────────────────
    // Protocol: "4 <ticket_id>"
    case CMD_REFUND: {
        if (args.empty()) {
            return {false, "发生错误。请提供订单号。\n"};
        }
        const int ticket_id = args[0];

        std::unique_lock<std::mutex> tlock(tickets_mutex_);

        if (ticket_id < 0 || ticket_id >= static_cast<int>(tickets_.size())) {
            return {false, "发生错误。该票不存在，请重试。\n"};
        }
        Ticket& t = tickets_[ticket_id];
        if (t.getUserID() != sd_ || t.getIsRefund()) {
            msg += "发生错误。该票已退换或您的输入有误，请重试。\n";
            return {true, std::move(msg)};
        }

        // Validate that the relative station pair was stored.
        const int rel_from = t.getRelativeFrom();
        const int rel_to   = t.getRelativeTo();
        if (rel_from == 0 && rel_to == 0) {
            return {false, "很抱歉，该票无法退换。\n"};
        }
        const int train_id = t.getTrainID();
        const int price    = t.getPrice();
        t.setRefunded();
        tlock.unlock();

        // Restore seats – unique_lock acquired inside refundTicket.
        trains[train_id].refundTicket(rel_from, rel_to);

        msg += "退票成功！退还金额：¥";
        msg += std::to_string(price);
        msg += "\n";
        return {true, std::move(msg)};
    }

    // ── cmd 5: profit summary ────────────────────────────────────────────
    case CMD_PROFIT: {
        msg += kSep;
        msg += "总收益：¥";
        msg += std::to_string(Ticket::total_income_.load(std::memory_order_relaxed));
        msg += "\n";

        std::unique_lock<std::mutex> tlock(tickets_mutex_);
        for (const auto& t : tickets_) {
            msg += '['; msg += std::to_string(t.getTicketID()); msg += "]  ";
            msg += "车次：";   msg += std::to_string(t.getTrainID());  msg += ",  ";
            msg += "站点：";   msg += t.getFromStationName(); msg += " ---> ";
                               msg += t.getToStationName();   msg += ",  ";
            msg += "售价： ¥"; msg += std::to_string(t.getPrice());   msg += ",  ";
            msg += "是否退票： "; msg += (t.getIsRefund() ? "是" : "否"); msg += "\n";
        }
        tlock.unlock();

        if (tickets_.empty()) msg += "当前没有订单！\n";
        return {true, std::move(msg)};
    }

    default:
        return {false, "未知操作，请检查您的输入，并重试。\n"};
    }
}
