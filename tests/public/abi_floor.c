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

/* First what each floor revision named must still be declared by the
 * headers above; then every declaration is made again and must agree. One
 * fragment per interface, named after the floor it holds. */
#define MAELYS_ABI_FLOOR_PRESENCE
#include "abi-3.h"
#include "abi-tls-1.h"
#include "abi-tls-files-1.h"
#include "abi-client-1.h"
#undef MAELYS_ABI_FLOOR_PRESENCE
#include "abi-3.h"
#include "abi-tls-1.h"
#include "abi-tls-files-1.h"
#include "abi-client-1.h"

#define FLOOR(macro, revision, version, fragment) \
    _Static_assert((macro) == (revision), fragment " is the floor " #macro " names"); \
    _Static_assert((macro) <= (version), "a floor cannot be above its revision")
FLOOR(MAELYS_EGRESS_ABI_COMPATIBLE_SINCE, 3u, MAELYS_EGRESS_ABI_VERSION, "abi-3.h");
FLOOR(MAELYS_EGRESS_TLS_ABI_COMPATIBLE_SINCE, 1u, MAELYS_EGRESS_TLS_ABI_VERSION, "abi-tls-1.h");
FLOOR(MAELYS_EGRESS_TLS_FILES_ABI_COMPATIBLE_SINCE, 1u, MAELYS_EGRESS_TLS_FILES_ABI_VERSION,
      "abi-tls-files-1.h");
FLOOR(MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE, 1u, MAELYS_EGRESS_CLIENT_ABI_VERSION,
      "abi-client-1.h");

int abi_floor_holds(void);
int abi_floor_holds(void) { return 1; }
