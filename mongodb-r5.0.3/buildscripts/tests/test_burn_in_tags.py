"""Unit tests for the burn_in_tags.py script."""
from collections import defaultdict
import json
import os
import sys
import unittest
from unittest.mock import MagicMock, patch

from shrub.v2 import ShrubProject

import buildscripts.ciconfig.evergreen as _evergreen
from buildscripts.burn_in_tests import TaskInfo
from buildscripts.tests.test_burn_in_tests import ns as burn_in_tests_ns
from buildscripts.ciconfig.evergreen import EvergreenProjectConfig

import buildscripts.burn_in_tags as under_test

# pylint: disable=missing-docstring,invalid-name,unused-argument,no-self-use,protected-access

EMPTY_PROJECT = {
    "buildvariants": [],
    "tasks": [],
}
TEST_FILE_PATH = os.path.join(os.path.dirname(__file__), "test_burn_in_tags_evergreen.yml")

NS = "buildscripts.burn_in_tags"


def ns(relative_name):  # pylint: disable-invalid-name
    """Return a full name from a name relative to the test module"s name space."""
    return NS + "." + relative_name


def get_expansions_data():
    return {
            "branch_name": "fake_branch",
            "build_variant": "enterprise-rhel-80-64-bit",
            "check_evergreen": 2,
            "distro_id": "rhel80-small",
            "is_patch": "true",
            "max_revisions": 25,
            "repeat_tests_max": 1000,
            "repeat_tests_min": 2,
            "repeat_tests_secs": 600,
            "revision": "fake_sha",
            "project": "fake_project",
            "task_id": "task id",
    }  # yapf: disable


def get_evergreen_config() -> EvergreenProjectConfig:
    return _evergreen.parse_evergreen_file(TEST_FILE_PATH, evergreen_binary=None)


class TestCreateEvgBuildVariantMap(unittest.TestCase):
    def test_create_evg_buildvariant_map(self):
        evg_conf_mock = get_evergreen_config()
        expansions_file_data = {"build_variant": "enterprise-rhel-80-64-bit"}

        buildvariant_map = under_test._create_evg_build_variant_map(expansions_file_data,
                                                                    evg_conf_mock)

        expected_buildvariant_map = {
            "enterprise-rhel-80-64-bit-majority-read-concern-off":
                "enterprise-rhel-80-64-bit-majority-read-concern-off-required",
            "enterprise-rhel-80-64-bit-inmem":
                "enterprise-rhel-80-64-bit-inmem-required"
        }
        self.assertEqual(buildvariant_map, expected_buildvariant_map)

    def test_create_evg_buildvariant_map_no_base_variants(self):
        evg_conf_mock = MagicMock()
        evg_conf_mock.parse_evergreen_file.return_value = get_evergreen_config()
        expansions_file_data = {"build_variant": "buildvariant-without-burn-in-tag-buildvariants"}

        buildvariant_map = under_test._create_evg_build_variant_map(expansions_file_data,
                                                                    evg_conf_mock)

        self.assertEqual(buildvariant_map, {})


class TestGenerateEvgBuildVariants(unittest.TestCase):
    def test_generate_evg_buildvariant_one_base_variant(self):
        evg_conf_mock = get_evergreen_config()
        base_variant = "enterprise-rhel-80-64-bit-inmem"
        generated_variant = "enterprise-rhel-80-64-bit-inmem-required"
        burn_in_tags_gen_variant = "enterprise-rhel-80-64-bit"
        variant = evg_conf_mock.get_variant(base_variant)

        build_variant = under_test._generate_evg_build_variant(variant, generated_variant,
                                                               burn_in_tags_gen_variant)

        generated_build_variant = build_variant.as_dict()
        self.assertEqual(generated_build_variant["name"], generated_variant)
        self.assertEqual(generated_build_variant["modules"], variant.modules)
        generated_expansions = generated_build_variant["expansions"]
        burn_in_bypass_expansion_value = generated_expansions.pop("burn_in_bypass")
        self.assertEqual(burn_in_bypass_expansion_value, burn_in_tags_gen_variant)
        self.assertEqual(generated_expansions, variant.expansions)


