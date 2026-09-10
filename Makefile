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
# depends on libmaelys-sys). dependencies/maelys-system.pin records the tag and
# the commit; an installed System must carry the same ABI and at least the
# pinned version.
MAELYS_SYSTEM_DIR ?= ../maelys-system
MAELYS_SYSTEM_PREFIX ?=
MAELYS_SYSTEM_TAG := $(word 1,$(shell cat dependencies/maelys-system.pin))
MAELYS_SYSTEM_PIN := $(word 2,$(shell cat dependencies/maelys-system.pin))
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
MAELYS_CLI_TAG := $(word 1,$(shell cat dependencies/maelys-cli.pin))
MAELYS_CLI_PIN := $(word 2,$(shell cat dependencies/maelys-cli.pin))
MAELYS_CLI_BUILD := $(abspath $(BUILD)/deps/maelys-cli)
MAELYS_CLI_LIB := $(MAELYS_CLI_BUILD)/lib/libmaelys_cli.a
MAELYS_CLI_EMBED := $(MAELYS_CLI_DIR)/tools/maelys-cli-embed
MAELYS_CLI_REFERENCE := $(MAELYS_CLI_DIR)/tools/generate_cli_reference.py
# agent-cli-spec owns the contract the command implements. Its conformance kit
# drives the built binary from the outside, so the pin must be the one the
# pinned framework targets; check-spec-contract holds the two together.
MAELYS_SPEC_DIR ?= ../agent-cli-spec
MAELYS_SPEC_TAG := $(word 1,$(shell cat dependencies/agent-cli-spec.pin))
MAELYS_SPEC_PIN := $(word 2,$(shell cat dependencies/agent-cli-spec.pin))
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

# src/core holds the decisions taken on bytes alone: it never names
# maelys_sys, a boundary scripts/audit-boundaries.sh enforces. src/server owns
# the descriptors and the reactor. The three files between them touch the host
# without being the server.
CORE_SOURCES := src/core/common.c src/core/sha256.c src/core/receipt.c \
	src/core/attestor.c src/core/policy.c src/core/profile.c src/core/tls.c \
	src/core/clienthello.c src/core/http.c src/core/socks.c
HOST_SOURCES := src/audit.c src/config.c src/connector.c
SERVER_SOURCES := src/server/server.c src/server/listener.c \
	src/server/connection.c src/server/relay.c src/server/quota.c \
	src/server/receipt.c src/server/connector.c src/server/admin.c
SOURCES := $(CORE_SOURCES) $(HOST_SOURCES) $(SERVER_SOURCES)
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

# `all` is declared before the dependency files are included: a rule read
# from an included makefile would otherwise become the default goal, and
# plain `make` would build one stale object instead of the product.
all: $(STATIC_LIB) $(CLI) $(TEST) $(PC) $(MANIFEST)

-include $(DEPENDENCIES)

.PHONY: all clean check test examples-check sdk-check audit check-system-contract check-cli-contract \
	system-integration-check mutation-check check-spec-contract conformance-check \
	config-reference contract-check lifecycle-contract-check schema-check package-homebrew \
	tls-mbedtls-check tls-wolfssl-check tls-providers-check tls-binaries \
	asan-ubsan tsan analyze fuzz fuzz-smoke install install-tls-modules install-check \
	public-check reproducible-check install-metadata-check dist

