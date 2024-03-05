#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


import sys
from string import ascii_letters
from typing import Any, Dict, List, Optional, Sequence

import click
from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.thrift.KvStore.thrift_types import InitializationEvent
from openr.thrift.OpenrCtrl.thrift_types import AdjacenciesFilter
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from openr.thrift.Types.thrift_types import DumpLinksReply, InterfaceDetails
from openr.utils import ipnetwork, printing


def interface_key(interface: str) -> int:
    """
    Used for sorting the interfaces by slot/sub-slot/port
    Normally strings are sorted in alphabetical order,
    sorted(['et1_1_1', 'et1_2_1', 'et1_10_1', 'et1_11_1'])
    gives ['et1_10_1', 'et1_11_1', 'et1_1_1', 'et1_2_1']
    because alphabetically 'et1_10_1' < 'et1_1_1'.
    To avoid this, this function calculates the key by using
    the numerical values, so we sort by slot then sub-slot then port.
    interface_key('et1_1_1') < interface_key('et1_10_1')
    Formula: key(etA_B_C) = A * 100^2 + B * 100 + C * 1
    """
    try:
        # assume that separator is constant '_'
        items = interface.lstrip(ascii_letters).split("_")
        key = 0
        factor = 1
        for unit in items[::-1]:
            key += int(unit) * factor
            factor *= 100
        return key
    except ValueError:
        return hash(interface)


