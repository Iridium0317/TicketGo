#include "Server.h"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <iostream>
#include <stdexcept>
#include <algorithm>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
Server::Server(std::deque<Train> trains)
    : trains_(std::move(trains))
{
    fd_to_slot_.assign(FD_MAP_SIZE, -1);
    fd_conn_id_.assign(FD_MAP_SIZE, 0);
    fd_inflight_.assign(FD_MAP_SIZE, 0);
    fd_closing_.assign(FD_MAP_SIZE, 0);
    fd_pending_.resize(FD_MAP_SIZE);
    inbuf_.resize(FD_MAP_SIZE);
    client_fd_.assign(BitmapAllocator::CAPACITY, -1);
}

Server::~Server() {
    stop();
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
    if (listen_fd_ >= 0) ::close(listen_fd_);
}

// ---------------------------------------------------------------------------
// setNonBlocking  –  static helper
// ---------------------------------------------------------------------------
bool Server::setNonBlocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

// ---------------------------------------------------------------------------
// epollAdd
// ---------------------------------------------------------------------------
bool Server::epollAdd(int fd) noexcept {
    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;   // edge-triggered
    ev.data.fd = fd;
    return ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

// ---------------------------------------------------------------------------
// epollRemove
// ---------------------------------------------------------------------------
void Server::epollRemove(int fd) noexcept {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    if (fd >= FD_MAP_SIZE) {
        ::close(fd);
        return;
    }
    // Event-loop thread only: drop any partial command and its memory.
    std::string().swap(inbuf_[fd]);

    std::lock_guard<std::mutex> lock(conn_mutex_);
    // A worker still holds a task for this fd: closing now would let
    // accept() hand the same fd to a new client before that worker sends
    // its reply.  The last worker closes it instead (see workerLoop).
    if (fd_inflight_[fd] > 0) {
        fd_closing_[fd] = 1;
        return;
    }
    closeConnLocked(fd);
}

// ---------------------------------------------------------------------------
// closeConnLocked  –  caller holds conn_mutex_
// ---------------------------------------------------------------------------
void Server::closeConnLocked(int fd) noexcept {
    ::close(fd);
    fd_conn_id_[fd] = 0;
    fd_closing_[fd] = 0;
    fd_pending_[fd].clear();

    // Return the slot to the allocator.
    if (fd_to_slot_[fd] != -1) {
        slot_alloc_.free(fd_to_slot_[fd]);
        client_fd_[fd_to_slot_[fd]] = -1;
        fd_to_slot_[fd] = -1;
    }
}

// ---------------------------------------------------------------------------
// initServer
// ---------------------------------------------------------------------------
int Server::initServer() {
    // ── Create listen socket ─────────────────────────────────────────────
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        ::perror("socket");
        return 1;
    }

    const int opt = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        ::perror("setsockopt");
        return 1;
    }

    if (!setNonBlocking(listen_fd_)) {
        ::perror("fcntl listen_fd");
        return 1;
    }

    address_.sin_family      = AF_INET;
    address_.sin_addr.s_addr = INADDR_ANY;
    address_.sin_port        = htons(SERVER_PORT);

    if (::bind(listen_fd_,
               reinterpret_cast<sockaddr*>(&address_),
               sizeof(address_)) < 0) {
        ::perror("bind");
        return 1;
    }

    if (::listen(listen_fd_, LISTEN_BACKLOG) < 0) {
        ::perror("listen");
        return 1;
    }

    std::cout << "TicketGo server listening on port " << SERVER_PORT << '\n';

    // ── Create epoll instance ────────────────────────────────────────────
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        ::perror("epoll_create1");
        return 1;
    }

    if (!epollAdd(listen_fd_)) {
        ::perror("epoll_ctl listen_fd");
        return 1;
    }

    // ── Start worker threads ─────────────────────────────────────────────
    for (auto& t : workers_) {
        t = std::thread(&Server::workerLoop, this);
    }

    std::cout << "Worker pool started (" << WORKER_THREAD_COUNT << " threads)\n";
    return 0;
}

