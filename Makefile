SHELL := /bin/sh

VERSION := $(shell cat VERSION)
SOURCE_VERSION := $(shell cat VERSION)
BUILD_PROFILE ?= release
BUILD := build/$(BUILD_PROFILE)
OBJ := $(BUILD)/obj
LIB := $(BUILD)/lib
BIN := $(BUILD)/bin
PREFIX ?= /usr/local
CC ?= cc
CXX ?= c++
# Maelys System is either built from the pinned checkout MAELYS_SYSTEM_DIR
# (the default, used by every gate) or taken already installed under
# MAELYS_SYSTEM_PREFIX (packaging, for instance the Homebrew formula that
# depends on libmaelys-sys). adapter/MAELYS_SYSTEM_PIN records the tag and
# the commit; an installed System must carry the same ABI and at least the
# pinned version.
MAELYS_SYSTEM_DIR ?= ../maelys-system
MAELYS_SYSTEM_PREFIX ?=
MAELYS_SYSTEM_TAG := $(word 1,$(shell cat adapter/MAELYS_SYSTEM_PIN))
MAELYS_SYSTEM_PIN := $(word 2,$(shell cat adapter/MAELYS_SYSTEM_PIN))
MAELYS_SYSTEM_VERSION := $(patsubst v%,%,$(MAELYS_SYSTEM_TAG))
ifeq ($(MAELYS_SYSTEM_PREFIX),)
MAELYS_SYSTEM_BUILD := $(abspath $(BUILD)/deps/maelys-system)
MAELYS_SYSTEM_LIB := $(MAELYS_SYSTEM_BUILD)/lib/libmaelys_sys.a
MAELYS_SYSTEM_INCLUDE := $(MAELYS_SYSTEM_DIR)/include
else
MAELYS_SYSTEM_LIB := $(MAELYS_SYSTEM_PREFIX)/lib/libmaelys_sys.a
MAELYS_SYSTEM_INCLUDE := $(MAELYS_SYSTEM_PREFIX)/include
endif
MAELYS_CLI_DIR ?= ../maelys-cli
MAELYS_CLI_TAG := $(word 1,$(shell cat adapter/MAELYS_CLI_PIN))
MAELYS_CLI_PIN := $(word 2,$(shell cat adapter/MAELYS_CLI_PIN))
MAELYS_CLI_BUILD := $(abspath $(BUILD)/deps/maelys-cli)
MAELYS_CLI_LIB := $(MAELYS_CLI_BUILD)/lib/libmaelys_cli.a
MAELYS_CLI_EMBED := $(MAELYS_CLI_DIR)/tools/maelys-cli-embed
MAELYS_CLI_REFERENCE := $(MAELYS_CLI_DIR)/tools/generate_cli_reference.py
GENERATED := $(BUILD)/generated
SANITIZE_FLAGS ?=

override CPPFLAGS := -Iinclude -I. -I$(MAELYS_SYSTEM_INCLUDE) $(CPPFLAGS) \
	-DMAELYS_EGRESS_BUILD_VERSION='"$(VERSION)"'
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -Wconversion \
	-Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 \
	-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 -pthread $(SANITIZE_FLAGS)
LDFLAGS += $(SANITIZE_FLAGS)
LDLIBS += $(MAELYS_SYSTEM_LIB) -pthread

SOURCES := src/common.c src/sha256.c src/receipt.c src/audit.c src/attestor.c src/policy.c src/config.c src/profile.c src/tls.c src/connector.c \
	src/clienthello.c \
	src/http.c src/socks.c \
	src/server.c src/server_listener.c src/server_connection.c src/server_relay.c \
	src/server_quota.c src/server_receipt.c src/server_connector.c src/server_admin.c
OBJECTS := $(SOURCES:%.c=$(OBJ)/%.o)
CLI_COMMON_SOURCES := cli/main.c cli/commands.c cli/config_catalog.c cli/config_file.c \
	cli/secrets.c cli/serve.c cli/reload.c cli/output.c
