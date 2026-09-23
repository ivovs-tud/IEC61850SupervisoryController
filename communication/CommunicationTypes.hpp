#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstddef>

#include "communication/libiec_wrapper.hpp"
#include "sc/ports/AttackTransport.hpp"

typedef enum rc {
    COMM_OK = 0,
    COMM_ERROR = -1,
} CommReturnCode;

typedef enum cs {
    COMM_DISCONNECTED = -1,
    COMM_CONNECTING   = 0,
    COMM_CONNECTED    = 1,
} CommStatus;

struct CommConfig
{
    struct OperatorServer {
        int                       port        {9001};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
    } operatorServer;

    struct AttackInterface {
        sc::ports::AttackTransport    transport   {sc::ports::AttackTransport::ZEROMQ};
        std::string                   bindAddress {"0.0.0.0"};
        int                       port        {9002};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
        std::chrono::milliseconds heartbeatInterval {std::chrono::milliseconds(200)};
        std::chrono::milliseconds leaseTimeout {std::chrono::milliseconds(750)};
        std::size_t                receiveBufferBytes {64 * 1024};
        std::size_t                transmitBufferBytes {64 * 1024};
        std::chrono::milliseconds  zmqHeartbeatInterval {std::chrono::milliseconds(200)};
        std::chrono::milliseconds  zmqHeartbeatTimeout {std::chrono::milliseconds(750)};
        std::chrono::milliseconds  tcpUserTimeout {std::chrono::milliseconds(0)};
    } attackInterface;

    struct DataHistorian {
        int                       port        {9003};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
    } dataHistorian;

    struct Mms {
        std::vector<TurbineEndpoint>  turbines;
        std::chrono::milliseconds     pollPeriod  {std::chrono::milliseconds(10)};
        bool                          reportingEnabled {true};
        std::chrono::milliseconds     reportTriggerPeriod {std::chrono::milliseconds(500)};
        std::string                   reportDataSetReference {"WPPD1$ds01"};
        std::string                   reportControlBlockReference {"WPPD1$RP$urcb01"};
        std::vector<std::string>      reportDataReferences {};
    } mms;

    struct Goose {
        std::string               networkInterface{"veth1"};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(4)};
    } goose;

    std::chrono::milliseconds orchestrationPeriod{std::chrono::milliseconds(100)};
};
