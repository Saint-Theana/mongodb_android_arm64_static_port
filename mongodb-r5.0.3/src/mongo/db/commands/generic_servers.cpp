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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/log_process_details.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/logv2/ramlog.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/exit.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/ntservice.h"
#include "mongo/util/processinfo.h"

#include <string>
#include <vector>

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(hangInGetLog);

class FeaturesCmd : public BasicCommand {
public:
    FeaturesCmd() : BasicCommand("features") {}
    std::string help() const override {
        return "return build level feature settings";
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {}  // No auth required
    virtual bool run(OperationContext* opCtx,
                     const std::string& ns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        if (getGlobalScriptEngine()) {
            BSONObjBuilder bb(result.subobjStart("js"));
            result.append("utf8", getGlobalScriptEngine()->utf8Ok());
            bb.done();
        }
        if (cmdObj["oidReset"].trueValue()) {
            result.append("oidMachineOld", static_cast<int>(OID::getMachineId()));
            OID::regenMachineId();
        }
        result.append("oidMachine", static_cast<int>(OID::getMachineId()));
        return true;
    }

} featuresCmd;

class HostInfoCmd : public BasicCommand {
public:
    HostInfoCmd() : BasicCommand("hostInfo") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "returns information about the daemon's host";
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::hostInfo);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {
        ProcessInfo p;
        BSONObjBuilder bSys, bOs;

        bSys.appendDate("currentTime", jsTime());
        bSys.append("hostname", prettyHostName());
        bSys.append("cpuAddrSize", static_cast<int>(p.getAddrSize()));
        bSys.append("memSizeMB", static_cast<long long>(p.getSystemMemSizeMB()));
        bSys.append("memLimitMB", static_cast<long long>(p.getMemSizeMB()));
        bSys.append("numCores", static_cast<int>(p.getNumAvailableCores()));
        bSys.append("cpuArch", p.getArch());
        bSys.append("numaEnabled", p.hasNumaEnabled());
        bOs.append("type", p.getOsType());
        bOs.append("name", p.getOsName());
        bOs.append("version", p.getOsVersion());

        result.append(StringData("system"), bSys.obj());
        result.append(StringData("os"), bOs.obj());
        p.appendSystemDetails(result);

        return true;
    }

} hostInfoCmd;

class CmdGetCmdLineOpts : public BasicCommand {
public:
    CmdGetCmdLineOpts() : BasicCommand("getCmdLineOpts") {}
    std::string help() const override {
        return "get argv";
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getCmdLineOpts);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* opCtx,
                     const std::string&,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        result.append("argv", serverGlobalParams.argvArray);
        result.append("parsed", serverGlobalParams.parsedOpts);
        return true;
    }

} cmdGetCmdLineOpts;

class LogRotateCmd : public BasicCommand {
public:
    LogRotateCmd() : BasicCommand("logRotate") {}
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    bool adminOnly() const override {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::logRotate);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    virtual bool run(OperationContext* opCtx,
                     const std::string& ns,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        BSONElement val = cmdObj.firstElement();

        boost::optional<StringData> logType = boost::none;
        if (val.type() == String) {
            logType = val.checkAndGetStringData();
        }

        if (logv2::rotateLogs(serverGlobalParams.logRenameOnRotate, logType)) {
            logProcessDetailsForLogRotate(opCtx->getServiceContext());
            return true;
        }

        return false;
    }

} logRotateCmd;

class GetLogCmd : public ErrmsgCommandDeprecated {
public:
    GetLogCmd() : ErrmsgCommandDeprecated("getLog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getLog);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        return "{ getLog : '*' }  OR { getLog : 'global' }";
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        return errmsgRunImpl<logv2::RamLog>(opCtx, dbname, cmdObj, errmsg, result);
    }

    template <typename RamLogType>
    bool errmsgRunImpl(OperationContext* opCtx,
                       const std::string& dbname,
                       const BSONObj& cmdObj,
                       std::string& errmsg,
                       BSONObjBuilder& result) {
        BSONElement val = cmdObj.firstElement();
        if (val.type() != String) {
            uasserted(ErrorCodes::TypeMismatch,
                      str::stream() << "Argument to getLog must be of type String; found "
                                    << val.toString(false) << " of type " << typeName(val.type()));
        }

        if (MONGO_unlikely(hangInGetLog.shouldFail())) {
            LOGV2(5113600, "Hanging in getLog");
            hangInGetLog.pauseWhileSet();
        }

        std::string p = val.String();
        if (p == "*") {
            std::vector<std::string> names;
            RamLogType::getNames(names);

            BSONArrayBuilder arr;
            for (unsigned i = 0; i < names.size(); i++) {
                arr.append(names[i]);
            }

            result.appendArray("names", arr.arr());
        } else {
            RamLogType* ramlog = RamLogType::getIfExists(p);
            if (!ramlog) {
                errmsg = str::stream() << "no RamLog named: " << p;
                return false;
            }
            typename RamLogType::LineIterator rl(ramlog);

            result.appendNumber("totalLinesWritten",
                                static_cast<long long>(rl.getTotalLinesWritten()));

            BSONArrayBuilder arr(result.subarrayStart("log"));
            while (rl.more())
                arr.append(rl.next());
            arr.done();
        }
        return true;
    }

} getLogCmd;

class ClearLogCmd : public BasicCommand {
public:
    ClearLogCmd() : BasicCommand("clearLog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual bool adminOnly() const {
        return true;
    }
    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        // No access control needed since this command is a testing-only command that must be
        // enabled at the command line.
        return Status::OK();
    }
    std::string help() const override {
        return "{ clearLog : 'global' }";
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        std::string logName;
        Status status = bsonExtractStringField(cmdObj, "clearLog", &logName);
        uassertStatusOK(status);

        if (logName != "global") {
            uasserted(ErrorCodes::InvalidOptions, "Only the 'global' log can be cleared");
        }
        auto clearRamlog = [&](auto* ramlog) {
            invariant(ramlog);
            ramlog->clear();
        };
        clearRamlog(logv2::RamLog::getIfExists(logName));

        return true;
    }
};

MONGO_REGISTER_TEST_COMMAND(ClearLogCmd);

}  // namespace

}  // namespace mongo
