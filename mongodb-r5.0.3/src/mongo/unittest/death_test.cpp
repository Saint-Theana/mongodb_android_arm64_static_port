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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <fmt/format.h>
#include <stdio.h>

#include "mongo/bson/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/exit_code.h"

#ifndef _WIN32
#include <cstdio>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__has_feature) && __has_feature(thread_sanitizer)
#include <sanitizer/common_interface_defs.h>
#endif

#include <sstream>

#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debugger.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace unittest {

class DeathTestSyscallException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

#define logAndThrowWithErrno(expr) logAndThrowWithErrnoAt(expr, __FILE__, __LINE__, errno)

void logAndThrowWithErrnoAt(const StringData expr,
                            const StringData file,
                            const unsigned line,
                            const int err) {
    using namespace fmt::literals;
    LOGV2_ERROR(24138,
                "{expr} failed: {error} @{file}:{line}",
                "expression failed",
                "expr"_attr = expr,
                "error"_attr = errnoWithDescription(err),
                "file"_attr = file,
                "line"_attr = line);
    breakpoint();
    throw DeathTestSyscallException(
        "{} failed: {} @{}:{}"_format(expr, errnoWithDescription(err), file, line));
}

#if defined(__has_feature) && __has_feature(thread_sanitizer)
// Our callback handler exits with the default TSAN exit code so we can check in the death test
// framework Without this, the use could override the exit code and get a false positive that the
// test passes in TSAN builds.
void sanitizerDieCallback() {
    _exit(EXIT_THREAD_SANITIZER);
}
#endif

void DeathTestBase::_doTest() {
#if defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(memory_sanitizer))
    LOGV2(5306900, "Skipping death test in sanitizer build");
    return;
#elif defined(_WIN32)
    LOGV2(24133, "Skipping death test on Windows");
    return;
#elif defined(__APPLE__) && (TARGET_OS_TV || TARGET_OS_WATCH)
    LOGV2(24134, "Skipping death test on tvOS/watchOS");
    return;
#else
    int pipes[2];
    if (pipe(pipes) == -1)
        logAndThrowWithErrno("pipe()");
    pid_t child;
    if ((child = fork()) == -1)
        logAndThrowWithErrno("fork()");
    if (child) {
        if (close(pipes[1]) == -1)
            logAndThrowWithErrno("close(pipe[1])");
        std::ostringstream os;
        FILE* pf = 0;
        if ((pf = fdopen(pipes[0], "r")) == NULL)
            logAndThrowWithErrno("fdopen(pipe[0], \"r\")");
        auto pfGuard = makeGuard([&] {
            if (fclose(pf) != 0)
                logAndThrowWithErrno("fclose(pf)");
        });
        LOGV2(5042601, "Death test starting");
        auto alwaysLogExit = makeGuard([] { LOGV2(5042602, "Death test finishing"); });

        char* lineBuf = nullptr;
        size_t lineBufSize = 0;
        auto lineBufGuard = makeGuard([&] { free(lineBuf); });
        while (true) {
            errno = 0;  // Needed as getline can return -1 without setting errno.
            ssize_t bytesRead = getline(&lineBuf, &lineBufSize, pf);
            if (bytesRead == -1) {
                if (errno == EINTR)
                    continue;
                break;
            }
            StringData line(lineBuf, bytesRead);
            if (line.empty())
                continue;
            if (line[line.size() - 1] == '\n')
                line = line.substr(0, line.size() - 1);
            if (line.empty())
                continue;
            int parsedLen = 0;
            BSONObj parsedChildLog;
            try {
                parsedChildLog = fromjson(lineBuf, &parsedLen);
            } catch (DBException&) {
                // ignore json parsing errors and dump the whole log line as text
                parsedLen = 0;
            }
            if (static_cast<size_t>(parsedLen) == line.size()) {
                LOGV2(20165, "child", "json"_attr = parsedChildLog);
            } else {
                LOGV2(20169, "child", "text"_attr = line);
            }
            os.write(lineBuf, bytesRead);
            invariant(os);
        }
        if (!feof(pf))
            logAndThrowWithErrno("getline(&buf, &bufSize, pf)");

        pid_t pid;
        int stat;
        while (child != (pid = waitpid(child, &stat, 0))) {
            invariant(pid == -1);
            const int err = errno;
            switch (err) {
                case EINTR:
                    continue;
                default:
                    logAndThrowWithErrno("waitpid(child, &stat, 0)");
            }
        }
        if (WIFSIGNALED(stat) || (WIFEXITED(stat) && WEXITSTATUS(stat) != 0)) {
// Exited with a signal or non-zero code. Validate the expected message.
#if defined(__has_feature) && __has_feature(thread_sanitizer)
            if (WEXITSTATUS(stat) == EXIT_THREAD_SANITIZER) {
                FAIL(
                    "Death test exited with Thread Sanitizer exit code, search test output for "
                    "'ThreadSanitizer' for more information");
            }
#endif
            if (_isRegex()) {
                ASSERT_STRING_SEARCH_REGEX(os.str(), _doGetPattern())
                    << " @" << _getFile() << ":" << _getLine();
            } else {
                ASSERT_STRING_CONTAINS(os.str(), _doGetPattern())
                    << " @" << _getFile() << ":" << _getLine();
            }
            LOGV2(5042603, "Death test test died as expected");
            return;
        } else {
            invariant(!WIFSTOPPED(stat));
        }
        FAIL("Expected death, found life\n\n") << os.str();
    }

    // This code only executes in the child process.
    if (close(pipes[0]) == -1)
        logAndThrowWithErrno("close(pipes[0])");
    if (dup2(pipes[1], 1) == -1)
        logAndThrowWithErrno("dup2(pipes[1], 1)");
    if (dup2(1, 2) == -1)
        logAndThrowWithErrno("dup2(1, 2)");

    // We disable the creation of core dump files in the child process since the child process
    // is expected to exit uncleanly. This avoids unnecessarily creating core dump files when
    // the child process calls std::abort() or std::terminate().
    const struct rlimit kNoCoreDump { 0U, 0U };
    if (setrlimit(RLIMIT_CORE, &kNoCoreDump) == -1)
        logAndThrowWithErrno("setrlimit(RLIMIT_CORE, &kNoCoreDump)");

#if defined(__has_feature) && __has_feature(thread_sanitizer)
    __sanitizer_set_death_callback(sanitizerDieCallback);
#endif

    try {
        auto test = _doMakeTest();
        LOGV2(23515, "Running DeathTest in child");
        test->run();
        LOGV2(20166, "Death test failed to die");
    } catch (const TestAssertionFailureException& tafe) {
        LOGV2(24137, "Death test threw test exception instead of dying", "exception"_attr = tafe);
    } catch (...) {
        LOGV2(20167, "Death test threw exception instead of dying");
    }
    // To fail the test, we must exit with a successful error code, because the parent process
    // is checking for the child to die with an exit code indicating an error.
    quickExit(EXIT_SUCCESS);
#endif
}

}  // namespace unittest
}  // namespace mongo
