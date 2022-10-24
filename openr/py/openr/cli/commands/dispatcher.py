# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from typing import Dict

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmdPy
from openr.OpenrCtrl import OpenrCtrl
from openr.utils import printing
from thrift.Thrift import TApplicationException


class FiltersCmd(OpenrCtrlCmdPy):
    def _run(self, client: OpenrCtrl.Client, json: bool, *args, **kwargs) -> None:
        try:
            dispatcher_filters = client.getDispatcherFilters()
        except TApplicationException:
            print("getDispatcherFilters failed. The host might not enable dispatcher")
            return

        utils.print_filters_table(dispatcher_filters, json)


class QueuesCmd(OpenrCtrlCmdPy):
    def _run(self, client: OpenrCtrl.Client, json: bool, *args, **kwargs) -> None:
        resp = client.getCounters()
        self.print_queue_counters(client, resp, json)

    def print_queue_counters(self, client: OpenrCtrl.Client, resp: Dict, json: bool):
        """print the queue counters for Dispatcher"""

        host_id = client.getMyNodeName()
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
