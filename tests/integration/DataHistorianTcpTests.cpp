#include "sc/communication/data_historian/DataHistorianRecord.hpp"
#include "sc/communication/data_historian/DataHistorianServer.hpp"
#include "SocketPlatform.hpp"

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

DataHistorianRecord makeRecord(uint32_t id, uint64_t timestamp, float yaw) {
    DataHistorianRecord record{};
    record.turbineId = id;
    record.unixTime = timestamp;
    record.yawAngle = yaw;
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

    DataHistorianServer server({port, std::chrono::milliseconds(1)});
    std::mutex receivedMutex;
    std::condition_variable receivedCv;
    std::vector<DataHistorianRecord> received;
    server.setRecordHandler([&](const DataHistorianRecord& record) {
        std::lock_guard<std::mutex> lock(receivedMutex);
        received.push_back(record);
        receivedCv.notify_all();
    });

    if (!server.start()) {
        std::cerr << "failed to start data historian TCP server\n";
        socket_cleanup();
        return 3;
    }
    const socket_t client = connectLoopback(port);
    if (client == INVALID_SOCKET_FD) {
        std::cerr << "failed to connect data historian test client\n";
        server.stop();
        socket_cleanup();
        return 4;
    }

    const std::array<DataHistorianRecord, 3> records{
        makeRecord(1, 1001, 10.5F),
        makeRecord(2, 1002, 20.5F),
        makeRecord(3, 1003, 30.5F),
    };
    const auto* first = reinterpret_cast<const uint8_t*>(&records[0]);
    constexpr std::size_t split = 13;
    if (!writeAll(client, first, split)) {
        std::cerr << "failed to write fragmented record prefix\n";
        socket_close(client);
        server.stop();
        socket_cleanup();
        return 5;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        if (!received.empty()) {
            std::cerr << "historian emitted an incomplete TCP record\n";
            socket_close(client);
            server.stop();
            socket_cleanup();
            return 6;
        }
    }

    std::vector<uint8_t> remainder(sizeof(DataHistorianRecord) - split + 2 * sizeof(DataHistorianRecord));
    std::memcpy(remainder.data(), first + split, sizeof(DataHistorianRecord) - split);
    std::memcpy(remainder.data() + sizeof(DataHistorianRecord) - split, &records[1], 2 * sizeof(DataHistorianRecord));
    if (!writeAll(client, remainder.data(), remainder.size())) {
        std::cerr << "failed to write coalesced historian records\n";
        socket_close(client);
        server.stop();
        socket_cleanup();
        return 7;
    }

    bool complete;
    {
        std::unique_lock<std::mutex> lock(receivedMutex);
        complete = receivedCv.wait_for(lock, std::chrono::seconds(3), [&]() {
            return received.size() == records.size();
        });
    }
    socket_close(client);
    server.stop();
    socket_cleanup();

    if (!complete || received.size() != records.size()) {
        std::cerr << "data historian did not reconstruct all TCP records\n";
        return 8;
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        if (received[index].turbineId != records[index].turbineId ||
            received[index].unixTime != records[index].unixTime ||
            received[index].yawAngle != records[index].yawAngle) {
            std::cerr << "data historian record order or content changed\n";
            return 9;
        }
    }
    return 0;
}
