"""Utility functions for creating MongoDB processes.

Handles all the nitty-gritty parameter conversion.
"""

import json
import os
import os.path
import stat

from buildscripts.resmokelib import config
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.core import jasper_process
from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.core import network
from buildscripts.resmokelib.testing.fixtures import standalone, shardedcluster
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib
from buildscripts.resmokelib.utils.history import make_historic, HistoryDict


def make_process(*args, **kwargs):
    """Choose whether to use python built in process or jasper."""
    process_cls = process.Process
    if config.SPAWN_USING == "jasper":
        process_cls = jasper_process.Process
    else:
        # remove jasper process specific args
        kwargs.pop("job_num", None)
        kwargs.pop("test_id", None)

    # Add the current working directory and /data/multiversion to the PATH.
    env_vars = kwargs.get("env_vars", {}).copy()
    path = [
        os.getcwd(),
        config.DEFAULT_MULTIVERSION_DIR,
    ]

    # If installDir is provided, add it early to the path
    if config.INSTALL_DIR is not None:
        path.append(config.INSTALL_DIR)
        env_vars["INSTALL_DIR"] = config.INSTALL_DIR

    path.append(env_vars.get("PATH", os.environ.get("PATH", "")))

    env_vars["PATH"] = os.pathsep.join(path)
    kwargs["env_vars"] = env_vars
    return process_cls(*args, **kwargs)


def mongod_program(logger, job_num, executable, process_kwargs, mongod_options):
    """
    Return a Process instance that starts mongod arguments constructed from 'mongod_options'.

    @param logger - The logger to pass into the process.
    @param executable - The mongod executable to run.
    @param process_kwargs - A dict of key-value pairs to pass to the process.
    @param mongod_options - A HistoryDict describing the various options to pass to the mongod.
    """

    args = [executable]
    mongod_options = mongod_options.copy()

    if "port" not in mongod_options:
        mongod_options["port"] = network.PortAllocator.next_fixture_port(job_num)
    suite_set_parameters = mongod_options.get("set_parameters", {})
    _apply_set_parameters(args, suite_set_parameters)
    mongod_options.pop("set_parameters")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, mongod_options)

    _set_keyfile_permissions(mongod_options)

    process_kwargs = make_historic(utils.default_if_none(process_kwargs, {}))
    process_kwargs["job_num"] = job_num
    if config.EXPORT_MONGOD_CONFIG == "regular":
        mongod_options.dump_history(f"{logger.name}_config.yml")
    elif config.EXPORT_MONGOD_CONFIG == "detailed":
        mongod_options.dump_history(f"{logger.name}_config.yml", include_location=True)
    return make_process(logger, args, **process_kwargs), mongod_options["port"]


def mongos_program(  # pylint: disable=too-many-arguments
        logger, job_num, test_id=None, executable=None, process_kwargs=None, mongos_options=None):
    """Return a Process instance that starts a mongos with arguments constructed from 'kwargs'."""
    args = [executable]

    mongos_options = mongos_options.copy()

    if "port" not in mongos_options:
        mongos_options["port"] = network.PortAllocator.next_fixture_port(job_num)
    suite_set_parameters = mongos_options.get("set_parameters", {})
    _apply_set_parameters(args, suite_set_parameters)
    mongos_options.pop("set_parameters")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, mongos_options)

    _set_keyfile_permissions(mongos_options)

    process_kwargs = make_historic(utils.default_if_none(process_kwargs, {}))
    process_kwargs["job_num"] = job_num
    process_kwargs["test_id"] = test_id
    return make_process(logger, args, **process_kwargs), mongos_options["port"]


