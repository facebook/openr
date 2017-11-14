`DeveloperGuide`
----------------

If you are actively using OpenR and interested in contributing back then we,
the OpenR team at Facebook, are happy to help you get started. Here are some
basic steps to get started with.

### Developer/User Community
---

Use [OpenR - Facebook Group](https://www.facebook.com/groups/openr/)
for communication with other members. In group you can ask questions or have
discussions on related topics with rest of the community members. You can expect
relatively quick (few hours) reply from OpenR core team at Facebook.


### Procedure
---

Open up `issue` on github describing feature of bug-fix you want to address in
detail and your approach. Discuss strategy (if big feature) with core team
before sending out the pull request to avoid multiple revisions over the issue
on github.

Code up your idea and send out pull request, one of the core team member will
get your code review, iterate if necessary and once finalized pull request
will be merged.

### Code References
---

Code is very well organized into different modules under `openr/` directory.
All of the python code lives under `openr/py`

### Developers Expectation
---

#### Code Quality
- Code should be properly formatted (following existing formatting).
- Inline code documentation. Code should be self expressive
- Have smaller and concrete changes. For e.g. separate out renaming/refactoring
  changes with functionality changes

#### Testing
- Write unit tests to cover code changes you make. We aim for 100% code coverage
- Make sure no existing test is broken
- Test on at least 1000 (one thousand) node topology using emulation. If you
  don't have enough servers , try with smaller topology (50/100). OpenR team
  will do on your behalf to run on larger topology if need be.
