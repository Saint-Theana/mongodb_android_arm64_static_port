/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <utility>

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_peer_info.h"
#include "mongo/util/net/ssl_types.h"
#endif

#include "asio.hpp"
#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

namespace mongo {
namespace transport {

MONGO_FAIL_POINT_DEFINE(transportLayerASIOshortOpportunisticReadWrite);

template <typename SuccessValue>
auto futurize(const std::error_code& ec, SuccessValue&& successValue) {
    using Result = Future<std::decay_t<SuccessValue>>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady(successValue);
}

Future<void> futurize(const std::error_code& ec) {
    using Result = Future<void>;
    if (MONGO_unlikely(ec)) {
        return Result::makeReady(errorCodeToStatus(ec));
    }
    return Result::makeReady();
}

using GenericSocket = asio::generic::stream_protocol::socket;

class TransportLayerASIO::ASIOSession final : public Session {
    ASIOSession(const ASIOSession&) = delete;
    ASIOSession& operator=(const ASIOSession&) = delete;

public:
    using Endpoint = asio::generic::stream_protocol::endpoint;

    // If the socket is disconnected while any of these options are being set, this constructor
    // may throw, but it is guaranteed to throw a mongo DBException.
    ASIOSession(TransportLayerASIO* tl,
                GenericSocket socket,
                bool isIngressSession,
                Endpoint endpoint = Endpoint(),
                std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr) try
        : _socket(std::move(socket)),
          _tl(tl),
          _isIngressSession(isIngressSession) {
        auto family = endpointToSockAddr(_socket.local_endpoint()).getType();
        if (family == AF_INET || family == AF_INET6) {
            _socket.set_option(asio::ip::tcp::no_delay(true));
            _socket.set_option(asio::socket_base::keep_alive(true));
            setSocketKeepAliveParams(_socket.native_handle());
        }

        _localAddr = endpointToSockAddr(_socket.local_endpoint());

        if (endpoint == Endpoint()) {
            // Inbound connection, query socket for remote.
            _remoteAddr = endpointToSockAddr(_socket.remote_endpoint());
        } else {
            // Outbound connection, get remote from resolved endpoint.
            // Necessary for TCP_FASTOPEN where the remote isn't connected yet.
            _remoteAddr = endpointToSockAddr(endpoint);
        }

        _local = HostAndPort(_localAddr.toString(true));
        _remote = HostAndPort(_remoteAddr.toString(true));
#ifdef MONGO_CONFIG_SSL
        _sslContext = transientSSLContext ? transientSSLContext : *tl->_sslContext;
        if (transientSSLContext) {
            logv2::DynamicAttributes attrs;
            if (transientSSLContext->targetClusterURI) {
                attrs.add("targetClusterURI", *transientSSLContext->targetClusterURI);
            }
            attrs.add("isIngress", isIngressSession);
            attrs.add("connectionId", id());
            attrs.add("remote", remote());
            LOGV2(5271001, "Initializing the ASIOSession with transient SSL context", attrs);
        }
#endif
    } catch (const DBException&) {
        throw;
    } catch (const asio::system_error& error) {
        uasserted(ErrorCodes::SocketException, error.what());
    } catch (...) {
        uasserted(50797, str::stream() << "Unknown exception while configuring socket.");
    }

