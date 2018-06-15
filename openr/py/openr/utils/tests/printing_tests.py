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

from openr.utils import printing


if __name__ == '__main__':
    data1 = [['eb01.frc3', 126, '10.254.104.25', 'fe80::21c:73ff:fed6:1718'],
             ['eb01.sjc1', 32, '10.254.105.34', 'fe80::21c:73ff:dasg:3425']
             ]
    column_labels1 = ['Neighbor', 'Metric', 'NextHop-v4', 'NextHop-v6']
    print(printing.render_horizontal_table(data1, column_labels1,
                                           'A sample for adj table'))

    data2 = [['eb01.atn1', 'tcp://[fe80::21c:73ff:fed6:17cc%po1021]:60002',
              'tcp://[fe80::21c:73ff:fed6:17cc%po1021]:60001'],
             ['eb01.atn1', 'tcp://[fe80::21c:73ff:fed6:17cc%po1021]:60002',
              'tcp://[fe80::21c:73ff:fed6:17cc%po1021]:60001'],
             ]
    column_labels2 = ['cmd via', 'pub via']
    print(printing.render_vertical_table(data2, column_labels2,
                                         'A sample for peers table'))
