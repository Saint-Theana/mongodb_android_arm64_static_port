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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/shell/shell_utils.h"

#include <algorithm>
#include <boost/filesystem.hpp>
#include <memory>
#include <set>
#include <stdlib.h>
#include <string>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#include <sys/types.h>
#endif

#include "mongo/base/shim.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/hasher.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/random.h"
#include "mongo/scripting/engine.h"
#include "mongo/shell/bench.h"
#include "mongo/shell/shell_options.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/shell/shell_utils_launcher.h"
#include "mongo/util/ctype.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/text.h"
#include "mongo/util/version.h"

namespace mongo::shell_utils {
namespace {
boost::filesystem::path getUserDir() {
#ifdef _WIN32
    auto wenvp = _wgetenv(L"USERPROFILE");
    if (wenvp)
        return toUtf8String(wenvp);

    return "./";
#else
    const auto homeDir = getenv("HOME");
    if (homeDir)
        return homeDir;

    // The storage for these variables has to live until the value is captured into a std::string at
    // the end of this function.  This is because getpwuid_r(3) doesn't use static storage, but
    // storage provided by the caller.  As a fallback, reserve enough space to store 8 paths, on the
    // theory that the pwent buffer probably needs about that many paths, to fully describe a user
    // -- shell paths, home directory paths, etc.

    const long pwentBufferSize = std::max<long>(sysconf(_SC_GETPW_R_SIZE_MAX), PATH_MAX * 8);

    struct passwd pwent;
    struct passwd* res;

    std::vector<char> buffer(pwentBufferSize);

    do {
        if (!getpwuid_r(getuid(), &pwent, &buffer[0], buffer.size(), &res))
            break;

        if (errno != EINTR)
            uasserted(mongo::ErrorCodes::InternalError,
                      "Unable to get home directory for the current user.");
    } while (errno == EINTR);

    return pwent.pw_dir;
#endif
}

}  // namespace
}  // namespace mongo::shell_utils

boost::filesystem::path mongo::shell_utils::getHistoryFilePath() {
    static const auto& historyFile = *new boost::filesystem::path(getUserDir() / ".dbshell");

    return historyFile;
}


