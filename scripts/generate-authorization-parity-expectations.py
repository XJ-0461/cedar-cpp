#!/usr/bin/env python3
"""Asks the reference Cedar CLI to authorize each case in a directory and writes the expectations
tests/unit/AuthorizerTests.cpp compares with.

Input, in the cases directory: `Entities.json`, shared by every case, and per case `<Name>`:
  <Name>.cedar     the policy set (static policies and templates);
  <Name>.request   four lines, `principal: <uid>`, `action: <uid>`, `resource: <uid>` and
                   `context: <JSON object>`;
  <Name>.links     optional; one template link per line,
                   `<link id> <template id> ?principal=<uid> ?resource=<uid>` (either slot optional).

Output: `<Name>.expected` per case, from `cedar authorize --verbose`, one item per line:
  decision: allow|deny
  reason: <policy id>                 one line per determining policy, sorted
  error: <policy id> <error kind>     one line per erroring policy, sorted
The error kind is ours (EvaluationErrorKind), read from the CLI's error message.
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

ERROR_LINE = re.compile(r"^error while evaluating policy `([^`]*)`: (.*)$")


def kind_of(message):
    for pattern, kind in KINDS:
        if pattern.search(message):
            return kind
    sys.exit(f"unrecognized evaluation error message: {message}")


def read_request(path):
    fields = {}
    with open(path, encoding="utf-8") as file:
        for line in file:
            line = line.rstrip("\n")
            if not line:
                continue
            key, _, value = line.partition(": ")
            fields[key] = value
    for key in ("principal", "action", "resource", "context"):
        if key not in fields:
            sys.exit(f"{path}: missing `{key}:` line")
    return fields


def read_links(path):
    links = []
    with open(path, encoding="utf-8") as file:
        for line in file:
            words = line.split()
            if not words:
                continue
            arguments = {}
            for word in words[2:]:
                slot, _, uid = word.partition("=")
                arguments[slot] = uid
            links.append({"link_id": words[0], "template_id": words[1], "args": arguments})
    return links


def expectations(output):
    lines = output.strip().splitlines()
    if not lines or lines[0] not in ("ALLOW", "DENY"):
        sys.exit(f"unexpected oracle output:\n{output}")
    errors = []
    reason = []
    in_reason = False
    for line in lines[1:]:
        match = ERROR_LINE.match(line)
        if match:
            errors.append((match.group(1), kind_of(match.group(2))))
        elif line.startswith("note: this decision was due to the following policies:"):
            in_reason = True
        elif in_reason and line.startswith("  "):
            reason.append(line.strip())
        elif line.strip():
            in_reason = False
    result = [f"decision: {lines[0].lower()}"]
    result += [f"reason: {policy}" for policy in sorted(reason)]
    result += [f"error: {policy} {kind}" for policy, kind in sorted(errors)]
    return "\n".join(result) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--oracle", required=True, help="the reference `cedar` binary")
    parser.add_argument("--cases-directory", required=True, help="the directory holding the cases")
    arguments = parser.parse_args()

    directory = arguments.cases_directory
    entities = os.path.join(directory, "Entities.json")
    names = sorted(name[: -len(".cedar")] for name in os.listdir(directory) if name.endswith(".cedar"))
    with tempfile.TemporaryDirectory() as scratch:
        for name in names:
            request = read_request(os.path.join(directory, name + ".request"))
            context = os.path.join(scratch, "context.json")
            with open(context, "w", encoding="utf-8") as file:
                file.write(request["context"])
            command = [arguments.oracle, "authorize", "--verbose",
                       "--policies", os.path.join(directory, name + ".cedar"), "--entities", entities,
                       "--principal", request["principal"], "--action", request["action"],
                       "--resource", request["resource"], "--context", context]
            links_path = os.path.join(directory, name + ".links")
            if os.path.exists(links_path):
                links = os.path.join(scratch, "links.json")
                with open(links, "w", encoding="utf-8") as file:
                    json.dump(read_links(links_path), file)
                command += ["--template-linked", links]
            answer = subprocess.run(command, capture_output=True, text=True, check=False)
            if answer.returncode not in (0, 2):
                sys.exit(f"{name}: oracle failed ({answer.returncode}):\n{answer.stdout}{answer.stderr}")
            with open(os.path.join(directory, name + ".expected"), "w", encoding="utf-8") as file:
                file.write(expectations(answer.stdout))
    print(f"wrote {len(names)} expectations to {directory}")


if __name__ == "__main__":
    main()
