#pragma once

#include "sc/application/AttackSignalType.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace sc::protocol::attack {

enum class MessageType : uint8_t {
    TX_DATA = 0x01,
    RQ_DATA = 0x02,
    AT_DATA = 0x04,
    CT_DATA = 0x08,
    CFG_DATA = 0x10,
    SIM_CTRL = 0x20,
    HEARTBEAT = 0x40,
    RELEASE = 0x80,
};

enum class ControlSignal : uint32_t {
    NONE = 0x00,
    TAP = 0x01,
    FDI = 0x02,
};

using TimeStamp = uint64_t;
using Bytes = std::vector<uint8_t>;

inline constexpr std::size_t TX_DATA_SIZE = 16;
inline constexpr std::size_t RQ_DATA_SIZE = 24;
inline constexpr std::size_t AT_DATA_SIZE = 24;
inline constexpr std::size_t CT_DATA_PREFIX_SIZE = 12;
inline constexpr std::size_t CFG_DATA_SIZE = 268;
inline constexpr std::size_t SIM_CTRL_SIZE = 2;
inline constexpr std::size_t HEARTBEAT_SIZE = 1;
inline constexpr std::size_t RELEASE_SIZE = 1;
inline constexpr std::size_t DEFAULT_BUFFER_LIMIT = 64 * 1024;

struct TxDataMessage {
    uint8_t turbineId{0};
    AttackInterface::SignalType dataType{AttackInterface::SignalType::NONE};
    uint8_t payloadLength{1};
    float value{0.0F};
};

struct RqDataMessage {
    uint8_t turbineId{0};
    AttackInterface::SignalType dataType{AttackInterface::SignalType::NONE};
    TimeStamp requestTime{0};
    TimeStamp expiryTime{0};
};

struct AtDataMessage {
    uint8_t turbineId{0};
    AttackInterface::SignalType dataType{AttackInterface::SignalType::NONE};
    TimeStamp attackTime{0};
    float fakeValue{0.0F};
};

struct CtDataMessage {
    ControlSignal signal{ControlSignal::NONE};
    AttackInterface::SignalType dataType{AttackInterface::SignalType::NONE};
    std::vector<uint8_t> enabled;
};

struct CfgDataMessage {
    std::string teamName;
    int32_t scenarioId{0};
    int32_t turbineController{0};
};

struct SimCtrlMessage {
    bool simStart{false};
};

struct HeartbeatMessage {};
struct ReleaseMessage {};

using Message = std::variant<TxDataMessage,
                             RqDataMessage,
                             AtDataMessage,
                             CtDataMessage,
                             CfgDataMessage,
                             SimCtrlMessage,
                             HeartbeatMessage,
                             ReleaseMessage>;

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {

inline void writeU32(Bytes& bytes, std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

inline void writeU64(Bytes& bytes, std::size_t offset, uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
    }
}

inline void writeI32(Bytes& bytes, std::size_t offset, int32_t value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bytes, offset, bits);
}

inline void writeFloat(Bytes& bytes, std::size_t offset, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeU32(bytes, offset, bits);
}

inline uint32_t readU32(const uint8_t* data, std::size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8U) |
           (static_cast<uint32_t>(data[offset + 2]) << 16U) |
           (static_cast<uint32_t>(data[offset + 3]) << 24U);
}

inline uint64_t readU64(const uint8_t* data, std::size_t offset) {
    uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint64_t>(data[offset + index]) << (index * 8U);
    }
    return value;
}

