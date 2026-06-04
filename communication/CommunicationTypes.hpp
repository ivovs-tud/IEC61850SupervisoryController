#pragma once

#include <string>
#include <vector>
#include <chrono>

#include "communication/libiec_wrapper.hpp"

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
        int                       port        {9002};
        std::chrono::milliseconds pollPeriod  {std::chrono::milliseconds(10)};
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
