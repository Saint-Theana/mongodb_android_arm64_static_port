/**
 * The ParallelTester class is used to test more than one test concurrently
 */
if (typeof _threadInject != "undefined") {
    // With --enableJavaScriptProtection functions are presented as Code objects.
    // This function evals all the Code objects then calls the provided start function.
    // arguments: [startFunction, startFunction args...]
    function _threadStartWrapper(testData) {
        // Recursively evals all the Code objects present in arguments
        // NOTE: This is a naive implementation that cannot handle cyclic objects.
        function evalCodeArgs(arg) {
            if (arg instanceof Code) {
                return eval("(" + arg.code + ")");
            } else if (arg !== null && isObject(arg)) {
                var newArg = arg instanceof Array ? [] : {};
                for (var prop in arg) {
                    if (arg.hasOwnProperty(prop)) {
                        newArg[prop] = evalCodeArgs(arg[prop]);
                    }
                }
                return newArg;
            }
            return arg;
        }
        var realStartFn;
        var newArgs = [];
        // We skip the first argument, which is always TestData.
        TestData = evalCodeArgs(testData);
        for (var i = 1, l = arguments.length; i < l; i++) {
            newArgs.push(evalCodeArgs(arguments[i]));
        }
        realStartFn = newArgs.shift();
        return realStartFn.apply(this, newArgs);
    }

    Thread = function() {
        var args = Array.prototype.slice.call(arguments);
        // Always pass TestData as the first argument.
        args.unshift(TestData);
        args.unshift(_threadStartWrapper);
        this.init.apply(this, args);
    };
    _threadInject(Thread.prototype);

    fork = function() {
        var t = new Thread(function() {});
        Thread.apply(t, arguments);
        return t;
    };

    // Helper class to generate a list of events which may be executed by a ParallelTester
    EventGenerator = function(me, collectionName, mean, host) {
        this.mean = mean;
        if (host == undefined)
            host = db.getMongo().host;
        this.events = new Array(me, collectionName, host);
    };

    EventGenerator.prototype._add = function(action) {
        this.events.push([Random.genExp(this.mean), action]);
    };

    EventGenerator.prototype.addInsert = function(obj) {
        this._add("t.insert( " + tojson(obj) + " )");
    };

    EventGenerator.prototype.addRemove = function(obj) {
        this._add("t.remove( " + tojson(obj) + " )");
    };

    EventGenerator.prototype.addCurrentOp = function() {
        this._add("db.currentOp()");
    };

    EventGenerator.prototype.addUpdate = function(objOld, objNew) {
        this._add("t.update( " + tojson(objOld) + ", " + tojson(objNew) + " )");
    };

    EventGenerator.prototype.addCheckCount = function(count, query, shouldPrint, checkQuery) {
        query = query || {};
        shouldPrint = shouldPrint || false;
        checkQuery = checkQuery || false;
        var action = "assert.eq( " + count + ", t.count( " + tojson(query) + " ) );";
        if (checkQuery) {
            action +=
                " assert.eq( " + count + ", t.find( " + tojson(query) + " ).toArray().length );";
        }
        if (shouldPrint) {
            action += " print( me + ' ' + " + count + " );";
        }
        this._add(action);
    };

    EventGenerator.prototype.getEvents = function() {
        return this.events;
    };

    EventGenerator.dispatch = function() {
        var args = Array.from(arguments);
        var me = args.shift();
        var collectionName = args.shift();
        var host = args.shift();
        var m = new Mongo(host);

        // We define 'db' and 't' as local variables so that calling eval() on the stringified
        // JavaScript expression 'args[i][1]' can take advantage of using them.
        var db = m.getDB("test");
        var t = db[collectionName];
        for (var i in args) {
            sleep(args[i][0]);
            eval(args[i][1]);
        }
    };

    // Helper class for running tests in parallel.  It assembles a set of tests
    // and then calls assert.parallelests to run them.
    ParallelTester = function() {
        assert.neq(db.getMongo().writeMode(), "legacy", "wrong shell write mode");
        this.params = new Array();
    };

    ParallelTester.prototype.add = function(fun, args) {
        args = args || [];
        args.unshift(fun);
        this.params.push(args);
    };

    ParallelTester.prototype.run = function(msg) {
        assert.parallelTests(this.params, msg);
    };

    // creates lists of tests from jstests dir in a format suitable for use by
    // ParallelTester.fileTester.  The lists will be in random order.
    // n: number of lists to split these tests into
    ParallelTester.createJstestsLists = function(n) {
        var params = new Array();
        for (var i = 0; i < n; ++i) {
            params.push([]);
        }

        var makeKeys = function(a) {
            var ret = {};
            for (var i in a) {
                ret[a[i]] = 1;
            }
            return ret;
        };

        // some tests can't run in parallel with most others
        var skipTests = makeKeys([
            "indexb.js",

            // Tests that set a parameter that causes the server to ignore
            // long index keys.
            "index_bigkeys_nofail.js",
            "index_bigkeys_validation.js",

            // Tests that set the notablescan parameter, which makes queries fail rather than use a
            // non-indexed plan.
            "notablescan.js",
            "notablescan_capped.js",

            "mr_fail_invalid_js.js",
            "run_program1.js",
            "bench_test1.js",

            // These tests use the getLastError command, which is unsafe to use in this environment,
            // since a previous test's cursors could be garbage collected in the middle of the next
            // test, which would reset the last error associated with the shell's client.
            "dropdb_race.js",
            "bulk_legacy_enforce_gle.js",

            // These tests use getLog to examine the logs. Tests which do so shouldn't be run in
            // this suite because any test being run at the same time could conceivably spam the
            // logs so much that the line they are looking for has been rotated off the server's
            // in-memory buffer of log messages, which only stores the 1024 most recent operations.
            "comment_field.js",
            "getlog2.js",
            "logprocessdetails.js",
            "queryoptimizera.js",
            "log_remote_op_wait.js",

            "connections_opened.js",  // counts connections, globally
            "opcounters_write_cmd.js",
            "set_param1.js",          // changes global state
            "geo_update_btree2.js",   // SERVER-11132 test disables table scans
            "update_setOnInsert.js",  // SERVER-9982
            "max_time_ms.js",         // Sensitive to query execution time, by design
            "autocomplete.js",        // Likewise.

            // This overwrites MinKey/MaxKey's singleton which breaks
            // any other test that uses MinKey/MaxKey
            "type6.js",

            // Assumes that other tests are not creating cursors.
            "kill_cursors.js",

            // Assumes that other tests are not starting operations.
            "currentop_shell.js",

            // These tests check global command counters.
            "find_and_modify_metrics.js",
            "update_metrics.js",

            // Views tests
            "views/invalid_system_views.js",      // Puts invalid view definitions in system.views.
            "views/views_all_commands.js",        // Drops test DB.
            "views/view_with_invalid_dbname.js",  // Puts invalid view definitions in system.views.

            // This test works close to the BSON document limit for entries in the durable catalog,
            // so running it in parallel with other tests will cause failures.
            "long_collection_names.js",

            // This test causes collMod commands to hang, which interferes with other tests running
            // collMod.
            "crud_ops_do_not_throw_locktimeout.js",

            // Can fail if isMaster takes too long on a loaded machine.
            "dbadmin.js",

            // Other tests will fail while the requireApiVersion server parameter is set.
            "require_api_version.js",

            // This test updates global memory usage counters in the bucket catalog in a way that
            // may affect other time-series tests running concurrently.
            "timeseries/timeseries_idle_buckets.js",

            // Assumes that other tests are not creating API version 1 incompatible data.
            "validate_db_metadata_command.js",
        ]);

        // Get files, including files in subdirectories.
        var getFilesRecursive = function(dir) {
            var files = listFiles(dir);
            var fileList = [];
            files.forEach(file => {
                if (file.isDirectory) {
                    getFilesRecursive(file.name).forEach(subDirFile => fileList.push(subDirFile));
                } else {
                    fileList.push(file);
                }
            });
            return fileList;
        };

        // The following tests cannot run when shell readMode is legacy.
        if (db.getMongo().readMode() === "legacy") {
            var requires_find_command = [
                "apply_ops_system_dot_views.js",
                "command_let_variables.js",
                "doc_validation_error.js",
                "merge_sort_collation.js",
                "explode_for_sort_fetch.js",
                "update_pipeline_shell_helpers.js",
                "update_with_pipeline.js",
                "verify_update_mods.js",
                "views/dbref_projection.js",
                "views/views_aggregation.js",
                "views/views_change.js",
                "views/views_drop.js",
                "views/views_find.js",
                "wildcard_index_collation.js"
            ];
            Object.assign(skipTests, makeKeys(requires_find_command));

            // Time-series collections require support for views, so are incompatible with legacy
            // readMode.
            const timeseriesTestFiles =
                getFilesRecursive('jstests/core/timeseries').map(f => ('timeseries/' + f.baseName));
            Object.assign(skipTests, makeKeys(timeseriesTestFiles));
        }

        // Transactions are not supported on standalone nodes so we do not run them here.
        let txnsTestFiles = getFilesRecursive("jstests/core/txns").map(f => ("txns/" + f.baseName));
        Object.assign(skipTests, makeKeys(txnsTestFiles));

        var parallelFilesDir = "jstests/core";

        // some tests can't be run in parallel with each other
        var serialTestsArr = [
            // These tests use fsyncLock.
            parallelFilesDir + "/fsync.js",
            parallelFilesDir + "/currentop.js",
            parallelFilesDir + "/killop_drop_collection.js",

            // These tests expect the profiler to be on or off at specific points. They should not
            // be run in parallel with tests that perform fsyncLock. User operations skip writing to
            // the system.profile collection while the server is fsyncLocked.
            //
            // Most profiler tests can be run in parallel with each other as they use test-specific
            // databases, with the exception of tests which modify slowms or the profiler's sampling
            // rate, since those affect profile settings globally.
            parallelFilesDir + "/apitest_db_profile_level.js",
            parallelFilesDir + "/geo_s2cursorlimitskip.js",
            parallelFilesDir + "/profile1.js",
            parallelFilesDir + "/profile2.js",
            parallelFilesDir + "/profile3.js",
            parallelFilesDir + "/profile_agg.js",
            parallelFilesDir + "/profile_count.js",
            parallelFilesDir + "/profile_delete.js",
            parallelFilesDir + "/profile_distinct.js",
            parallelFilesDir + "/profile_find.js",
            parallelFilesDir + "/profile_findandmodify.js",
            parallelFilesDir + "/profile_getmore.js",
            parallelFilesDir + "/profile_hide_index.js",
            parallelFilesDir + "/profile_insert.js",
            parallelFilesDir + "/profile_list_collections.js",
            parallelFilesDir + "/profile_list_indexes.js",
            parallelFilesDir + "/profile_mapreduce.js",
            parallelFilesDir + "/profile_no_such_db.js",
            parallelFilesDir + "/profile_query_hash.js",
            parallelFilesDir + "/profile_sampling.js",
            parallelFilesDir + "/profile_update.js",

            // These tests rely on a deterministically refreshable logical session cache. If they
            // run in parallel, they could interfere with the cache and cause failures.
            parallelFilesDir + "/list_all_local_sessions.js",
            parallelFilesDir + "/list_all_sessions.js",
            parallelFilesDir + "/list_local_sessions.js",
            parallelFilesDir + "/list_sessions.js",
        ];
        var serialTests = makeKeys(serialTestsArr);

        // prefix the first thread with the serialTests
        // (which we will exclude from the rest of the threads below)
        params[0] = serialTestsArr;
        var files = getFilesRecursive(parallelFilesDir);
        files = Array.shuffle(files);

        var i = 0;
        files.forEach(function(x) {
            if ((/[\/\\]_/.test(x.name)) || (!/\.js$/.test(x.name)) ||
                (x.name.match(parallelFilesDir + "/(.*\.js)")[1] in skipTests) ||  //
                (x.name in serialTests)) {
                print(" >>>>>>>>>>>>>>> skipping " + x.name);
                return;
            }
            // add the test to run in one of the threads.
            params[i % n].push(x.name);
            ++i;
        });

        // randomize ordering of the serialTests
        params[0] = Array.shuffle(params[0]);

        for (var i in params) {
            params[i].unshift(i);
        }

        return params;
    };

    // runs a set of test files
    // first argument is an identifier for this tester, remaining arguments are file names
    ParallelTester.fileTester = function() {
        var args = Array.from(arguments);
        var suite = args.shift();
        args.forEach(function(x) {
            print("         S" + suite + " Test : " + x + " ...");
            var time = Date.timeFunc(function() {
                // Create a new connection to the db for each file. If tests share the same
                // connection it can create difficult to debug issues.
                db = new Mongo(db.getMongo().host).getDB(db.getName());
                gc();
                load(x);
            }, 1);
            print("         S" + suite + " Test : " + x + " " + time + "ms");
        });
    };

    // params: array of arrays, each element of which consists of a function followed
    // by zero or more arguments to that function.  Each function and its arguments will
    // be called in a separate thread.
    // msg: failure message
    assert.parallelTests = function(params, msg) {
        function wrapper(fun, argv, globals) {
            if (globals.hasOwnProperty("TestData")) {
                TestData = globals.TestData;
            }

            try {
                fun.apply(0, argv);
                return {passed: true};
            } catch (e) {
                print("\n********** Parallel Test FAILED: " + tojson(e) + "\n");
                return {
                    passed: false,
                    testName: tojson(e).match(/Error: error loading js file: (.*\.js)/)[1]
                };
            }
        }

        var runners = new Array();
        for (var i in params) {
            var param = params[i];
            var test = param.shift();

            // Make a shallow copy of TestData so we can override the test name to
            // prevent tests on different threads that to use jsTestName() as the
            // collection name from colliding.
            const clonedTestData = Object.assign({}, TestData);
            clonedTestData.testName = `ParallelTesterThread${i}`;
            var t = new Thread(wrapper, test, param, {TestData: clonedTestData});
            runners.push(t);
        }

        runners.forEach(function(x) {
            x.start();
        });
        var nFailed = 0;
        var failedTests = [];
        // SpiderMonkey doesn't like it if we exit before all threads are joined
        // (see SERVER-19615 for a similar issue).
        runners.forEach(function(x) {
            if (!x.returnData().passed) {
                ++nFailed;
                failedTests.push(x.returnData().testName);
            }
        });
        msg += ": " + tojsononeline(failedTests);
        assert.eq(0, nFailed, msg);
    };
}

if (typeof CountDownLatch !== 'undefined') {
    CountDownLatch = Object.extend(function(count) {
        if (!(this instanceof CountDownLatch)) {
            return new CountDownLatch(count);
        }
        this._descriptor = CountDownLatch._new.apply(null, arguments);

        // NOTE: The following methods have to be defined on the instance itself,
        //       and not on its prototype. This is because properties on the
        //       prototype are lost during the serialization to BSON that occurs
        //       when passing data to a child thread.

        this.await = function() {
            CountDownLatch._await(this._descriptor);
        };
        this.countDown = function() {
            CountDownLatch._countDown(this._descriptor);
        };
        this.getCount = function() {
            return CountDownLatch._getCount(this._descriptor);
        };
    }, CountDownLatch);
}
