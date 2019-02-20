#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import, division, print_function, unicode_literals

from builtins import object

import click
from openr.cli.commands import prefix_mgr
from openr.cli.utils.options import breeze_option


class PrefixMgrCli(object):
    def __init__(self):
        self.prefixmgr.add_command(WithdrawCli().withdraw)
        self.prefixmgr.add_command(AdvertiseCli().advertise)
        self.prefixmgr.add_command(ViewCli().view)
        self.prefixmgr.add_command(SyncCli().sync)

    @click.group()
    @breeze_option("--prefix_mgr_cmd_port", type=int, help="Prefix Manager port")
    @click.pass_context
    def prefixmgr(ctx, prefix_mgr_cmd_port):  # noqa: B902
        """ CLI tool to peek into Prefix Manager module. """
        pass


class WithdrawCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.pass_obj
    def withdraw(cli_opts, prefixes):  # noqa: B902
        """ Withdraw the prefixes being advertised from this node """

        prefix_mgr.WithdrawCmd(cli_opts).run(prefixes)


class AdvertiseCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.option(
        "--prefix-type",
        "-t",
        default="BREEZE",
        help="Type or client-ID associated with prefix.",
    )
    @click.option(
        "--forwarding-type",
        default="IP",
        help="Use label forwarding instead of IP forwarding in data path",
    )
    @click.pass_obj
    def advertise(cli_opts, prefixes, prefix_type, forwarding_type):  # noqa: B902
        """ Advertise the prefixes from this node with specific type """

        prefix_mgr.AdvertiseCmd(cli_opts).run(prefixes, prefix_type, forwarding_type)


class SyncCli(object):
    @click.command()
    @click.argument("prefixes", nargs=-1)
    @click.option(
        "--prefix-type",
        "-t",
        default="BREEZE",
        help="Type or client-ID associated with prefix.",
    )
    @click.option(
        "--forwarding-type",
        default="IP",
        help="Use label forwarding instead of IP forwarding in data path",
    )
    @click.pass_obj
    def sync(cli_opts, prefixes, prefix_type, forwarding_type):  # noqa: B902
        """ Sync the prefixes from this node with specific type """

        prefix_mgr.SyncCmd(cli_opts).run(prefixes, prefix_type, forwarding_type)


class ViewCli(object):
    @click.command()
    @click.pass_obj
    def view(cli_opts):  # noqa: B902
        """ View the prefix of this node """

        prefix_mgr.ViewCmd(cli_opts).run()
