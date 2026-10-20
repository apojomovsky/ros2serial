# Contributing to ros2serial

Thanks for your interest. The project is in its bootstrap phase, so the most useful contributions right now are feedback on the design ([`docs/00-design.md`](docs/00-design.md)) and on the issues.

[`AGENTS.md`](AGENTS.md) is the normative rulebook for everyone, humans and agents: how work is picked up, how to build and test, the takeoff ritual before a pull request, commit and writing rules. It is written for autonomous agents, so it reads that way. Anything here that contradicts it loses.

In short:

- Everything runs in the Docker dev image through `make` (`make bootstrap`, then `make test`).
- Tests come first: the first commit of a pull request adds the failing test.
- Conventional Commits, no trailers, no em-dashes.
- Before starting on an issue, say so on it, so two people don't work on the same thing.
