# Third-party notices

## Maelys System 0.5.0

Maelys Egress links and redistributes Maelys System at commit
`7a5b232bcd4dafe103966d7f88d99c7acc19deaf`
(https://github.com/maelys-dev/maelys-system), licensed under the Mozilla
Public License, v. 2.0, the same license as this repository. Its complete,
unmodified source is available at the repository and commit named above.

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at https://mozilla.org/MPL/2.0/.

## Maelys CLI 0.1.0

The `maelys-egress` command-line binary statically links `libmaelys_cli`
from Maelys CLI at tag `v0.1.0`, commit
`9f620aed2cc17dc4f977cab32bf734ac471de3e5`
(https://github.com/maelys-dev/maelys-cli). The Maelys Egress library
`libmaelys_egress` does not link it.

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