namespace mongo {
namespace JSFiles {
extern const JSFile servers;
extern const JSFile shardingtest;
extern const JSFile servers_misc;
extern const JSFile replsettest;
extern const JSFile data_consistency_checker;
extern const JSFile bridge;
extern const JSFile feature_compatibility_version;
}  // namespace JSFiles

namespace {

std::unique_ptr<DBClientBase> benchRunConfigCreateConnectionImplProvider(
    const BenchRunConfig& config) {
    const ConnectionString connectionString = uassertStatusOK(ConnectionString::parse(config.host));
    auto swConn{connectionString.connect("BenchRun")};
    uassert(16158, swConn.getStatus().reason(), swConn.isOK());
    return std::move(swConn.getValue());
}

auto benchRunConfigCreateConnectionImplRegistration = MONGO_WEAK_FUNCTION_REGISTRATION(
    BenchRunConfig::createConnectionImpl, benchRunConfigCreateConnectionImplProvider);

// helper functions for isBalanced
bool isUseCmd(std::string code) {
    size_t first_space = code.find(" ");
    if (first_space)
        code = code.substr(0, first_space);
    return code == "use";
}

/**
 * Skip over a quoted string, including quotes escaped with backslash
 *
 * @param code      String
 * @param start     Starting position within string, always > 0
 * @param quote     Quote character (single or double quote)
 * @return          Position of ending quote, or code.size() if no quote found
 */
size_t skipOverString(const std::string& code, size_t start, char quote) {
    size_t pos = start;
    while (pos < code.size()) {
        pos = code.find(quote, pos);
        if (pos == std::string::npos) {
            return code.size();
        }
        // We want to break if the quote we found is not escaped, but we need to make sure
        // that the escaping backslash is not itself escaped.  Comparisons of start and pos
        // are to keep us from reading beyond the beginning of the quoted string.
        //
        if (start == pos || code[pos - 1] != '\\' ||   // previous char was backslash
            start == pos - 1 || code[pos - 2] == '\\'  // char before backslash was not another
        ) {
            break;  // The quote we found was not preceded by an unescaped backslash; it is real
        }
        ++pos;  // The quote we found was escaped with backslash, so it doesn't count
    }
    return pos;
}

bool isOpSymbol(char c) {
    static std::string OpSymbols = "~!%^&*-+=|:,<>/?.";

    for (size_t i = 0; i < OpSymbols.size(); i++)
        if (OpSymbols[i] == c)
            return true;
    return false;
}

}  // namespace

namespace shell_utils {


bool isBalanced(const std::string& code) {
    if (isUseCmd(code))
        return true;  // don't balance "use <dbname>" in case dbname contains special chars
    int curlyBrackets = 0;
    int squareBrackets = 0;
    int parens = 0;
    bool danglingOp = false;

    for (size_t i = 0; i < code.size(); i++) {
        switch (code[i]) {
            case '/':
                if (i + 1 < code.size() && code[i + 1] == '/') {
                    while (i < code.size() && code[i] != '\n')
                        i++;
                }
                continue;
            case '{':
                curlyBrackets++;
                break;
            case '}':
                if (curlyBrackets <= 0)
                    return true;
                curlyBrackets--;
                break;
            case '[':
                squareBrackets++;
                break;
            case ']':
                if (squareBrackets <= 0)
                    return true;
                squareBrackets--;
                break;
            case '(':
                parens++;
                break;
            case ')':
                if (parens <= 0)
                    return true;
                parens--;
                break;
            case '"':
            case '\'':
                i = skipOverString(code, i + 1, code[i]);
                if (i >= code.size()) {
                    return true;  // Do not let unterminated strings enter multi-line mode
                }
                break;
            case '\\':
                if (i + 1 < code.size() && code[i + 1] == '/')
                    i++;
                break;
            case '+':
            case '-':
                if (i + 1 < code.size() && code[i + 1] == code[i]) {
                    i++;
                    continue;  // postfix op (++/--) can't be a dangling op
                }
                break;
        }
        if (i >= code.size()) {
            danglingOp = false;
            break;
        }
        if ("~!%^&*-+=|:,<>/?."_sd.find(code[i]) != std::string::npos)
            danglingOp = true;
        else if (!ctype::isSpace(code[i]))
            danglingOp = false;
    }

    return curlyBrackets == 0 && squareBrackets == 0 && parens == 0 && !danglingOp;
}


std::string dbConnect;

static const char* argv0 = nullptr;
EnterpriseShellCallback* enterpriseCallback = nullptr;

void RecordMyLocation(const char* _argv0) {
    argv0 = _argv0;
}

// helpers

BSONObj makeUndefined() {
    BSONObjBuilder b;
    b.appendUndefined("");
    return b.obj();
}
const BSONObj undefinedReturn = makeUndefined();

BSONElement singleArg(const BSONObj& args) {
    uassert(12597, "need to specify 1 argument", args.nFields() == 1);
    return args.firstElement();
}

// real methods

BSONObj JSGetMemInfo(const BSONObj& args, void* data) {
    ProcessInfo pi;
    uassert(10258, "processinfo not supported", pi.supported());

    BSONObjBuilder e;
    e.append("virtual", pi.getVirtualMemorySize());
    e.append("resident", pi.getResidentSize());

    BSONObjBuilder b;
    b.append("ret", e.obj());

    return b.obj();
}

thread_local auto _prng = PseudoRandom(0);

BSONObj JSSrand(const BSONObj& a, void* data) {
    int64_t seed;
    // grab the least significant bits of either the supplied argument or
    // a random number from SecureRandom.
    if (a.nFields() == 1 && a.firstElement().isNumber()) {
        seed = a.firstElement().safeNumberLong();
    } else {
        seed = SecureRandom().nextInt64();
    }
    _prng = PseudoRandom(seed);
    return BSON("" << static_cast<double>(seed));
}

BSONObj JSRand(const BSONObj& a, void* data) {
    uassert(12519, "rand accepts no arguments", a.nFields() == 0);
    return BSON("" << _prng.nextCanonicalDouble());
}

BSONObj isWindows(const BSONObj& a, void* data) {
    uassert(13006, "isWindows accepts no arguments", a.nFields() == 0);
#ifdef _WIN32
    return BSON("" << true);
#else
    return BSON("" << false);
#endif
}

BSONObj getBuildInfo(const BSONObj& a, void* data) {
    uassert(16822, "getBuildInfo accepts no arguments", a.nFields() == 0);
    BSONObjBuilder b;
    VersionInfoInterface::instance().appendBuildInfo(&b);
    return BSON("" << b.done());
}

BSONObj _setShellFailPoint(const BSONObj& a, void* data) {
    if (a.nFields() != 1) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint takes exactly 1 argument, but was given "
                                << a.nFields());
    }

