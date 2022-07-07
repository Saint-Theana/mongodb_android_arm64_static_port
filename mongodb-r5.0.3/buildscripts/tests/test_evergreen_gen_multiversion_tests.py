''' Tests for the multiversion generators '''

import os
import unittest
from tempfile import TemporaryDirectory

from mock import patch, MagicMock
from click.testing import CliRunner

from buildscripts import evergreen_gen_multiversion_tests as under_test
from buildscripts.util.fileops import read_yaml_file

# pylint: disable=missing-docstring, no-self-use


class TestGenerateExcludeYaml(unittest.TestCase):
    def setUp(self):
        self._tmpdir = TemporaryDirectory()

    def tearDown(self):
        if self._tmpdir is not None:
            self._tmpdir.cleanup()

    def assert_contents(self, expected):
        actual = read_yaml_file(os.path.join(self._tmpdir.name, under_test.EXCLUDE_TAGS_FILE))
        self.assertEqual(actual, expected)

    def patch_and_run(self, latest, last_lts):
        """
        Helper to patch and run the test.
        """
        mock_multiversion_methods = {
            'get_backports_required_hash_for_shell_version': MagicMock(),
            'get_last_lts_yaml': MagicMock(return_value=last_lts)
        }

        with patch.multiple('buildscripts.evergreen_gen_multiversion_tests',
                            **mock_multiversion_methods):
            with patch('buildscripts.evergreen_gen_multiversion_tests.read_yaml_file',
                       return_value=latest) as mock_read_yaml:

                output = os.path.join(self._tmpdir.name, under_test.EXCLUDE_TAGS_FILE)
                runner = CliRunner()
                result = runner.invoke(under_test.generate_exclude_yaml, [f"--output={output}"])

                self.assertEqual(result.exit_code, 0, result)
                mock_read_yaml.assert_called_once()
                mock_multiversion_methods[
                    'get_backports_required_hash_for_shell_version'].assert_called_once()
                mock_multiversion_methods['get_last_lts_yaml'].assert_called_once()

    def test_create_yaml_suite1(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}],
                'suites': {
                    'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                               {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        last_lts_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites':
                    {'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]}
            }
        }

        expected = {
            'selector': {
                'js_test': {'jstests/fake_file1.js': ['suite1_backport_required_multiversion']}
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_suite2(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}],
                'suites': {
                    'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                               {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}],
                    'suite2': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'}]
                }
            }
        }

        last_lts_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites':
                    {'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]}
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': [
                        'suite1_backport_required_multiversion',
                        'suite2_backport_required_multiversion'
                    ]
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_both_all_are_none(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': None, 'suites': {
                    'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                               {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        last_lts_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': None, 'suites': {
                    'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        expected = {
            'selector': {
                'js_test': {'jstests/fake_file1.js': ['suite1_backport_required_multiversion']}
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_old_all_is_none(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}],
                'suites': {
                    'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                               {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        last_lts_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': None, 'suites': {
                    'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': ['suite1_backport_required_multiversion'],
                    'jstests/fake_file0.js': ['backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    def test_create_yaml_suite1_and_all(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'},
                        {'ticket': 'fake_ticket4', 'test_file': 'jstests/fake_file4.js'}],
                'suites': {
                    'suite1': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'},
                               {'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
                }
            }
        }

        last_lts_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'}], 'suites':
                    {'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]}
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file1.js': ['suite1_backport_required_multiversion'],
                    'jstests/fake_file4.js': ['backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)

    # Can delete after backporting the changed yml syntax.
    def test_not_backported(self):
        latest_yaml = {
            'last-continuous': None, 'last-lts': {
                'all': [{'ticket': 'fake_ticket0', 'test_file': 'jstests/fake_file0.js'},
                        {'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'}],
                'suites': {
                    'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'},
                               {'ticket': 'fake_ticket3', 'test_file': 'jstests/fake_file3.js'}]
                }
            }
        }

        last_lts_yaml = {
            'all': [{'ticket': 'fake_ticket1', 'test_file': 'jstests/fake_file1.js'}], 'suites': {
                'suite1': [{'ticket': 'fake_ticket2', 'test_file': 'jstests/fake_file2.js'}]
            }
        }

        expected = {
            'selector': {
                'js_test': {
                    'jstests/fake_file0.js': ['backport_required_multiversion'],
                    'jstests/fake_file3.js': ['suite1_backport_required_multiversion']
                }
            }
        }

        self.patch_and_run(latest_yaml, last_lts_yaml)
        self.assert_contents(expected)


if __name__ == '__main__':
    unittest.main()
