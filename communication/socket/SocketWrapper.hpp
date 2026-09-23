#pragma once

#include <iostream>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

#include <zmq.hpp>

#include "TcpServer.hpp"

#include "common/PeriodicTask.hpp"
#include "sc/ports/AttackChannel.hpp"

typedef enum rp { R_SOCKET_OK = 0, R_SOCKET_ALREADY_RUNNING = 0x01, R_SOCKET_ERROR = 0xFF } SocketReturnCode;

typedef enum p {
    tcpSOCKET_ERROR         = -2,
    tcpSOCKET_DISCONNECTING = -1,
    tcpSOCKET_CLOSED        =  0,
    tcpSOCKET_CONNECTING    =  1,
    tcpSOCKET_CONNECTED     =  2,
    tcpSOCKET_RECEIVING     =  3,
    tcpSOCKET_TRANSMITTING  =  4,
} tcpSocketStatus;

using OperatorCallback = std::function<void(const uint8_t*, size_t)>;
using AttackCallback = std::function<void(const uint8_t*, size_t)>;
using DataHistorianCallback = std::function<void(const uint8_t*, size_t)>;
// ---------------------------------------------------------------------------
// SocketWrapper – owns three PeriodicTask-based socket servers.
//
//   OperatorServer         – PULL socket that receives float-vector commands
//                            from the operator HMI.
//   AttackInterfaceServer  – PULL socket that receives attack / injection
//                            commands from a test harness.
//   DataHistorianServer    – TCP socket that receives turbine telemetry.
//
// Both servers run in their own threads, polling at a configurable rate.
// Each uses onStart() to bind the socket and onStop() to tear it down.
// ---------------------------------------------------------------------------
class SocketWrapper : public sc::ports::AttackChannel
{
private:
    // -----------------------------------------------------------------------
    // OperatorServer
    // -----------------------------------------------------------------------
    class OperatorServer : public PeriodicTask
    {
    public:
        explicit OperatorServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        ~OperatorServer() override { stop(); }
        void setPort(int port);
        void setCallback(OperatorCallback cb);
        tcpSocketStatus status() const;

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        int                          port_{9001};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        OperatorCallback             callback_;
        std::atomic<tcpSocketStatus>    status_{tcpSOCKET_CLOSED};
    };

    // -----------------------------------------------------------------------
    // AttackInterfaceServer
    // -----------------------------------------------------------------------
    class AttackInterfaceServer : public PeriodicTask
    {
    public:
        explicit AttackInterfaceServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        ~AttackInterfaceServer() override { stop(); }
        void setPort(int port);
        void setCallback(AttackCallback cb);
        void setLeaseCheckCallback(sc::ports::AttackLeaseCheckHandler callback);
        void setDisconnectCallback(sc::ports::AttackDisconnectHandler callback);
        void configure(std::size_t receiveBufferBytes,
                       std::size_t transmitBufferBytes,
                       std::chrono::milliseconds heartbeatInterval,
                       std::chrono::milliseconds heartbeatTimeout);
        tcpSocketStatus status() const;
        bool txData(const uint8_t* data, size_t dataSize);

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        static constexpr std::size_t kMaxSendsPerCycle = 8;

        void drainOutboundQueue();

        int                          port_{9002};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        AttackCallback               callback_;
        sc::ports::AttackLeaseCheckHandler leaseCheckCallback_;
        sc::ports::AttackDisconnectHandler disconnectCallback_;
        std::atomic<tcpSocketStatus> status_{tcpSOCKET_CLOSED};
        std::mutex                   outboundMutex_;
        std::deque<std::vector<uint8_t>> outboundQueue_;
        std::size_t receiveBufferBytes_{64 * 1024};
        std::size_t transmitBufferBytes_{64 * 1024};
        std::size_t queuedBytes_{0};
        std::chrono::milliseconds heartbeatInterval_{200};
        std::chrono::milliseconds heartbeatTimeout_{750};
    };

    // -----------------------------------------------------------------------
    // DataHistorian Server
    // Uses raw TCP to support a wide range of simulator devices.
    // -----------------------------------------------------------------------
    class DataHistorianServer : public PeriodicTask {
    public:
        explicit DataHistorianServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        ~DataHistorianServer() override { stop(); }
        void setPort(int port);
        void setCallback(DataHistorianCallback cb);
        tcpSocketStatus status() const;

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        void clientConnected(TcpServer::ClientId clientId);
        void bytesReceived(TcpServer::ClientId clientId, const uint8_t* data, std::size_t size);
        void clientDisconnected(TcpServer::ClientId clientId, const std::string& reason);

        TcpServer                    tcpServer_;
        DataHistorianCallback        callback_;
        std::atomic<tcpSocketStatus> status_{tcpSOCKET_CLOSED};
        std::unordered_map<TcpServer::ClientId, std::vector<uint8_t>> receiveBuffers_;
    };

    OperatorServer         opServer_;
    AttackInterfaceServer  attackServer_;
    DataHistorianServer    dataHistorianServer_;
    std::chrono::system_clock::time_point lastActivityTime_;

public:
    SocketWrapper();

    SocketWrapper(int opPort, int op_ms, int attackPort, int attack_ms) : opServer_(std::chrono::milliseconds(op_ms)), attackServer_(std::chrono::milliseconds(attack_ms)) {
        opServer_.setPort(opPort);
        attackServer_.setPort(attackPort);
    }

    SocketWrapper(int opPort, int op_ms, int attackPort, int attack_ms, int dataHistorianPort, int dataHistorian_ms)
        : opServer_(std::chrono::milliseconds(op_ms)),
          attackServer_(std::chrono::milliseconds(attack_ms)),
          dataHistorianServer_(std::chrono::milliseconds(dataHistorian_ms)) {
        opServer_.setPort(opPort);
        attackServer_.setPort(attackPort);
        dataHistorianServer_.setPort(dataHistorianPort);
    }

    tcpSocketStatus StartOperatorServer(int port);
    tcpSocketStatus StopOperatorServer();
    void         AttachOpServerCallback(OperatorCallback callback);

    tcpSocketStatus StartAttackInterfaceServer(int port);
    tcpSocketStatus StopAttackInterfaceServer();
    void         AttachAttackInterfaceCallback(AttackCallback callback);
    void ConfigureAttackInterface(std::size_t receiveBufferBytes,
                                  std::size_t transmitBufferBytes,
                                  std::chrono::milliseconds heartbeatInterval,
                                  std::chrono::milliseconds heartbeatTimeout);
    void         txAttackInterfaceData(const std::shared_ptr<void>& data, size_t dataSize);
    void setReceiveHandler(sc::ports::AttackReceiveHandler handler) override;
    void setLeaseCheckHandler(sc::ports::AttackLeaseCheckHandler handler) override;
    void setDisconnectHandler(sc::ports::AttackDisconnectHandler handler) override;
    bool send(const uint8_t* data, std::size_t size) override;
    void setFailureHandler(PeriodicTask::FailureHandler handler);

    tcpSocketStatus StartDataHistorianServer(int port);
    tcpSocketStatus StopDataHistorianServer();
    void         AttachDataHistorianCallback(DataHistorianCallback callback);
};