ifeq ($(MAELYS_SYSTEM_PREFIX),)
check-system-contract:
	@test -f "$(MAELYS_SYSTEM_DIR)/include/maelys/sys.h" || \
		{ echo "MAELYS_SYSTEM_DIR must name maelys-system" >&2; exit 1; }
	@test "$$(git -C "$(MAELYS_SYSTEM_DIR)" rev-parse HEAD)" = "$(MAELYS_SYSTEM_PIN)" || \
		{ echo "maelys-system must be pinned to $(MAELYS_SYSTEM_PIN)" >&2; exit 1; }
	@git -C "$(MAELYS_SYSTEM_DIR)" diff --quiet "$(MAELYS_SYSTEM_PIN)" -- || \
		{ echo "pinned maelys-system checkout is modified" >&2; exit 1; }
	@test -z "$$(git -C "$(MAELYS_SYSTEM_DIR)" ls-files --others --exclude-standard)" || \
		{ echo "pinned maelys-system checkout carries untracked files" >&2; exit 1; }
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
	@git -C "$(MAELYS_CLI_DIR)" diff --quiet "$(MAELYS_CLI_PIN)" -- || \
		{ echo "pinned maelys-cli checkout is modified" >&2; exit 1; }
	@test -z "$$(git -C "$(MAELYS_CLI_DIR)" ls-files --others --exclude-standard)" || \
		{ echo "pinned maelys-cli checkout carries untracked files" >&2; exit 1; }
	@grep -Fq '#define MAELYS_CLI_ABI 1' "$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"
	@grep -Fq '#define MAELYS_CLI_VERSION "$(MAELYS_CLI_TAG:v%=%)"' \
		"$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"
	@grep -Fq '#define MAELYS_CLI_CONTRACT "agent-cli/v2"' \
		"$(MAELYS_CLI_DIR)/include/maelys/cli/version.h"
	@# The framework writes four texts into this repository through
	@# 'maelys agents install', which nothing here regenerates. Each one names
	@# the version it came from, so a pin moved without that command leaves a
	@# text behind; the composed guide cannot be derived from the checkout
	@# without the dispatcher and its own dependencies, but its stamp can.
	@for text in docs/maelys-cli-guide.md .claude/skills/maelys-cli-command/SKILL.md \
			AGENTS.md CLAUDE.md; do \
		grep -Fq 'maelys-cli $(MAELYS_CLI_TAG:v%=%)' "$$text" || \
			{ echo "$$text was written by another maelys-cli than $(MAELYS_CLI_TAG); run 'maelys agents install . --apply' from the pinned checkout" >&2; exit 1; }; \
	done

check-spec-contract:
	@test -f "$(MAELYS_SPEC_DIR)/conformance/run.py" || \
		{ echo "MAELYS_SPEC_DIR must name agent-cli-spec" >&2; exit 1; }
	@test "$$(git -C "$(MAELYS_SPEC_DIR)" rev-parse HEAD)" = "$(MAELYS_SPEC_PIN)" || \
		{ echo "agent-cli-spec must be pinned to $(MAELYS_SPEC_TAG) ($(MAELYS_SPEC_PIN))" >&2; exit 1; }
	@git -C "$(MAELYS_SPEC_DIR)" diff --quiet "$(MAELYS_SPEC_PIN)" -- || \
		{ echo "pinned agent-cli-spec checkout is modified" >&2; exit 1; }
	@test -z "$$(git -C "$(MAELYS_SPEC_DIR)" ls-files --others --exclude-standard)" || \
		{ echo "pinned agent-cli-spec checkout carries untracked files" >&2; exit 1; }
	@test "$$(sed -n 1p "$(MAELYS_CLI_DIR)/dependencies/agent-cli-spec.pin")" = "$(MAELYS_SPEC_TAG)" || \
		{ echo "agent-cli-spec $(MAELYS_SPEC_TAG) is not the version maelys-cli $(MAELYS_CLI_TAG) targets" >&2; exit 1; }

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
# VERSION is a prerequisite because CPPFLAGS embeds it in every object as
# MAELYS_EGRESS_BUILD_VERSION: without it, a version bump left the binary
# reporting the previous version until something else forced a rebuild.
$(OBJ)/cli/%.o: cli/%.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ)/%.o: %.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# rm -f so the archive is exactly its objects rather than an accumulation
# across builds, and ZERO_AR_DATE so the same objects always give the same
# bytes; reproducible-check holds both to their word.
$(STATIC_LIB): $(OBJECTS)
	@mkdir -p $(@D)
	rm -f $@
	ZERO_AR_DATE=1 $(AR) rcs $@ $^

$(OBJ)/providers/tls_mbedtls.o: providers/tls_mbedtls.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $$(pkg-config --cflags mbedtls) -MMD -MP -c $< -o $@

$(MBEDTLS_LIB): $(OBJ)/providers/tls_mbedtls.o
	@mkdir -p $(@D)
	ar rcs $@ $^

$(OBJ)/providers/tls_wolfssl.o: providers/tls_wolfssl.c VERSION
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $$(pkg-config --cflags wolfssl) -MMD -MP -c $< -o $@

$(WOLFSSL_LIB): $(OBJ)/providers/tls_wolfssl.o
	@mkdir -p $(@D)
	ar rcs $@ $^

