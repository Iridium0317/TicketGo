cmake_minimum_required(VERSION 3.14)
project(TicketGo_Modern CXX)

# ── C++17 required for: std::string_view, std::shared_mutex, std::sample,
#    std::from_chars, if-constexpr, structured bindings.
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)  # use -std=c++17, not -std=gnu++17

# ── Compiler hardening flags (GCC / Clang) ────────────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -fstack-protector-strong
    # Uncomment for address-sanitiser runs:
    # -fsanitize=address,undefined
    # -fno-omit-frame-pointer
)

# ── Threads (required for std::thread, std::mutex, etc.) ─────────────────
find_package(Threads REQUIRED)  # sets Threads::Threads target

# ── Entity (shared between server and – if needed – tests) ───────────────
add_library(entity STATIC
    Entity/Station.cc
    Entity/Ticket.cc
    Entity/Train.cc
    Entity/Task.cc
)
target_include_directories(entity PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Entity)
target_link_libraries(entity PUBLIC Threads::Threads)

# ── Server binary ─────────────────────────────────────────────────────────
add_executable(ticketgo_server
    Server/server_main.cc
    Server/Server.cc
)
target_include_directories(ticketgo_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Entity
)
target_link_libraries(ticketgo_server PRIVATE entity Threads::Threads)

# epoll is Linux-only; gate the build on Linux.
# if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
#     message(WARNING
#         "ticketgo_server uses Linux epoll and can only be built on Linux.")
# endif()

# ── Client binary ─────────────────────────────────────────────────────────
add_executable(ticketgo_client
    Client/client_main.cc
    Client/Client.cc
)
target_include_directories(ticketgo_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Client
)
target_link_libraries(ticketgo_client PRIVATE Threads::Threads)

# ── Benchmark binary (Linux + macOS, no epoll dependency) ────────────────
# Self-contained: bench_main.cc copies the BitmapAllocator inline and uses
# only STL, so it does NOT link entity or require Linux.
add_executable(ticketgo_bench
    benchmark/bench_main.cc
)
target_include_directories(ticketgo_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Entity
)
target_link_libraries(ticketgo_bench PRIVATE Threads::Threads)
# Optimise aggressively so we measure the lock overhead, not codegen noise.
target_compile_options(ticketgo_bench PRIVATE -O3 -march=native)

# ── Optional: AddressSanitiser build type ─────────────────────────────────
# cmake -DCMAKE_BUILD_TYPE=Asan ..
if(CMAKE_BUILD_TYPE STREQUAL "Asan")
    foreach(tgt ticketgo_server ticketgo_client ticketgo_bench entity)
        target_compile_options(${tgt} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${tgt} PRIVATE
            -fsanitize=address,undefined)
    endforeach()
endif()
