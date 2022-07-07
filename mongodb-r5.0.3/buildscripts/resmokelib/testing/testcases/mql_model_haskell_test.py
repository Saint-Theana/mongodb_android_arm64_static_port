"""The unittest.TestCase for MQL Haskell tests."""

import os
import os.path

from buildscripts.resmokelib import core
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.testcases import interface
from buildscripts.resmokelib.utils import globstar


class MqlModelHaskellTestCase(interface.ProcessTestCase):
    """A MQL Haskell Model test to execute."""

    REGISTERED_NAME = "mql_model_haskell_test"

    def __init__(self, logger, json_filename, mql_executable=None):
        """Initialize the MqlModelHaskellTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "MQL Haskell Model test", json_filename)

        self.json_test_file = json_filename

        # Determine the top level directory where we start a search for a mql binary
        self.top_level_dirname = os.path.join(os.path.normpath(json_filename).split(os.sep)[0], "")

        # Our haskell cabal build produces binaries in an unique directory
        # .../dist-sandbox-<some hex hash>/...
        # so we use a glob pattern to fish out the binary
        mql_executable = utils.default_if_none(mql_executable, "mql-model/dist/dist*/build/mql/mql")
        execs = globstar.glob(mql_executable)
        if len(execs) != 1:
            raise errors.StopExecution("There must be a single mql binary in {}".format(execs))

        self.program_executable = execs[0]

    def _make_process(self):
        program_opts = {"job_num": self.fixture.job_num, "test_id": self._id}
        return core.programs.make_process(self.logger, [
            self.program_executable, "--test", self.json_test_file, "--prefix",
            self.top_level_dirname
        ], **program_opts)
