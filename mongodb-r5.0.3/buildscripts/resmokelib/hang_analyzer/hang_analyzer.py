"""Hang Analyzer module.

A prototype hang analyzer for Evergreen integration to help investigate test timeouts

1. Script supports taking dumps, and/or dumping a summary of useful information about a process
2. Script will iterate through a list of interesting processes,
    and run the tools from step 1. The list of processes can be provided as an option.
3. Java processes will be dumped using jstack, if available.

Supports Linux, MacOS X, and Windows.
"""
import glob
import logging
import os
import platform
import re
import signal
import sys
import traceback
import getpass

import psutil
import distro

from buildscripts.resmokelib import config as resmoke_core_config

from buildscripts.resmokelib.hang_analyzer import extractor
from buildscripts.resmokelib.hang_analyzer import dumper
from buildscripts.resmokelib.hang_analyzer import process
from buildscripts.resmokelib.hang_analyzer import process_list
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand


class HangAnalyzer(Subcommand):
    """Main class for the hang analyzer subcommand."""

    def __init__(self, options, logger=None, **kwargs):  # pylint: disable=unused-argument
        """
        Configure processe lists based on options.

        :param options: Options as parsed by parser.py
        :param logger: Logger to be used. If not specified, one will be created.
        :param kwargs: additional args
        """
        self.options = options
        self.root_logger = None
        self.interesting_processes = [
            # Remove "python", "java" from the list to avoid hang analyzer multiple invocations
            "mongo",
            "mongod",
            "mongos",
            "_test",
            "dbtest"
        ]
        self.go_processes = []
        self.process_ids = []

        self._configure_processes()
        self._setup_logging(logger)
        self.debug_symbols_url = self._configure_debug_symbols_download()

    def execute(self):  # pylint: disable=too-many-branches,too-many-locals,too-many-statements
        """
        Execute hang analysis.

        1. Get a list of interesting processes
        2. Dump useful information or take core dumps
        """

        self._log_system_info()

        extractor.extract_debug_symbols(self.root_logger, self.debug_symbols_url)
        dumpers = dumper.get_dumpers(self.root_logger, self.options.debugger_output)

        processes = process_list.get_processes(self.process_ids, self.interesting_processes,
                                               self.options.process_match, self.root_logger)

        max_dump_size_bytes = int(self.options.max_core_dumps_size) * 1024 * 1024

        # Suspending all processes, except python, to prevent them from getting unstuck when
        # the hang analyzer attaches to them.
        for pinfo in [pinfo for pinfo in processes if not pinfo.name.startswith("python")]:
            for pid in pinfo.pidv:
                process.pause_process(self.root_logger, pinfo.name, pid)

        # Dump python processes by signalling them. The resmoke.py process will generate
        # the report.json, when signalled, so we do this before attaching to other processes.
        for pinfo in [pinfo for pinfo in processes if pinfo.name.startswith("python")]:
            for pid in pinfo.pidv:
                process.signal_python(self.root_logger, pinfo.name, pid)

        trapped_exceptions = []

        dump_pids = {}
        # Dump all processes, except python & java.
        for pinfo in [pinfo for pinfo in processes if not re.match("^(java|python)", pinfo.name)]:
            try:
                dumpers.dbg.dump_info(
                    pinfo, self.options.dump_core
                    and _check_dump_quota(max_dump_size_bytes, dumpers.dbg.get_dump_ext()))
            except dumper.DumpError as err:
                self.root_logger.error(err.message)
                dump_pids = {**err.dump_pids, **dump_pids}
            except Exception as err:  # pylint: disable=broad-except
                self.root_logger.info("Error encountered when invoking debugger %s", err)
                trapped_exceptions.append(traceback.format_exc())

        # Dump java processes using jstack.
        for pinfo in [pinfo for pinfo in processes if pinfo.name.startswith("java")]:
            for pid in pinfo.pidv:
                try:
                    dumpers.jstack.dump_info(self.root_logger, self.options.debugger_output,
                                             pinfo.name, pid)
                except Exception as err:  # pylint: disable=broad-except
                    self.root_logger.info("Error encountered when invoking debugger %s", err)
                    trapped_exceptions.append(traceback.format_exc())

        # Signal go processes to ensure they print out stack traces, and die on POSIX OSes.
        # On Windows, this will simply kill the process since python emulates SIGABRT as
        # TerminateProcess.
        # Note: The stacktrace output may be captured elsewhere (i.e. resmoke).
        for pinfo in [pinfo for pinfo in processes if pinfo.name in self.go_processes]:
            for pid in pinfo.pidv:
                self.root_logger.info("Sending signal SIGABRT to go process %s with PID %d",
                                      pinfo.name, pid)
                process.signal_process(self.root_logger, pid, signal.SIGABRT)

        self.root_logger.info("Done analyzing all processes for hangs")

        # Kill and abort processes if "-k" was specified.
        if self.options.kill_processes:
            process.teardown_processes(self.root_logger, processes, dump_pids)
        else:
            # Resuming all suspended processes.
            for pinfo in [pinfo for pinfo in processes if not pinfo.name.startswith("python")]:
                for pid in pinfo.pidv:
                    process.resume_process(self.root_logger, pinfo.name, pid)

        for exception in trapped_exceptions:
            self.root_logger.info(exception)
        if trapped_exceptions:
            raise RuntimeError(
                "Exceptions were thrown while dumping. There may still be some valid dumps.")

    def _configure_debug_symbols_download(self):
        resmoke_constructed_url = resmoke_core_config.DEBUG_SYMBOLS_URL
        user_configured_url = self.options.debug_symbols_url
        if resmoke_constructed_url and user_configured_url:
            raise ValueError("The debug symbols URL has to be either generated, "
                             "or passed in through the command line, but not both. "
                             "resmoke_constructed_url: %s, user_configured_url: %s" %
                             (resmoke_constructed_url, user_configured_url))

        return resmoke_constructed_url or user_configured_url

    def _configure_processes(self):
        if self.options.debugger_output is None:
            self.options.debugger_output = ['stdout']

        if self.options.process_ids is not None:
            # self.process_ids is an int list of PIDs
            self.process_ids = [int(pid) for pid in self.options.process_ids.split(',')]

        if self.options.process_names is not None:
            self.interesting_processes = self.options.process_names.split(',')

        if self.options.go_process_names is not None:
            self.go_processes = self.options.go_process_names.split(',')
            self.interesting_processes += self.go_processes

    def _setup_logging(self, logger):
        if logger is None:
            self.root_logger = logging.Logger("hang_analyzer", level=logging.DEBUG)
        else:
            self.root_logger = logger

        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        self.root_logger.addHandler(handler)

    def _log_system_info(self):
        self.root_logger.info("Python Version: %s", sys.version)
        self.root_logger.info("OS: %s", platform.platform())

        try:
            if sys.platform == "win32" or sys.platform == "cygwin":
                self.root_logger.info("Windows Distribution: %s", platform.win32_ver())
            else:
                self.root_logger.info("Linux Distribution: %s", distro.linux_distribution())

        except AttributeError:
            self.root_logger.warning("Cannot determine Linux distro since Python is too old")

        try:
            current_login = getpass.getuser()
            self.root_logger.info("Current Login: %s", current_login)
            uid = os.getuid()
            self.root_logger.info("Current UID: %s", uid)
        except AttributeError:
            self.root_logger.warning(
                "Cannot determine Unix Current Login, not supported on Windows")


