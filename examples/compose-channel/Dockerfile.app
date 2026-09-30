# The existing sidecar's named build stage supplies the installed client SDK.
# It is a build input only: no proxy binary, core archive or compiler is copied.
ARG MAELYS_EGRESS_BUILD_IMAGE=maelys-egress-build:local
FROM ${MAELYS_EGRESS_BUILD_IMAGE} AS build
COPY app.c /example/app.c
RUN cc -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
      -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow \
      -I/stage/usr/include /example/app.c /stage/usr/lib/libmaelys_egress_client.a \
      -o /example/native-app && \
    ! nm -u /example/native-app | grep -E ' (maelys_sys_|pthread_|maelys_cli_)' && \
    ! nm /example/native-app | grep -E ' maelys_(cli_|egress_(server|config|policy|connector|channel_broker)_)'

FROM ubuntu:24.04
COPY --from=build /example/native-app /usr/local/bin/native-app
USER 10002:10002
ENTRYPOINT ["/usr/local/bin/native-app"]