def mongo_shell_program(  # pylint: disable=too-many-arguments,too-many-branches,too-many-locals,too-many-statements
        logger, job_num, test_id=None, executable=None, connection_string=None, filename=None,
        process_kwargs=None, **kwargs):
    """Return a Process instance that starts a mongo shell.

    The shell is started with the given connection string and arguments constructed from 'kwargs'.
    """

    executable = utils.default_if_none(
        utils.default_if_none(executable, config.MONGO_EXECUTABLE), config.DEFAULT_MONGO_EXECUTABLE)
    args = [executable]

    eval_sb = []  # String builder.
    global_vars = kwargs.pop("global_vars", {}).copy()

    if filename is not None:
        test_name = os.path.splitext(os.path.basename(filename))[0]
    else:
        test_name = None
    shortcut_opts = {
        "backupOnRestartDir": (config.BACKUP_ON_RESTART_DIR, None),
        "enableMajorityReadConcern": (config.MAJORITY_READ_CONCERN, True),
        "mixedBinVersions": (config.MIXED_BIN_VERSIONS, ""),
        "noJournal": (config.NO_JOURNAL, False),
        "storageEngine": (config.STORAGE_ENGINE, ""),
        "storageEngineCacheSizeGB": (config.STORAGE_ENGINE_CACHE_SIZE, ""),
        "testName": (test_name, ""),
        "transportLayer": (config.TRANSPORT_LAYER, ""),
        "wiredTigerCollectionConfigString": (config.WT_COLL_CONFIG, ""),
        "wiredTigerEngineConfigString": (config.WT_ENGINE_CONFIG, ""),
        "wiredTigerIndexConfigString": (config.WT_INDEX_CONFIG, ""),

        # Evergreen variables.
        "evergreenDebugSymbolsUrl": (config.DEBUG_SYMBOLS_URL, ""),
    }

    test_data = global_vars.get("TestData", {}).copy()
    for opt_name in shortcut_opts:
        (opt_value, opt_default) = shortcut_opts[opt_name]
        if opt_value is not None:
            test_data[opt_name] = opt_value
        elif opt_name not in test_data:
            # Only use 'opt_default' if the property wasn't set in the YAML configuration.
            test_data[opt_name] = opt_default

    global_vars["TestData"] = test_data

    if config.EVERGREEN_TASK_ID is not None:
        test_data["inEvergreen"] = True

    # Initialize setParameters for mongod and mongos, to be passed to the shell via TestData. Since
    # they are dictionaries, they will be converted to JavaScript objects when passed to the shell
    # by the _format_shell_vars() function.
    mongod_set_parameters = test_data.get("setParameters", {}).copy()
    mongos_set_parameters = test_data.get("setParametersMongos", {}).copy()
    mongocryptd_set_parameters = test_data.get("setParametersMongocryptd", {}).copy()

    # Propagate additional setParameters to mongod processes spawned by the mongo shell. Command
    # line options to resmoke.py override the YAML configuration.
    if config.MONGOD_SET_PARAMETERS is not None:
        mongod_set_parameters.update(utils.load_yaml(config.MONGOD_SET_PARAMETERS))

    # Propagate additional setParameters to mongos processes spawned by the mongo shell. Command
    # line options to resmoke.py override the YAML configuration.
    if config.MONGOS_SET_PARAMETERS is not None:
        mongos_set_parameters.update(utils.load_yaml(config.MONGOS_SET_PARAMETERS))

    # Propagate additional setParameters to mongocryptd processes spawned by the mongo shell.
    # Command line options to resmoke.py override the YAML configuration.
    if config.MONGOCRYPTD_SET_PARAMETERS is not None:
        mongocryptd_set_parameters.update(utils.load_yaml(config.MONGOCRYPTD_SET_PARAMETERS))

    fixturelib = FixtureLib()
    mongod_launcher = standalone.MongodLauncher(fixturelib)

    # If the 'logComponentVerbosity' setParameter for mongod was not already specified, we set its
    # value to a default.
    mongod_set_parameters.setdefault("logComponentVerbosity",
                                     mongod_launcher.default_mongod_log_component_verbosity())

    # If the 'enableFlowControl' setParameter for mongod was not already specified, we set its value
    # to a default.
    if config.FLOW_CONTROL is not None:
        mongod_set_parameters.setdefault("enableFlowControl", config.FLOW_CONTROL == "on")

    # Set the default value for minimum resharding operation duration to 5 seconds.
    mongod_set_parameters.setdefault("reshardingMinimumOperationDurationMillis", 5000)

    mongos_launcher = shardedcluster.MongosLauncher(fixturelib)
    # If the 'logComponentVerbosity' setParameter for mongos was not already specified, we set its
    # value to a default.
    mongos_set_parameters.setdefault("logComponentVerbosity",
                                     mongos_launcher.default_mongos_log_component_verbosity())

    test_data["setParameters"] = mongod_set_parameters
    test_data["setParametersMongos"] = mongos_set_parameters
    test_data["setParametersMongocryptd"] = mongocryptd_set_parameters

    test_data["undoRecorderPath"] = config.UNDO_RECORDER_PATH

    # There's a periodic background thread that checks for and aborts expired transactions.
    # "transactionLifetimeLimitSeconds" specifies for how long a transaction can run before expiring
    # and being aborted by the background thread. It defaults to 60 seconds, which is too short to
    # be reliable for our tests. Setting it to 24 hours, so that it is longer than the Evergreen
    # execution timeout.
    if "transactionLifetimeLimitSeconds" not in test_data:
        test_data["transactionLifetimeLimitSeconds"] = 24 * 60 * 60

    if "eval_prepend" in kwargs:
        eval_sb.append(str(kwargs.pop("eval_prepend")))

    # If nodb is specified, pass the connection string through TestData so it can be used inside the
    # test, then delete it so it isn't given as an argument to the mongo shell.
    if "nodb" in kwargs and connection_string is not None:
        test_data["connectionString"] = connection_string
        connection_string = None

    for var_name in global_vars:
        _format_shell_vars(eval_sb, [var_name], global_vars[var_name])

    if "eval" in kwargs:
        eval_sb.append(str(kwargs.pop("eval")))

    # Load this file to allow a callback to validate collections before shutting down mongod.
    eval_sb.append("load('jstests/libs/override_methods/validate_collections_on_shutdown.js');")

    # Load a callback to check UUID consistency before shutting down a ShardingTest.
    eval_sb.append(
        "load('jstests/libs/override_methods/check_uuids_consistent_across_cluster.js');")

    # Load a callback to check index consistency before shutting down a ShardingTest.
    eval_sb.append(
        "load('jstests/libs/override_methods/check_indexes_consistent_across_cluster.js');")

    # Load a callback to check that all orphans are deleted before shutting down a ShardingTest.
    eval_sb.append("load('jstests/libs/override_methods/check_orphans_are_deleted.js');")

    # Load this file to retry operations that fail due to in-progress background operations.
    eval_sb.append(
        "load('jstests/libs/override_methods/implicitly_retry_on_background_op_in_progress.js');")

    eval_sb.append(
        "(function() { Timestamp.prototype.toString = function() { throw new Error(\"Cannot toString timestamps. Consider using timestampCmp() for comparison or tojson(<variable>) for output.\"); } })();"
    )

    eval_str = "; ".join(eval_sb)
    args.append("--eval")
    args.append(eval_str)

    if config.SHELL_READ_MODE is not None:
        kwargs["readMode"] = config.SHELL_READ_MODE

    if config.SHELL_WRITE_MODE is not None:
        kwargs["writeMode"] = config.SHELL_WRITE_MODE

    if connection_string is not None:
        # The --host and --port options are ignored by the mongo shell when an explicit connection
        # string is specified. We remove these options to avoid any ambiguity with what server the
        # logged mongo shell invocation will connect to.
        if "port" in kwargs:
            kwargs.pop("port")

        if "host" in kwargs:
            kwargs.pop("host")

    # Apply the rest of the command line arguments.
    _apply_kwargs(args, kwargs)

    if connection_string is not None:
        args.append(connection_string)

    # Have the mongo shell run the specified file.
    if filename is not None:
        args.append(filename)

    _set_keyfile_permissions(test_data)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    process_kwargs["job_num"] = job_num
    process_kwargs["test_id"] = test_id
    return make_process(logger, args, **process_kwargs)