inline int32_t readI32(const uint8_t* data, std::size_t offset) {
    const uint32_t bits = readU32(data, offset);
    int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline float readFloat(const uint8_t* data, std::size_t offset) {
    const uint32_t bits = readU32(data, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline AttackInterface::SignalType signalType(uint32_t value) {
    using AttackInterface::SignalType;
    switch (static_cast<SignalType>(value)) {
        case SignalType::WIND_SPEED:
        case SignalType::WIND_DIRECTION:
        case SignalType::TURBINE_STATUS:
        case SignalType::POWER:
        case SignalType::YAW_ANGLE:
        case SignalType::ROTOR_SPEED:
        case SignalType::PITCH_ANGLE:
        case SignalType::YAW_SETPOINT:
        case SignalType::POWER_SETPOINT:
        case SignalType::GENERATOR_TORQUE:
        case SignalType::OPERATION_COMMAND:
        case SignalType::NONE:
        case SignalType::ARRAY: return static_cast<SignalType>(value);
    }
    throw ProtocolError("unknown attack signal type");
}

inline ControlSignal controlSignal(uint32_t value) {
    switch (static_cast<ControlSignal>(value)) {
        case ControlSignal::NONE:
        case ControlSignal::TAP:
        case ControlSignal::FDI: return static_cast<ControlSignal>(value);
    }
    throw ProtocolError("unknown attack control signal");
}

} // namespace detail

inline MessageType messageType(uint8_t header) {
    switch (static_cast<MessageType>(header)) {
        case MessageType::TX_DATA:
        case MessageType::RQ_DATA:
        case MessageType::AT_DATA:
        case MessageType::CT_DATA:
        case MessageType::CFG_DATA:
        case MessageType::SIM_CTRL:
        case MessageType::HEARTBEAT:
        case MessageType::RELEASE: return static_cast<MessageType>(header);
    }
    throw ProtocolError("unknown attack message header");
}

inline std::size_t messageSize(MessageType type, std::size_t turbineCount) {
    switch (type) {
        case MessageType::TX_DATA: return TX_DATA_SIZE;
        case MessageType::RQ_DATA: return RQ_DATA_SIZE;
        case MessageType::AT_DATA: return AT_DATA_SIZE;
        case MessageType::CT_DATA: return CT_DATA_PREFIX_SIZE + turbineCount;
        case MessageType::CFG_DATA: return CFG_DATA_SIZE;
        case MessageType::SIM_CTRL: return SIM_CTRL_SIZE;
        case MessageType::HEARTBEAT: return HEARTBEAT_SIZE;
        case MessageType::RELEASE: return RELEASE_SIZE;
    }
    throw ProtocolError("unknown attack message type");
}

inline Bytes encode(const TxDataMessage& message) {
    Bytes bytes(TX_DATA_SIZE, 0);
    bytes[0] = static_cast<uint8_t>(MessageType::TX_DATA);
    bytes[1] = message.turbineId;
    detail::writeU32(bytes, 4, static_cast<uint32_t>(message.dataType));
    bytes[8] = message.payloadLength;
    detail::writeFloat(bytes, 12, message.value);
    return bytes;
}

inline Bytes encode(const RqDataMessage& message) {
    Bytes bytes(RQ_DATA_SIZE, 0);
    bytes[0] = static_cast<uint8_t>(MessageType::RQ_DATA);
    bytes[1] = message.turbineId;
    detail::writeU32(bytes, 4, static_cast<uint32_t>(message.dataType));
    detail::writeU64(bytes, 8, message.requestTime);
    detail::writeU64(bytes, 16, message.expiryTime);
    return bytes;
}

inline Bytes encode(const AtDataMessage& message) {
    Bytes bytes(AT_DATA_SIZE, 0);
    bytes[0] = static_cast<uint8_t>(MessageType::AT_DATA);
    bytes[1] = message.turbineId;
    detail::writeU32(bytes, 4, static_cast<uint32_t>(message.dataType));
    detail::writeU64(bytes, 8, message.attackTime);
    detail::writeFloat(bytes, 16, message.fakeValue);
    return bytes;
}

inline Bytes encode(const CtDataMessage& message) {
    Bytes bytes(CT_DATA_PREFIX_SIZE + message.enabled.size(), 0);
    bytes[0] = static_cast<uint8_t>(MessageType::CT_DATA);
    detail::writeU32(bytes, 4, static_cast<uint32_t>(message.signal));
    detail::writeU32(bytes, 8, static_cast<uint32_t>(message.dataType));
    std::copy(message.enabled.begin(), message.enabled.end(), bytes.begin() + CT_DATA_PREFIX_SIZE);
    return bytes;
}

inline Bytes encode(const CfgDataMessage& message) {
    if (message.teamName.size() > 255) {
        throw ProtocolError("attack session label exceeds 255 bytes");
    }
    Bytes bytes(CFG_DATA_SIZE, 0);
    bytes[0] = static_cast<uint8_t>(MessageType::CFG_DATA);
    std::copy(message.teamName.begin(), message.teamName.end(), bytes.begin() + 1);
    detail::writeI32(bytes, 260, message.scenarioId);
    detail::writeI32(bytes, 264, message.turbineController);
    return bytes;
}

inline Bytes encode(const SimCtrlMessage& message) {
    return {static_cast<uint8_t>(MessageType::SIM_CTRL), static_cast<uint8_t>(message.simStart)};
}

inline Bytes encode(const HeartbeatMessage&) {
    return {static_cast<uint8_t>(MessageType::HEARTBEAT)};
}

inline Bytes encode(const ReleaseMessage&) {
    return {static_cast<uint8_t>(MessageType::RELEASE)};
}

inline Bytes encode(const Message& message) {
    return std::visit([](const auto& value) { return encode(value); }, message);
}

inline Message decode(const uint8_t* data, std::size_t size, std::size_t turbineCount) {
    if (data == nullptr || size == 0) throw ProtocolError("empty attack message");
    const MessageType type = messageType(data[0]);
    const std::size_t expectedSize = messageSize(type, turbineCount);
    if (size != expectedSize) throw ProtocolError("invalid attack message size");

    switch (type) {
        case MessageType::TX_DATA:
            return TxDataMessage{data[1], detail::signalType(detail::readU32(data, 4)), data[8], detail::readFloat(data, 12)};
        case MessageType::RQ_DATA:
            return RqDataMessage{data[1], detail::signalType(detail::readU32(data, 4)), detail::readU64(data, 8), detail::readU64(data, 16)};
        case MessageType::AT_DATA:
            return AtDataMessage{data[1], detail::signalType(detail::readU32(data, 4)), detail::readU64(data, 8), detail::readFloat(data, 16)};
        case MessageType::CT_DATA: {
            const auto flagsBegin = data + CT_DATA_PREFIX_SIZE;
            if (!std::all_of(flagsBegin, data + size, [](uint8_t value) { return value <= 1; })) {
                // Pointer bytes from the retired native CT_DATA layout must never become enable flags.
                throw ProtocolError("invalid attack control enable flag");
            }
            return CtDataMessage{
                detail::controlSignal(detail::readU32(data, 4)),
                detail::signalType(detail::readU32(data, 8)),
                std::vector<uint8_t>(flagsBegin, data + size)};
        }
        case MessageType::CFG_DATA: {
            const uint8_t* nameBegin = data + 1;
            const uint8_t* nameEnd = std::find(nameBegin, data + 257, uint8_t{0});
            return CfgDataMessage{
                std::string(reinterpret_cast<const char*>(nameBegin), reinterpret_cast<const char*>(nameEnd)),
                detail::readI32(data, 260),
                detail::readI32(data, 264)};
        }
        case MessageType::SIM_CTRL:
            if (data[1] > 1) throw ProtocolError("invalid simulation control flag");
            return SimCtrlMessage{data[1] != 0};
        case MessageType::HEARTBEAT: return HeartbeatMessage{};
        case MessageType::RELEASE: return ReleaseMessage{};
    }
    throw ProtocolError("unknown attack message type");
}

inline Message decode(const Bytes& bytes, std::size_t turbineCount) {
    return decode(bytes.data(), bytes.size(), turbineCount);
}

class StreamDecoder {
public:
    explicit StreamDecoder(std::size_t turbineCount, std::size_t maxBufferedBytes = DEFAULT_BUFFER_LIMIT)
        : turbineCount_(turbineCount), maxBufferedBytes_(maxBufferedBytes) {
        if (turbineCount_ == 0) throw std::invalid_argument("turbine count must be positive");
        if (maxBufferedBytes_ == 0) throw std::invalid_argument("attack receive buffer limit must be positive");
    }

    std::vector<Bytes> push(const uint8_t* data, std::size_t size) {
        if (data == nullptr && size != 0) throw std::invalid_argument("attack stream data is null");
        if (size > maxBufferedBytes_ - buffer_.size()) {
            throw ProtocolError("attack receive buffer limit exceeded");
        }
        if (size != 0) buffer_.insert(buffer_.end(), data, data + size);

        std::vector<Bytes> messages;
        std::size_t consumed = 0;
        while (consumed < buffer_.size()) {
            const MessageType type = messageType(buffer_[consumed]);
            const std::size_t expectedSize = messageSize(type, turbineCount_);
            if (buffer_.size() - consumed < expectedSize) break;

            Bytes message(buffer_.begin() + static_cast<std::ptrdiff_t>(consumed),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(consumed + expectedSize));
            decode(message, turbineCount_);
            messages.push_back(std::move(message));
            consumed += expectedSize;
        }
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return messages;
    }

    std::vector<Bytes> push(const Bytes& bytes) { return push(bytes.data(), bytes.size()); }

    void finish() const {
        if (!buffer_.empty()) throw ProtocolError("truncated attack message at end of stream");
    }

    void reset() { buffer_.clear(); }
    std::size_t bufferedBytes() const { return buffer_.size(); }

private:
    std::size_t turbineCount_;
    std::size_t maxBufferedBytes_;
    Bytes buffer_;
};

} // namespace sc::protocol::attack
