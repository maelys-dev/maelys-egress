#!/usr/bin/env python3
"""Hold the declared examples, and the README to them.

The framework parses each example of the catalog at startup and refuses the
catalog when a command no longer accepts one: that is what keeps an example
true. What it cannot know is this product's own: that every command it adds
has one, and that the invocations the README shows are declared examples,
so that a line of the README cannot name what the binary does not accept.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

BINARY = Path(sys.argv[1]).resolve()
ROOT = Path(__file__).resolve().parents[1]
ENV = dict(os.environ, MAELYS_CLI_FORMAT="json")
PROGRAM = "maelys-egress"
# The commands this product declares in cli/main.c. The others are the
# framework's, and their examples, if any, are its own.
OWN = ("config.describe", "config.validate", "serve", "channel.broker", "channel.exec")


def invoke(*args, env=ENV):
    run = subprocess.run([str(BINARY), *args, "--non-interactive"], env=env,
                         capture_output=True, text=True, timeout=10)
    assert run.returncode == 0 and not run.stderr, (args, run.returncode, run.stderr)
    return run.stdout


def catalog(*args):
    return {c["id"]: c for c in json.loads(invoke("describe", *args))["data"]["commands"]}


def readme_invocations():
    """Each line of a shell block of the README that starts the program.

    A block marked `sh` is one to copy into a shell; the others are
    configuration, output or a diagram, where the program's name is a word.
    """
    lines, shell = [], False
    for line in (ROOT / "README.md").read_text(encoding="utf-8").splitlines():
        if line.startswith("```"):
            shell = not shell and line.strip() == "```sh"
        elif shell and line.startswith(PROGRAM + " "):
            lines.append(line.split()[1:])
    return lines


def main():
    commands = catalog()
    assert set(OWN) <= set(commands), sorted(commands)
    declared = []
    for identifier in OWN:
        examples = commands[identifier].get("examples")
        assert examples, f"{identifier} declares no example"
        for example in examples:
            words, summary = example["words"], example["summary"]
            # An example starts with the words of its command and says what
            # the line does in a sentence.
            assert words[:len(commands[identifier]["pattern"])] == commands[identifier]["pattern"], words
            assert summary.endswith(".") and len(summary) > 20, summary
            declared.append(words)
        # One descriptor says what the catalog says, and the summary omits
        # the examples as it omits the output schema.
        assert catalog(identifier)[identifier]["examples"] == examples, identifier
        # The help of the command shows each example under its own heading,
        # after the program's name. Words are compared, not columns: a long
        # line is wrapped to the width of the help.
        text = invoke("help", identifier, env=dict(os.environ)).split("EXAMPLES\n", 1)
        assert len(text) == 2, f"help {identifier} has no EXAMPLES section"
        shown = " ".join(text[1].split())
        for example in examples:
            assert " ".join([PROGRAM, *example["words"]]) in shown, (identifier, example)
    assert all("examples" not in command for command in catalog("--summary").values())

    # The README shows invocations, and each is a declared example: the
    # framework has parsed it against the binary this test was given.
    shown = readme_invocations()
    assert shown, "the README shows no invocation: this check reads nothing"
    for words in shown:
        assert words in declared, (
            f"README.md shows '{PROGRAM} {' '.join(words)}', which no command declares "
            "as an example in cli/main.c")
    print(f"examples: {len(declared)} declared on {len(OWN)} commands, "
          f"{len(shown)} README invocations are among them")


if __name__ == "__main__":
    main()
