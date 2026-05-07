#include "Client.h"
#include <cstdlib>
#include <iostream>

int main() {
    Client client;
    if (client.init() != 0) {
        std::cerr << "Failed to connect to server.\n";
        return EXIT_FAILURE;
    }
    return client.core();
}
