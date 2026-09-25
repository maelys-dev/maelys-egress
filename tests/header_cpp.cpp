#include "maelys/egress.h"
#include "maelys/egress_tls.h"
#include "maelys/egress_tls_modules.h"
#include "maelys/egress_profile.h"

static_assert(MAELYS_EGRESS_ABI_VERSION == 3u, "unexpected Egress ABI");
static_assert(MAELYS_EGRESS_TLS_ABI_VERSION == 1u, "unexpected TLS seam ABI");
static_assert(MAELYS_EGRESS_TLS_FILES_ABI_VERSION == 1u, "unexpected TLS files ABI");

int main() { return 0; }
