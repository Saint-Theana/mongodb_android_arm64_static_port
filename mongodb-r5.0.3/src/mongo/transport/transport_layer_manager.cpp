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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_manager.h"

#include <iostream>
#include <limits>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

template <typename Callable>
void TransportLayerManager::_foreach(Callable&& cb) const {
    {
        stdx::lock_guard<Latch> lk(_tlsMutex);
        for (auto&& tl : _tls) {
            cb(tl.get());
        }
    }
}

StatusWith<SessionHandle> TransportLayerManager::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    return _tls.front()->connect(peer, sslMode, timeout, transientSSLParams);
}

Future<SessionHandle> TransportLayerManager::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {
    return _tls.front()->asyncConnect(peer, sslMode, reactor, timeout, transientSSLContext);
}

ReactorHandle TransportLayerManager::getReactor(WhichReactor which) {
    return _tls.front()->getReactor(which);
}

// TODO Right now this and setup() leave TLs started if there's an error. In practice the server
// exits with an error and this isn't an issue, but we should make this more robust.
Status TransportLayerManager::start() {
    for (auto&& tl : _tls) {
        auto status = tl->start();
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManager::shutdown() {
    _foreach([](TransportLayer* tl) { tl->shutdown(); });
}

// TODO Same comment as start()
Status TransportLayerManager::setup() {
    for (auto&& tl : _tls) {
        auto status = tl->setup();
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

Status TransportLayerManager::addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl) {
    auto ptr = tl.get();
    {
        stdx::lock_guard<Latch> lk(_tlsMutex);
        _tls.emplace_back(std::move(tl));
    }
    return ptr->start();
}

std::unique_ptr<TransportLayer> TransportLayerManager::makeAndStartDefaultEgressTransportLayer() {
    transport::TransportLayerASIO::Options opts(&serverGlobalParams);
    opts.mode = transport::TransportLayerASIO::Options::kEgress;
    opts.ipList.clear();

    auto ret = std::make_unique<transport::TransportLayerASIO>(opts, nullptr);
    uassertStatusOK(ret->setup());
    uassertStatusOK(ret->start());
    return std::unique_ptr<TransportLayer>(std::move(ret));
}

std::unique_ptr<TransportLayer> TransportLayerManager::createWithConfig(
    const ServerGlobalParams* config, ServiceContext* ctx) {
    auto sep = ctx->getServiceEntryPoint();

    transport::TransportLayerASIO::Options opts(config);
    opts.transportMode = transport::Mode::kSynchronous;

    std::vector<std::unique_ptr<TransportLayer>> retVector;
    retVector.emplace_back(std::make_unique<transport::TransportLayerASIO>(opts, sep));
    return std::make_unique<TransportLayerManager>(std::move(retVector));
}

#ifdef MONGO_CONFIG_SSL
Status TransportLayerManager::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                                 bool asyncOCSPStaple) {
    for (auto&& tl : _tls) {
        auto status = tl->rotateCertificates(manager, asyncOCSPStaple);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
TransportLayerManager::createTransientSSLContext(const TransientSSLParams& transientSSLParams) {

    Status firstError(ErrorCodes::InvalidSSLConfiguration,
                      "Failure creating transient SSL context");
    for (auto&& tl : _tls) {
        auto statusOrContext = tl->createTransientSSLContext(transientSSLParams);
        if (statusOrContext.isOK()) {
            return std::move(statusOrContext.getValue());
        }
        firstError = statusOrContext.getStatus();
    }
    return firstError;
}

#endif

}  // namespace transport
}  // namespace mongo