class TestGenerateEvgTasks(unittest.TestCase):
    @patch(ns("create_tests_by_task"))
    def test_generate_evg_tasks_no_tests_changed(self, create_tests_by_task_mock):
        evg_conf_mock = get_evergreen_config()
        create_tests_by_task_mock.return_value = {}
        expansions_file_data = get_expansions_data()
        buildvariant_map = {
            "enterprise-rhel-80-64-bit-inmem": "enterprise-rhel-80-64-bit-inmem-required",
            "enterprise-rhel-80-64-bit-majority-read-concern-off":
                "enterprise-rhel-80-64-bit-majority-read-concern-off-required",
        }  # yapf: disable
        shrub_config = ShrubProject()
        evergreen_api = MagicMock()
        repo = MagicMock(working_dir=os.getcwd())
        under_test._generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data,
                                       buildvariant_map, [repo], evg_conf_mock)

        self.assertEqual(shrub_config.as_dict(), EMPTY_PROJECT)

    @patch(ns("create_tests_by_task"))
    def test_generate_evg_tasks_one_test_changed(self, create_tests_by_task_mock):
        evg_conf_mock = get_evergreen_config()
        create_tests_by_task_mock.return_value = {
            "aggregation_mongos_passthrough": TaskInfo(
                display_task_name="aggregation_mongos_passthrough",
                resmoke_args="--suites=aggregation_mongos_passthrough --storageEngine=wiredTiger",
                tests=["jstests/aggregation/ifnull.js"],
                require_multiversion=None,
                distro="",
            )
        }  # yapf: disable
        expansions_file_data = get_expansions_data()
        buildvariant_map = {
            "enterprise-rhel-80-64-bit-inmem": "enterprise-rhel-80-64-bit-inmem-required",
            "enterprise-rhel-80-64-bit-majority-read-concern-off":
                "enterprise-rhel-80-64-bit-majority-read-concern-off-required",
        }  # yapf: disable
        shrub_config = ShrubProject.empty()
        evergreen_api = MagicMock()
        repo = MagicMock(working_dir=os.getcwd())
        evergreen_api.test_stats_by_project.return_value = [
            MagicMock(test_file="dir/test2.js", avg_duration_pass=10)
        ]
        under_test._generate_evg_tasks(evergreen_api, shrub_config, expansions_file_data,
                                       buildvariant_map, [repo], evg_conf_mock)

        generated_config = shrub_config.as_dict()
        self.assertEqual(len(generated_config["buildvariants"]), 2)
        first_generated_build_variant = generated_config["buildvariants"][0]
        self.assertIn(first_generated_build_variant["name"], buildvariant_map.values())
        self.assertEqual(first_generated_build_variant["display_tasks"][0]["name"], "burn_in_tests")
        self.assertEqual(
            first_generated_build_variant["display_tasks"][0]["execution_tasks"][0],
            f"burn_in:aggregation_mongos_passthrough_0_{first_generated_build_variant['name']}")


EXPANSIONS_FILE_DATA = {
    "build_variant": "enterprise-rhel-80-64-bit",
    "revision": "badf00d000000000000000000000000000000000", "max_revisions": "1000",
    "branch_name": "mongodb-mongo-master", "is_patch": "false", "distro_id": "rhel62-small",
    "repeat_tests_min": "2", "repeat_tests_max": "1000", "repeat_tests_secs": "600", "project":
        "mongodb-mongo-master", "task_id": "task id"
}

CREATE_EVG_BUILD_VARIANT_MAP = {
    'enterprise-rhel-80-64-bit-majority-read-concern-off':
        'enterprise-rhel-80-64-bit-majority-read-concern-off-required',
    'enterprise-rhel-80-64-bit-inmem':
        'enterprise-rhel-80-64-bit-inmem-required'
}

