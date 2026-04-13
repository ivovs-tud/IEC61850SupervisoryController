#pragma once

#include <iostream>
#include <mutex>
#include <optional>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>

#include <zmq.hpp>
#include <msgpack.hpp>

#include "socket_platform.h"

#include "common/PeriodicTask.hpp"

typedef enum rp { R_SOCKET_OK = 0, R_SOCKET_ALREADY_RUNNING = 0x01, R_SOCKET_ERROR = 0xFF } SocketReturnCode;

typedef enum p {
    SOCKET_ERROR         = -2,
    SOCKET_DISCONNECTING = -1,
    SOCKET_CLOSED        =  0,
    SOCKET_CONNECTING    =  1,
    SOCKET_CONNECTED     =  2,
    SOCKET_RECEIVING     =  3,
    SOCKET_TRANSMITTING  =  4,
} SocketStatus;

using OperatorCallback = std::function<void(const std::vector<float>&)>;
using AttackCallback = std::function<void(const uint8_t*, size_t)>;
using DataHistorianCallback = std::function<void(const uint8_t*, size_t)>;
constexpr int TCP_BUFFER_SIZE = 1024;
constexpr int TCP_MAX_CONNECTIONS = 5;
// ---------------------------------------------------------------------------
// SocketWrapper – owns two PeriodicTask-based socket servers.
//
//   OperatorServer         – PULL socket that receives float-vector commands
//                            from the operator HMI.
//   AttackInterfaceServer  – PULL socket that receives attack / injection
//                            commands from a test harness.
//
// Both servers run in their own threads, polling at a configurable rate.
// Each uses onStart() to bind the socket and onStop() to tear it down.
// ---------------------------------------------------------------------------
class SocketWrapper
{
private:
    // -----------------------------------------------------------------------
    // OperatorServer
    // -----------------------------------------------------------------------
    class OperatorServer : public PeriodicTask
    {
    public:
        explicit OperatorServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(OperatorCallback cb);
        SocketStatus status() const;

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        int                          port_{9001};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        OperatorCallback             callback_;
        std::atomic<SocketStatus>    status_{SOCKET_CLOSED};
    };

    // -----------------------------------------------------------------------
    // AttackInterfaceServer
    // -----------------------------------------------------------------------
    class AttackInterfaceServer : public PeriodicTask
    {
    public:
        explicit AttackInterfaceServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(AttackCallback cb);
        SocketStatus status() const;
        void txData(const std::shared_ptr<void>&data, size_t dataSize);

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        int                          port_{9002};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        AttackCallback               callback_;
        std::atomic<SocketStatus>    status_{SOCKET_CLOSED};
    };

    // -----------------------------------------------------------------------
    // DataHistorian Server 
    // Uses Pure TCP since it should be supported by a large range of devices
    // -----------------------------------------------------------------------
    class DataHistorianServer : public PeriodicTask
    {
    public:
        explicit DataHistorianServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(DataHistorianCallback cb);
        SocketStatus status() const;

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

        void acceptNewClients();
        void readFromClient(socket_t fd);
        void removeClient(socket_t client_fd);

    private:
        int                          port_{9003};
        struct tcp_server {
            socket_t server_fd = INVALID_SOCKET_FD;
            socket_t client_fds[TCP_MAX_CONNECTIONS];
            struct sockaddr_in address{};
            socklen_t addrlen = sizeof(address);
            int opt = 1;
            char buffer[TCP_BUFFER_SIZE] = {0};
        } tcpServer_;
        
        DataHistorianCallback        callback_;
        std::atomic<SocketStatus>    status_{SOCKET_CLOSED};

    };

    OperatorServer         opServer_;
    AttackInterfaceServer  attackServer_;
    DataHistorianServer    dataHistorianServer_;
    std::chrono::system_clock::time_point lastActivityTime_;

public:
    SocketWrapper();

    SocketWrapper(int opPort, int op_ms, int attackPort, int attack_ms)
        : opServer_(std::chrono::milliseconds(op_ms)),
          attackServer_(std::chrono::milliseconds(attack_ms))
    {
        opServer_.setPort(opPort);
        attackServer_.setPort(attackPort);
    }

    SocketWrapper(int opPort, int op_ms, int attackPort, int attack_ms, int dataHistorianPort, int dataHistorian_ms)
        : opServer_(std::chrono::milliseconds(op_ms)),
          attackServer_(std::chrono::milliseconds(attack_ms)),
          dataHistorianServer_(std::chrono::milliseconds(dataHistorian_ms))
    {
        opServer_.setPort(opPort);
        attackServer_.setPort(attackPort);
        dataHistorianServer_.setPort(dataHistorianPort);
    }

    SocketStatus StartOperatorServer(int port);
    SocketStatus StopOperatorServer();
    void         AttachServerCallback(OperatorCallback callback);

    SocketStatus StartAttackInterfaceServer(int port);
    SocketStatus StopAttackInterfaceServer();
    void         AttachAttackInterfaceCallback(AttackCallback callback);
    void         txAttackInterfaceData(const std::shared_ptr<void>&data, size_t dataSize);

    SocketStatus StartDataHistorianServer(int port);
    SocketStatus StopDataHistorianServer();
    void         AttachDataHistorianCallback(DataHistorianCallback callback);
};