    ~ASIOSession() {
        end();
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

    const SockAddr& remoteAddr() const override {
        return _remoteAddr;
    }

    const SockAddr& localAddr() const override {
        return _localAddr;
    }

    void end() override {
        if (getSocket().is_open()) {
            std::error_code ec;
            getSocket().shutdown(GenericSocket::shutdown_both, ec);
            if ((ec) && (ec != asio::error::not_connected)) {
                LOGV2_ERROR(23841,
                            "Error shutting down socket: {error}",
                            "Error shutting down socket",
                            "error"_attr = ec.message());
            }
        }
    }

    StatusWith<Message> sourceMessage() noexcept override try {
        ensureSync();
        return sourceMessageImpl().getNoThrow();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Future<Message> asyncSourceMessage(const BatonHandle& baton = nullptr) noexcept override try {
        ensureAsync();
        return sourceMessageImpl(baton);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Status waitForData() noexcept override try {
        ensureSync();
        asio::error_code ec;
        getSocket().wait(asio::ip::tcp::socket::wait_read, ec);
        return errorCodeToStatus(ec);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Future<void> asyncWaitForData() noexcept override try {
        ensureAsync();
        return getSocket().async_wait(asio::ip::tcp::socket::wait_read, UseFuture{});
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Status sinkMessage(Message message) noexcept override try {
        ensureSync();

        return write(asio::buffer(message.buf(), message.size()))
            .then([this, &message] {
                if (_isIngressSession) {
                    networkCounter.hitPhysicalOut(message.size());
                }
            })
            .getNoThrow();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    Future<void> asyncSinkMessage(Message message,
                                  const BatonHandle& baton = nullptr) noexcept override try {
        ensureAsync();
        return write(asio::buffer(message.buf(), message.size()), baton)
            .then([this, message /*keep the buffer alive*/]() {
                if (_isIngressSession) {
                    networkCounter.hitPhysicalOut(message.size());
                }
            });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    void cancelAsyncOperations(const BatonHandle& baton = nullptr) override {
        LOGV2_DEBUG(4615608,
                    3,
                    "Cancelling outstanding I/O operations on connection to {remote}",
                    "Cancelling outstanding I/O operations on connection to remote",
                    "remote"_attr = _remote);
        if (baton && baton->networking() && baton->networking()->cancelSession(*this)) {
            // If we have a baton, it was for networking, and it owned our session, then we're done.
            return;
        }

        getSocket().cancel();
    }

    void setTimeout(boost::optional<Milliseconds> timeout) override {
        invariant(!timeout || timeout->count() > 0);
        _configuredTimeout = timeout;
    }

    bool isConnected() override {
        // socket.is_open() only returns whether the socket is a valid file descriptor and
        // if we haven't marked this socket as closed already.
        if (!getSocket().is_open())
            return false;

        auto swPollEvents = pollASIOSocket(getSocket(), POLLIN, Milliseconds{0});
        if (!swPollEvents.isOK()) {
            if (swPollEvents != ErrorCodes::NetworkTimeout) {
                LOGV2_WARNING(4615609,
                              "Failed to poll socket for connectivity check: {error}",
                              "Failed to poll socket for connectivity check",
                              "error"_attr = swPollEvents.getStatus());
                return false;
            }
            return true;
        }

        auto revents = swPollEvents.getValue();
        if (revents & POLLIN) {
            char testByte;
            int size = ::recv(getSocket().native_handle(), &testByte, sizeof(testByte), MSG_PEEK);
            if (size == sizeof(testByte)) {
                return true;
            } else if (size == -1) {
                LOGV2_WARNING(4615610,
                              "Failed to check socket connectivity: {error}",
                              "Failed to check socket connectivity",
                              "error"_attr = errnoWithDescription(errno));
            }
            // If size == 0 then we got disconnected and we should return false.
        }

        return false;
    }

#ifdef MONGO_CONFIG_SSL
    const SSLConfiguration* getSSLConfiguration() const override {
        if (_sslContext->manager) {
            return &_sslContext->manager->getSSLConfiguration();
        }
        return nullptr;
    }

    const std::shared_ptr<SSLManagerInterface> getSSLManager() const override {
        return _sslContext->manager;
    }
#endif

protected:
    friend class TransportLayerASIO;
    friend TransportLayerASIO::BatonASIO;

#ifdef MONGO_CONFIG_SSL
    // The unique_lock here is held by TransportLayerASIO to synchronize with the asyncConnect
    // timeout callback. It will be unlocked before the SSL actually handshake begins.
    Future<void> handshakeSSLForEgressWithLock(stdx::unique_lock<Latch> lk,
                                               const HostAndPort& target,
                                               const ReactorHandle& reactor) {
        if (!_sslContext->egress) {
            return Future<void>::makeReady(Status(ErrorCodes::SSLHandshakeFailed,
                                                  "SSL requested but SSL support is disabled"));
        }

        _sslSocket.emplace(std::move(_socket), *_sslContext->egress, removeFQDNRoot(target.host()));
        lk.unlock();

        auto doHandshake = [&] {
            if (_blockingMode == Sync) {
                std::error_code ec;
                _sslSocket->handshake(asio::ssl::stream_base::client, ec);
                return futurize(ec);
            } else {
                return _sslSocket->async_handshake(asio::ssl::stream_base::client, UseFuture{});
            }
        };
        return doHandshake().then([this, target, reactor] {
            _ranHandshake = true;

            return getSSLManager()
                ->parseAndValidatePeerCertificate(_sslSocket->native_handle(),
                                                  _sslSocket->get_sni(),
                                                  target.host(),
                                                  target,
                                                  reactor)
                .then([this](SSLPeerInfo info) {
                    SSLPeerInfo::forSession(shared_from_this()) = info;
                });
        });
    }

    // For synchronous connections where we don't have an async timer, just take a dummy lock and
    // pass it to the WithLock version of handshakeSSLForEgress
    Future<void> handshakeSSLForEgress(const HostAndPort& target) {
        auto mutex = MONGO_MAKE_LATCH();
        return handshakeSSLForEgressWithLock(stdx::unique_lock<Latch>(mutex), target, nullptr);
    }
#endif

    void ensureSync() {
        asio::error_code ec;
        if (_blockingMode != Sync) {
            getSocket().non_blocking(false, ec);
            fassert(40490, errorCodeToStatus(ec));
            _blockingMode = Sync;
        }

        if (_socketTimeout != _configuredTimeout) {
            // Change boost::none (which means no timeout) into a zero value for the socket option,
            // which also means no timeout.
            auto timeout = _configuredTimeout.value_or(Milliseconds{0});
            getSocket().set_option(ASIOSocketTimeoutOption<SO_SNDTIMEO>(timeout), ec);
            if (auto status = errorCodeToStatus(ec); !status.isOK()) {
                tasserted(5342000, status.reason());
            }

            getSocket().set_option(ASIOSocketTimeoutOption<SO_RCVTIMEO>(timeout), ec);
            if (auto status = errorCodeToStatus(ec); !status.isOK()) {
                tasserted(5342001, status.reason());
            }

            _socketTimeout = _configuredTimeout;
        }
    }

    void ensureAsync() {
        if (_blockingMode == Async)
            return;

        // Socket timeouts currently only effect synchronous calls, so make sure the caller isn't
        // expecting a socket timeout when they do an async operation.
        invariant(!_configuredTimeout);

        asio::error_code ec;
        getSocket().non_blocking(true, ec);
        fassert(50706, errorCodeToStatus(ec));
        _blockingMode = Async;
    }

private:
    template <int Name>
    class ASIOSocketTimeoutOption {
    public:
#ifdef _WIN32
        using TimeoutType = DWORD;

        ASIOSocketTimeoutOption(Milliseconds timeoutVal) : _timeout(timeoutVal.count()) {}

#else
        using TimeoutType = timeval;

        ASIOSocketTimeoutOption(Milliseconds timeoutVal) {
            _timeout.tv_sec = duration_cast<Seconds>(timeoutVal).count();
            const auto minusSeconds = timeoutVal - Seconds{_timeout.tv_sec};
            _timeout.tv_usec = duration_cast<Microseconds>(minusSeconds).count();
        }
#endif

        template <typename Protocol>
        int name(const Protocol&) const {
            return Name;
        }

        template <typename Protocol>
        const TimeoutType* data(const Protocol&) const {
            return &_timeout;
        }

        template <typename Protocol>
        std::size_t size(const Protocol&) const {
            return sizeof(_timeout);
        }

        template <typename Protocol>
        int level(const Protocol&) const {
            return SOL_SOCKET;
        }

    private:
        TimeoutType _timeout;
    };

    GenericSocket& getSocket() {
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return static_cast<GenericSocket&>(_sslSocket->lowest_layer());
        }
#endif
        return _socket;
    }

    Future<Message> sourceMessageImpl(const BatonHandle& baton = nullptr) {
        static constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

        auto headerBuffer = SharedBuffer::allocate(kHeaderSize);
        auto ptr = headerBuffer.get();
        return read(asio::buffer(ptr, kHeaderSize), baton)
            .then([headerBuffer = std::move(headerBuffer), this, baton]() mutable {
                if (checkForHTTPRequest(asio::buffer(headerBuffer.get(), kHeaderSize))) {
                    return sendHTTPResponse(baton);
                }

                const auto msgLen = size_t(MSGHEADER::View(headerBuffer.get()).getMessageLength());
                if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
                    StringBuilder sb;
                    sb << "recv(): message msgLen " << msgLen << " is invalid. "
                       << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
                    const auto str = sb.str();
                    LOGV2(4615638,
                          "recv(): message msgLen {msgLen} is invalid. Min: {min} Max: {max}",
                          "recv(): message mstLen is invalid.",
                          "msgLen"_attr = msgLen,
                          "min"_attr = kHeaderSize,
                          "max"_attr = MaxMessageSizeBytes);

                    return Future<Message>::makeReady(Status(ErrorCodes::ProtocolError, str));
                }

                if (msgLen == kHeaderSize) {
                    // This probably isn't a real case since all (current) messages have bodies.
                    if (_isIngressSession) {
                        networkCounter.hitPhysicalIn(msgLen);
                    }
                    return Future<Message>::makeReady(Message(std::move(headerBuffer)));
                }

                auto buffer = SharedBuffer::allocate(msgLen);
                memcpy(buffer.get(), headerBuffer.get(), kHeaderSize);

                MsgData::View msgView(buffer.get());
                return read(asio::buffer(msgView.data(), msgView.dataLen()), baton)
                    .then([this, buffer = std::move(buffer), msgLen]() mutable {
                        if (_isIngressSession) {
                            networkCounter.hitPhysicalIn(msgLen);
                        }
                        return Message(std::move(buffer));
                    });
            });
    }

    template <typename MutableBufferSequence>
    Future<void> read(const MutableBufferSequence& buffers, const BatonHandle& baton = nullptr) {
        // TODO SERVER-47229 Guard active ops for cancellation here.
#ifdef MONGO_CONFIG_SSL
        if (_sslSocket) {
            return opportunisticRead(*_sslSocket, buffers, baton);
        } else if (!_ranHandshake) {
            invariant(asio::buffer_size(buffers) >= sizeof(MSGHEADER::Value));

            return opportunisticRead(_socket, buffers, baton)
                .then([this, buffers]() mutable {
                    _ranHandshake = true;
                    return maybeHandshakeSSLForIngress(buffers);
                })
                .then([this, buffers, baton](bool needsRead) mutable {
                    if (needsRead) {
                        return read(buffers, baton);
                    } else {
                        return Future<void>::makeReady();
                    }
                });
        }
#endif
        return opportunisticRead(_socket, buffers, baton);
    }

    template <typename ConstBufferSequence>
    Future<void> write(const ConstBufferSequence& buffers, const BatonHandle& baton = nullptr) {
        // TODO SERVER-47229 Guard active ops for cancellation here.
#ifdef MONGO_CONFIG_SSL
        _ranHandshake = true;
        if (_sslSocket) {
#ifdef __linux__
            // We do some trickery in asio (see moreToSend), which appears to work well on linux,
            // but fails on other platforms.
            return opportunisticWrite(*_sslSocket, buffers, baton);
#else
            if (_blockingMode == Async) {
                // Opportunistic writes are broken for async egress SSL (switching between blocking
                // and non-blocking mode corrupts the TLS exchange).
                return asio::async_write(*_sslSocket, buffers, UseFuture{}).ignoreValue();
            } else {
                return opportunisticWrite(*_sslSocket, buffers, baton);
            }
#endif
        }
#endif
        return opportunisticWrite(_socket, buffers, baton);
    }

    template <typename Stream, typename MutableBufferSequence>
    Future<void> opportunisticRead(Stream& stream,
                                   const MutableBufferSequence& buffers,
                                   const BatonHandle& baton = nullptr) {
        std::error_code ec;
        size_t size;

        if (MONGO_unlikely(transportLayerASIOshortOpportunisticReadWrite.shouldFail()) &&
            _blockingMode == Async) {
            asio::mutable_buffer localBuffer = buffers;

            if (buffers.size()) {
                localBuffer = asio::mutable_buffer(buffers.data(), 1);
            }

            do {
                size = asio::read(stream, localBuffer, ec);
            } while (ec == asio::error::interrupted);  // retry syscall EINTR

            if (!ec && buffers.size() > 1) {
                ec = asio::error::would_block;
            }
        } else {
            do {
                size = asio::read(stream, buffers, ec);
            } while (ec == asio::error::interrupted);  // retry syscall EINTR
        }

        if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
            (_blockingMode == Async)) {
            // asio::read is a loop internally, so some of buffers may have been read into already.
            // So we need to adjust the buffers passed into async_read to be offset by size, if
            // size is > 0.
            MutableBufferSequence asyncBuffers(buffers);
            if (size > 0) {
                asyncBuffers += size;
            }

            if (auto networkingBaton = baton ? baton->networking() : nullptr;
                networkingBaton && networkingBaton->canWait()) {
                return networkingBaton->addSession(*this, NetworkingBaton::Type::In)
                    .onError([](Status error) {
                        if (ErrorCodes::isShutdownError(error)) {
                            // If the baton has detached, it will cancel its polling. We catch that
                            // error here and return Status::OK so that we invoke
                            // opportunisticRead() again and switch to asio::async_read() below.
                            return Status::OK();
                        }

                        return error;
                    })
                    .then([&stream, asyncBuffers, baton, this] {
                        return opportunisticRead(stream, asyncBuffers, baton);
                    });
            }

            return asio::async_read(stream, asyncBuffers, UseFuture{}).ignoreValue();
        } else {
            return futurize(ec);
        }
    }

    /**
     * moreToSend checks the ssl socket after an opportunisticWrite.  If there are still bytes to
     * send, we manually send them off the underlying socket.  Then we hook that up with a future
     * that gets us back to sending from the ssl side.
     *
     * There are two variants because we call opportunisticWrite on generic sockets and ssl sockets.
     * The generic socket impl never has more to send (because it doesn't have an inner socket it
     * needs to keep sending).
     */
    template <typename ConstBufferSequence>
    boost::optional<Future<void>> moreToSend(GenericSocket& socket,
                                             const ConstBufferSequence& buffers,
                                             const BatonHandle& baton) {
        return boost::none;
    }

#ifdef MONGO_CONFIG_SSL
    template <typename ConstBufferSequence>
    boost::optional<Future<void>> moreToSend(asio::ssl::stream<GenericSocket>& socket,
                                             const ConstBufferSequence& buffers,
                                             const BatonHandle& baton) {
        if (_sslSocket->getCoreOutputBuffer().size()) {
            return opportunisticWrite(getSocket(), _sslSocket->getCoreOutputBuffer(), baton)
                .then([this, &socket, buffers, baton] {
                    return opportunisticWrite(socket, buffers, baton);
                });
        }

        return boost::none;
    }

    boost::optional<std::string> getSniName() const override {
        return SSLPeerInfo::forSession(shared_from_this()).sniName;
    }
#endif

    template <typename Stream, typename ConstBufferSequence>
    Future<void> opportunisticWrite(Stream& stream,
                                    const ConstBufferSequence& buffers,
                                    const BatonHandle& baton = nullptr) {
        std::error_code ec;
        std::size_t size;

        if (MONGO_unlikely(transportLayerASIOshortOpportunisticReadWrite.shouldFail()) &&
            _blockingMode == Async) {
            asio::const_buffer localBuffer = buffers;

            if (buffers.size()) {
                localBuffer = asio::const_buffer(buffers.data(), 1);
            }

            do {
                size = asio::write(stream, localBuffer, ec);
            } while (ec == asio::error::interrupted);  // retry syscall EINTR
            if (!ec && buffers.size() > 1) {
                ec = asio::error::would_block;
            }
        } else {
            do {
                size = asio::write(stream, buffers, ec);
            } while (ec == asio::error::interrupted);  // retry syscall EINTR
        }

        if (((ec == asio::error::would_block) || (ec == asio::error::try_again)) &&
            (_blockingMode == Async)) {

            // asio::write is a loop internally, so some of buffers may have been read into already.
            // So we need to adjust the buffers passed into async_write to be offset by size, if
            // size is > 0.
            ConstBufferSequence asyncBuffers(buffers);
            if (size > 0) {
                asyncBuffers += size;
            }

            if (auto more = moreToSend(stream, asyncBuffers, baton)) {
                return std::move(*more);
            }

            if (auto networkingBaton = baton ? baton->networking() : nullptr;
                networkingBaton && networkingBaton->canWait()) {
                return networkingBaton->addSession(*this, NetworkingBaton::Type::Out)
                    .onError([](Status error) {
                        if (ErrorCodes::isCancellationError(error)) {
                            // If the baton has detached, it will cancel its polling. We catch that
                            // error here and return Status::OK so that we invoke
                            // opportunisticWrite() again and switch to asio::async_write() below.
                            return Status::OK();
                        }

                        return error;
                    })
                    .then([&stream, asyncBuffers, baton, this] {
                        return opportunisticWrite(stream, asyncBuffers, baton);
                    });
            }

            return asio::async_write(stream, asyncBuffers, UseFuture{}).ignoreValue();
        } else {
            return futurize(ec);
        }
    }

#ifdef MONGO_CONFIG_SSL
    template <typename MutableBufferSequence>
    Future<bool> maybeHandshakeSSLForIngress(const MutableBufferSequence& buffer) {
        invariant(asio::buffer_size(buffer) >= sizeof(MSGHEADER::Value));
        MSGHEADER::ConstView headerView(asio::buffer_cast<char*>(buffer));
        auto responseTo = headerView.getResponseToMsgId();

        if (checkForHTTPRequest(buffer)) {
            return Future<bool>::makeReady(false);
        }
        // This logic was taken from the old mongo/util/net/sock.cpp.
        //
        // It lets us run both TLS and unencrypted mongo over the same port.
        //
        // The first message received from the client should have the responseTo field of the wire
        // protocol message needs to be 0 or -1. Otherwise the connection is either sending
        // garbage or a TLS Hello packet which will be caught by the TLS handshake.
        if (responseTo != 0 && responseTo != -1) {
            if (!_sslContext->ingress) {
                return Future<bool>::makeReady(
                    Status(ErrorCodes::SSLHandshakeFailed,
                           "SSL handshake received but server is started without SSL support"));
            }

            auto tlsAlert = checkTLSRequest(buffer);
            if (tlsAlert) {
                return opportunisticWrite(getSocket(),
                                          asio::buffer(tlsAlert->data(), tlsAlert->size()))
                    .then([] {
                        return Future<bool>::makeReady(
                            Status(ErrorCodes::SSLHandshakeFailed,
                                   "SSL handshake failed, as client requested disabled protocol"));
                    });
            }

            _sslSocket.emplace(std::move(_socket), *_sslContext->ingress, "");
            auto doHandshake = [&] {
                if (_blockingMode == Sync) {
                    std::error_code ec;
                    _sslSocket->handshake(asio::ssl::stream_base::server, buffer, ec);
                    return futurize(ec, asio::buffer_size(buffer));
                } else {
                    return _sslSocket->async_handshake(
                        asio::ssl::stream_base::server, buffer, UseFuture{});
                }
            };
            return doHandshake().then([this](size_t size) {
                if (_sslSocket->get_sni()) {
                    auto sniName = _sslSocket->get_sni().get();
                    LOGV2_DEBUG(4908000,
                                2,
                                "Client connected with SNI extension",
                                "sniName"_attr = sniName);
                } else {
                    LOGV2_DEBUG(4908001, 2, "Client connected without SNI extension");
                }
                if (SSLPeerInfo::forSession(shared_from_this()).subjectName.empty()) {
                    return getSSLManager()
                        ->parseAndValidatePeerCertificate(_sslSocket->native_handle(),
                                                          _sslSocket->get_sni(),
                                                          "",
                                                          _remote,
                                                          nullptr)
                        .then([this](SSLPeerInfo info) -> bool {
                            SSLPeerInfo::forSession(shared_from_this()) = info;
                            return true;
                        });
                }

                return Future<bool>::makeReady(true);
            });
        } else if (_tl->_sslMode() == SSLParams::SSLMode_requireSSL) {
            uasserted(ErrorCodes::SSLHandshakeFailed,
                      "The server is configured to only allow SSL connections");
        } else {
            if (!sslGlobalParams.disableNonSSLConnectionLogging &&
                _tl->_sslMode() == SSLParams::SSLMode_preferSSL) {
                LOGV2(23838,
                      "SSL mode is set to 'preferred' and connection {connectionId} to {remote} is "
                      "not using SSL.",
                      "SSL mode is set to 'preferred' and connection to remote is not using SSL.",
                      "connectionId"_attr = id(),
                      "remote"_attr = remote());
            }
            return Future<bool>::makeReady(false);
        }
    }
#endif

    template <typename Buffer>
    bool checkForHTTPRequest(const Buffer& buffers) {
        invariant(asio::buffer_size(buffers) >= 4);
        const StringData bufferAsStr(asio::buffer_cast<const char*>(buffers), 4);
        return (bufferAsStr == "GET "_sd);
    }

    // Called from read() to send an HTTP response back to a client that's trying to use HTTP
    // over a native MongoDB port. This returns a Future<Message> to match its only caller, but it
    // always contains an error, so it could really return Future<Anything>
    Future<Message> sendHTTPResponse(const BatonHandle& baton = nullptr) {
        constexpr auto userMsg =
            "It looks like you are trying to access MongoDB over HTTP"
            " on the native driver port.\r\n"_sd;

        static const std::string httpResp = str::stream() << "HTTP/1.0 200 OK\r\n"
                                                             "Connection: close\r\n"
                                                             "Content-Type: text/plain\r\n"
                                                             "Content-Length: "
                                                          << userMsg.size() << "\r\n\r\n"
                                                          << userMsg;

        return write(asio::buffer(httpResp.data(), httpResp.size()), baton)
            .onError(
                [](const Status& status) {
                    return Status(
                        ErrorCodes::ProtocolError,
                        str::stream()
                            << "Client sent an HTTP request over a native MongoDB connection, "
                               "but there was an error sending a response: "
                            << status.toString());
                })
            .then([] {
                return StatusWith<Message>(
                    ErrorCodes::ProtocolError,
                    "Client sent an HTTP request over a native MongoDB connection");
            });
    }

    enum BlockingMode {
        Unknown,
        Sync,
        Async,
    };

    BlockingMode _blockingMode = Unknown;

    HostAndPort _remote;
    HostAndPort _local;

    SockAddr _remoteAddr;
    SockAddr _localAddr;

    boost::optional<Milliseconds> _configuredTimeout;
    boost::optional<Milliseconds> _socketTimeout;

    GenericSocket _socket;
#ifdef MONGO_CONFIG_SSL
    boost::optional<asio::ssl::stream<decltype(_socket)>> _sslSocket;
    bool _ranHandshake = false;
    std::shared_ptr<const SSLConnectionContext> _sslContext;
#endif

    TransportLayerASIO* const _tl;
    bool _isIngressSession;
};

}  // namespace transport
}  // namespace mongo
