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
import zmq

from thrift.protocol.TCompactProtocol import TCompactProtocolFactory
from thrift.protocol.TJSONProtocol import TJSONProtocolFactory

from openr.cli.commands import decision
from openr.utils.consts import Consts
from openr.cli.utils.utils import parse_nodes


class DecisionContext(object):
    def __init__(self, verbose, zmq_ctx, host, timeout,
                 decision_rep_port, lm_cmd_port, kv_rep_port, fib_agent_port,
                 json, enable_color):
        '''
            :param zmq_ctx: the ZMQ context to create zmq sockets
            :param host string: the openr router host
            :param decision_rep_port int: the decision module port
            :param json bool: whether to use JSON proto or Compact for thrift
            :param enable_color bool: whether to turn on coloring display
        '''

        self.verbose = verbose
        self.host = host
        self.timeout = timeout
        self.zmq_ctx = zmq_ctx
        self.enable_color = enable_color

        self.decision_rep_port = decision_rep_port
        self.lm_cmd_port = lm_cmd_port
        self.kv_rep_port = kv_rep_port
        self.fib_agent_port = fib_agent_port

        self.proto_factory = (TJSONProtocolFactory if json
                              else TCompactProtocolFactory)


class DecisionCli(object):
    def __init__(self):
        self.decision.add_command(PathCli().path)
        self.decision.add_command(DecisionAdjCli().adj)
        self.decision.add_command(DecisionPrefixesCli().prefixes)
        self.decision.add_command(DecisionRoutesCli().routes)
        self.decision.add_command(DecisionValidateCli().validate)

    @click.group()
    @click.option('--decision_rep_port', default=Consts.DECISION_REP_PORT,
                  help='Decision port')
    @click.option('--json/--no-json', default=False,
                  help='Use JSON serializer')
    @click.option('--verbose/--no-verbose', default=False,
                  help='Print verbose information')
    @click.pass_context
    def decision(ctx, decision_rep_port, json, verbose):  # noqa: B902
        ''' CLI tool to peek into Decision module. '''

        ctx.obj = DecisionContext(
            verbose, zmq.Context(),
            ctx.obj.hostname,
            ctx.obj.timeout,
            ctx.obj.ports_config.get('decision_rep_port', None) or decision_rep_port,
            ctx.obj.ports_config.get('lm_cmd_port', None) or
            Consts.LINK_MONITOR_CMD_PORT,
            ctx.obj.ports_config.get('kv_rep_port', None) or Consts.KVSTORE_REP_PORT,
            ctx.obj.ports_config.get('fib_agent_port', None) or Consts.FIB_AGENT_PORT,
            json,
            ctx.obj.enable_color)


class PathCli(object):

    @click.command()
    @click.option('--src', default='', help='source node, '
                  'default will be the current host')
    @click.option('--dst', default='', help='destination node or prefix, '
                  'default will be the current host')
    @click.option('--max-hop', default=256, help='max hop count')
    @click.pass_obj
    def path(cli_opts, src, dst, max_hop):  # noqa: B902
        ''' path from src to dst '''

        decision.PathCmd(cli_opts).run(src, dst, max_hop)


class DecisionRoutesCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Get routes for a list of nodes. Default will get '
                       'host\'s routes. Get routes for all nodes if \'all\' is given.')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def routes(cli_opts, nodes, json):  # noqa: B902
        ''' Request the routing table from Decision module '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        decision.DecisionRoutesCmd(cli_opts).run(nodes, json)


class DecisionPrefixesCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Dump prefixes for a list of nodes. Default will dump host\'s '
                       'prefixes. Dump prefixes for all nodes if \'all\' is given.')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def prefixes(cli_opts, nodes, json):  # noqa: B902
        ''' show the prefixes from Decision module '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        decision.DecisionPrefixesCmd(cli_opts).run(nodes, json)


class DecisionAdjCli(object):

    @click.command()
    @click.option('--nodes', default='',
                  help='Dump adjacencies for a list of nodes. Default will dump '
                       'host\'s adjs. Dump adjs for all nodes if \'all\' is given')
    @click.option('--bidir/--no-bidir', default=True,
                  help='Only bidir adjacencies')
    @click.option('--json/--no-json', default=False,
                  help='Dump in JSON format')
    @click.pass_obj
    def adj(cli_opts, nodes, bidir, json):  # noqa: B902
        ''' dump the link-state adjacencies from Decision module '''

        nodes = parse_nodes(cli_opts.host, nodes, cli_opts.lm_cmd_port)
        decision.DecisionAdjCmd(cli_opts).run(nodes, bidir, json)


class DecisionValidateCli(object):

    @click.command()
    @click.pass_obj
    def validate(cli_opts):  # noqa: B902
        ''' Check all prefix & adj dbs in Decision against that in KvStore '''

        decision.DecisionValidateCmd(cli_opts).run()