def _format_shell_vars(sb, paths, value):
    """
    Format 'value' in a way that can be passed to --eval.

    :param sb: string builder array for the output string.
    :param paths: path of keys represented as a list.
    :param value: value in the object corresponding to the keys in `paths`
    :return: Nothing.
    """

    # Convert the list ["a", "b", "c"] into the string 'a["b"]["c"]'
    def bracketize(lst):
        return lst[0] + ''.join(f'["{i}"]' for i in lst[1:])

    # Only need to do special handling for JSON objects.
    if not isinstance(value, (dict, HistoryDict)):
        sb.append("%s = %s" % (bracketize(paths), json.dumps(value)))
        return

    # Avoid including curly braces and colons in output so that the command invocation can be
    # copied and run through bash.
    sb.append("%s = new Object()" % bracketize(paths))
    for subkey in value:
        _format_shell_vars(sb, paths + [subkey], value[subkey])


def dbtest_program(logger, job_num, test_id=None, executable=None, suites=None, process_kwargs=None,
                   **kwargs):  # pylint: disable=too-many-arguments
    """Return a Process instance that starts a dbtest with arguments constructed from 'kwargs'."""

    executable = utils.default_if_none(executable, config.DEFAULT_DBTEST_EXECUTABLE)
    args = [executable]

    if suites is not None:
        args.extend(suites)

    kwargs["enableMajorityReadConcern"] = config.MAJORITY_READ_CONCERN
    if config.STORAGE_ENGINE is not None:
        kwargs["storageEngine"] = config.STORAGE_ENGINE

    if config.FLOW_CONTROL is not None:
        kwargs["flowControl"] = (config.FLOW_CONTROL == "on")

    return generic_program(logger, args, job_num, test_id=test_id, process_kwargs=process_kwargs,
                           **kwargs)


