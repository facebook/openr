#!/usr/bin/env python3

#
# Copyright (c) 2014-present, Facebook, Inc.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
#

from __future__ import absolute_import
from __future__ import print_function
from __future__ import unicode_literals
from __future__ import division
from builtins import range

import tabulate


def caption_fmt(caption):
    ''' Format a caption '''

    if caption:
        return '\n== {}  ==\n'.format(caption)
    return ''


def render_horizontal_table(data, column_labels=(), caption='', tablefmt="simple"):
    ''' Render tabular data with one item per line

        :param data:  An iterable (e.g. a tuple() or list) containing the rows
                      of the table, where each row is an iterable containing
                      the columns of the table (strings).
        :param column_names: An iterable of column names (strings).
        :param caption: Title of the table (a string).
        :param tablefmt: Table formats. Supports 'plain', 'simple'.
                         'simple' format generates a separating line '----' between
                         column label and contents, while 'plain' format does not.

        :return: The rendered table (a string).
    '''

    return '{}{}{}'.format(caption_fmt(caption),
                           '\n' if caption else '',
                           tabulate.tabulate(data, column_labels, tablefmt))


def render_vertical_table(data, column_labels='', caption=''):
    ''' Render tabular data with one column per line

        :param data:  An iterable (e.g. a tuple() or list) containing the rows
                      of the table, where each row is an iterable containing
                      the columns of the table (strings).
        :param column_names: An iterable of column names (strings).
        :param caption: Title of the table (a string).

        :return: The rendered table (a string).
    '''

    table_str = ''

    for item in sorted(data, key=lambda x: x[0]):
        if not item:
            break

        item_str = '> {}\n'.format(item[0])
        for idx in range(1, len(item)):
            if column_labels:
                item_str += '{} '.format(column_labels[idx - 1])
            item_str += '{}\n'.format(item[idx])
        table_str += '{}\n'.format(item_str)

    return '{}\n{}'.format(caption_fmt(caption), table_str)
