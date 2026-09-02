# Agent instructions

The public CLI is an executable protocol shared by humans, scripts, SDKs and
LLMs. Before adding or changing a command, option, configuration key, exit
status or lifecycle event, read and follow
[`skills/egress-cli-contract/SKILL.md`](skills/egress-cli-contract/SKILL.md).

Do not hand-edit files under `docs/generated/`. Update the central catalog or
protocol schema, run `make cli-reference`, and commit the generated result.

No backward-compatibility alias should be introduced without an explicit
product decision. The 0.x series prefers one clear contract over parallel old
and new spellings.