def genny_program(logger, job_num, test_id=None, executable=None, process_kwargs=None, **kwargs):
    """Return a Process instance that starts a genny executable with arguments constructed from 'kwargs'."""
    executable = utils.default_if_none(executable, config.DEFAULT_GENNY_EXECUTABLE)
    args = [executable]
    return generic_program(logger, args, job_num, test_id=test_id, process_kwargs=process_kwargs,
                           **kwargs)


def generic_program(logger, args, job_num, test_id=None, process_kwargs=None, **kwargs):
    """Return a Process instance that starts an arbitrary executable.

    The executable arguments are constructed from 'kwargs'.

    The args parameter is an array of strings containing the command to execute.
    """

    if not utils.is_string_list(args):
        raise ValueError("The args parameter must be a list of command arguments")

    _apply_kwargs(args, kwargs)

    process_kwargs = utils.default_if_none(process_kwargs, {})
    process_kwargs["job_num"] = job_num
    process_kwargs["test_id"] = test_id
    return make_process(logger, args, **process_kwargs)


def _apply_set_parameters(args, set_parameter):
    """Convert key-value pairs from 'kwargs' into --setParameter key=value arguments.

    This result is appended to 'args'.
    """

    for param_name in set_parameter:
        param_value = set_parameter[param_name]
        # --setParameter takes boolean values as lowercase strings.
        if isinstance(param_value, bool):
            param_value = "true" if param_value else "false"
        args.append("--setParameter")
        args.append("%s=%s" % (param_name, param_value))


def _apply_kwargs(args, kwargs):
    """Convert key-value pairs from 'kwargs' into --key value arguments.

    This result is appended to 'args'.
    A --flag without a value is represented with the empty string.
    """

    for arg_name in kwargs:
        arg_value = str(kwargs[arg_name])
        if arg_value:
            args.append("--%s=%s" % (arg_name, arg_value))
        else:
            args.append("--%s" % (arg_name))


def _set_keyfile_permissions(opts):
    """Change the permissions of keyfiles in 'opts' to 600, (only user can read and write the file).

    This necessary to avoid having the mongod/mongos fail to start up
    because "permissions on the keyfiles are too open".

    We can't permanently set the keyfile permissions because git is not
    aware of them.
    """
    if "keyFile" in opts:
        os.chmod(opts["keyFile"], stat.S_IRUSR | stat.S_IWUSR)
    if "encryptionKeyFile" in opts:
        os.chmod(opts["encryptionKeyFile"], stat.S_IRUSR | stat.S_IWUSR)
