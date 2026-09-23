#include "common/DataHistorian.hpp"
#include "communication/socket/SocketWrapper.hpp"
#include "communication/socket/socket_platform.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

int reserveLoopbackPort() {
    const socket_t probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe == INVALID_SOCKET_FD) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        socket_close(probe);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        socket_close(probe);
        return -1;
    }
    const int port = ntohs(address.sin_port);
    socket_close(probe);
    return port;
}

socket_t connectLoopback(int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    for (int attempt = 0; attempt < 100; ++attempt) {
        const socket_t client = socket(AF_INET, SOCK_STREAM, 0);
        if (client != INVALID_SOCKET_FD &&
            connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return client;
        }
        socket_close(client);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return INVALID_SOCKET_FD;
}

bool writeAll(socket_t client, const uint8_t* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = socket_write(client, data + offset, size - offset);
        if (written <= 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

DH_TCP_DATA makeRecord(uint32_t id, uint64_t timestamp, float yaw) {
    DH_TCP_DATA record{};
    record.nID = id;
    record.nUnixTime = timestamp;
    record.YwAng = yaw;
    return record;
}

} // namespace

int main() {
    if (!socket_init()) {
        std::cerr << "failed to initialize test socket subsystem\n";
        return 1;
    }
    const int port = reserveLoopbackPort();
    if (port < 0) {
        std::cerr << "failed to reserve loopback port\n";
        socket_cleanup();
        return 2;
    }

    SocketWrapper wrapper(9001, 10, 9002, 10, port, 1);
    std::mutex receivedMutex;
    std::condition_variable receivedCv;
    std::vector<DH_TCP_DATA> received;
    bool invalidSize = false;
    wrapper.AttachDataHistorianCallback([&](const uint8_t* data, std::size_t size) {
        std::lock_guard<std::mutex> lock(receivedMutex);
        if (size != sizeof(DH_TCP_DATA)) {
            invalidSize = true;
        } else {
            DH_TCP_DATA record{};
            std::memcpy(&record, data, sizeof(record));
            received.push_back(record);
        }
        receivedCv.notify_all();
    });

    if (wrapper.StartDataHistorianServer(port) < tcpSOCKET_CONNECTED) {
        std::cerr << "failed to start data historian TCP server\n";
        socket_cleanup();
        return 3;
    }
    const socket_t client = connectLoopback(port);
    if (client == INVALID_SOCKET_FD) {
        std::cerr << "failed to connect data historian test client\n";
        wrapper.StopDataHistorianServer();
        socket_cleanup();
        return 4;
    }

    const std::array<DH_TCP_DATA, 3> records{
        makeRecord(1, 1001, 10.5F),
        makeRecord(2, 1002, 20.5F),
        makeRecord(3, 1003, 30.5F),
    };
    const auto* first = reinterpret_cast<const uint8_t*>(&records[0]);
    constexpr std::size_t split = 13;
    if (!writeAll(client, first, split)) {
        std::cerr << "failed to write fragmented record prefix\n";
        socket_close(client);
        wrapper.StopDataHistorianServer();
        socket_cleanup();
        return 5;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        if (!received.empty()) {
            std::cerr << "historian emitted an incomplete TCP record\n";
            socket_close(client);
            wrapper.StopDataHistorianServer();
            socket_cleanup();
            return 6;
        }
    }

    std::vector<uint8_t> remainder(sizeof(DH_TCP_DATA) - split + 2 * sizeof(DH_TCP_DATA));
    std::memcpy(remainder.data(), first + split, sizeof(DH_TCP_DATA) - split);
    std::memcpy(remainder.data() + sizeof(DH_TCP_DATA) - split, &records[1], 2 * sizeof(DH_TCP_DATA));
    if (!writeAll(client, remainder.data(), remainder.size())) {
        std::cerr << "failed to write coalesced historian records\n";
        socket_close(client);
        wrapper.StopDataHistorianServer();
        socket_cleanup();
        return 7;
    }

    bool complete;
    {
        std::unique_lock<std::mutex> lock(receivedMutex);
        complete = receivedCv.wait_for(lock, std::chrono::seconds(3), [&]() {
            return received.size() == records.size() || invalidSize;
        });
    }
    socket_close(client);
    wrapper.StopDataHistorianServer();
    socket_cleanup();

    if (!complete || invalidSize || received.size() != records.size()) {
        std::cerr << "data historian did not reconstruct all TCP records\n";
        return 8;
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (received[index].nID != records[index].nID ||
            received[index].nUnixTime != records[index].nUnixTime ||
            received[index].YwAng != records[index].YwAng) {
            std::cerr << "data historian record order or content changed\n";
            return 9;
        }
    }
    return 0;
}