def _check_dump_quota(quota, ext):
    """Check if sum of the files with ext is within the specified quota in megabytes."""

    files = glob.glob("*." + ext)

    size_sum = 0
    for file_name in files:
        size_sum += os.path.getsize(file_name)

    return size_sum <= quota


class HangAnalyzerPlugin(PluginInterface):
    """Integration-point for hang-analyzer."""

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""
        if subcommand == 'hang-analyzer':
            return HangAnalyzer(parsed_args, **kwargs)
        return None

    def add_subcommand(self, subparsers):
        """Create and add the parser for the hang analyzer subcommand."""
        parser = subparsers.add_parser("hang-analyzer", help=__doc__)

        parser.add_argument(
            '-m', '--process-match', dest='process_match', choices=('contains',
                                                                    'exact'), default='contains',
            help="Type of match for process names (-p & -g), specify 'contains', or"
            " 'exact'. Note that the process name match performs the following"
            " conversions: change all process names to lowecase, strip off the file"
            " extension, like '.exe' on Windows. Default is 'contains'.")
        parser.add_argument('-p', '--process-names', dest='process_names',
                            help='Comma separated list of process names to analyze')
        parser.add_argument('-g', '--go-process-names', dest='go_process_names',
                            help='Comma separated list of go process names to analyze')
        parser.add_argument(
            '-d', '--process-ids', dest='process_ids', default=None,
            help='Comma separated list of process ids (PID) to analyze, overrides -p &'
            ' -g')
        parser.add_argument('-c', '--dump-core', dest='dump_core', action="store_true",
                            default=False, help='Dump core file for each analyzed process')
        parser.add_argument('-s', '--max-core-dumps-size', dest='max_core_dumps_size',
                            default=10000,
                            help='Maximum total size of core dumps to keep in megabytes')
        parser.add_argument(
            '-o', '--debugger-output', dest='debugger_output', action="append", choices=('file',
                                                                                         'stdout'),
            default=None, help="If 'stdout', then the debugger's output is written to the Python"
            " process's stdout. If 'file', then the debugger's output is written"
            " to a file named debugger_<process>_<pid>.log for each process it"
            " attaches to. This option can be specified multiple times on the"
            " command line to have the debugger's output written to multiple"
            " locations. By default, the debugger's output is written only to the"
            " Python process's stdout.")
        parser.add_argument('-k', '--kill-processes', dest='kill_processes', action="store_true",
                            default=False,
                            help="Kills the analyzed processes after analysis completes.")
        parser.add_argument('-ds', '--debug-symbols-url', dest='debug_symbols_url', metavar="URL",
                            type=str, default=None)
