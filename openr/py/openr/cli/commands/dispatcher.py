# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe


from typing import Mapping, Sequence

from openr.py.openr.cli.utils import utils
from openr.py.openr.cli.utils.commands import OpenrCtrlCmd
from openr.py.openr.utils import printing
from openr.thrift.OpenrCtrlCpp.thrift_clients import OpenrCtrlCpp as OpenrCtrlCppClient
from thrift.python.exceptions import ApplicationError


class FiltersCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self, client: OpenrCtrlCppClient.Async, json: bool, *args, **kwargs
    ) -> None:
        try:
            dispatcher_filters = await client.getDispatcherFilters()
        except ApplicationError as e:
            print(
                f"getDispatcherFilters failed. The host might not enable dispatcher. Error message {e}"
            )
            return

        self.print_filters_table(dispatcher_filters, json)

    def print_filters_table(self, filters: Sequence[Sequence[str]], json: bool) -> None:
        """print Dispatcher filters

        :param filters as list of lists
        """

        column_labels = ["Reader ID", "Prefix Filters"]

        output = []
        for i, prefix_filters in enumerate(filters):
            reader = "#{}".format(i)

            # replace the empty filter by a tag "(empty prefix: match all)"
            prefix_filters = [
                f if f else "(empty prefix: match all)" for f in prefix_filters
            ]
            if prefix_filters:
                row = [reader, ", ".join(prefix_filters)]
            else:
                row = [reader, "(no filter: unfiltered)"]

            output.append(row)

        if json:
            json_data = {k: v for k, v in output}
            print(utils.json_dumps(json_data))
        else:
            print(printing.render_horizontal_table(output, column_labels))


class QueuesCmd(OpenrCtrlCmd):
    # pyre-fixme[14]: `_run` overrides method defined in `OpenrCtrlCmd` inconsistently.
    async def _run(
        self, client: OpenrCtrlCppClient.Async, json: bool, *args, **kwargs
    ) -> None:
        resp = await client.getCounters()
        await self.print_queue_counters(client, resp, json)

    async def print_queue_counters(
        self, client: OpenrCtrlCppClient.Async, resp: Mapping[str, int], json: bool
    ):
        """print the queue counters for Dispatcher"""

        host_id = await client.getMyNodeName()
        caption = "{}'s Dispatcher counters".format(host_id)

        output = []

        for key, counter in sorted(resp.items()):
            if not key.startswith(
                "messaging.replicate_queue.kvStorePublicationsQueue"
            ) and not key.startswith("messaging.rw_queue.kvStorePublicationsQueue"):
                continue

            output.append([key, ":", counter])
        if json:
            json_data = {k: v for k, _, v in output}
            print(utils.json_dumps(json_data))
        else:
            print(
                printing.render_horizontal_table(
                    output, caption=caption, tablefmt="plain"
                )
            )
            print()
