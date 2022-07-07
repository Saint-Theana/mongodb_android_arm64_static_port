"""Setup multiversion mongodb.

Downloads and installs particular mongodb versions (each binary is renamed
to include its version) into an install directory and symlinks the binaries
with versions to another directory. This script supports community and
enterprise builds.
"""
import logging
import os
import re
import sys
import time

import structlog
import yaml

from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.setup_multiversion import config, download, evergreen_conn, github_conn

SUBCOMMAND = "setup-multiversion"

LOGGER = structlog.getLogger(__name__)


def setup_logging(debug=False):
    """Enable logging."""
    log_level = logging.DEBUG if debug else logging.INFO
    logging.basicConfig(
        format="[%(asctime)s - %(name)s - %(levelname)s] %(message)s",
        level=log_level,
        stream=sys.stdout,
    )
    logging.getLogger("urllib3").setLevel(logging.WARNING)
    logging.getLogger("s3transfer").setLevel(logging.WARNING)
    logging.getLogger("botocore").setLevel(logging.WARNING)
    logging.getLogger("boto3").setLevel(logging.WARNING)
    logging.getLogger("evergreen").setLevel(logging.WARNING)
    logging.getLogger("github").setLevel(logging.WARNING)
    structlog.configure(logger_factory=structlog.stdlib.LoggerFactory())


class SetupMultiversion(Subcommand):
    """Main class for the setup multiversion subcommand."""

    # pylint: disable=too-many-instance-attributes
    def __init__(self, options):
        """Initialize."""
        setup_logging(options.debug)
        cwd = os.getcwd()
        self.install_dir = os.path.join(cwd, options.install_dir)
        self.link_dir = os.path.join(cwd, options.link_dir)

        self.edition = options.edition.lower() if options.edition else None
        self.platform = options.platform.lower() if options.platform else None
        self.architecture = options.architecture.lower() if options.architecture else None
        self.use_latest = options.use_latest
        self.versions = options.versions

        self.download_binaries = options.download_binaries
        self.download_symbols = options.download_symbols
        self.download_artifacts = options.download_artifacts

        self.evg_api = evergreen_conn.get_evergreen_api(options.evergreen_config)
        # In evergreen github oauth token is stored as `token ******`, so we remove the leading part
        self.github_oauth_token = options.github_oauth_token.replace(
            "token ", "") if options.github_oauth_token else None
        with open(config.SETUP_MULTIVERSION_CONFIG) as file_handle:
            raw_yaml = yaml.safe_load(file_handle)
        self.config = config.SetupMultiversionConfig(raw_yaml)

    @staticmethod
    def _get_bin_suffix(version, evg_project_id):
        """Get the multiversion bin suffix from the evergreen project ID."""
        if re.match(r"(\d+\.\d+)", version):
            # If the cmdline version is already a semvar, just use that.
            return version
        elif evg_project_id == "mongodb-mongo-master":
            # If the version is not a semvar and the project is the master waterfall,
            # we can't add a suffix.
            return ""
        else:
            # Use the Evergreen project ID as fallback.
            return re.search(r"(\d+\.\d+$)", evg_project_id).group(0)

    def execute(self):
        """Execute setup multiversion mongodb."""

        for version in self.versions:
            LOGGER.info("Setting up version.", version=version)
            LOGGER.info("Fetching download URL from Evergreen.")

            try:
                urls = {}
                if self.use_latest:
                    urls = self.get_latest_urls(version)
                if not urls:
                    LOGGER.warning("Latest URL is not available or not requested, "
                                   "we fallback to getting the URL for a specific "
                                   "version.")
                    urls = self.get_urls(version)

                artifacts_url = urls.get("Artifacts", "") if self.download_artifacts else None
                binaries_url = urls.get("Binaries", "") if self.download_binaries else None
                download_symbols_url = None

                if self.download_symbols:
                    download_symbols_url = urls.get(" mongo-debugsymbols.tgz", "")
                    if not download_symbols_url:
                        download_symbols_url = urls.get(" mongo-debugsymbols.zip", "")

                bin_suffix = self._get_bin_suffix(version, urls["project_id"])
                # Give each version a unique install dir
                install_dir = os.path.join(self.install_dir, version)

                self.setup_mongodb(artifacts_url, binaries_url, download_symbols_url, install_dir,
                                   bin_suffix, self.link_dir)

            except (github_conn.GithubConnError, evergreen_conn.EvergreenConnError,
                    download.DownloadError) as ex:
                LOGGER.error(ex)
                exit(1)

            else:
                LOGGER.info("Setup version completed.", version=version)
                LOGGER.info("-" * 50)

    def get_latest_urls(self, version):
        """Return latest urls."""
        urls = {}

        evg_project = f"mongodb-mongo-v{version}"
        if version == "master":
            evg_project = "mongodb-mongo-master"

        if evg_project not in self.config.evergreen_projects:
            return urls

        LOGGER.debug("Found evergreen project.", evergreen_project=evg_project)
        # Assuming that project names contain <major>.<minor> version
        major_minor_version = version

        buildvariant_name = self.get_buildvariant_name(major_minor_version)
        LOGGER.debug("Found buildvariant.", buildvariant_name=buildvariant_name)

        evg_versions = evergreen_conn.get_evergreen_versions(self.evg_api, evg_project)

        for evg_version in evg_versions:
            if hasattr(evg_version, "build_variants_map"):
                if buildvariant_name not in evg_version.build_variants_map:
                    buildvariant_name = self.fallback_to_generic_buildvariant(major_minor_version)

                curr_urls = evergreen_conn.get_compile_artifact_urls(self.evg_api, evg_version,
                                                                     buildvariant_name)
                if "Binaries" in curr_urls:
                    urls = curr_urls
                    break

        return urls

    def get_urls(self, version):
        """Return urls."""
        git_tag, commit_hash = github_conn.get_git_tag_and_commit(self.github_oauth_token, version)
        LOGGER.info("Found git attributes.", git_tag=git_tag, commit_hash=commit_hash)

        evg_project, evg_version = evergreen_conn.get_evergreen_project_and_version(
            self.config, self.evg_api, commit_hash)
        LOGGER.debug("Found evergreen project.", evergreen_project=evg_project)
        try:
            major_minor_version = re.findall(r"\d+\.\d+", evg_project)[-1]
        except IndexError:
            major_minor_version = "master"

        buildvariant_name = self.get_buildvariant_name(major_minor_version)
        LOGGER.debug("Found buildvariant.", buildvariant_name=buildvariant_name)
        if buildvariant_name not in evg_version.build_variants_map:
            buildvariant_name = self.fallback_to_generic_buildvariant(major_minor_version)

        urls = evergreen_conn.get_compile_artifact_urls(self.evg_api, evg_version,
                                                        buildvariant_name)

        return urls

    @staticmethod
    def setup_mongodb(artifacts_url, binaries_url, symbols_url, install_dir, bin_suffix=None,
                      link_dir=None):
        # pylint: disable=too-many-arguments
        """Download, extract and symlink."""

        for url in [artifacts_url, binaries_url, symbols_url]:
            if url is not None:

                def try_download(download_url):
                    tarball = download.download_from_s3(download_url)
                    download.extract_archive(tarball, install_dir)
                    os.remove(tarball)

                try:
                    try_download(url)
                except Exception as err:  # pylint: disable=broad-except
                    LOGGER.warning("Setting up tarball failed with error, retrying once...",
                                   error=err)
                    time.sleep(1)
                    try_download(url)

        if binaries_url is not None:
            if not link_dir:
                raise ValueError("link_dir must be specified if downloading binaries")
            download.symlink_version(bin_suffix, install_dir, link_dir)

    def get_buildvariant_name(self, major_minor_version):
        """Return buildvariant name.

        Wrapper around evergreen_conn.get_buildvariant_name().
        """

        return evergreen_conn.get_buildvariant_name(
            config=self.config, edition=self.edition, platform=self.platform,
            architecture=self.architecture, major_minor_version=major_minor_version)

    def fallback_to_generic_buildvariant(self, major_minor_version):
        """Return generic buildvariant name.

        Wrapper around evergreen_conn.get_generic_buildvariant_name().
        """

        return evergreen_conn.get_generic_buildvariant_name(config=self.config,
                                                            major_minor_version=major_minor_version)


