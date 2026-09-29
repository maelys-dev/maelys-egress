FROM alpine:3.22

RUN apk add --no-cache ca-certificates curl && \
    addgroup -S -g 10002 app && \
    adduser -S -D -H -u 10002 -G app app
COPY --chmod=0755 app.sh /usr/local/bin/example-app

USER 10002:10002
ENTRYPOINT ["/usr/local/bin/example-app"]
