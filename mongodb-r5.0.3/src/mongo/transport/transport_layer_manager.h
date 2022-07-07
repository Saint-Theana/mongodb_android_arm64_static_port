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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/platform/mutex.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

namespace mongo {
struct ServerGlobalParams;
class ServiceContext;

namespace transport {

/**
 * This TransportLayerManager is a TransportLayer implementation that holds other
 * TransportLayers. Mongod and Mongos can treat this like the "only" TransportLayer
 * and not be concerned with which other TransportLayer implementations it holds
 * underneath.
 */
class TransportLayerManager final : public TransportLayer {
    TransportLayerManager(const TransportLayerManager&) = delete;
    TransportLayerManager& operator=(const TransportLayerManager&) = delete;

public:
    explicit TransportLayerManager(std::vector<std::unique_ptr<TransportLayer>> tls,
                                   const WireSpec& wireSpec = WireSpec::instance())
        : TransportLayer(wireSpec), _tls(std::move(tls)) {}

    explicit TransportLayerManager(const WireSpec& wireSpec = WireSpec::instance())
        : TransportLayer(wireSpec) {}

    StatusWith<SessionHandle> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<TransientSSLParams> transientSSLParams) override;
    Future<SessionHandle> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr) override;

    Status start() override;
    void shutdown() override;
    Status setup() override;

    ReactorHandle getReactor(WhichReactor which) override;

    // TODO This method is not called anymore, but may be useful to add new TransportLayers
    // to the manager after it's been created.
    Status addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl);

    /*
     * This initializes a TransportLayerManager with the global configuration of the server.
     *
     * To setup networking in mongod/mongos, create a TransportLayerManager with this function,
     * then call
     * tl->setup();
     * serviceContext->setTransportLayer(std::move(tl));
     * serviceContext->getTransportLayer->start();
     */
    static std::unique_ptr<TransportLayer> createWithConfig(const ServerGlobalParams* config,
                                                            ServiceContext* ctx);

    static std::unique_ptr<TransportLayer> makeAndStartDefaultEgressTransportLayer();

    BatonHandle makeBaton(OperationContext* opCtx) const override {
        stdx::lock_guard<Latch> lk(_tlsMutex);
        // TODO: figure out what to do about managers with more than one transport layer.
        invariant(_tls.size() == 1);
        return _tls[0]->makeBaton(opCtx);
    }

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;

    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override;
#endif
private:
    template <typename Callable>
    void _foreach(Callable&& cb) const;

    mutable Mutex _tlsMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "TransportLayerManager::_tlsMutex");
    std::vector<std::unique_ptr<TransportLayer>> _tls;
};

}  // namespace transport
}  // namespace mongo
