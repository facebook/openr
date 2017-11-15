`DeveloperGuide`
----------------

If you are actively using OpenR and interested in contributing back then we,
the OpenR team at Facebook, are happy to help you get started. Here are some
basic steps to get started.

### Developer/User Community
---

Use [OpenR - Facebook Group](https://www.facebook.com/groups/openr/)
for communication with other members. In the group, you can ask questions or
have discussions on related topics with the rest of the community members. You
can expect relatively quick (few hours) reply from the OpenR core team at
Facebook.


### Procedure
---

Open up an `issue` on GitHub describing in detail the feature or bug-fix you
want to address and your approach. Discuss strategy (for big features) with the
core team before sending out the pull request to avoid multiple revisions over
the issue.

Code up your idea and send out a pull request. One of the core team members will
get your code reviewed, iterate if necessary, and, once finalized, we will merge
your pull request.

### Code References
---

The code is very well organized into different modules under the `openr/`
directory. All of the python code lives under `openr/py`

### Developers Expectation
---

#### Code Quality
- Code should be properly formatted (following existing formatting).
- Inline code documentation. Code should be self-expressive
- Prefer smaller, focussed changes. For example, separate renaming/refactoring
changes from new features or bug fixes.

#### Testing
- Write unit tests to cover code changes you make. We aim for 100% code coverage
- Make sure no existing test is broken
- Once released, use our emulation testbed to test on at least a 1000 node
topology. If you don't have enough servers, try with a smaller topology, 50-100
nodes. If necessary, the OpenR team can test on a larger topology on your
behalf.
