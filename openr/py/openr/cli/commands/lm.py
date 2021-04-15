#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import sys
from builtins import object
from typing import Any, Dict, List

import click
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing


class LMCmdBase(OpenrCtrlCmd):
    """
    Base class for LinkMonitor cmds. All of LinkMonitor cmd
    is spawn out of this.
    """

    def toggle_node_overload_bit(
        self, client: OpenrCtrl.Client, overload: bool, yes: bool = False
    ) -> None:
        links = client.getInterfaces()
        host = links.thisNodeName
        print()

        if overload and links.isOverloaded:
            print("Node {} is already overloaded.\n".format(host))
            sys.exit(0)

        if not overload and not links.isOverloaded:
            print("Node {} is not overloaded.\n".format(host))
            sys.exit(0)

        action = "set overload bit" if overload else "unset overload bit"
        if not utils.yesno(
            "Are you sure to {} for node {} ?".format(action, host), yes
        ):
            print()
            return

        if overload:
            client.setNodeOverload()
        else:
            client.unsetNodeOverload()

        print("Successfully {}..\n".format(action))

    def toggle_link_overload_bit(
        self,
        client: OpenrCtrl.Client,
        overload: bool,
        interface: str,
        yes: bool = False,
    ) -> None:
        links = client.getInterfaces()
        print()

        if interface not in links.interfaceDetails:
            print("No such interface: {}".format(interface))
            return

        if overload and links.interfaceDetails[interface].isOverloaded:
            print("Interface is already overloaded.\n")
            sys.exit(0)

        if not overload and not links.interfaceDetails[interface].isOverloaded:
            print("Interface is not overloaded.\n")
            sys.exit(0)

        action = "set overload bit" if overload else "unset overload bit"
        question_str = "Are you sure to {} for interface {} ?"
        if not utils.yesno(question_str.format(action, interface), yes):
            print()
            return

        if overload:
            client.setInterfaceOverload(interface)
        else:
            client.unsetInterfaceOverload(interface)

        print("Successfully {} for the interface.\n".format(action))

    def check_link_overriden(
        self, links: openr_types.DumpLinksReply, interface: str, metric: int
    ) -> bool:
        """
        This function call will comapre the metricOverride in the following way:
        1) metricOverride NOT set -> return None;
        2) metricOverride set -> return True/False;
        """
        metricOverride = links.interfaceDetails[interface].metricOverride
        if not metricOverride:
            # pyre-fixme[7]: Expected `bool` but got `None`.
            return None
        return metricOverride == metric

    def toggle_link_metric(
        self,
        client: OpenrCtrl.Client,
        override: bool,
        interface: str,
        metric: int,
        yes: bool,
    ) -> None:
        links = client.getInterfaces()
        print()

        if interface not in links.interfaceDetails:
            print("No such interface: {}".format(interface))
            return

        status = self.check_link_overriden(links, interface, metric)
        if not override and status is None:
            print("Interface hasn't been assigned metric override.\n")
            sys.exit(0)

        if override and status:
            print(
                "Interface: {} has already been set with metric: {}.\n".format(
                    interface, metric
                )
            )
            sys.exit(0)

        action = "set override metric" if override else "unset override metric"
        question_str = "Are you sure to {} for interface {} ?"
        if not utils.yesno(question_str.format(action, interface), yes):
            print()
            return

        if override:
            client.setInterfaceMetric(interface, metric)
        else:
            client.unsetInterfaceMetric(interface)

        print("Successfully {} for the interface.\n".format(action))


class SetNodeOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, yes: bool = False) -> None:
        self.toggle_node_overload_bit(client, True, yes)


class UnsetNodeOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, yes: bool = False) -> None:
        self.toggle_node_overload_bit(client, False, yes)


class SetLinkOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, interface: str, yes: bool) -> None:
        self.toggle_link_overload_bit(client, True, interface, yes)


class UnsetLinkOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, interface: str, yes: bool) -> None:
        self.toggle_link_overload_bit(client, False, interface, yes)


class SetLinkMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self, client: OpenrCtrl.Client, interface: str, metric: str, yes: bool
    ) -> None:
        self.toggle_link_metric(client, True, interface, int(metric), yes)


class UnsetLinkMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, interface: str, yes: bool) -> None:
        self.toggle_link_metric(client, False, interface, 0, yes)


class SetAdjMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self,
        client: OpenrCtrl.Client,
        node: str,
        interface: str,
        metric: str,
        yes: bool,
    ) -> None:
        client.setAdjacencyMetric(interface, node, int(metric))


class UnsetAdjMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(
        self, client: OpenrCtrl.Client, node: str, interface: str, yes: bool
    ) -> None:
        client.unsetAdjacencyMetric(interface, node)


class LMAdjCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, nodes: set, json: bool) -> None:
        adj_db = client.getLinkMonitorAdjacencies()

        # adj_dbs is built with ONLY one single (node, adjDb). Ignpre bidir option
        adjs_map = utils.adj_dbs_to_dict(
            {adj_db.thisNodeName: adj_db}, nodes, False, self.iter_dbs
        )
        if json:
            utils.print_json(adjs_map)
        else:
            utils.print_adjs_table(adjs_map, None, None)


class LMLinksCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    def _run(self, client: OpenrCtrl.Client, only_suppressed: bool, json: bool) -> None:
        links = client.getInterfaces()
        if only_suppressed:
            links.interfaceDetails = {
                k: v for k, v in links.interfaceDetails.items() if v.linkFlapBackOffMs
            }
        if json:
            self.print_links_json(links)
        else:
            if utils.is_color_output_supported():
                overload_color = "red" if links.isOverloaded else "green"
                overload_status = click.style(
                    "{}".format("YES" if links.isOverloaded else "NO"),
                    fg=overload_color,
                )
                caption = "Node Overload: {}".format(overload_status)
                self.print_links_table(links.interfaceDetails, caption)
            else:
                caption = "Node Overload: {}".format(
                    "YES" if links.isOverloaded else "NO"
                )
                self.print_links_table(links.interfaceDetails, caption)

    def interface_info_to_dict(self, interface_info):
        def _update(interface_info_dict, interface_info):
            interface_info_dict.update(
                {
                    "networks": [
                        ipnetwork.sprint_prefix(prefix)
                        for prefix in interface_info.networks
                    ]
                }
            )

        return utils.thrift_to_dict(interface_info, _update)

    def interface_details_to_dict(self, interface_details):
        def _update(interface_details_dict, interface_details):
            interface_details_dict.update(
                {"info": self.interface_info_to_dict(interface_details.info)}
            )

        return utils.thrift_to_dict(interface_details, _update)

    def links_to_dict(self, links):
        def _update(links_dict, links):
            links_dict.update(
                {
                    "interfaceDetails": {
                        k: self.interface_details_to_dict(v)
                        for k, v in links.interfaceDetails.items()
                    }
                }
            )
            del links_dict["thisNodeName"]

        return utils.thrift_to_dict(links, _update)

    def print_links_json(self, links):

        links_dict = {links.thisNodeName: self.links_to_dict(links)}
        print(utils.json_dumps(links_dict))

    @classmethod
    def build_table_rows(cls, interfaces: Dict[str, object]) -> List[List[str]]:
        rows = []
        for (k, v) in sorted(interfaces.items()):
            raw_row = cls.build_table_row(k, v)
            addrs = raw_row[3]
            raw_row[3] = ""
            rows.append(raw_row)
            for addrStr in addrs:
                rows.append(["", "", "", addrStr])
        return rows

    @staticmethod
    def build_table_row(k: str, v: object) -> List[Any]:
        # pyre-fixme[16]: `object` has no attribute `metricOverride`.
        metric_override = v.metricOverride if v.metricOverride else ""
        # pyre-fixme[16]: `object` has no attribute `info`.
        if v.info.isUp:
            backoff_sec = int(
                # pyre-fixme[16]: `object` has no attribute `linkFlapBackOffMs`.
                (v.linkFlapBackOffMs if v.linkFlapBackOffMs else 0)
                / 1000
            )
            if backoff_sec == 0:
                state = "Up"
            elif not utils.is_color_output_supported():
                state = backoff_sec
            else:
                state = click.style("Hold ({} s)".format(backoff_sec), fg="yellow")
        else:
            state = (
                click.style("Down", fg="red")
                if utils.is_color_output_supported()
                else "Down"
            )
        # pyre-fixme[16]: `object` has no attribute `isOverloaded`.
        if v.isOverloaded:
            metric_override = (
                click.style("Overloaded", fg="red")
                if utils.is_color_output_supported()
                else "Overloaded"
            )
        addrs = []
        for prefix in v.info.networks:
            addrStr = ipnetwork.sprint_addr(prefix.prefixAddress.addr)
            addrs.append(addrStr)
        row = [k, state, metric_override, addrs]
        return row

    @classmethod
    def print_links_table(cls, interfaces, caption=None):
        """
        @param interfaces: dict<interface-name, InterfaceDetail>
        @param caption: Caption to show on table name
        """

        columns = ["Interface", "Status", "Metric Override", "Addresses"]
        rows = cls.build_table_rows(interfaces)

        print(printing.render_horizontal_table(rows, columns, caption))
        print()
