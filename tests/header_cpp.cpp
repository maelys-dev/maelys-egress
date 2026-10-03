#include "maelys/egress.h"
#include "maelys/egress_tls.h"
#include "maelys/egress_tls_modules.h"
#include "maelys/egress_profile.h"
#include "maelys/egress_channel.h"
#include "maelys/egress_client.h"

static_assert(MAELYS_EGRESS_ABI_VERSION == 5u, "unexpected Egress ABI revision");
static_assert(MAELYS_EGRESS_ABI_COMPATIBLE_SINCE == 3u, "unexpected Egress ABI floor");
static_assert(MAELYS_EGRESS_TLS_ABI_VERSION == 1u, "unexpected TLS seam ABI");
static_assert(MAELYS_EGRESS_TLS_ABI_COMPATIBLE_SINCE == 1u, "unexpected TLS seam ABI floor");
static_assert(MAELYS_EGRESS_TLS_FILES_ABI_VERSION == 1u, "unexpected TLS files ABI");
static_assert(MAELYS_EGRESS_TLS_FILES_ABI_COMPATIBLE_SINCE == 1u, "unexpected TLS files ABI floor");
static_assert(MAELYS_EGRESS_CHANNEL_PROTOCOL_VERSION == 1u, "unexpected channel protocol");
static_assert(MAELYS_EGRESS_CLIENT_ABI_VERSION == 2u, "unexpected channel client ABI revision");
static_assert(MAELYS_EGRESS_CLIENT_ABI_COMPATIBLE_SINCE == 1u, "unexpected channel client ABI floor");

int main() { return 0; }