CLI_SOURCES := $(CLI_COMMON_SOURCES) cli/tls_listener.c
CLI_COMMON_OBJECTS := $(CLI_COMMON_SOURCES:%.c=$(OBJ)/%.o)
CLI_OBJECTS := $(CLI_SOURCES:%.c=$(OBJ)/%.o)
# Only the command-line binary links libmaelys_cli; the library never does.
CLI_CPPFLAGS := -I$(MAELYS_CLI_DIR)/include -I$(GENERATED)
CLI_SCHEMAS := $(wildcard cli/schemas/*.json)
CLI_SCHEMA_SYMBOLS := $(foreach schema,$(CLI_SCHEMAS),\
	egress_$(subst -,_,$(basename $(notdir $(schema))))_schema=$(schema))
CLI_SCHEMA_OBJECT := $(OBJ)/generated/egress_schemas.o
DEPENDENCIES := $(OBJECTS:.o=.d) $(CLI_OBJECTS:.o=.d) \
	$(OBJ)/tests/test_egress.d $(OBJ)/tests/test_operations.d
STATIC_LIB := $(LIB)/libmaelys_egress.a
MBEDTLS_LIB := $(LIB)/libmaelys_egress_tls_mbedtls.a
WOLFSSL_LIB := $(LIB)/libmaelys_egress_tls_wolfssl.a
CLI := $(BIN)/maelys-egress
MBEDTLS_CLI := $(BIN)/maelys-egress-mbedtls
WOLFSSL_CLI := $(BIN)/maelys-egress-wolfssl
TEST := $(BIN)/test-egress
OPERATIONS_TEST := $(BIN)/test-operations
MBEDTLS_TEST := $(BIN)/test-tls-mbedtls
WOLFSSL_TEST := $(BIN)/test-tls-wolfssl
TLS_TEST_STAMP := $(BUILD)/tls-fixtures/generated
TLS_TEST_CERT := $(BUILD)/tls-fixtures/tls-cert.pem
TLS_TEST_KEY := $(BUILD)/tls-fixtures/tls-key.pem
PC := $(LIB)/pkgconfig/maelys-egress.pc
MANIFEST := $(BUILD)/share/maelys/commands/egress.json
EXAMPLE_NAMES := basic_proxy native_connector policy_reload metrics_snapshot durable_audit custom_attestor
EXAMPLE_BINS := $(EXAMPLE_NAMES:%=$(BIN)/example-%)

-include $(DEPENDENCIES)

.PHONY: all clean check test examples-check sdk-check audit check-system-contract check-cli-contract \
	system-integration-check mutation-check \
	cli-reference contract-check lifecycle-contract-check schema-check package-homebrew \
	tls-mbedtls-check tls-wolfssl-check tls-providers-check tls-binaries \
	asan-ubsan tsan analyze fuzz fuzz-smoke install install-tls-modules install-check dist

all: $(STATIC_LIB) $(CLI) $(TEST) $(PC) $(MANIFEST)

ifeq ($(MAELYS_SYSTEM_PREFIX),)
check-system-contract:
	@test -f "$(MAELYS_SYSTEM_DIR)/include/maelys/sys.h" || \
		{ echo "MAELYS_SYSTEM_DIR must name maelys-system" >&2; exit 1; }
	@test "$$(git -C "$(MAELYS_SYSTEM_DIR)" rev-parse HEAD)" = "$(MAELYS_SYSTEM_PIN)" || \
		{ echo "maelys-system must be pinned to $(MAELYS_SYSTEM_PIN)" >&2; exit 1; }
	@git -C "$(MAELYS_SYSTEM_DIR)" diff --quiet "$(MAELYS_SYSTEM_PIN)" -- include src || \
		{ echo "pinned maelys-system contract is modified" >&2; exit 1; }
	@test "$$(cat "$(MAELYS_SYSTEM_DIR)/VERSION")" = "$(MAELYS_SYSTEM_VERSION)" || \
		{ echo "pinned maelys-system must be version $(MAELYS_SYSTEM_VERSION)" >&2; exit 1; }
	@grep -Fq '#define MAELYS_SYS_ABI_VERSION 1u' \
		"$(MAELYS_SYSTEM_DIR)/include/maelys/sys/version.h"

$(MAELYS_SYSTEM_LIB): check-system-contract
	$(MAKE) -C $(MAELYS_SYSTEM_DIR) BUILD=$(MAELYS_SYSTEM_BUILD) CC=$(CC) CXX=$(CXX) \
		VERSION=$(MAELYS_SYSTEM_VERSION) CPPFLAGS= \
		CFLAGS='-O2 -g $(SANITIZE_FLAGS)' all
else
check-system-contract:
	@test -f "$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h" -a -f "$(MAELYS_SYSTEM_LIB)" || \
		{ echo "MAELYS_SYSTEM_PREFIX must hold an installed maelys-system" >&2; exit 1; }
	@grep -Fq '#define MAELYS_SYS_ABI_VERSION 1u' \
		"$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h" || \
		{ echo "installed maelys-system has another ABI than 1" >&2; exit 1; }
	@installed=$$(sed -n 's/^#define MAELYS_SYS_VERSION "\(.*\)"$$/\1/p' \
		"$(MAELYS_SYSTEM_INCLUDE)/maelys/sys/version.h"); \
	oldest=$$(printf '%s\n%s\n' "$(MAELYS_SYSTEM_VERSION)" "$$installed" | \
		sort -t. -k1,1n -k2,2n -k3,3n | sed -n '1p'); \
	test "$$oldest" = "$(MAELYS_SYSTEM_VERSION)" || \
		{ echo "installed maelys-system $$installed is older than $(MAELYS_SYSTEM_VERSION)" >&2; exit 1; }

$(OBJECTS) $(CLI_OBJECTS) $(OBJ)/tests/test_egress.o $(OBJ)/tests/test_operations.o: | check-system-contract
endif

check-cli-contract:
	@test -f "$(MAELYS_CLI_DIR)/include/maelys/cli.h" || \
		{ echo "MAELYS_CLI_DIR must name maelys-cli" >&2; exit 1; }
	@test "$$(git -C "$(MAELYS_CLI_DIR)" rev-parse HEAD)" = "$(MAELYS_CLI_PIN)" || \
		{ echo "maelys-cli must be pinned to $(MAELYS_CLI_TAG) ($(MAELYS_CLI_PIN))" >&2; exit 1; }
	@git -C "$(MAELYS_CLI_DIR)" diff --quiet "$(MAELYS_CLI_PIN)" -- include src tools || \
		{ echo "pinned maelys-cli contract is modified" >&2; exit 1; }
	@grep -Fq '#define MAELYS_CLI_ABI 1' "$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"
	@grep -Fq '#define MAELYS_CLI_VERSION "$(MAELYS_CLI_TAG:v%=%)"' \
		"$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"
	@grep -Fq '#define MAELYS_CLI_CONTRACT "agent-cli/v2"' \
		"$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"

$(MAELYS_CLI_LIB): check-cli-contract
	$(MAKE) -C $(MAELYS_CLI_DIR) BUILD=$(MAELYS_CLI_BUILD) CC=$(CC) CPPFLAGS= \
		CFLAGS='-O2 -g $(SANITIZE_FLAGS)' LDFLAGS='$(SANITIZE_FLAGS)' $(MAELYS_CLI_LIB)

$(GENERATED)/egress_schemas.c: $(CLI_SCHEMAS) | $(MAELYS_CLI_LIB)
	@mkdir -p $(@D)
	$(MAELYS_CLI_EMBED) $(CLI_SCHEMA_SYMBOLS) > $@.tmp && mv $@.tmp $@

$(GENERATED)/egress_schemas.h: $(CLI_SCHEMAS) | $(MAELYS_CLI_LIB)
	@mkdir -p $(@D)
	$(MAELYS_CLI_EMBED) --header $(CLI_SCHEMA_SYMBOLS) > $@.tmp && mv $@.tmp $@

$(CLI_SCHEMA_OBJECT): $(GENERATED)/egress_schemas.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJECTS): | $(MAELYS_SYSTEM_LIB)

# The CLI rule precedes the generic one: make 3.81 picks the first matching
# pattern rule, newer versions the shortest stem, and both must choose it.
$(OBJ)/cli/%.o: cli/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	@mkdir -p $(@D)
	ar rcs $@ $^

$(OBJ)/providers/tls_mbedtls.o: providers/tls_mbedtls.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $$(pkg-config --cflags mbedtls) -MMD -MP -c $< -o $@

$(MBEDTLS_LIB): $(OBJ)/providers/tls_mbedtls.o
	@mkdir -p $(@D)
	ar rcs $@ $^

$(OBJ)/providers/tls_wolfssl.o: providers/tls_wolfssl.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $$(pkg-config --cflags wolfssl) -MMD -MP -c $< -o $@

$(WOLFSSL_LIB): $(OBJ)/providers/tls_wolfssl.o
	@mkdir -p $(@D)
	ar rcs $@ $^

$(CLI_OBJECTS): | $(MAELYS_SYSTEM_LIB) $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h

$(CLI): $(CLI_OBJECTS) $(CLI_SCHEMA_OBJECT) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/cli/tls_listener-mbedtls.o: cli/tls_listener.c | $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) \
		-DMAELYS_EGRESS_TLS_FACTORY=maelys_egress_tls_mbedtls_create \
		-MMD -MP -c $< -o $@

$(MBEDTLS_CLI): $(OBJ)/cli/tls_listener-mbedtls.o $(CLI_COMMON_OBJECTS) $(CLI_SCHEMA_OBJECT) $(MBEDTLS_LIB) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) \
		$$(pkg-config --libs mbedtls mbedx509 mbedcrypto) -o $@

$(OBJ)/cli/tls_listener-wolfssl.o: cli/tls_listener.c | $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) \
		-DMAELYS_EGRESS_TLS_FACTORY=maelys_egress_tls_wolfssl_create \
		-MMD -MP -c $< -o $@

$(WOLFSSL_CLI): $(OBJ)/cli/tls_listener-wolfssl.o $(CLI_COMMON_OBJECTS) $(CLI_SCHEMA_OBJECT) $(WOLFSSL_LIB) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $$(pkg-config --libs wolfssl) -o $@

$(OBJ)/tests/test_egress.o: tests/test_egress.c | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST): $(OBJ)/tests/test_egress.o $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/tests/test_operations.o: tests/test_operations.c | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OPERATIONS_TEST): $(OBJ)/tests/test_operations.o $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/example-%: examples/%.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(TLS_TEST_STAMP):
	@mkdir -p $(@D)
	openssl req -x509 -newkey rsa:2048 -nodes \
		-keyout $(TLS_TEST_KEY) -out $(TLS_TEST_CERT) -days 2 \
		-subj '/CN=localhost' \
		-addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' >/dev/null 2>&1
	@touch $@

$(MBEDTLS_TEST): tests/test_tls_provider.c $(MBEDTLS_LIB) $(STATIC_LIB) $(TLS_TEST_STAMP) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DMAELYS_TLS_FACTORY=maelys_egress_tls_mbedtls_create \
		tests/test_tls_provider.c $(MBEDTLS_LIB) $(STATIC_LIB) \
		$(LDLIBS) $$(pkg-config --libs mbedtls mbedx509 mbedcrypto) -o $@

$(WOLFSSL_TEST): tests/test_tls_provider.c $(WOLFSSL_LIB) $(STATIC_LIB) $(TLS_TEST_STAMP) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) \
		-DMAELYS_TLS_FACTORY=maelys_egress_tls_wolfssl_create \
		tests/test_tls_provider.c $(WOLFSSL_LIB) $(STATIC_LIB) \
		$(LDLIBS) $$(pkg-config --libs wolfssl) -o $@

$(PC): pkgconfig/maelys-egress.pc.in VERSION adapter/MAELYS_SYSTEM_PIN
	@mkdir -p $(@D)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' \
		-e 's|@SYSTEM_VERSION@|$(MAELYS_SYSTEM_VERSION)|g' $< >$@

# Manifest read by the `maelys` dispatcher of maelys-cli: it registers the
# installed daemon as `maelys egress`.
$(MANIFEST): packaging/maelys/egress.json.in VERSION
	@mkdir -p $(@D)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@VERSION@|$(VERSION)|g' $< >$@

test: all $(OPERATIONS_TEST)
	$(TEST)
	$(OPERATIONS_TEST)
	tests/test_cli.sh $(CLI)

# The command reference and contract come from the maelys-cli generator; the
# configuration-key reference is Egress specific. Both read `describe`; the
# generator omits versions by default, so a release never rewrites them.
cli-reference: $(CLI)
	python3 $(MAELYS_CLI_REFERENCE) --build $(abspath $(BIN)) \
		--markdown docs/generated/cli-reference.md \
		--json docs/generated/cli-contract.json maelys-egress
	python3 tools/generate_config_reference.py --binary $(abspath $(CLI)) \
		--output docs/generated/config-reference.md

# Rejects stale generated documentation; part of `check`.
contract-check: $(CLI)
	@mkdir -p $(BUILD)/contract
	python3 $(MAELYS_CLI_REFERENCE) --build $(abspath $(BIN)) \
		--markdown $(BUILD)/contract/cli-reference.md \
		--json $(BUILD)/contract/cli-contract.json maelys-egress
	python3 tools/generate_config_reference.py --binary $(abspath $(CLI)) \
		--output $(BUILD)/contract/config-reference.md
	@for name in cli-reference.md cli-contract.json config-reference.md; do \
		cmp -s $(BUILD)/contract/$$name docs/generated/$$name || \
			{ echo "docs/generated/$$name drifted; run make cli-reference" >&2; exit 1; }; \
	done
	@echo "contract-check: ok"

lifecycle-contract-check: $(CLI)
	tests/test_cli.sh $(CLI)

# Every emitted envelope, data object and lifecycle line must conform to the
# committed schemas; part of `check`.
schema-check: $(CLI)
	python3 tools/check_schemas.py --binary $(abspath $(CLI))

examples-check: $(EXAMPLE_BINS)
	$(BIN)/example-policy_reload
	$(BIN)/example-metrics_snapshot
	$(BIN)/example-custom_attestor

sdk-check: $(CLI)
	@grep -Fq 'version = "$(SOURCE_VERSION)"' sdk/python/pyproject.toml
	@grep -Fq '__version__ = "$(SOURCE_VERSION)"' \
		sdk/python/src/maelys_egress/__init__.py
	@node -e 'const p=require("./sdk/node/package.json"); if (p.version !== "$(SOURCE_VERSION)") process.exit(1)'
	PYTHONDONTWRITEBYTECODE=1 MAELYS_EGRESS_BINARY=$(abspath $(CLI)) \
		python3 -m unittest discover -s sdk/python/tests -p 'test_*.py'
	MAELYS_EGRESS_BINARY=$(abspath $(CLI)) node --test sdk/node/test/egress.test.js

tls-mbedtls-check: $(MBEDTLS_TEST)
	MAELYS_TLS_TEST_CERT=$(TLS_TEST_CERT) MAELYS_TLS_TEST_KEY=$(TLS_TEST_KEY) \
		$(MBEDTLS_TEST)

tls-wolfssl-check: $(WOLFSSL_TEST)
	MAELYS_TLS_TEST_CERT=$(TLS_TEST_CERT) MAELYS_TLS_TEST_KEY=$(TLS_TEST_KEY) \
		$(WOLFSSL_TEST)

tls-providers-check: tls-mbedtls-check tls-wolfssl-check

tls-binaries: $(MBEDTLS_CLI) $(WOLFSSL_CLI)

audit:
	scripts/audit-boundaries.sh

# Renders the Homebrew formula for an already-pushed tag (default: VERSION).
package-homebrew:
	scripts/render-homebrew-formula.sh v$(VERSION)

system-integration-check: $(STATIC_LIB) $(MAELYS_SYSTEM_LIB)
	@symbols="$$(nm -u $(STATIC_LIB))"; \
	for symbol in maelys_sys_loop_create maelys_sys_loop_watch_fd \
		maelys_sys_loop_step maelys_sys_loop_stop maelys_sys_fd_close \
		maelys_sys_socket_send_nosigpipe maelys_sys_monotonic_ms; do \
		echo "$$symbols" | grep -q "$$symbol" || \
			{ echo "Egress does not consume $$symbol" >&2; exit 1; }; \
	done
	@echo "maelys-system reactor dependency is real"

check: test examples-check sdk-check audit system-integration-check contract-check schema-check
	$(CXX) -Iinclude -std=c++17 -Wall -Wextra -Wpedantic -Werror \
		tests/header_cpp.cpp -c -o $(BUILD)/header-cpp.o

mutation-check:
	scripts/mutation-check.sh

asan-ubsan:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=asan-ubsan CC=clang CXX=clang++ \
		SANITIZE_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' check

tsan:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=tsan CC=clang CXX=clang++ \
		SANITIZE_FLAGS='-fsanitize=thread -fno-omit-frame-pointer' check
	@for attempt in 1 2 3 4 5 6 7 8 9 10; do \
		build/tsan/bin/test-egress >/dev/null || exit 1; \
		build/tsan/bin/test-operations >/dev/null || exit 1; \
	done
	@echo "TSan lifecycle/policy-replacement repetition: 10/10 passed"

analyze: | $(MAELYS_SYSTEM_LIB)
	$(CC) --analyze $(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L \
		-D_XOPEN_SOURCE=700 $(SOURCES)

$(BIN)/fuzz-http: fuzz/fuzz_http.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/fuzz-socks: fuzz/fuzz_socks.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/fuzz-clienthello: fuzz/fuzz_clienthello.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

fuzz-smoke:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=fuzz-smoke CC=clang CXX=clang++ \
		CFLAGS='-O1 -g -DMAELYS_FUZZ_STANDALONE' \
		SANITIZE_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
		build/fuzz-smoke/bin/fuzz-http build/fuzz-smoke/bin/fuzz-socks \
		build/fuzz-smoke/bin/fuzz-clienthello
	build/fuzz-smoke/bin/fuzz-http
	build/fuzz-smoke/bin/fuzz-socks
	build/fuzz-smoke/bin/fuzz-clienthello

fuzz:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=fuzz CC=clang CXX=clang++ \
		CFLAGS='-O1 -g' \
		SANITIZE_FLAGS='-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer' \
		build/fuzz/bin/fuzz-http build/fuzz/bin/fuzz-socks \
		build/fuzz/bin/fuzz-clienthello
	build/fuzz/bin/fuzz-http -runs=10000
	build/fuzz/bin/fuzz-socks -runs=10000
	build/fuzz/bin/fuzz-clienthello -runs=10000

install: $(STATIC_LIB) $(CLI) $(PC) $(MANIFEST) $(MAELYS_SYSTEM_LIB)
ifeq ($(MAELYS_SYSTEM_PREFIX),)
	$(MAKE) -C $(MAELYS_SYSTEM_DIR) BUILD=$(MAELYS_SYSTEM_BUILD) \
		VERSION=$(MAELYS_SYSTEM_VERSION) CPPFLAGS= DESTDIR=$(DESTDIR) \
		PREFIX=$(PREFIX) install
endif
	install -d $(DESTDIR)$(PREFIX)/include/maelys $(DESTDIR)$(PREFIX)/lib/pkgconfig \
		$(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/doc/maelys-egress \
		$(DESTDIR)$(PREFIX)/share/maelys/commands
	install -m 0644 $(MANIFEST) $(DESTDIR)$(PREFIX)/share/maelys/commands/
	install -m 0644 include/maelys/egress.h include/maelys/egress_tls.h \
		include/maelys/egress_profile.h \
		$(DESTDIR)$(PREFIX)/include/maelys/
	install -m 0644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 0644 $(PC) $(DESTDIR)$(PREFIX)/lib/pkgconfig/
	install -m 0755 $(CLI) $(DESTDIR)$(PREFIX)/bin/
	install -m 0644 README.md CHANGELOG.md LICENSE LICENSING.md SECURITY.md \
		THIRD_PARTY_NOTICES.md docs/*.md \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/
	install -d $(DESTDIR)$(PREFIX)/share/doc/maelys-egress/generated \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/protocol \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/skills/egress-cli-contract
	install -m 0644 docs/generated/*.md docs/generated/*.json \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/generated/
	install -m 0644 protocol/*.json \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/protocol/
	install -m 0644 skills/egress-cli-contract/SKILL.md \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/skills/egress-cli-contract/
	install -d $(DESTDIR)$(PREFIX)/share/doc/maelys-egress/examples
	install -m 0644 packaging/maelys-egress.conf.example \
		packaging/homebrew/maelys-egress.rb.in \
		packaging/systemd/maelys-egress.service \
		packaging/launchd/com.maelys.egress.plist docker/Dockerfile.sidecar \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/examples/
	install -d $(DESTDIR)$(PREFIX)/share/doc/maelys-egress/examples/c
	install -m 0644 examples/*.c examples/README.md \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/examples/c/
	install -d $(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/python/src/maelys_egress \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/python/tests \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/node/test
	install -m 0644 sdk/python/LICENSE sdk/python/README.md \
		sdk/python/pyproject.toml \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/python/
	install -m 0644 sdk/python/src/maelys_egress/__init__.py \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/python/src/maelys_egress/
	install -m 0644 sdk/python/tests/test_sdk.py \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/python/tests/
	install -m 0644 sdk/node/LICENSE sdk/node/README.md sdk/node/index.js \
		sdk/node/package.json \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/node/
	install -m 0644 sdk/node/test/egress.test.js \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/sdk/node/test/

install-tls-modules: tls-binaries
	install -d $(DESTDIR)$(PREFIX)/include/maelys $(DESTDIR)$(PREFIX)/lib \
		$(DESTDIR)$(PREFIX)/bin
	install -m 0644 include/maelys/egress_tls_modules.h \
		$(DESTDIR)$(PREFIX)/include/maelys/
	install -m 0644 $(MBEDTLS_LIB) $(WOLFSSL_LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 0755 $(MBEDTLS_CLI) $(WOLFSSL_CLI) $(DESTDIR)$(PREFIX)/bin/

install-check: all
	@set -e; stage="$$(mktemp -d)"; trap 'rm -rf "$$stage"' EXIT; \
	$(MAKE) DESTDIR="$$stage" install; \
	test -x "$$stage$(PREFIX)/bin/maelys-egress"; \
	test -f "$$stage$(PREFIX)/lib/libmaelys_egress.a"; \
	if [ -z "$(MAELYS_SYSTEM_PREFIX)" ]; then test -f "$$stage$(PREFIX)/lib/libmaelys_sys.a"; fi; \
	test -f "$$stage$(PREFIX)/share/maelys/commands/egress.json"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/examples/c/basic_proxy.c"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/sdk/python/pyproject.toml"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/sdk/node/package.json"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/generated/cli-reference.md"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/protocol/egress-lifecycle-v1.schema.json"

dist:
	@mkdir -p dist
	git archive --format=tar --prefix=maelys-egress-$(VERSION)/ HEAD | \
		gzip -n >dist/maelys-egress-$(VERSION).tar.gz
	cd dist && shasum -a 256 maelys-egress-$(VERSION).tar.gz > \
		maelys-egress-$(VERSION).tar.gz.sha256

clean:
	rm -rf build
