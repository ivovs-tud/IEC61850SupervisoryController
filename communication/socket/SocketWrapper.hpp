#pragma once

#include <iostream>
#include <chrono>
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
    tcpSOCKET_ERROR         = -2,
    tcpSOCKET_DISCONNECTING = -1,
    tcpSOCKET_CLOSED        =  0,
    tcpSOCKET_CONNECTING    =  1,
    tcpSOCKET_CONNECTED     =  2,
    tcpSOCKET_RECEIVING     =  3,
    tcpSOCKET_TRANSMITTING  =  4,
} TcpSocketStatus;

using OperatorCallback = std::function<void(const uint8_t*, size_t)>;
using AttackCallback = std::function<void(const uint8_t*, size_t)>;
using DataHistorianCallback = std::function<void(const uint8_t*, size_t)>;
constexpr int TCP_BUFFER_SIZE = 1024;
constexpr int TCP_MAX_CONNECTIONS = 1;

/**
 * Owns the controller's operator, attack-interface, and data-historian sockets.
 *
 * Each nested server is a PeriodicTask so socket polling stays independent of
 * IEC RX/TX timing. Operator and attack-interface traffic use ZeroMQ; the data
 * historian endpoint stays raw TCP for compatibility with simple external
 * devices and simulator tooling.
 */
class SocketWrapper {
private:
    /** Receives operator/HMI commands. */
    class OperatorServer : public PeriodicTask {
    public:
        explicit OperatorServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(OperatorCallback cb);
        TcpSocketStatus status() const;

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        int                          port_{9001};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        OperatorCallback             callback_;
        std::atomic<TcpSocketStatus>    status_{tcpSOCKET_CLOSED};
    };

    /** Receives attack-control packets and transmits tapped values. */
    class AttackInterfaceServer : public PeriodicTask {
    public:
        explicit AttackInterfaceServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(AttackCallback cb);
        TcpSocketStatus status() const;
        void txData(const std::shared_ptr<void>& data, size_t dataSize);

    protected:
        void onStart()  override;
        void execute()  override;
        void onStop()   override;

    private:
        int                          port_{9002};
        zmq::context_t               context_;
        std::optional<zmq::socket_t> socket_;
        AttackCallback               callback_;
        std::atomic<TcpSocketStatus>    status_{tcpSOCKET_CLOSED};
    };

    /** Receives simulator data-historian samples over raw TCP. */
    class DataHistorianServer : public PeriodicTask {
    public:
        explicit DataHistorianServer(std::chrono::milliseconds pollPeriod = std::chrono::milliseconds(10));
        void setPort(int port);
        void setCallback(DataHistorianCallback cb);
        TcpSocketStatus status() const;

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
        std::atomic<TcpSocketStatus>    status_{tcpSOCKET_CLOSED};
        
        // Raw TCP clients may vanish without a close; timeout stale slots.
        std::chrono::steady_clock::time_point lastPacketTime_[TCP_MAX_CONNECTIONS];
        std::chrono::milliseconds clientTimeoutInterval_{2000};
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

    TcpSocketStatus startOperatorServer(int port);
    TcpSocketStatus stopOperatorServer();
    void attachOperatorServerCallback(OperatorCallback callback);

    TcpSocketStatus startAttackInterfaceServer(int port);
    TcpSocketStatus stopAttackInterfaceServer();
    void attachAttackInterfaceCallback(AttackCallback callback);
    void txAttackInterfaceData(const std::shared_ptr<void>& data, size_t dataSize);

    TcpSocketStatus startDataHistorianServer(int port);
    TcpSocketStatus stopDataHistorianServer();
    void attachDataHistorianCallback(DataHistorianCallback callback);
};
