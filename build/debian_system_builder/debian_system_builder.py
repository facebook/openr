#!/usr/bin/env python
# Copyright (c) Facebook, Inc. and its affiliates.
from __future__ import absolute_import, division, print_function, unicode_literals

import distutils.spawn
import os

import fbcode_builder_path
from fbcode_builder import FBCodeBuilder
from shell_builder import ShellFBCodeBuilder
from shell_quoting import ShellQuoted, path_join, raw_shell, shell_comment, shell_join
from utils import recursively_flatten_list


"""
debian_system_builder.py allows running the fbcode_builder logic on the host
rather than in a container and installs libraries and programs to the system.

It emits a bash script with set -exo pipefail configured such that
any failing step will cause the script to exit with failure.

== How to run it? ==

cd build
python debian_system_builder/debian_system_builder.py > ./build_openr.sh
sudo chmod +x build_openr.sh
sudo ./build_openr.sh
"""


class DebianSystemFBCodeBuilder(ShellFBCodeBuilder):

    # Overwrite configure to remove prefix for system build
    def configure(self, name=None):
        autoconf_options = {}
        if name is not None:
            autoconf_options.update(
                self.option("{0}:autoconf_options".format(name), {})
            )
        return [
            self.run(
                ShellQuoted(
                    'LDFLAGS="$LDFLAGS" '
                    'CFLAGS="$CFLAGS" '
                    'CPPFLAGS="$CPPFLAGS" '
                    "./configure {args}"
                ).format(
                    args=shell_join(
                        " ",
                        (
                            ShellQuoted("{k}={v}").format(k=k, v=v)
                            for k, v in autoconf_options.items()
                        ),
                    )
                )
            )
        ]

    # Overwrite cmake_configure to remove prefix for system build
    def cmake_configure(self, name, cmake_path=".."):
        cmake_defines = {"BUILD_SHARED_LIBS": "ON"}
        cmake_defines.update(self.option("{0}:cmake_defines".format(name), {}))
        return [
            self.run(
                ShellQuoted(
                    'CXXFLAGS="$CXXFLAGS -fPIC" '
                    'CFLAGS="$CFLAGS -fPIC" '
                    "cmake {args} {cmake_path}"
                ).format(
                    args=shell_join(
                        " ",
                        (
                            ShellQuoted("-D{k}={v}").format(k=k, v=v)
                            for k, v in cmake_defines.items()
                        ),
                    ),
                    cmake_path=cmake_path,
                )
            )
        ]

    def github_project_workdir(self, project, path):
        # Only check out a non-default branch if requested. This especially
        # makes sense when building from a local repo.
        git_hash = self.option(
            "{0}:git_hash".format(project),
            # Any repo that has a hash in deps/github_hashes defaults to
            # that, with the goal of making builds maximally consistent.
            self._github_hashes.get(project, ""),
        )
        maybe_change_branch = (
            [self.run(ShellQuoted("git checkout {hash}").format(hash=git_hash))]
            if git_hash
            else []
        )

        base_dir = self.option("projects_dir")

        local_repo_dir = self.option("{0}:local_repo_dir".format(project), "")
        return self.step(
            "Check out {0}, workdir {1}".format(project, path),
            [
                self.workdir(base_dir),
                self.run(
                    ShellQuoted(
                        "if [[ ! -d {d} ]]; then \n"
                        "\tgit clone https://github.com/{p}\n"
                        "fi"
                    ).format(
                        p=project, d=path_join(base_dir, os.path.basename(project))
                    )
                )
                if not local_repo_dir
                else self.copy_local_repo(local_repo_dir, os.path.basename(project)),
                self.workdir(path_join(base_dir, os.path.basename(project), path)),
            ]
            + maybe_change_branch,
        )
    # Cmake system install
    def make_and_install(self, make_vars=None):
        return [
            self.parallel_make(make_vars),
            self.run(
                ShellQuoted("sudo make install VERBOSE=1 {vars}").format(
                    vars=self._make_vars(make_vars)
                )
            ),
            self.run(ShellQuoted("sudo ldconfig")),
        ]

    def debian_deps(self):
        return super(DebianSystemFBCodeBuilder, self).debian_deps() + [
            "python-setuptools",
            "python3-setuptools",
            "python-pip",
            "ccache",
        ]

    def setup(self):
        steps = [
            ShellQuoted("#!/bin/bash\n"),
            ShellQuoted("set -exo pipefail"),
            self.install_debian_deps(),
        ]
        if self.has_option("ccache_dir"):
            ccache_dir = self.option("ccache_dir")
            steps += [
                ShellQuoted(
                    # Set CCACHE_DIR before the `ccache` invocations below.
                    "export CCACHE_DIR={ccache_dir} "
                    'CC="ccache ${{CC:-gcc}}" CXX="ccache ${{CXX:-g++}}"'
                ).format(ccache_dir=ccache_dir)
            ]
        return steps


def install_dir():
    install_dir = os.environ.get("INSTALL_DIR")
    if not install_dir:
        install_dir = "/usr/local"
    return install_dir


def projects_dir():
    projects_dir = os.environ.get("PROJECTS_DIR")
    if not projects_dir:
        projects_dir = "/usr/local/src"
    return projects_dir


def ccache_dir():
    ccache_dir = os.environ.get("CCACHE_DIR")
    if not ccache_dir:
        ccache_dir = "/ccache"
    return ccache_dir


def gcc_version():
    gcc_version = os.environ.get("GCC_VERSION")
    if not gcc_version:
        gcc_version = "5"
    return gcc_version


if __name__ == "__main__":
    from utils import read_fbcode_builder_config, build_fbcode_builder_config

    install_dir = install_dir()
    projects_dir = projects_dir()

    config_file = os.path.join(
        os.path.dirname(__file__), "debian_system_fbcode_builder_config.py"
    )
    config = read_fbcode_builder_config(config_file)
    builder = DebianSystemFBCodeBuilder(projects_dir=projects_dir)

    if distutils.spawn.find_executable("ccache"):
        ccache_dir = ccache_dir()
        builder.add_option("ccache_dir", ccache_dir)
    # Option is required by fbcode_builder_spec
    builder.add_option("prefix", install_dir)
    builder.add_option("make_parallelism", 4)
    gcc_version = gcc_version()
    builder.add_option("gcc_version", gcc_version)
    make_steps = build_fbcode_builder_config(config)
    steps = make_steps(builder)
    print(builder.render(steps))
