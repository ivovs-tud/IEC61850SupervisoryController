#include "sc/communication/CommunicationConfig.hpp"

#include "sc/runtime/RuntimeConfig.hpp"

#include <utility>

namespace sc::communication {

CommunicationConfig makeCommunicationConfig(const sc::runtime::RuntimeConfig& runtime)
{
    CommunicationConfig config;
    config.hmi.period = runtime.hmi.period;
    config.hmi.windowSize = runtime.hmi.windowSize;
    config.hmi.publisherEndpoint = runtime.hmi.publisherEndpoint;
    config.hmi.commandEndpoint = runtime.hmi.commandEndpoint;
    config.hmi.alarmAcknowledgementEnabled = runtime.monitoring.alarmAcknowledgementEnabled;
    config.operatorServer.port = runtime.communication.operatorServer.port;
    config.operatorServer.pollPeriod = runtime.communication.operatorServer.pollPeriod;
    config.attackInterface.transport = runtime.communication.attackInterface.transport;
    config.attackInterface.bindAddress = runtime.communication.attackInterface.bindAddress;
    config.attackInterface.port = runtime.communication.attackInterface.port;
    config.attackInterface.pollPeriod = runtime.communication.attackInterface.pollPeriod;
    config.attackInterface.heartbeatInterval = runtime.communication.attackInterface.heartbeatInterval;
    config.attackInterface.leaseTimeout = runtime.communication.attackInterface.leaseTimeout;
    config.attackInterface.reuseLastFdiValueOnFailure = runtime.communication.attackInterface.reuseLastFdiValueOnFailure;
    config.attackInterface.receiveBufferBytes = runtime.communication.attackInterface.receiveBufferBytes;
    config.attackInterface.transmitBufferBytes = runtime.communication.attackInterface.transmitBufferBytes;
    config.attackInterface.configurationTimeout = runtime.communication.attackInterface.configurationTimeout;
    config.attackInterface.tcpUserTimeout = runtime.communication.attackInterface.tcpUserTimeout;
    config.dataHistorian.port = runtime.communication.dataHistorian.port;
    config.dataHistorian.pollPeriod = runtime.communication.dataHistorian.pollPeriod;

    config.mms.turbines.reserve(runtime.turbines.size());
    for (const auto& endpoint : runtime.turbines) {
        TurbineEndpoint turbine;
        turbine.host = endpoint.host;
        turbine.port = endpoint.port;
        turbine.iedName = endpoint.iedName;
        turbine.logicalDevice = endpoint.logicalDevice;
        config.mms.turbines.push_back(std::move(turbine));
    }
    config.mms.pollPeriod = runtime.communication.mms.pollPeriod;
    config.mms.reconnectInitialDelay = runtime.communication.mms.reconnectInitialDelay;
    config.mms.reconnectMaxDelay = runtime.communication.mms.reconnectMaxDelay;
    config.mms.reportingEnabled = false;
    for (const auto& report : runtime.communication.mms.reports) {
        if (!report.enabled) {
            continue;
        }
        config.mms.reportingEnabled = true;
        config.mms.reportTriggerPeriod = report.integrityPeriod;
        config.mms.reportDataSetReference = report.dataSetReference;
        config.mms.reportControlBlockReference = report.controlBlockReference;
        config.mms.reportDataReferences = report.dataReferences;
        break;
    }
    config.goose.enabled = runtime.communication.goose.enabled;
    config.goose.networkInterface = runtime.communication.goose.networkInterface;
    return config;
}

} // namespace sc::communication
