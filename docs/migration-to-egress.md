# Migrating to Maelys Egress

Version 0.9.0 renames the complete product surface. Behaviour, configuration
keys and wire protocols are unchanged, but source and binary compatibility are
intentionally not preserved: consumers must move to the single Egress
namespace.

| Before 0.9 | Egress 0.9 |
|---|---|
| `maelys-netd` | `maelys-egress` |
| `libmaelys_netd.a` | `libmaelys_egress.a` |
| `maelys/netd.h` | `maelys/egress.h` |
| `maelys/netd_profile.h` | `maelys/egress_profile.h` |
| `maelys/netd_tls.h` | `maelys/egress_tls.h` |
| `maelys_netd_*` | `maelys_egress_*` |
| `MAELYS_NETD_*` | `MAELYS_EGRESS_*` |
| `maelys-netd.pc` | `maelys-egress.pc` |
| Python `maelys_netd` | Python `maelys_egress` |
| Node `@maelys/netd` | Node `@maelys/egress` |
| `MAELYS_NETD_BINARY` | `MAELYS_EGRESS_BINARY` |
| Prometheus `maelys_netd_*` | Prometheus `maelys_egress_*` |

For C consumers, change the include, type and function namespaces together:

```c
#include <maelys/egress.h>

maelys_egress_policy_t *policy = NULL;
char *error = NULL;

if (maelys_egress_policy_create(&policy, &error) != MAELYS_EGRESS_OK) {
    maelys_egress_error_free(error);
    return 1;
}
```

Installed service identities and paths also change. Create the new service
account and private runtime directories rather than reusing a directory owned
by the former daemon identity:

```text
/etc/maelys-egress.conf
/run/maelys-egress/
/var/log/maelys-egress/
```

The strict configuration language itself does not change. Existing files may
be copied to the new path after updating any filesystem paths they contain.
Durable audit journals remain cryptographically valid because the canonical
receipt representation and HMAC framing are unchanged. Renamed metrics and
API symbols are outside the journal. Use a deliberate operational migration
rather than moving a live journal while either daemon is running.

There are no compatibility headers, duplicate symbols or legacy binary
symlinks. This keeps applications from accidentally loading both public
namespaces into one process and gives downstream adapters one unambiguous
contract to pin.
