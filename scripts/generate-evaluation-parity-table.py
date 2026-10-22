#!/usr/bin/env python3
"""Asks the reference Cedar CLI to evaluate every expression in an input list against one request,
entity file and context file, and writes the parity table tests/unit/EvaluatorTests.cpp runs.

Input: one expression per line; blank lines and lines starting with `#` are skipped. A line may
end with a tab and a note (`span: <reason>`), copied to the table, that marks the error span as
not compared.

Output: one line per expression, tab-separated:
  value  <expression>  <Rust's printed value>                    <note>
  error  <expression>  <error kind>  <offset>:<length> or empty  <note>
The error kind is ours (EvaluationErrorKind), read from the CLI's error message. The printed value
is the CLI's text: constructor arguments as written, sets re-sorted by printed text, record keys
quoted, so the test compares it as a value, not as text.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

KINDS = [
    (re.compile(r"^entity `.*` does not exist$", re.S), "EntityDoesNotExist"),
    (re.compile(r"^`.*` does not have the attribute `.*`$", re.S), "EntityAttributeDoesNotExist"),
    (re.compile(r"^`.*` does not have the tag `.*`$", re.S), "EntityTagDoesNotExist"),
    (re.compile(r"^record does not have the attribute `.*`$", re.S), "RecordAttributeDoesNotExist"),
    (re.compile(r"^type error: "), "TypeError"),
    (re.compile(r"^wrong number of arguments provided to extension function "), "WrongArgumentCount"),
    (re.compile(r"^integer overflow "), "IntegerOverflow"),
    (re.compile(r"^template slot `.*` was not linked$"), "UnlinkedSlot"),
    (re.compile(r"^error while evaluating `.*` extension function"), "ExtensionFunctionExecution"),
    (re.compile(r"^recursion limit reached$"), "RecursionLimit"),
]


def kind_of(cause):
    for pattern, kind in KINDS:
        if pattern.search(cause):
            return kind
    raise SystemExit(f"unrecognized evaluation error: {cause}")


def main():
    parser = argparse.ArgumentParser()
    for flag in ("--oracle", "--input", "--entities", "--context", "--principal", "--action", "--resource",
                 "--output"):
        parser.add_argument(flag, required=True)
    arguments = parser.parse_args()

    rows = []
    with open(arguments.input, encoding="utf-8") as lines:
        for line in lines:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            expression, _, note = line.partition("\t")
            command = [arguments.oracle, "evaluate", "-f", "json", "--principal", arguments.principal,
                       "--action", arguments.action, "--resource", arguments.resource, "--context",
                       arguments.context, "--entities", arguments.entities, "--", expression]
            completed = subprocess.run(command, capture_output=True, text=True)
            if completed.returncode == 0:
                rows.append(["value", expression, completed.stdout.strip(), note])
                continue
            report = json.loads(completed.stderr.strip() or completed.stdout.strip())
            if report.get("message") != "failed to evaluate the expression":
                raise SystemExit(f"the oracle did not evaluate `{expression}`: {report.get('message')}")
            span = ""
            labels = report.get("labels") or []
            if labels:
                span = f"{labels[0]['span']['offset']}:{labels[0]['span']['length']}"
            rows.append(["error", expression, kind_of(report["causes"][0]), span, note])

    directory = os.path.dirname(os.path.abspath(arguments.output))
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=directory, delete=False) as temporary:
        for row in rows:
            if any("\t" in cell or "\n" in cell for cell in row):
                raise SystemExit(f"a cell contains a tab or line break: {row}")
            temporary.write("\t".join(row) + "\n")
    os.replace(temporary.name, arguments.output)
    print(f"wrote {len(rows)} expressions to {arguments.output}")


if __name__ == "__main__":
    sys.exit(main())