class LMCmdBase(OpenrCtrlCmd):
    """
    Base class for LinkMonitor cmds. All of LinkMonitor cmd
    is spawn out of this.
    """

    async def toggle_node_overload_bit(
        self, client: OpenrCtrlCppClient.Async, overload: bool, yes: bool = False
    ) -> None:
        """[Hard-Drain] Node level overload"""

        links = await client.getInterfaces()
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
            await client.setNodeOverload()
        else:
            await client.unsetNodeOverload()

        print("Successfully {}..\n".format(action))

    async def toggle_link_overload_bit(
        self,
        client: OpenrCtrlCppClient.Async,
        overload: bool,
        interface: str,
        yes: bool = False,
    ) -> None:
        """[Hard-Drain] Link level overload"""

        links = await client.getInterfaces()
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
            await client.setInterfaceOverload(interface)
        else:
            await client.unsetInterfaceOverload(interface)

        print("Successfully {} for the interface.\n".format(action))

    async def toggle_node_metric_inc(
        self, client: OpenrCtrlCppClient.Async, metric_inc: int, yes: bool = False
    ) -> None:
        """[Soft-Drain] Node level metric increment"""

        links = await client.getInterfaces()
        host = links.thisNodeName

        # ATTN:
        #   - sys.exit(0) will NOT print out existing AdjacencyDatabase
        #   - return will print out existing AdjacencyDatabase
        if metric_inc < 0:
            print(f"Can't set negative node metric increment on: {host}")
            sys.exit(0)

        if metric_inc and links.nodeMetricIncrementVal == metric_inc:
            print(f"Node metric increment already set with: {metric_inc}. No-op.\n")
            return

        if not metric_inc and links.nodeMetricIncrementVal == 0:
            print(f"No node metric increment has been set on: {host}. No-op.\n")
            return

        action = "set node metric inc" if metric_inc else "unset node metric inc"
        question_str = "Are you sure to {} for node {} ?"
        if not utils.yesno(question_str.format(action, host), yes):
            sys.exit(0)

        if metric_inc:
            await client.setNodeInterfaceMetricIncrement(metric_inc)
        else:
            await client.unsetNodeInterfaceMetricIncrement()

        print(f"Successfully {action} for node {host}.\n")

    async def toggle_link_metric_inc(
        self,
        client: OpenrCtrlCppClient.Async,
        interfaces: List[str],
        metric_inc: int,
        yes: bool,
    ) -> None:
        """[Soft-Drain] Link level metric increment"""

        links = await client.getInterfaces()
        host = links.thisNodeName

        # ATTN:
        #   - sys.exit(0) will NOT print out existing AdjacencyDatabase
        #   - return will print out existing AdjacencyDatabase

        # Check input before executing anything
        if metric_inc < 0:
            print(f"Can't set negative link metric increment on: {host}")
            sys.exit(0)

        for interface in interfaces:
            if interface not in links.interfaceDetails:
                print(f"No such interface: {interface} on node: {host}")
                sys.exit(0)

        intefaces_to_process = []

        # Get Confirmation
        for interface in interfaces:
            if (
                metric_inc
                and links.interfaceDetails[interface].linkMetricIncrementVal
                == metric_inc
            ):
                print(
                    f"Link metric increment already set with: {metric_inc} for interface {interface}. No-op.\n"
                )
                continue

            if (
                not metric_inc
                and links.interfaceDetails[interface].linkMetricIncrementVal == 0
            ):
                print(
                    f"No link metric increment has been set on: {interface}. No-op.\n"
                )
                continue
            intefaces_to_process.append(interface)

        action = "set link metric inc" if metric_inc else "unset link metric inc"
        question_str = "Are you sure to {} for link {} on node {} ?"
        if not utils.yesno(
            question_str.format(action, ",".join(intefaces_to_process), host), yes
        ):
            sys.exit(0)

        if metric_inc:
            await client.setInterfaceMetricIncrementMulti(
                intefaces_to_process, metric_inc
            )
        else:
            await client.unsetInterfaceMetricIncrementMulti(intefaces_to_process)

        print(
            f"Success {len(intefaces_to_process)}, Skipped {len(interfaces) - len(intefaces_to_process)}"
        )

    def check_link_overriden(
        self, links: DumpLinksReply, interface: str, metric: int
    ) -> Optional[bool]:
        """
        This function call will comapre the metricOverride in the following way:
        1) metricOverride NOT set -> return None;
        2) metricOverride set -> return True/False;
        """
        metricOverride = links.interfaceDetails[interface].metricOverride
        if not metricOverride:
            return None
        return metricOverride == metric

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
    def build_table_rows(
        cls, interfaces: Dict[str, InterfaceDetails]
    ) -> List[List[str]]:
        rows = []
        for interface in sorted(interfaces, key=interface_key):
            details = interfaces[interface]
            row = cls.build_table_row(interface, details)
            rows.append(row)
        return rows

    @staticmethod
    def build_table_row(k: str, v: InterfaceDetails) -> List[Any]:
        metric_override = ""
        if v.metricOverride:
            # [TO BE DEPRECATED]
            metric_override = v.metricOverride
        if v.linkMetricIncrementVal > 0:
            metric_override = v.linkMetricIncrementVal
        if v.isOverloaded:
            metric_override = (
                click.style("Overloaded", fg="red")
                if utils.is_color_output_supported()
                else "Overloaded"
            )
        if v.info.isUp:
            backoff_sec = int(
                (v.linkFlapBackOffMs if v.linkFlapBackOffMs else 0) / 1000
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
        addrs = []
        for prefix in v.info.networks:
            addrStr = ipnetwork.sprint_addr(prefix.prefixAddress.addr)
            addrs.append(addrStr)
        addresses = " ".join(addrs)
        row = [k, state, metric_override, addresses]
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


class SetNodeOverloadCmd(LMCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        yes: bool = False,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_node_overload_bit(client, True, yes)


class UnsetNodeOverloadCmd(LMCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        yes: bool = False,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_node_overload_bit(client, False, yes)


class SetLinkOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        interface: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_link_overload_bit(client, True, interface, yes)


class UnsetLinkOverloadCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        interface: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_link_overload_bit(client, False, interface, yes)


class IncreaseNodeMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        metric: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_node_metric_inc(client, int(metric), yes)


class ClearNodeMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_node_metric_inc(client, 0, yes)


class IncreaseLinkMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        interfaces: List[str],
        metric: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_link_metric_inc(client, interfaces, int(metric), yes)


class ClearLinkMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        interfaces: List[str],
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await self.toggle_link_metric_inc(client, interfaces, 0, yes)


class OverrideAdjMetricCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        node: str,
        interface: str,
        metric: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await client.setAdjacencyMetric(interface, node, int(metric))


class ClearAdjMetricOverrideCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        node: str,
        interface: str,
        yes: bool,
        *args,
        **kwargs,
    ) -> None:
        await client.unsetAdjacencyMetric(interface, node)


class LMAdjCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        nodes: set,
        json: bool,
        areas: Sequence[str] = (),
        *args,
        **kwargs,
    ) -> None:
        area_filters = AdjacenciesFilter(selectAreas=set(areas))
        adj_dbs = await client.getLinkMonitorAdjacenciesFiltered(area_filters)

        for adj_db in adj_dbs:
            if adj_db and adj_db.area and not json:
                click.secho(f"Area: {adj_db.area}", bold=True)
            # adj_db is built with ONLY one single (node, adjDb). Ignpre bidir option
            adjs_map = utils.adj_dbs_to_dict(
                {adj_db.thisNodeName: adj_db}, nodes, False, self.iter_dbs
            )
            if json:
                utils.print_json(adjs_map)
            else:
                utils.print_adjs_table(adjs_map, None, None)


class LMLinksCmd(LMCmdBase):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        only_suppressed: bool,
        json: bool,
        *args,
        **kwargs,
    ) -> None:
        links = await client.getInterfaces()
        if only_suppressed:
            links = links(
                interfaceDetails={
                    k: v
                    for k, v in links.interfaceDetails.items()
                    if v.linkFlapBackOffMs
                }
            )
        if json:
            self.print_links_json(links)
        else:
            overload_status = None
            node_metric_inc_status = None
            if utils.is_color_output_supported():
                # [Hard-Drain]
                overload_color = "red" if links.isOverloaded else "green"
                overload_status = click.style(
                    "{}".format("YES" if links.isOverloaded else "NO"),
                    fg=overload_color,
                )
                # [Soft-Drain]
                node_metric_inc_color = (
                    "red" if links.nodeMetricIncrementVal > 0 else "green"
                )
                node_metric_inc_status = click.style(
                    "{}".format(links.nodeMetricIncrementVal),
                    fg=node_metric_inc_color,
                )
            else:
                overload_status = "YES" if links.isOverloaded else "NO"
                node_metric_inc_status = "{}".format(links.nodeMetricIncrementVal)

            caption = "Node Overload: {}, Node Metric Increment: {}".format(
                overload_status, node_metric_inc_status
            )
            self.print_links_table(links.interfaceDetails, caption)