$(CLI_OBJECTS): | $(MAELYS_SYSTEM_LIB) $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h

$(CLI): $(CLI_OBJECTS) $(CLI_SCHEMA_OBJECT) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/cli/tls_listener-mbedtls.o: cli/tls_listener.c VERSION | $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) \
		-DMAELYS_EGRESS_TLS_FACTORY=maelys_egress_tls_mbedtls_create \
		-MMD -MP -c $< -o $@

$(MBEDTLS_CLI): $(OBJ)/cli/tls_listener-mbedtls.o $(CLI_COMMON_OBJECTS) $(CLI_SCHEMA_OBJECT) $(MBEDTLS_LIB) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) \
		$$(pkg-config --libs mbedtls mbedx509 mbedcrypto) -o $@

$(OBJ)/cli/tls_listener-wolfssl.o: cli/tls_listener.c VERSION | $(MAELYS_CLI_LIB) $(GENERATED)/egress_schemas.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CLI_CPPFLAGS) $(CFLAGS) \
		-DMAELYS_EGRESS_TLS_FACTORY=maelys_egress_tls_wolfssl_create \
		-MMD -MP -c $< -o $@

$(WOLFSSL_CLI): $(OBJ)/cli/tls_listener-wolfssl.o $(CLI_COMMON_OBJECTS) $(CLI_SCHEMA_OBJECT) $(WOLFSSL_LIB) $(STATIC_LIB) $(MAELYS_CLI_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) $$(pkg-config --libs wolfssl) -o $@

$(OBJ)/tests/test_egress.o: tests/test_egress.c VERSION | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(TEST): $(OBJ)/tests/test_egress.o $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ)/tests/test_operations.o: tests/test_operations.c VERSION | $(MAELYS_SYSTEM_LIB)
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

# The pkg-config file and the manifest read by the `maelys` dispatcher both
# describe the installation: the manifest binds `maelys egress` to the
# installed binary by absolute path and by digest, so the dispatcher refuses
# another binary left at that path. Render on every invocation, because PREFIX
# and the linker flags change the result without any source timestamp moving;
# identical bytes keep their mtime.
.PHONY: install-metadata
install-metadata: $(CLI)
	python3 scripts/render-install-metadata.py --prefix="$(PREFIX)" \
		--version="$(VERSION)" \
		--system-version="$(MAELYS_SYSTEM_VERSION)" \
		--binary="$(CLI)" --pkgconfig="$(PC)" --manifest="$(MANIFEST)"

$(PC) $(MANIFEST): | install-metadata
	@test -f $@

test: all $(OPERATIONS_TEST)
	$(TEST)
	$(OPERATIONS_TEST)
	tests/test_cli.sh $(CLI)

# The command reference and contract come from the maelys-cli generator; the
# configuration-key reference is Egress specific. Both read `describe`; the
# generator omits versions by default, so a release never rewrites them.
# docs/cli.md and docs/cli-contract.json belong to the release socle, which
# runs maelys-cli's generator at the pinned commit and reads docs/cli.reference
# for the build holding the programs: 'maelys-release adopt' writes them,
# 'check' compares them, and check-product.yml does it in this repository's CI.
# The configuration reference is this product's own, generated here.
config-reference: $(CLI)
	python3 tools/generate_config_reference.py --binary $(abspath $(CLI)) \
		--output docs/generated/config-reference.md