    if (!a.firstElement().isABSONObj()) {
        uasserted(ErrorCodes::BadValue,
                  str::stream() << "_setShellFailPoint given a non-object as an argument.");
    }

    auto cmdObj = a.firstElement().Obj();
    setGlobalFailPoint(cmdObj.firstElement().str(), cmdObj);

    return BSON("" << true);
}

BSONObj computeSHA256Block(const BSONObj& a, void* data) {
    std::vector<ConstDataRange> blocks;

    auto ele = a[0];

    BSONObjBuilder bob;
    switch (ele.type()) {
        case BinData: {
            int len;
            const char* ptr = ele.binData(len);
            SHA256Block::computeHash({ConstDataRange(ptr, len)}).appendAsBinData(bob, ""_sd);

            break;
        }
        case String: {
            auto str = ele.valueStringData();
            SHA256Block::computeHash({ConstDataRange(str.rawData(), str.size())})
                .appendAsBinData(bob, ""_sd);
            break;
        }
        default:
            uasserted(ErrorCodes::BadValue, "Can only computeSHA256Block of strings and bindata");
    }

    return bob.obj();
}

/**
 * This function computes a hash value for a document.
 * Specifically, this is the same hash function that is used to form a hashed index,
 * and thus used to generate shard keys for a collection.
 *
 * e.g.
 * > // For a given collection prepared like so:
 * > use mydb
 * > db.mycollection.createIndex({ x: "hashed" })
 * > sh.shardCollection("mydb.mycollection", { x: "hashed" })
 * > // And a sample object like so:
 * > var obj = { x: "Whatever key", y: 2, z: 10.0 }
 * > // The hashed value of the shard key can be acquired by passing in the shard key value:
 * > convertShardKeyToHashed("Whatever key")
 */
BSONObj convertShardKeyToHashed(const BSONObj& a, void* data) {
    uassert(10151, "convertShardKeyToHashed accepts 1 argument", a.nFields() == 1);
    const auto& objEl = a.firstElement();

    auto key = BSONElementHasher::hash64(objEl, BSONElementHasher::DEFAULT_HASH_SEED);
    return BSON("" << key);
}

BSONObj replMonitorStats(const BSONObj& a, void* data) {
    uassert(17134,
            "replMonitorStats requires a single string argument (the ReplSet name)",
            a.nFields() == 1 && a.firstElement().type() == String);

    auto name = a.firstElement().valuestrsafe();
    auto rsm = ReplicaSetMonitor::get(name);
    if (!rsm) {
        return BSON(""
                    << "no ReplSetMonitor exists by that name");
    }

    BSONObjBuilder result;
    rsm->appendInfo(result);
    // Stats are like {replSetName: {hosts: [{ ... }, { ... }]}}.
    return result.obj()[name].Obj().getOwned();
}

BSONObj useWriteCommandsDefault(const BSONObj& a, void* data) {
    return BSON("" << shellGlobalParams.useWriteCommandsDefault);
}

BSONObj writeMode(const BSONObj&, void*) {
    return BSON("" << shellGlobalParams.writeMode);
}

