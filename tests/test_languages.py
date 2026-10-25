#!/usr/bin/env python3
"""Hold the human text in French and English, and what never changes with it.

The framework chooses the language of the help and of the text of a failure
(--lang, MAELYS_CLI_LANG, then the locale) and shows in English any text a
table does not name. What it cannot know is this product's own: that the
French table of cli/languages.c names every text of the catalog, so that a
sentence changed in English is not left behind, and that what an agent
reads -- describe, the envelopes, version -- is the same bytes in every
language.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

BINARY = Path(sys.argv[1]).resolve()
# The commands this product declares in cli/main.c. The others, and the
# global options, are the framework's, which translates them itself.
OWN = ("config.describe", "config.validate", "serve", "channel.broker", "channel.exec")
# No locale, no MAELYS_CLI_LANG: each call names the language it wants.
BASE = {name: value for name, value in os.environ.items()
        if name not in ("LANG", "LC_ALL", "LC_MESSAGES", "MAELYS_CLI_LANG", "MAELYS_CLI_FORMAT")}
MISSING = "/nonexistent/maelys-egress.conf"


def run(*args, env=None, status=0):
    done = subprocess.run([str(BINARY), *args, "--non-interactive"], env=dict(BASE, **(env or {})),
                          capture_output=True, text=True, timeout=10)
    assert done.returncode == status, (args, env, done.returncode, done.stderr)
    return done


def catalog_texts(catalog):
    """Every human text of the catalog that this product wrote."""
    texts = [catalog["x-maelys-summary"], catalog["x-maelys-guidance"]]
    for command in catalog["commands"]:
        if command["id"] not in OWN:
            continue
        texts.append(command["purpose"])
        for item in command["input"]["operands"] + command["input"]["options"]:
            texts.append(item["summary"])
        texts.extend(example["summary"] for example in command.get("examples", []))
    return texts


def main():
    catalog = json.loads(run("describe", "--format", "json").stdout)["data"]
    assert {c["id"] for c in catalog["commands"]} >= set(OWN), catalog["commands"]

    # The catalog is written in English and translated to French, whole.
    translations = catalog["x-maelys-translations"]
    assert translations["canonical"] == "en", translations
    assert sorted(translations["languages"]) == ["fr"], sorted(translations["languages"])
    french = translations["languages"]["fr"]
    texts = catalog_texts(catalog)
    missing = [text for text in texts if text not in french]
    assert not missing, f"cli/languages.c does not translate: {missing}"
    assert json.loads(run("version", "--format", "json").stdout)["data"]["lang"] == "en"

    # What an agent reads is the same bytes in every language.
    for args in (("describe", "--format", "json"), ("describe", "--summary", "--format", "json"),
                 ("version", "--format", "json"), ("describe", "channel.exec", "--format", "json")):
        english = run(*args).stdout
        assert run(*args, "--lang", "fr").stdout == english, args
        assert run(*args, env={"LANG": "fr_FR.UTF-8"}).stdout == english, args
    english = run("config", "validate", "--config", MISSING, "--format", "json", status=1).stderr
    hint = json.loads(english)["error"]["hint"]
    assert hint == "Name an existing configuration file readable by the daemon.", english
    assert run("config", "validate", "--config", MISSING, "--format", "json", "--lang", "fr",
               status=1).stderr == english

    # A person reads the help, and the text of a failure, in the language
    # asked: --lang first, then MAELYS_CLI_LANG, then the locale.
    purpose = "Décrit chaque clé de configuration prise en charge et ses contraintes."
    for env, args in (({}, ("--lang", "fr")), ({"MAELYS_CLI_LANG": "fr"}, ()),
                      ({"LANG": "fr_FR.UTF-8"}, ()), ({"LC_ALL": "fr_CA.UTF-8"}, ())):
        shown = " ".join(run("help", *args, env=env).stdout.split())
        assert "COMMANDES" in shown and purpose in shown, (env, args, shown[:300])
    for env, args in (({}, ()), ({"LANG": "fr_FR.UTF-8"}, ("--lang", "en")),
                      ({"LC_ALL": "C", "LANG": "fr_FR.UTF-8"}, ()), ({}, ("--lang", "de"))):
        shown = " ".join(run("help", *args, env=env).stdout.split())
        assert "COMMANDS" in shown, (env, args, shown[:300])
        assert "Describe every supported configuration key and constraint." in shown, (env, args)
    shown = " ".join(run("help", "config.validate", "--lang", "fr").stdout.split())
    for text in ("UTILISATION", "EXEMPLES", "Fichier de configuration complet du démon "
                 "(schema_version = 1). Obligatoire."):
        assert text in shown, (text, shown)
    failure = run("config", "validate", "--config", MISSING, "--lang", "fr", status=1).stderr
    assert "Nommez un fichier de configuration existant, lisible par le démon." in failure, failure
    failure = run("config", "validate", "--config", MISSING, status=1).stderr
    assert "Name an existing configuration file readable by the daemon." in failure, failure
    print(f"languages: {len(texts)} catalog texts translated to French, "
          "describe, envelopes and version the same in every language")


if __name__ == "__main__":
    main()