# Rejects stale generated documentation; part of `check`.
contract-check: $(CLI)
	@mkdir -p $(BUILD)/contract
	python3 tools/generate_config_reference.py --binary $(abspath $(CLI)) \
		--output $(BUILD)/contract/config-reference.md
	@cmp -s $(BUILD)/contract/config-reference.md docs/generated/config-reference.md || \
		{ echo "docs/generated/config-reference.md drifted; run make config-reference" >&2; exit 1; }
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
	@grep -Fq 'version = "$(SOURCE_VERSION)"' sdk/python/pyproject.toml || \
		{ echo "sdk/python/pyproject.toml does not declare $(SOURCE_VERSION)" >&2; exit 1; }
	@grep -Fq '__version__ = "$(SOURCE_VERSION)"' \
		sdk/python/src/maelys_egress/__init__.py || \
		{ echo "sdk/python/src/maelys_egress/__init__.py does not declare $(SOURCE_VERSION)" >&2; exit 1; }
	@node -e 'const p=require("./sdk/node/package.json"); if (p.version !== "$(SOURCE_VERSION)") process.exit(1)' || \
		{ echo "sdk/node/package.json does not declare $(SOURCE_VERSION)" >&2; exit 1; }
	@for sdk in python node; do \
		named="$$(grep -o "maelys-egress-$$sdk-sdk-[0-9][0-9.]*[0-9]" \
			sdk/$$sdk/README.md | sort -u)"; \
		test -n "$$named" || \
			{ echo "sdk/$$sdk/README.md names no SDK archive to install" >&2; exit 1; }; \
		test "$$named" = "maelys-egress-$$sdk-sdk-$(SOURCE_VERSION)" || \
			{ echo "sdk/$$sdk/README.md installs $$(echo "$$named" | paste -sd, -), not maelys-egress-$$sdk-sdk-$(SOURCE_VERSION)" >&2; exit 1; }; \
	done
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
		maelys_sys_socket_create maelys_sys_socket_bind_with maelys_sys_socket_listen \
		maelys_sys_socket_accept maelys_sys_socket_connect_start \
		maelys_sys_socket_connect_complete maelys_sys_socket_receive \
		maelys_sys_socket_send maelys_sys_socket_shutdown \
		maelys_sys_socket_release maelys_sys_socket_detach \
		maelys_sys_fd_set_blocking maelys_sys_fd_wait maelys_sys_file_lock_acquire \
		maelys_sys_file_unlink_same maelys_sys_monotonic_ms; do \
		echo "$$symbols" | grep -q "$$symbol" || \
			{ echo "Egress does not consume $$symbol" >&2; exit 1; }; \
	done
	@echo "maelys-system reactor and socket dependency is real"

# The specification drives the built binary from the outside and validates
# every answer, the envelope and each command's declared output schema
# included. It only reads: no command that writes is invoked.
conformance-check: $(CLI) check-spec-contract
	python3 $(MAELYS_SPEC_DIR)/conformance/run.py $(abspath $(CLI))

check: test examples-check sdk-check audit system-integration-check contract-check schema-check \
	conformance-check public-check reproducible-check install-metadata-check
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

# Text diagnostics keep the analyzer from writing one .plist per source into
# the working directory; -analyzer-werror is what actually fails the gate, as
# plain --analyze and even -Werror report findings and still exit 0.
analyze: | $(MAELYS_SYSTEM_LIB)
	$(CC) --analyze -Xclang -analyzer-output=text -Xclang -analyzer-werror \
		$(CPPFLAGS) -std=c11 -D_POSIX_C_SOURCE=200809L \
		-D_XOPEN_SOURCE=700 $(SOURCES)

$(BIN)/fuzz-http: tests/fuzz/fuzz_http.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/fuzz-socks: tests/fuzz/fuzz_socks.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BIN)/fuzz-clienthello: tests/fuzz/fuzz_clienthello.c $(STATIC_LIB) | $(MAELYS_SYSTEM_LIB)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# The committed seeds carry the structure each parser looks for, so a run
# spends its budget on the boundaries instead of rediscovering that a request
# starts with a method. libFuzzer writes what it finds into the build tree and
# reads tests/fuzz/corpus/ without touching it.
fuzz-smoke:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=fuzz-smoke CC=clang CXX=clang++ \
		CFLAGS='-O1 -g -DMAELYS_FUZZ_STANDALONE' \
		SANITIZE_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
		build/fuzz-smoke/bin/fuzz-http build/fuzz-smoke/bin/fuzz-socks \
		build/fuzz-smoke/bin/fuzz-clienthello
	build/fuzz-smoke/bin/fuzz-http tests/fuzz/corpus/http
	build/fuzz-smoke/bin/fuzz-socks tests/fuzz/corpus/socks
	build/fuzz-smoke/bin/fuzz-clienthello tests/fuzz/corpus/clienthello