class LMValidateCmd(LMCmdBase):
    async def _run(
        self,
        client: OpenrCtrlCppClient.Async,
        *args,
        **kwargs,
    ) -> bool:

        is_pass = True

        # Get Data
        links = await client.getInterfaces()
        initialization_events = await client.getInitializationEvents()
        openr_config = await client.getRunningConfigThrift()

        # Run the validation checks
        init_is_pass, init_err_msg_str = self.validate_init_event(
            initialization_events,
            InitializationEvent.LINK_DISCOVERED,
        )

        is_pass = is_pass and init_is_pass

        regex_invalid_interfaces = self._validate_interface_regex(
            links, openr_config.areas
        )

        is_pass = is_pass and (len(regex_invalid_interfaces) == 0)

        # Render Validation Results
        self.print_initialization_event_check(
            init_is_pass,
            init_err_msg_str,
            InitializationEvent.LINK_DISCOVERED,
            "link monitor",
        )
        self._print_interface_validation_info(regex_invalid_interfaces)

        return is_pass

    def _validate_interface_regex(
        self, links: DumpLinksReply, areas: Sequence[Any]
    ) -> Dict[str, Any]:
        """
        Checks if each interface passes the regexes of atleast one area
        Returns a dictionary interface : interfaceDetails of the invalid interfaces
        """

        interfaces = list(links.interfaceDetails.keys())
        invalid_interfaces = set()

        for interface in interfaces:
            # The interface must match the regexes of atleast one area to pass
            passes_regex_check = False

            for area in areas:
                incl_regexes = area.include_interface_regexes
                excl_regexes = area.exclude_interface_regexes
                redistr_regexes = area.redistribute_interface_regexes

                passes_incl_regexes = self.validate_regexes(
                    incl_regexes, [interface], True  # expect at least one regex match
                )
                passes_excl_regexes = self.validate_regexes(
                    excl_regexes, [interface], False  # expect no regex match
                )
                passes_redistr_regexes = self.validate_regexes(
                    redistr_regexes, [interface], True
                )

                if (
                    passes_incl_regexes or passes_redistr_regexes
                ) and passes_excl_regexes:
                    passes_regex_check = True
                    break

            if not passes_regex_check:
                invalid_interfaces.add(interface)

        invalid_interface_dict = {
            interface: links.interfaceDetails[interface]
            for interface in invalid_interfaces
        }

        return invalid_interface_dict

    def _print_interface_validation_info(
        self,
        invalid_interfaces: Dict[str, Any],
    ) -> None:

        click.echo(
            self.validation_result_str(
                "link monitor", "Interface Regex Check", (len(invalid_interfaces) == 0)
            )
        )

        if len(invalid_interfaces) > 0:
            click.echo("Information about interfaces failing the regex check")
            self.print_links_table(invalid_interfaces)
