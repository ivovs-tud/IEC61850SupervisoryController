#include "LibIecGooseReceiver.hpp"

#include <utility>
#include <vector>

extern "C" {
#include <goose_receiver.h>
#include <goose_subscriber.h>
#include <iec61850_client.h>
}

struct LibIecGooseReceiver::Impl {
    struct CallbackContext {
        GooseCallback callback;
    };

    GooseReceiver receiver{nullptr};
    std::vector<std::unique_ptr<CallbackContext>> callbackContexts;
};

LibIecGooseReceiver::LibIecGooseReceiver() : impl_(std::make_unique<Impl>())
{
}

LibIecGooseReceiver::~LibIecGooseReceiver()
{
    stopGooseReceiver();
}

bool LibIecGooseReceiver::configureGooseReceiver(const std::string& networkInterface)
{
    stopGooseReceiver();
    impl_->receiver = GooseReceiver_create();
    if (!impl_->receiver) return false;
    GooseReceiver_setInterfaceId(impl_->receiver, networkInterface.c_str());
    return true;
}

bool LibIecGooseReceiver::addGooseSubscriber(const std::string& controlBlockReference,
                                        uint16_t appId,
                                        GooseCallback callback)
{
    if (!impl_->receiver || GooseReceiver_isRunning(impl_->receiver) ||
        controlBlockReference.empty() || !callback) {
        return false;
    }

    GooseSubscriber subscriber = GooseSubscriber_create(
        const_cast<char*>(controlBlockReference.c_str()), nullptr);
    if (!subscriber) return false;

    uint8_t multicastMac[6] = {0x01, 0x0c, 0xcd, 0x01, 0x00, 0x01};
    GooseSubscriber_setDstMac(subscriber, multicastMac);
    GooseSubscriber_setAppId(subscriber, appId);

    auto context = std::make_unique<Impl::CallbackContext>();
    context->callback = std::move(callback);
    GooseSubscriber_setListener(subscriber, [](GooseSubscriber subscriber, void* userData) {
        auto* context = static_cast<Impl::CallbackContext*>(userData);
        if (!context || !context->callback || !GooseSubscriber_isValid(subscriber)) return;

        MmsValue* values = GooseSubscriber_getDataSetValues(subscriber);
        if (!values) return;

        MmsValue* value = values;
        const MmsType type = MmsValue_getType(values);
        if (type == MMS_ARRAY || type == MMS_STRUCTURE) {
            value = MmsValue_getElement(values, 0);
        }
        if (!value) return;

        const MmsType valueType = MmsValue_getType(value);
        if (valueType != MMS_INTEGER && valueType != MMS_UNSIGNED) return;

        const char* reference = GooseSubscriber_getGoCbRef(subscriber);
        context->callback(reference ? reference : "", MmsValue_toInt32(value));
    }, context.get());
    GooseReceiver_addSubscriber(impl_->receiver, subscriber);
    impl_->callbackContexts.push_back(std::move(context));
    return true;
}

bool LibIecGooseReceiver::startGooseReceiver()
{
    if (!impl_->receiver) return false;
    if (!GooseReceiver_isRunning(impl_->receiver)) GooseReceiver_start(impl_->receiver);
    return GooseReceiver_isRunning(impl_->receiver);
}

void LibIecGooseReceiver::stopGooseReceiver()
{
    if (!impl_ || !impl_->receiver) return;
    if (GooseReceiver_isRunning(impl_->receiver)) GooseReceiver_stop(impl_->receiver);
    GooseReceiver_destroy(impl_->receiver);
    impl_->receiver = nullptr;
    impl_->callbackContexts.clear();
}

bool LibIecGooseReceiver::gooseReceiverRunning() const
{
    return impl_ && impl_->receiver && GooseReceiver_isRunning(impl_->receiver);
}
