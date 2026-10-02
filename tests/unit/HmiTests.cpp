#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include "sc/communication/hmi/HmiConfig.hpp"
#include "HmiInterface.hpp"
#include "SocketPlatform.hpp"

#include <msgpack.hpp>
#include <zmq.hpp>

namespace {

class TestHmiInterface : public HmiInterface {
public:
    using HmiInterface::HmiInterface;
    void open() { onStart(); }
    void runOnce() { execute(); }
    void close() { onStop(); }
};

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

template <typename Pack>
void sendCommand(zmq::socket_t& socket, Pack pack) {
    msgpack::sbuffer buffer;
    msgpack::packer<msgpack::sbuffer> writer(buffer);
    pack(writer);
    socket.send(zmq::buffer(buffer.data(), buffer.size()), zmq::send_flags::none);
}

} // namespace

TEST_CASE("HMI data collection reads each shared-data section") {
    SharedData shared;
    shared.configureTurbineCount(2);
    {
        std::lock_guard<std::mutex> lock(shared.collected.mutex);
        shared.collected.lastPower = {1.0, 2.0};
        shared.collected.lastWS = {7.0, 8.0};
    }
    {
        std::lock_guard<std::mutex> lock(shared.processed.mutex);
        shared.processed.connectedTurbines = 2;
        shared.processed.windSpeed = 7.5F;
        shared.processed.measuredTotalPowerHistory.push_back(3.0);
    }
    {
        std::lock_guard<std::mutex> lock(shared.control.mutex);
        shared.control.powerSetpoints = {4.0F, -1.0F};
        shared.control.turbineController = {ControlData::controllerDownregulation,
                                            ControlData::controllerDownregulation};
    }
    {
        std::lock_guard<std::mutex> lock(shared.monitoring.mutex);
        shared.monitoring.alarmStaticBounds = true;
    }
    {
        std::lock_guard<std::mutex> lock(shared.interface.mutex);
        shared.interface.systemRunning = true;
        shared.interface.attackTapEnabled = 3;
    }

    const HmiData data = collectHmiData(shared);

    REQUIRE(data.turbinePower == std::vector<double>{1.0, 2.0});
    REQUIRE(data.turbineWindSpeed == std::vector<double>{7.0, 8.0});
    REQUIRE(data.powerSetpoints == std::vector<float>{4.0F, -1.0F});
    REQUIRE(data.measuredTotalPower == 3.0);
    REQUIRE(data.farmWindSpeed == 7.5);
    REQUIRE(data.connectedTurbines == 2);
    REQUIRE(data.operationMode == static_cast<int>(ControlData::controllerDownregulation));
    REQUIRE(data.systemRunning);
    REQUIRE(data.alarmStaticBounds);
    REQUIRE(data.attackTapEnabled == 3);
}

TEST_CASE("HMI signal accessors use the collected display data") {
    const HmiConfig config = defaultHmiConfig(2);
    HmiData data;
    data.turbinePower = {1.0, 2.0};
    data.powerSetpoints = {3.0F, -1.0F};

    const std::vector<double> values = config.signals.front().accessor(data);

    REQUIRE(values.size() == 4);
    REQUIRE(values[0] == 1.0);
    REQUIRE(values[1] == 3.0);
    REQUIRE(values[2] == 2.0);
    REQUIRE(std::isnan(values[3]));
}

TEST_CASE("HMI drains command bursts and publishes the configured snapshot shape") {
    REQUIRE(socket_init());
    const int publisherPort = reserveLoopbackPort();
    int commandPort = reserveLoopbackPort();
    while (commandPort == publisherPort) commandPort = reserveLoopbackPort();
    REQUIRE(publisherPort > 0);
    REQUIRE(commandPort > 0);

    auto& shared = SharedData::instance();
    shared.configureTurbineCount(2);
    {
        std::lock_guard<std::mutex> lock(shared.control.mutex);
        shared.control.turbineController = {1, 1};
        shared.control.turbineEnabled = {1, 1};
        shared.control.yawSteeringEnabled = false;
        shared.control.alarmAcknowledgementRequested = false;
    }

    HmiConfig config = defaultHmiConfig(2);
    config.publisherEndpoint = "tcp://127.0.0.1:" + std::to_string(publisherPort);
    config.commandEndpoint = "tcp://127.0.0.1:" + std::to_string(commandPort);
    config.alarmAcknowledgementEnabled = true;
    TestHmiInterface interface(config, std::chrono::milliseconds(10));
    interface.open();

    zmq::context_t context;
    zmq::socket_t subscriber(context, zmq::socket_type::sub);
    subscriber.set(zmq::sockopt::subscribe, "");
    subscriber.connect(config.publisherEndpoint);
    zmq::socket_t commands(context, zmq::socket_type::push);
    commands.connect(config.commandEndpoint);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sendCommand(commands, [](auto& writer) {
        writer.pack_array(2); writer.pack("set_mode"); writer.pack(2);
    });
    sendCommand(commands, [](auto& writer) {
        writer.pack_array(3); writer.pack("set_button_state"); writer.pack("Yaw Steering"); writer.pack(1);
    });
    sendCommand(commands, [](auto& writer) {
        writer.pack_array(3); writer.pack("set_turbine_enable"); writer.pack(2); writer.pack(0);
    });
    sendCommand(commands, [](auto& writer) {
        writer.pack_array(2); writer.pack("acknowledge_alarms"); writer.pack(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    zmq::message_t snapshot;
    bool received = false;
    for (int attempt = 0; attempt < 20 && !received; ++attempt) {
        interface.runOnce();
        received = subscriber.recv(snapshot, zmq::recv_flags::dontwait).has_value();
        if (!received) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    {
        std::lock_guard<std::mutex> lock(shared.control.mutex);
        REQUIRE(shared.control.turbineController == std::vector<uint32_t>{3, 3});
        REQUIRE(shared.control.yawSteeringEnabled);
        REQUIRE(shared.control.turbineEnabled == std::vector<uint32_t>{1, 0});
        REQUIRE(shared.control.alarmAcknowledgementRequested);
    }
    REQUIRE(received);
    const auto unpacked = msgpack::unpack(
        static_cast<const char*>(snapshot.data()), snapshot.size());
    const auto object = unpacked.get();
    REQUIRE(object.type == msgpack::type::ARRAY);
    REQUIRE(object.via.array.size == 10);
    REQUIRE(object.via.array.ptr[9].via.array.ptr[0].as<std::string>() ==
            "alarm_acknowledgement");

    interface.close();
    socket_cleanup();
}
