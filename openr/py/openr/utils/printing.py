#!/usr/bin/env python3
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import datetime
from builtins import range
from typing import Iterable, Union

import tabulate


def caption_fmt(caption):
    """Format a caption"""

    if caption:
        return "\n== {}  ==\n".format(caption)
    return ""


def get_timestamp() -> str:
    return "Timestamp: {}\n".format(
        datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
    )


def sprint_bytes(bytes: Union[int, float]) -> str:
    """Return formatted bytes. e.g. 12KB, 15.1MB"""

    if bytes < 1024:
        return f"{bytes} B"
    bytes /= 1024
    if bytes < 1024:
        return f"{bytes:.2f} KB"
    bytes /= 1024
    if bytes < 1024:
        return f"{bytes:.2f} MB"
    bytes /= 1024
    return f"{bytes:.2f} GB"


def render_horizontal_table(data, column_labels=(), caption="", tablefmt="simple"):
    """Render tabular data with one item per line

    :param data:  An iterable (e.g. a tuple() or list) containing the rows
                  of the table, where each row is an iterable containing
                  the columns of the table (strings).
    :param column_names: An iterable of column names (strings).
    :param caption: Title of the table (a string).
    :param tablefmt: Table formats. Supports 'plain', 'simple'.
                     'simple' format generates a separating line '----' between
                     column label and contents, while 'plain' format does not.

    :return: The rendered table (a string).
    """

    return "{}{}{}".format(
        caption_fmt(caption),
        "\n" if caption else "",
        tabulate.tabulate(data, column_labels, tablefmt),
    )


def render_vertical_table(
    data: Iterable[Iterable[str]],
    column_labels: Iterable[str] = "",
    caption: str = "",
    element_prefix: str = ">",
    element_suffix: str = "",
    timestamp: bool = False,
):
    """Render tabular data with one column per line

    :param data:  An iterable (e.g. a tuple() or list) containing the rows
                  of the table, where each row is an iterable containing
                  the columns of the table (strings).
    :param column_names: An iterable of column names (strings).
    :param caption: Title of the table (a string).
    :param element_prefix: Starting prefix for each item. (string)
    :param element_suffix: Ending/terminator for each item. (string)
    :param timestamp: Prints time for each item. (bool)

    :return: The rendered table (a string).
    """

    table_str = ""
    if caption:
        table_str += f"{caption_fmt(caption)}\n"

    for item in sorted(data, key=lambda x: x[0]):
        if not item:
            break

        item_str = ""
        if timestamp:
            item_str += get_timestamp()

        # pyre-fixme[16]: `Iterable` has no attribute `__getitem__`.
        item_str += f"{element_prefix} {item[0]} {element_suffix}\n"
        # pyre-fixme[6]: Expected `Sized` for 1st param but got `Iterable[str]`.
        for idx in range(1, len(item)):
            if column_labels:
                item_str += f"{column_labels[idx - 1]} "
            item_str += f"{item[idx]}\n"
        table_str += f"{item_str}\n"

    return f"{table_str}"
