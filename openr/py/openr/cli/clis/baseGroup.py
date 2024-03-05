# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import click


class deduceCommandGroup(click.Group):
    def get_command(self, ctx, cmd_name):
        """
        Allows user to call a command by its prefix instead of full name, given it is not ambiguous.
        """
        rv = click.Group.get_command(self, ctx, cmd_name)
        if rv is not None:
            return rv
        matches = [x for x in self.list_commands(ctx) if x.startswith(cmd_name)]
        if not matches:
            return None
        elif len(matches) == 1:
            print("Running command: {}".format(matches[0]))
            return click.Group.get_command(self, ctx, matches[0])
        ctx.fail(
            f"Ambiguous command: {cmd_name} matches with: {', '.join(sorted(matches))}"
        )

    def resolve_command(self, ctx, args):
        # return the full command name
        _, cmd, args = super().resolve_command(ctx, args)
        return cmd.name, cmd, args
