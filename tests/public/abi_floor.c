/*
 * The floor of each public interface, held by the compiler. The current
 * headers come first; the frozen fragments then declare again everything
 * the floor revision declared. A prototype, an enumerator's value, a
 * numeric macro or the layout of an open structure that no longer matches
 * stops this file from compiling: that is a break, and a break raises
 * MAELYS_EGRESS_ABI_COMPATIBLE_SINCE (or the client's) in the same change
 * that regenerates the fragment. Additions never touch this file.
 */
#include <maelys/egress.h>
#include <maelys/egress_tls.h>
#include <maelys/egress_profile.h>
#include <maelys/egress_tls_modules.h>
#include <maelys/egress_channel.h>
#include <maelys/egress_client.h>

/* First what the floor revision named must still be declared by the
 * headers above; then every declaration is made again and must agree. */
#define MAELYS_ABI_FLOOR_PRESENCE
#include "abi-core-3.h"
#include "abi-client-1.h"
#undef MAELYS_ABI_FLOOR_PRESENCE
#include "abi-core-3.h"
#include "abi-client-1.h"

_Static_assert(MAELYS_EGRESS_ABI_COMPATIBLE_SINCE == 3u,
               "abi-core-3.h is the floor MAELYS_EGRESS_ABI_COMPATIBLE_SINCE names");
_Static_assert(MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE == 1u,
               "abi-client-1.h is the floor MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE names");
_Static_assert(MAELYS_EGRESS_ABI_COMPATIBLE_SINCE <= MAELYS_EGRESS_ABI_VERSION,
               "the floor cannot be above the revision");
_Static_assert(MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE <= MAELYS_EGRESS_CLIENT_ABI_VERSION,
               "the floor cannot be above the revision");

int abi_floor_holds(void);
int abi_floor_holds(void) { return 1; }