// ---------------------------------------------------------------------------
// run  –  epoll event loop (main thread only)
// ---------------------------------------------------------------------------
void Server::run() {
    std::array<epoll_event, MAX_EVENTS> events;

    while (!stop_flag_.load(std::memory_order_relaxed)) {
        const int n = ::epoll_wait(epoll_fd_, events.data(),
                                   static_cast<int>(events.size()), -1);
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted by signal – retry
            ::perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;

            if (fd == listen_fd_) {
                // ── New connection ────────────────────────────────────────
                handleNewConnection();
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                // ── Error or hang-up ──────────────────────────────────────
                std::cout << "Client fd=" << fd << " error/hup; closing.\n";
                epollRemove(fd);
            } else if (events[i].events & EPOLLIN) {
                // ── Data available ────────────────────────────────────────
                if (!handleClientData(fd)) {
                    epollRemove(fd);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------
void Server::stop() {
    if (stop_flag_.exchange(true)) return;  // already stopping

    // Wake all workers so they can exit their wait.
    queue_cv_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
}

// ---------------------------------------------------------------------------
// handleNewConnection
// Called ONLY from the main-thread event loop.
// Because epoll_wait is single-threaded here, conn_mutex_ protects the
// allocator from potential future multi-threaded accept() extensions.
// ---------------------------------------------------------------------------
void Server::handleNewConnection() {
    // Edge-triggered: drain all pending accept() calls.
    while (true) {
        sockaddr_in client_addr{};
        socklen_t   addr_len = sizeof(client_addr);

        const int client_fd = ::accept4(listen_fd_,
                                        reinterpret_cast<sockaddr*>(&client_addr),
                                        &addr_len,
                                        SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // no more
            ::perror("accept4");
            break;
        }

        // ── Allocate a bitmap slot ────────────────────────────────────────
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (client_fd >= FD_MAP_SIZE || slot_alloc_.full()) {
            std::cerr << "Connection limit reached; rejecting fd=" << client_fd << '\n';
            ::close(client_fd);
            continue;
        }

        const int slot = slot_alloc_.allocate();
        fd_to_slot_[client_fd] = slot;
        client_fd_[slot]       = client_fd;
        fd_conn_id_[client_fd] = next_conn_id_++;

        if (!epollAdd(client_fd)) {
            ::perror("epoll_ctl client");
            slot_alloc_.free(slot);
            fd_to_slot_[client_fd] = -1;
            client_fd_[slot]       = -1;
            fd_conn_id_[client_fd] = 0;
            ::close(client_fd);
            continue;
        }

        char ip_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, sizeof(ip_str));
        std::cout << "New connection fd=" << client_fd
                  << "  ip=" << ip_str
                  << "  port=" << ntohs(client_addr.sin_port)
                  << "  slot=" << slot << '\n';
    }
}

// ---------------------------------------------------------------------------
// handleClientData
// Called ONLY from the main-thread event loop.
// Edge-triggered: we must read until EAGAIN/EWOULDBLOCK.
// Returns false if the connection should be closed.
//
// TCP delivers a byte stream, not messages: one read() may hold two
// commands, or half of one.  Bytes are appended to the connection's inbuf_
// and only complete '\n'-terminated lines become tasks; a trailing partial
// line stays in inbuf_ until the rest of it arrives.
// ---------------------------------------------------------------------------
bool Server::handleClientData(int fd) {
    char buf[MAX_BUFFER];
    std::string& in = inbuf_[fd];   // fd < FD_MAP_SIZE, checked at accept
    bool peer_closed = false;

    while (true) {
        const ssize_t n = ::read(fd, buf, MAX_BUFFER);

        if (n > 0) {
            in.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            // Peer closed.  Still run the complete commands read above.
            peer_closed = true;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // done
            ::perror("read");
            return false;
        }
    }

    // ── Split complete lines off the front of the buffer ─────────────────
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = in.find('\n', start);
        if (nl == std::string::npos) break;

        std::string line = in.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        if (line.empty()) continue;

        std::cout << "Received from [" << fd << "]: " << line << '\n';
        // Hand off to worker pool – main thread does NO business logic here.
        if (!pushTask(fd, std::move(line))) {
            std::cerr << "Too many pipelined commands; dropping fd=" << fd << '\n';
            return false;
        }
    }
    in.erase(0, start);

    if (in.size() > MAX_LINE_BYTES) {
        std::cerr << "Command too long; dropping fd=" << fd << '\n';
        return false;
    }

    if (peer_closed) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &len);
        char ip_str[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &peer.sin_addr, ip_str, sizeof(ip_str));
        std::cout << "Client disconnected  ip=" << ip_str
                  << "  fd=" << fd << '\n';
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// pushTask  –  producer side
//
// A connection has at most one task queued or running.  If two commands
// from one client went to two workers, the replies could come back in the
// wrong order, so later commands wait in fd_pending_ and the worker that
// finishes the current one queues the next (see workerLoop).
// ---------------------------------------------------------------------------
bool Server::pushTask(int sd, std::string content) {
    std::uint64_t conn_id;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        if (fd_inflight_[sd] > 0) {
            if (fd_pending_[sd].size() >= MAX_PENDING_PER_CONN) return false;
            fd_pending_[sd].push_back(std::move(content));
            return true;
        }
        conn_id = fd_conn_id_[sd];
        fd_inflight_[sd] = 1;   // keeps sd open until the reply is sent
    }

    // Task stores a reference to tickets_ and tickets_mutex_; both have
    // server lifetime, so this is safe.
    enqueueTask(Task(sd, conn_id, std::move(content), tickets_, tickets_mutex_));
    return true;
}

// ---------------------------------------------------------------------------
// enqueueTask
// ---------------------------------------------------------------------------
void Server::enqueueTask(Task task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

// ---------------------------------------------------------------------------
// workerLoop  –  consumer side (runs on each worker thread)
// ---------------------------------------------------------------------------
void Server::workerLoop() {
    while (true) {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // Wait until there is work OR we have been asked to stop.
        queue_cv_.wait(lock, [this] {
            // // 等待，直到（条件）为真
            return !task_queue_.empty() || stop_flag_.load(std::memory_order_relaxed);
        });

        if (task_queue_.empty()) {
            // stop_flag_ is set and queue is drained – exit cleanly.
            break;
        }

        Task task = std::move(task_queue_.front());
        task_queue_.pop();
        lock.unlock();   // release before doing any I/O

        // ── Execute business logic ────────────────────────────────────────
        // trains_ is only mutated via Train::buyTicket/refundTicket which
        // each hold their own shared_mutex internally → safe here.
        TaskResult result = task.doTask(trains_);

        // ── Send reply ────────────────────────────────────────────────────
        const std::string& reply = result.message;
        if (!reply.empty()) {
            // Loop to handle partial sends on a non-blocking socket.
            std::size_t sent = 0;
            while (sent < reply.size()) {
                const ssize_t s = ::send(task.getSd(),
                                         reply.data() + sent,
                                         reply.size() - sent,
                                         MSG_NOSIGNAL);
                if (s <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        // Socket buffer full – yield and retry.
                        std::this_thread::yield();
                        continue;
                    }
                    break;   // client disconnected mid-send
                }
                sent += static_cast<std::size_t>(s);
            }
        }

        // ── Queue this connection's next command, or release the fd ───────
        // If the client disconnected while this task was pending, the event
        // loop deferred the close to us.
        {
            const int fd = task.getSd();
            std::string   next;
            std::uint64_t conn_id = 0;
            bool          has_next = false;
            {
                std::lock_guard<std::mutex> conn_lock(conn_mutex_);
                if (!fd_pending_[fd].empty()) {
                    // fd stays in flight; the next command keeps it open.
                    next = std::move(fd_pending_[fd].front());
                    fd_pending_[fd].pop_front();
                    conn_id  = fd_conn_id_[fd];
                    has_next = true;
                } else {
                    fd_inflight_[fd] = 0;
                    if (fd_closing_[fd]) closeConnLocked(fd);
                }
            }
            // conn_mutex_ is released before queue_mutex_ is taken, so the
            // two are never held together.
            if (has_next) {
                enqueueTask(Task(fd, conn_id, std::move(next),
                                 tickets_, tickets_mutex_));
            }
        }
    }
}
