#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


from builtins import range

import tabulate
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl


class ViewFibCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        *args,
        **kwargs,
    ) -> None:
        resp = client.getPerfDb()
        headers = ["Node", "Events", "Duration", "Unix Timestamp"]
        for i in range(len(resp.eventInfo)):
            rows = []
            recent_ts = resp.eventInfo[i].events[0].unixTs
            total_duration = 0
            for perf_event in resp.eventInfo[i].events:
                node_name = perf_event.nodeName
                event_name = perf_event.eventDescr
                duration = perf_event.unixTs - recent_ts
                total_duration += duration
                recent_ts = perf_event.unixTs
                rows.append([node_name, event_name, duration, recent_ts])
            print("Perf Event Item: {}, total duration: {}ms".format(i, total_duration))
            print(tabulate.tabulate(rows, headers=headers))
            print()