fuzz:
	$(MAKE) clean
	$(MAKE) BUILD_PROFILE=fuzz CC=clang CXX=clang++ \
		CFLAGS='-O1 -g' \
		SANITIZE_FLAGS='-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer' \
		build/fuzz/bin/fuzz-http build/fuzz/bin/fuzz-socks \
		build/fuzz/bin/fuzz-clienthello
	@mkdir -p build/fuzz/corpus/http build/fuzz/corpus/socks build/fuzz/corpus/clienthello
	build/fuzz/bin/fuzz-http build/fuzz/corpus/http tests/fuzz/corpus/http -runs=10000
	build/fuzz/bin/fuzz-socks build/fuzz/corpus/socks tests/fuzz/corpus/socks -runs=10000
	build/fuzz/bin/fuzz-clienthello build/fuzz/corpus/clienthello tests/fuzz/corpus/clienthello -runs=10000

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
		THIRD_PARTY_NOTICES.md docs/*.md docs/cli-contract.json \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/
	install -d $(DESTDIR)$(PREFIX)/share/doc/maelys-egress/generated \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/protocol
	install -m 0644 docs/generated/*.md \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/generated/
	install -m 0644 protocol/*.json \
		$(DESTDIR)$(PREFIX)/share/doc/maelys-egress/protocol/
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

# An embedder builds against the installed headers and archive, found through
# pkg-config alone. Staging the real install and linking a real consumer there
# proves the published artifact stands on its own; the repository's own build
# cannot supply what the install forgot, nor hide a dependency the library
# must not carry. The examples are shipped for embedders to copy, so they are
# built the same way: examples-check compiles them against this tree, which
# says nothing about whether the installed library is enough for them.
public-check: all
	@set -e; stage="$$(mktemp -d)"; trap 'rm -rf "$$stage"' EXIT; \
	$(MAKE) DESTDIR="$$stage" install >/dev/null; \
	for pc in "$$stage$(PREFIX)"/lib/pkgconfig/*.pc; do \
		sed "s|^prefix=.*|prefix=$$stage$(PREFIX)|" "$$pc" >"$$pc.staged"; \
		mv "$$pc.staged" "$$pc"; \
	done; \
	flags="$$(PKG_CONFIG_PATH="$$stage$(PREFIX)/lib/pkgconfig" \
		pkg-config --static --cflags --libs maelys-egress)"; \
	$(CC) $(CFLAGS) tests/public/consumer.c $$flags $(LDFLAGS) \
		-o "$$stage/consumer"; \
	"$$stage/consumer"; \
	for example in examples/*.c; do \
		$(CC) $(CFLAGS) "$$example" $$flags $(LDFLAGS) -o "$$stage/example" || \
			{ echo "public-check: $$example does not build against the installed library" >&2; \
			  exit 1; }; \
	done; \
	echo "public-check: the consumer ran and every example builds through pkg-config"

# The published archive must be a function of its objects alone, so that the
# same sources give the same bytes to anyone who rebuilds them.
reproducible-check: $(STATIC_LIB)
	@mkdir -p $(BUILD)/reproducible
	@rm -f $(BUILD)/reproducible/libmaelys_egress.a
	ZERO_AR_DATE=1 $(AR) rcs $(BUILD)/reproducible/libmaelys_egress.a $(OBJECTS)
	cmp $(STATIC_LIB) $(BUILD)/reproducible/libmaelys_egress.a
	@echo "reproducible-check: identical archive from the same objects"

# The manifest and the pkg-config file are the two installed files that name
# the installation itself; this stages one under two prefixes and reads back
# what the dispatcher would read.
install-metadata-check: all
	python3 tests/test_install.py $(BUILD) PREFIX="$(PREFIX)" \
		MAELYS_SYSTEM_DIR="$(MAELYS_SYSTEM_DIR)" \
		MAELYS_SYSTEM_PREFIX="$(MAELYS_SYSTEM_PREFIX)" \
		MAELYS_CLI_DIR="$(MAELYS_CLI_DIR)"

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
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/cli.md"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/cli-contract.json"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/generated/config-reference.md"; \
	test -f "$$stage$(PREFIX)/share/doc/maelys-egress/protocol/egress-lifecycle-v1.schema.json"

dist:
	@mkdir -p dist
	git archive --format=tar --prefix=maelys-egress-$(VERSION)/ HEAD | \
		gzip -n >dist/maelys-egress-$(VERSION).tar.gz
	cd dist && shasum -a 256 maelys-egress-$(VERSION).tar.gz > \
		maelys-egress-$(VERSION).tar.gz.sha256

clean:
	rm -rf build
