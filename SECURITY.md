# Security policy

Please report suspected vulnerabilities privately through GitHub Security
Advisories for `maelys-dev/maelys-egress`. Do not open a public issue before a
fix is available.

The supported security surface is the latest tagged release. Reports should
include the version, platform, proxy protocol, policy/configuration and the
smallest reproducer that demonstrates the boundary violation.

Maelys Egress is a policy enforcement component, not a sandbox. A report that
assumes the workload retains an ambient network path is outside Egress's stated
boundary unless it also demonstrates a bypass in an integrated confinement
profile.