CREATE_TEST_MEMBERSHIP_MAP = {
    "jstests/aggregation/accumulators/accumulator_js.js": [
        "aggregation", "aggregation_auth", "aggregation_disabled_optimization", "aggregation_ese",
        "aggregation_ese_gcm", "aggregation_facet_unwind_passthrough",
        "aggregation_mongos_passthrough", "aggregation_one_shard_sharded_collections",
        "aggregation_read_concern_majority_passthrough", "aggregation_secondary_reads",
        "aggregation_sharded_collections_passthrough"
    ], "jstests/core/create_collection.js": [
        "core", "core_auth", "core_ese", "core_ese_gcm", "core_minimum_batch_size", "core_op_query",
        "cwrwc_passthrough", "cwrwc_rc_majority_passthrough", "cwrwc_wc_majority_passthrough",
        "logical_session_cache_replication_100ms_refresh_jscore_passthrough",
        "logical_session_cache_replication_10sec_refresh_jscore_passthrough",
        "logical_session_cache_replication_1sec_refresh_jscore_passthrough",
        "logical_session_cache_replication_default_refresh_jscore_passthrough",
        "logical_session_cache_standalone_100ms_refresh_jscore_passthrough",
        "logical_session_cache_standalone_10sec_refresh_jscore_passthrough",
        "logical_session_cache_standalone_1sec_refresh_jscore_passthrough",
        "logical_session_cache_standalone_default_refresh_jscore_passthrough",
        "read_concern_linearizable_passthrough", "read_concern_majority_passthrough",
        "causally_consistent_read_concern_snapshot_passthrough",
        "replica_sets_initsync_jscore_passthrough",
        "replica_sets_initsync_static_jscore_passthrough", "replica_sets_jscore_passthrough",
        "replica_sets_kill_primary_jscore_passthrough",
        "replica_sets_kill_secondaries_jscore_passthrough",
        "replica_sets_reconfig_jscore_passthrough",
        "replica_sets_terminate_primary_jscore_passthrough", "retryable_writes_jscore_passthrough",
        "retryable_writes_jscore_stepdown_passthrough", "secondary_reads_passthrough",
        "session_jscore_passthrough", "write_concern_majority_passthrough"
    ]
}


class TestAcceptance(unittest.TestCase):
    @patch(ns("write_file_to_dir"))
    @patch(ns("_create_evg_build_variant_map"))
    @patch(ns("EvergreenFileChangeDetector"))
    def test_no_tests_run_if_none_changed(self, find_changed_tests_mock,
                                          create_evg_build_variant_map_mock, write_to_file_mock):
        """
        Given a git repository with no changes,
        When burn_in_tags is run,
        Then no tests are discovered to run.
        """
        repos = [MagicMock(working_dir=os.getcwd())]
        evg_conf_mock = MagicMock()
        find_changed_tests_mock.return_value.find_changed_tests.return_value = {}

        create_evg_build_variant_map_mock.return_value = CREATE_EVG_BUILD_VARIANT_MAP

        under_test.burn_in(EXPANSIONS_FILE_DATA, evg_conf_mock, MagicMock(), repos)

        write_to_file_mock.assert_called_once()
        shrub_config = write_to_file_mock.call_args[0][2]
        self.assertEqual(EMPTY_PROJECT, json.loads(shrub_config))

    @unittest.skipIf(sys.platform.startswith("win"), "not supported on windows")
    @patch(ns("write_file_to_dir"))
    @patch(ns("_create_evg_build_variant_map"))
    @patch(ns("EvergreenFileChangeDetector"))
    @patch(burn_in_tests_ns("create_test_membership_map"))
    def test_tests_generated_if_a_file_changed(
            self, create_test_membership_map_mock, find_changed_tests_mock,
            create_evg_build_variant_map_mock, write_to_file_mock):
        """
        Given a git repository with changes,
        When burn_in_tags is run,
        Then some tags are discovered to run.
        """
        create_test_membership_map_mock.return_value = defaultdict(list, CREATE_TEST_MEMBERSHIP_MAP)

        repos = [MagicMock(working_dir=os.getcwd())]
        evg_conf = get_evergreen_config()
        create_evg_build_variant_map_mock.return_value = CREATE_EVG_BUILD_VARIANT_MAP
        find_changed_tests_mock.return_value.find_changed_tests.return_value = {
            'jstests/slow1/large_role_chain.js',
            'jstests/aggregation/accumulators/accumulator_js.js'
        }

        under_test.burn_in(EXPANSIONS_FILE_DATA, evg_conf, MagicMock(), repos)

        write_to_file_mock.assert_called_once()
        written_config = write_to_file_mock.call_args[0][2]
        written_config_map = json.loads(written_config)

        n_tasks = len(written_config_map["tasks"])
        # Ensure we are generating at least one task for the test.
        self.assertGreaterEqual(n_tasks, 1)

        written_build_variants = written_config_map["buildvariants"]
        written_build_variants_name = [variant['name'] for variant in written_build_variants]
        self.assertEqual(
            set(CREATE_EVG_BUILD_VARIANT_MAP.values()), set(written_build_variants_name))

        tasks = written_config_map["tasks"]
        self.assertGreaterEqual(len(tasks), len(CREATE_EVG_BUILD_VARIANT_MAP))

        self.assertTrue(
            all(
                len(display_tasks) == 1 for display_tasks in
                [build_variant["display_tasks"] for build_variant in written_build_variants]))
