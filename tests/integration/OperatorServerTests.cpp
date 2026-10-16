#include "OperatorCommand.hpp"
#include "OperatorServer.hpp"
#include "SocketPlatform.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>
#include <zmq.hpp>

namespace {

int reserveLoopbackPort()
{
    const socket_t probe = socket(AF_INET, SOCK_STREAM, 0);
    if (probe == INVALID_SOCKET_FD)
        return -1;
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

} // namespace

TEST_CASE("operator server decodes commands and rejects invalid reference power")
{
    REQUIRE(socket_init());
    const int port = reserveLoopbackPort();
    REQUIRE(port > 0);

    OperatorServer server({port, std::chrono::milliseconds(1)});
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<OperatorCommand> received;
    server.setCommandHandler([&](const OperatorCommand& command) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(command);
        changed.notify_all();
    });
    REQUIRE(server.start());

    zmq::context_t context;
    zmq::socket_t client(context, zmq::socket_type::pair);
    client.connect("tcp://127.0.0.1:" + std::to_string(port));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    const float reference = 123.5F;
    REQUIRE(client.send(zmq::buffer(&reference, sizeof(reference)), zmq::send_flags::none).has_value());
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&]() { return received.size() == 1; }));
        REQUIRE(std::holds_alternative<RequestedPowerCommand>(received[0]));
        REQUIRE(std::get<RequestedPowerCommand>(received[0]).value == Catch::Approx(reference));
    }

    std::array<uint8_t, 5> simulation{};
    const uint32_t marker = 0x01010101;
    std::memcpy(simulation.data(), &marker, sizeof(marker));
    simulation[4] = 1;
    REQUIRE(client.send(zmq::buffer(simulation), zmq::send_flags::none).has_value());
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(2), [&]() { return received.size() == 2; }));
        REQUIRE(std::holds_alternative<SimulationStateCommand>(received[1]));
        REQUIRE(std::get<SimulationStateCommand>(received[1]).running);
    }

    const float invalidReference = std::numeric_limits<float>::infinity();
    REQUIRE(client.send(zmq::buffer(&invalidReference, sizeof(invalidReference)), zmq::send_flags::none).has_value());
    const std::array<uint8_t, 3> malformed{1, 2, 3};
    REQUIRE(client.send(zmq::buffer(malformed), zmq::send_flags::none).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lock(mutex);
        REQUIRE(received.size() == 2);
    }

    server.stop();
    socket_cleanup();
}