class SetupMultiversionPlugin(PluginInterface):
    """Integration point for setup-multiversion-mongodb."""

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""

        if subcommand == SUBCOMMAND:
            return SetupMultiversion(parsed_args)

        return None

    def add_subcommand(self, subparsers):
        """Create and add the parser for the subcommand."""
        parser = subparsers.add_parser(SUBCOMMAND, help=__doc__)

        parser.add_argument("-i", "--installDir", dest="install_dir", required=True,
                            help="Directory to install the download archive. [REQUIRED]")
        parser.add_argument(
            "-l", "--linkDir", dest="link_dir", required=True,
            help="Directory to contain links to all binaries for each version "
            "in the install directory. [REQUIRED]")
        editions = ("base", "enterprise", "targeted")
        parser.add_argument("-e", "--edition", dest="edition", choices=editions,
                            default="enterprise",
                            help="Edition of the build to download, [default: %(default)s].")
        parser.add_argument(
            "-p", "--platform", dest="platform", required=True,
            help="Platform to download [REQUIRED]. "
            f"Available platforms can be found in {config.SETUP_MULTIVERSION_CONFIG}.")
        parser.add_argument(
            "-a", "--architecture", dest="architecture", default="x86_64",
            help="Architecture to download, [default: %(default)s]. Examples include: "
            "'arm64', 'ppc64le', 's390x' and 'x86_64'.")
        parser.add_argument(
            "-u", "--useLatest", dest="use_latest", action="store_true",
            help="If specified, the latest version from Evergreen will be downloaded, if it exists, "
            "for the version specified. For example, if specifying version 4.4 for download, the latest "
            "version from `mongodb-mongo-v4.4` Evergreen project will be downloaded. Otherwise the latest "
            "by git tag version will be downloaded.")
        parser.add_argument(
            "versions", nargs="*", help=
            "Examples: 4.0, 4.0.1, 4.0.0-rc0. If 'rc' is included in the version name, we'll use the exact rc, "
            "otherwise we'll pull the highest non-rc version compatible with the version specified."
        )

        parser.add_argument("-db", "--downloadBinaries", dest="download_binaries",
                            action="store_true", default=True,
                            help="whether to download binaries, default to True.")
        parser.add_argument("-ds", "--downloadSymbols", dest="download_symbols",
                            action="store_true", default=False,
                            help="whether to download debug symbols.")
        parser.add_argument("-da", "--downloadArtifacts", dest="download_artifacts",
                            action="store_true", default=False,
                            help="whether to download artifacts.")
        parser.add_argument(
            "-ec", "--evergreenConfig", dest="evergreen_config",
            help="Location of evergreen configuration file. If not specified it will look "
            f"for it in the following locations: {evergreen_conn.EVERGREEN_CONFIG_LOCATIONS}")
        parser.add_argument(
            "-gt", "--githubOauthToken", dest="github_oauth_token",
            help="Set the token to increase your rate limit. In most cases it works without auth. "
            "Otherwise you can pass OAuth token to increase the github API rate limit. See "
            "https://developer.github.com/v3/#rate-limiting")
        parser.add_argument("-d", "--debug", dest="debug", action="store_true", default=False,
                            help="Set DEBUG logging level.")