BSONObj readMode(const BSONObj&, void*) {
    return BSON("" << shellGlobalParams.readMode);
}

BSONObj shouldRetryWrites(const BSONObj&, void* data) {
    return BSON("" << shellGlobalParams.shouldRetryWrites);
}

BSONObj shouldUseImplicitSessions(const BSONObj&, void* data) {
    return BSON("" << shellGlobalParams.shouldUseImplicitSessions);
}

BSONObj apiParameters(const BSONObj&, void* data) {
    return BSON("" << BSON("apiVersion" << shellGlobalParams.apiVersion << "apiStrict"
                                        << shellGlobalParams.apiStrict << "apiDeprecationErrors"
                                        << shellGlobalParams.apiDeprecationErrors));
}

BSONObj interpreterVersion(const BSONObj& a, void* data) {
    uassert(16453, "interpreterVersion accepts no arguments", a.nFields() == 0);
    return BSON("" << getGlobalScriptEngine()->getInterpreterVersionString());
}

BSONObj fileExistsJS(const BSONObj& a, void*) {
    uassert(40678,
            "fileExists expects one string argument",
            a.nFields() == 1 && a.firstElement().type() == String);
    return BSON("" << fileExists(a.firstElement().valuestrsafe()));
}

BSONObj isInteractive(const BSONObj& a, void*) {
    return BSON("" << shellGlobalParams.runShell);
}

void installShellUtils(Scope& scope) {
    scope.injectNative("getMemInfo", JSGetMemInfo);
    scope.injectNative("_replMonitorStats", replMonitorStats);
    scope.injectNative("_srand", JSSrand);
    scope.injectNative("_rand", JSRand);
    scope.injectNative("_isWindows", isWindows);
    scope.injectNative("_setShellFailPoint", _setShellFailPoint);
    scope.injectNative("interpreterVersion", interpreterVersion);
    scope.injectNative("getBuildInfo", getBuildInfo);
    scope.injectNative("computeSHA256Block", computeSHA256Block);
    scope.injectNative("convertShardKeyToHashed", convertShardKeyToHashed);
    scope.injectNative("fileExists", fileExistsJS);
    scope.injectNative("isInteractive", isInteractive);

    installShellUtilsLauncher(scope);
    installShellUtilsExtended(scope);
}

void setEnterpriseShellCallback(EnterpriseShellCallback* callback) {
    enterpriseCallback = callback;
}

void initializeEnterpriseScope(Scope& scope) {
    if (enterpriseCallback != nullptr) {
        enterpriseCallback(scope);
    }
}

void initScope(Scope& scope) {
    // Need to define this method before JSFiles::utils is executed.
    scope.injectNative("_useWriteCommandsDefault", useWriteCommandsDefault);
    scope.injectNative("_writeMode", writeMode);
    scope.injectNative("_readMode", readMode);
    scope.injectNative("_shouldRetryWrites", shouldRetryWrites);
    scope.injectNative("_shouldUseImplicitSessions", shouldUseImplicitSessions);
    scope.injectNative("_apiParameters", apiParameters);
    scope.externalSetup();
    mongo::shell_utils::installShellUtils(scope);
    scope.execSetup(JSFiles::servers);
    scope.execSetup(JSFiles::shardingtest);
    scope.execSetup(JSFiles::servers_misc);
    scope.execSetup(JSFiles::replsettest);
    scope.execSetup(JSFiles::data_consistency_checker);
    scope.execSetup(JSFiles::bridge);
    scope.execSetup(JSFiles::feature_compatibility_version);

    initializeEnterpriseScope(scope);

    scope.injectNative("benchRun", BenchRunner::benchRunSync);
    scope.injectNative("benchRunSync", BenchRunner::benchRunSync);
    scope.injectNative("benchStart", BenchRunner::benchStart);
    scope.injectNative("benchFinish", BenchRunner::benchFinish);

    if (!dbConnect.empty()) {
        uassert(12513, "connect failed", scope.exec(dbConnect, "(connect)", false, true, false));
    }
}

