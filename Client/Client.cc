#include "Client.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
int Client::init() {
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0) {
        ::perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(CLIENT_PORT);

    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) <= 0) {
        ::perror("inet_pton");
        return 1;
    }

    if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::perror("connect (确认服务端已启动)");
        return 1;
    }

    recv_buf_.resize(CLIENT_BUFFER + 1);
    return 0;
}

// ---------------------------------------------------------------------------
// sendRequest
// ---------------------------------------------------------------------------
int Client::sendRequest(const std::string& content) {
    const ssize_t sent = ::send(sock_, content.data(), content.size(), 0);
    if (sent < 0) {
        ::perror("send");
        return 1;
    }

    const ssize_t n = ::recv(sock_, recv_buf_.data(), CLIENT_BUFFER, 0);
    if (n < 0) {
        ::perror("recv");
        return 1;
    }
    recv_buf_[n] = '\0';
    std::cout << recv_buf_.c_str() << '\n';
    return 0;
}

// ---------------------------------------------------------------------------
// welcomePrinter
// ---------------------------------------------------------------------------
void Client::welcomePrinter() const {
    std::cout << "========================================\n"
              << "欢迎使用 TicketGo 订票系统！\n"
              << "本系统默认采用优化后的放票模式。"
                 "因本系统为简单 Demo，不基于数据库，"
                 "因此系统将不会保存您的购买信息。\n";
}

// ---------------------------------------------------------------------------
// operations  –  prints menu, reads choice
// ---------------------------------------------------------------------------
int Client::operations() const {
    std::cout << "========================================\n"
              << "请输入您想进行的操作前面的数字！\n"
              << "[1] 购票\n"
              << "[2] 查看订单\n"
              << "[3] 退票\n"
              << "[4] 查询余票\n"
              << "[5] 查看车票收益\n";
    int choice = 0;
    std::scanf("%d", &choice);
    return choice;
}

// ---------------------------------------------------------------------------
// showAllTrains
// ---------------------------------------------------------------------------
int Client::showAllTrains() {
    std::cout << "========================================\n"
              << "以下为当前开设的列车\n";
    if (sendRequest("2") != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    std::cout << "========================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// buyTicketByTrain
// ---------------------------------------------------------------------------
int Client::buyTicketByTrain() {
    showAllTrains();
    std::cout << "请选择您想要购买的列车序号、起始站序号、终点站序号（以空格分隔）。\n"
              << "例如，选择列车[1],第(1)站至第(4)站，请输入 \"1 1 4\"\n"
              << "请输入：";

    int train_id{}, from_station{}, to_station{};
    std::scanf("%d%d%d", &train_id, &from_station, &to_station);

    // Build protocol string without sprintf.
    std::string req = "1 ";
    req += std::to_string(train_id);    req += ' ';
    req += std::to_string(from_station); req += ' ';
    req += std::to_string(to_station);  req += " 1";

    if (sendRequest(req) != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    std::cout << "========================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// checkAvailableSeats
// ---------------------------------------------------------------------------
int Client::checkAvailableSeats() {
    showAllTrains();
    std::cout << "请选择您想要查询余票的列车序号、起始站序号、终点站序号（以空格分隔）。\n"
              << "例如，选择列车[1],第(1)站至第(4)站，请输入 \"1 1 4\"\n"
              << "请输入：";

    int train_id{}, from_station{}, to_station{};
    std::scanf("%d%d%d", &train_id, &from_station, &to_station);

    std::string req = "1 ";
    req += std::to_string(train_id);    req += ' ';
    req += std::to_string(from_station); req += ' ';
    req += std::to_string(to_station);  req += " 0";

    if (sendRequest(req) != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    std::cout << "========================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// checkMyTickets
// ---------------------------------------------------------------------------
int Client::checkMyTickets() {
    std::cout << "========================================\n";
    if (sendRequest("3") != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    std::cout << "========================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// refund
// ---------------------------------------------------------------------------
int Client::refund() {
    std::cout << "以下为您的所有订单（含已退票），请输入想要退票的订单号：\n";
    checkMyTickets();

    int order_id{};
    std::scanf("%d", &order_id);

    std::string req = "4 ";
    req += std::to_string(order_id);

    if (sendRequest(req) != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// checkIncome
// ---------------------------------------------------------------------------
int Client::checkIncome() {
    std::cout << "========================================\n";
    if (sendRequest("5") != 0) {
        std::cout << "出错了，请重试。\n";
        return 1;
    }
    std::cout << "========================================\n";
    return 0;
}

// ---------------------------------------------------------------------------
// core  –  main REPL loop
// ---------------------------------------------------------------------------
int Client::core() {
    welcomePrinter();
    while (true) {
        switch (operations()) {
        case 1:  buyTicketByTrain();                           break;
        case 2:  std::cout << "以下为您的所有订单：\n";
                 checkMyTickets();                             break;
        case 3:  refund();                                     break;
        case 4:  checkAvailableSeats();                        break;
        case 5:  checkIncome();                                break;
        default: std::cout << "出错了。请检查您的输入，再试一次。\n"; break;
        }
    }
}
