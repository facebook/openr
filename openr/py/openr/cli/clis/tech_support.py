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

import click
import sys

from openr.cli.commands.tech_support import TechSupportCmd


class TechSupportCli(object):

    @click.command(name='tech-support')
    @click.pass_context
    def tech_support(ctx):  # noqa: B902
        ''' Extensive logging of Open/R's state for debugging '''

        '''
        - Counters (FIB) and recent log samples

        - Recent perf events
        '''

        sys.exit(TechSupportCmd(ctx.obj).run())