Prompter::Prompter(const std::string& prompt) : _prompt(prompt), _confirmed() {}

bool Prompter::confirm() {
    if (_confirmed) {
        return true;
    }

    // The printf and scanf functions provide thread safe i/o.

    printf("\n%s (y/n): ", _prompt.c_str());

    char yn = '\0';
    int nScanMatches = scanf("%c", &yn);
    bool matchedY = (nScanMatches == 1 && (yn == 'y' || yn == 'Y'));

    return _confirmed = matchedY;
}

ConnectionRegistry::ConnectionRegistry() = default;

void ConnectionRegistry::registerConnection(DBClientBase& client, StringData uri) {
    BSONObj info;
    BSONObj command;
    // If apiStrict is set override it, whatsmyuri is not in the Versioned API.
    if (client.getApiParameters().getStrict()) {
        command = BSON("whatsmyuri" << 1 << "apiStrict" << false);
    } else {
        command = BSON("whatsmyuri" << 1);
    }

    if (client.runCommand("admin", command, info)) {
        stdx::lock_guard<Latch> lk(_mutex);
        _connectionUris[uri.toString()].insert(info["you"].str());
    }
}

void ConnectionRegistry::killOperationsOnAllConnections(bool withPrompt) const {
    Prompter prompter("do you want to kill the current op(s) on the server?");
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto& connection : _connectionUris) {
        std::string errmsg;

        auto uri = uassertStatusOK(MongoURI::parse(connection.first));
        std::unique_ptr<DBClientBase> conn(uri.connect("MongoDB Shell", errmsg));
        if (!conn) {
            continue;
        }

        const std::set<std::string>& uris = connection.second;

        BSONObj currentOpRes;
        conn->runPseudoCommand("admin", "currentOp", "$cmd.sys.inprog", {}, currentOpRes);
        if (!currentOpRes["inprog"].isABSONObj()) {
            // We don't have permissions (or the call didn't succeed) - go to the next connection.
            continue;
        }
        auto inprog = currentOpRes["inprog"].embeddedObject();
        for (const auto& op : inprog) {
            // For sharded clusters, `client_s` is used instead and `client` is not present.
            std::string client;
            if (auto elem = op["client"]) {
                // mongod currentOp client
                if (elem.type() != String) {
                    std::cout << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type()) << std::endl;
                    continue;
                }
                client = elem.str();
            } else if (auto elem = op["client_s"]) {
                // mongos currentOp client
                if (elem.type() != String) {
                    std::cout << "Ignoring operation " << op["opid"].toString(false)
                              << "; expected 'client_s' field in currentOp response to have type "
                                 "string, but found "
                              << typeName(elem.type()) << std::endl;
                    continue;
                }
                client = elem.str();
            } else {
                // Internal operation, like TTL index.
                continue;
            }
            if (uris.count(client)) {
                if (!withPrompt || prompter.confirm()) {
                    BSONObjBuilder cmdBob;
                    BSONObj info;
                    cmdBob.appendAs(op["opid"], "op");
                    auto cmdArgs = cmdBob.done();
                    conn->runPseudoCommand("admin", "killOp", "$cmd.sys.killop", cmdArgs, info);
                } else {
                    return;
                }
            }
        }
    }
}

ConnectionRegistry connectionRegistry;

void onConnect(DBClientBase& c, StringData uri) {
    if (shellGlobalParams.nokillop) {
        return;
    }

    // Only override the default rpcProtocols if they were set on the command line.
    if (shellGlobalParams.rpcProtocols) {
        c.setClientRPCProtocols(*shellGlobalParams.rpcProtocols);
    }

    connectionRegistry.registerConnection(c, uri);
}

bool fileExists(const std::string& file) {
    try {
#ifdef _WIN32
        boost::filesystem::path p(toWideString(file.c_str()));
#else
        boost::filesystem::path p(file);
#endif
        return boost::filesystem::exists(p);
    } catch (...) {
        return false;
    }
}


Mutex& mongoProgramOutputMutex(*(new Mutex()));
}  // namespace shell_utils
}  // namespace mongo
