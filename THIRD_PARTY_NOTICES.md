# Third-party notices

## Maelys System 0.9.0

Maelys Egress links and redistributes Maelys System at tag `v0.9.1`, commit
`6663c83a5f6035055b72d3ad0067ac2ad306fc2e`
(https://github.com/maelys-dev/maelys-system), licensed under the Mozilla
Public License, v. 2.0, the same license as this repository. Its complete,
unmodified source is available at the repository and commit named above.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at https://mozilla.org/MPL/2.0/.

## Maelys CLI 0.5.19

The `maelys-egress` command-line binary statically links `libmaelys_cli`
from Maelys CLI at tag `v0.5.19`, commit
`6868bd13cfdccc0cdf59f40174f2bdc76f57bd04`
(https://github.com/maelys-dev/maelys-cli). The Maelys Egress library
`libmaelys_egress` does not link it, and neither archive links
`libmaelys_cli_extension` or maelys-json.

Maelys CLI is licensed under the Mozilla Public License, v. 2.0, the same
license as this repository. Its complete, unmodified source is available at
the repository and tag named above.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at https://mozilla.org/MPL/2.0/.

## Optional TLS modules

The source tree contains optional adapters for Mbed TLS (Apache License 2.0 or
GPLv2, depending on the upstream release) and wolfSSL (GPLv2 or a commercial
license). These libraries are not vendored and are not linked into the default
Maelys Egress artifacts. Users and distributors who build an optional module or
provider-specific binary are responsible for satisfying the selected upstream
library's license. Maelys Egress's adapter source remains under this repository's
MPL-2.0 license.
