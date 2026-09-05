# Third-party notices

## Maelys System 0.8.0

Maelys Egress links and redistributes Maelys System at tag `v0.8.0`, commit
`93103a1d0297ea3f334cbc84079c93be6e9b0efd`
(https://github.com/maelys-dev/maelys-system), licensed under the Mozilla
Public License, v. 2.0, the same license as this repository. Its complete,
unmodified source is available at the repository and commit named above.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at https://mozilla.org/MPL/2.0/.

## Maelys CLI 0.5.1

The `maelys-egress` command-line binary statically links `libmaelys_cli`
from Maelys CLI at tag `v0.5.1`, commit
`193786914f19f2f42b12815a267ba2d4ff8a6f3a`
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
