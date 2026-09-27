#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

using GooseCallback = std::function<void(const std::string&, int32_t)>;

// Owns raw libiec61850 GOOSE resources. Connection and retry policy belong to
// IEC61850Manager.
class libiec_wrapper
{
public:
    libiec_wrapper();
    ~libiec_wrapper();

    libiec_wrapper(const libiec_wrapper&) = delete;
    libiec_wrapper& operator=(const libiec_wrapper&) = delete;

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
