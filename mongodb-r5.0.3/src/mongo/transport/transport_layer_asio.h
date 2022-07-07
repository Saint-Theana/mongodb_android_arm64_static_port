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

#include <functional>
#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_mode.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"

namespace asio {
class io_context;

template <typename Protocol>
class basic_socket_acceptor;

namespace generic {
class stream_protocol;
}  // namespace generic

namespace ssl {
class context;
}  // namespace ssl
}  // namespace asio

namespace mongo {

class ServiceContext;
class ServiceEntryPoint;

namespace transport {

// This fail point simulates reads and writes that always return 1 byte and fail with EAGAIN
extern FailPoint transportLayerASIOshortOpportunisticReadWrite;

// This fail point will cause an asyncConnect to timeout after it's successfully connected
// to the remote peer
extern FailPoint transportLayerASIOasyncConnectTimesOut;

/**
 * A TransportLayer implementation based on ASIO networking primitives.
 */
class TransportLayerASIO final : public TransportLayer {
    TransportLayerASIO(const TransportLayerASIO&) = delete;
    TransportLayerASIO& operator=(const TransportLayerASIO&) = delete;

public:
    constexpr static auto kSlowOperationThreshold = Seconds(1);

    struct Options {
        constexpr static auto kIngress = 0x1;
        constexpr static auto kEgress = 0x10;

        explicit Options(const ServerGlobalParams* params);
        Options() = default;

        int mode = kIngress | kEgress;

        bool isIngress() const {
            return mode & kIngress;
        }

        bool isEgress() const {
            return mode & kEgress;
        }

        int port = ServerGlobalParams::DefaultDBPort;  // port to bind to
        std::vector<std::string> ipList;               // addresses to bind to
#ifndef _WIN32
        bool useUnixSockets = true;  // whether to allow UNIX sockets in ipList
#endif
        bool enableIPv6 = false;                  // whether to allow IPv6 sockets in ipList
        Mode transportMode = Mode::kSynchronous;  // whether accepted sockets should be put into
                                                  // non-blocking mode after they're accepted
        size_t maxConns = DEFAULT_MAX_CONN;       // maximum number of active connections
    };

    TransportLayerASIO(const Options& opts,
                       ServiceEntryPoint* sep,
                       const WireSpec& wireSpec = WireSpec::instance());

    virtual ~TransportLayerASIO();

    StatusWith<SessionHandle> connect(HostAndPort peer,
                                      ConnectSSLMode sslMode,
                                      Milliseconds timeout,
                                      boost::optional<TransientSSLParams> transientSSLParams) final;

    Future<SessionHandle> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr) final;

    Status setup() final;

    ReactorHandle getReactor(WhichReactor which) final;

    Status start() final;

    void shutdown() final;

    int listenerPort() const {
        return _listenerPort;
    }

#ifdef __linux__
    BatonHandle makeBaton(OperationContext* opCtx) const override;
#endif

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;

    /**
     * Creates a transient SSL context using targeted (non default) SSL params.
     * @param transientSSLParams overrides any value in stored SSLConnectionContext.
     * @param optionalManager provides an optional SSL manager, otherwise the default one will be
     * used.
     */
    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override;
#endif

private:
    class BatonASIO;
    class ASIOSession;
    class ASIOReactor;

    using ASIOSessionHandle = std::shared_ptr<ASIOSession>;
    using ConstASIOSessionHandle = std::shared_ptr<const ASIOSession>;
    using GenericAcceptor = asio::basic_socket_acceptor<asio::generic::stream_protocol>;

    void _acceptConnection(GenericAcceptor& acceptor);

    template <typename Endpoint>
    StatusWith<ASIOSessionHandle> _doSyncConnect(
        Endpoint endpoint,
        const HostAndPort& peer,
        const Milliseconds& timeout,
        boost::optional<TransientSSLParams> transientSSLParams);

    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> _createSSLContext(
        std::shared_ptr<SSLManagerInterface>& manager,
        SSLParams::SSLModes sslMode,
        bool asyncOCSPStaple) const;

    void _runListener() noexcept;

#ifdef MONGO_CONFIG_SSL
    SSLParams::SSLModes _sslMode() const;
#endif

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "TransportLayerASIO::_mutex");

    // There are three reactors that are used by TransportLayerASIO. The _ingressReactor contains
    // all the accepted sockets and all ingress networking activity. The _acceptorReactor contains
    // all the sockets in _acceptors.  The _egressReactor contains egress connections.
    //
    // TransportLayerASIO should never call run() on the _ingressReactor.
    // In synchronous mode, this will cause a massive performance degradation due to
    // unnecessary wakeups on the asio thread for sockets we don't intend to interact
    // with asynchronously. The additional IO context avoids registering those sockets
    // with the acceptors epoll set, thus avoiding those wakeups.  Calling run will
    // undo that benefit.
    //
    // TransportLayerASIO should run its own thread that calls run() on the _acceptorReactor
    // to process calls to async_accept - this is the equivalent of the "listener" thread in
    // other TransportLayers.
    //
    // The underlying problem that caused this is here:
    // https://github.com/chriskohlhoff/asio/issues/240
    //
    // It is important that the reactors be declared before the vector of acceptors (or any other
    // state that is associated with the reactors), so that we destroy any existing acceptors or
    // other reactor associated state before we drop the refcount on the reactor, which may destroy
    // it.
    std::shared_ptr<ASIOReactor> _ingressReactor;
    std::shared_ptr<ASIOReactor> _egressReactor;
    std::shared_ptr<ASIOReactor> _acceptorReactor;

#ifdef MONGO_CONFIG_SSL
    synchronized_value<std::shared_ptr<const SSLConnectionContext>> _sslContext;
#endif

    std::vector<std::pair<SockAddr, GenericAcceptor>> _acceptors;

    // Only used if _listenerOptions.async is false.
    struct Listener {
        stdx::thread thread;
        stdx::condition_variable cv;
        bool active = false;
    };
    Listener _listener;

    ServiceEntryPoint* const _sep = nullptr;

    Options _listenerOptions;
    // The real incoming port in case of _listenerOptions.port==0 (ephemeral).
    int _listenerPort = 0;

    bool _isShutdown = false;
};

}  // namespace transport
}  // namespace mongo
