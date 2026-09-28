#pragma once

#include "sc/communication/iec61850/IecReferences.hpp"

#include <cstdint>
#include <memory>
#include <string>

// Owns raw libiec61850 GOOSE resources. Connection and retry policy belong to
// IEC61850Manager.
class LibIecGooseReceiver
{
public:
    LibIecGooseReceiver();
    ~LibIecGooseReceiver();

    LibIecGooseReceiver(const LibIecGooseReceiver&) = delete;
    LibIecGooseReceiver& operator=(const LibIecGooseReceiver&) = delete;

    bool configureGooseReceiver(const std::string& networkInterface);
    bool addGooseSubscriber(const std::string& controlBlockReference,
                            uint16_t appId,
                            GooseCallback callback);
    bool startGooseReceiver();
    void stopGooseReceiver();
    bool gooseReceiverRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
