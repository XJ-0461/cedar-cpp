#!/usr/bin/env python3
"""Runs each validation parity case through the reference CLI and writes the expectations.

Reads a cases table whose header names its columns: `case`, `schema` (relative to the table's
directory), `mode`, `policy` (the policy set text), and optionally `links`, the template-linked
policies: `-`, or `;`-separated `template:link:slot=uid` entries with `+` between the slots of one
link (`policy0:link0:?principal=User::"u"+?resource=Doc::"d"`), and `level`, `-` or the maximum
entity dereference level to validate at. Other columns are ignored. Each case is validated with
`<oracle> validate --schema <schema> --policies <file> [--template-linked <links.json>] [--level
<level>] --validation-mode <mode> --error-format json`, and the script writes
`case<TAB>verdict<TAB>kinds`: verdict is `pass` or `fail` (the CLI's exit status) and kinds is
the sorted, comma-separated set of validation diagnostic kinds the oracle reported, errors and
warnings, named as cedar-cpp's CodeName (`validation.unexpected-type`), or `-`. With
`--string-warnings` a fourth column, `strings`, lists the string-check warnings in the oracle's
order as `;`-separated `policy:kind` entries, or `-`.

The CLI's JSON output names no kinds, so each message is classified by its wording below; an
unrecognized message stops the script. Permissive cases need a CLI built with the
`permissive-validate` feature.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# The string checks quote the policy's text, which may contain any of the wordings below, so they
# are matched first and against the whole message.
STRING_CHECK_PATTERNS = [
    (r"^for policy `[^`]*`, string `.*` contains mixed scripts$", "validation.mixed-script-string"),
    (r"^for policy `[^`]*`, string `.*` contains BIDI control characters$",
     "validation.bidirectional-characters-in-string"),
    (r"^for policy `[^`]*`, identifier `.*` contains BIDI control characters$",
     "validation.bidirectional-characters-in-identifier"),
    (r"^for policy `[^`]*`, identifier `.*` contains the character `.*` which is not a printable ASCII character "
     r"and falls outside of the General Security Profile for Identifiers$", "validation.confusable-identifier"),
    (r"^for policy `[^`]*`, identifier `.*` contains mixed scripts$", "validation.mixed-script-identifier"),
]

KIND_PATTERNS = [
    (r"unable to guarantee safety of access to optional attribute", "validation.unsafe-optional-attribute-access"),
    (r"unable to guarantee safety of access to tag", "validation.unsafe-tag-access"),
    (r"`\.getTag\(\)` is not allowed", "validation.no-tags-allowed"),
    (r"^for policy `[^`]*`, attribute .* not found", "validation.unsafe-attribute-access"),
    (r"unexpected type: expected", "validation.unexpected-type"),
    (r"^the types .* are not compatible", "validation.incompatible-types"),
    (r"empty set literals are forbidden", "validation.empty-set-forbidden"),
    (r"extension constructors may not be called with non-literal", "validation.non-literal-extension-constructor"),
    (r"error during extension function argument validation", "validation.function-argument-validation"),
    (r"wrong number of arguments in extension function", "validation.wrong-number-arguments"),
    (r"undefined extension function", "validation.undefined-function"),
    (r"unrecognized entity type", "validation.unrecognized-entity-type"),
    (r"unrecognized action", "validation.unrecognized-action-id"),
    (r"is of an enumerated entity type", "validation.invalid-enum-entity"),
    (r"policy is impossible", "validation.impossible-policy"),
    (r"unable to find an applicable action", "validation.invalid-action-application"),
    (r"internal invariant violated", "validation.internal-invariant-violation"),
    (r"schema is incomplete", "validation.incomplete-schema"),
    (r"this policy requires level \d+, which exceeds the maximum allowed level",
     "validation.entity-dereference-level-violation"),
    (r"entity literals cannot be dereferenced at any level", "validation.entity-dereference-level-violation"),
]


def classify_string_check(message):
    for pattern, kind in STRING_CHECK_PATTERNS:
        if re.search(pattern, message, re.DOTALL):
            return kind
    return None


def classify(message):
    string_check = classify_string_check(message)
    if string_check:
        return string_check
    for pattern, kind in KIND_PATTERNS:
        if re.search(pattern, message):
            return kind
    raise SystemExit(f"unrecognized oracle message: {message}")


def messages_of(output):
    report = json.loads(output)
    severity = report.get("severity")
    if severity == "advice":
        return []
    if report.get("message") not in ("policy set validation failed", "policy set validation passed"):
        raise SystemExit(f"oracle did not validate: {output.strip()}")
    messages = list(report.get("causes", []))
    messages += [related["message"] for related in report.get("related", [])]
    return messages


def string_warnings_of(messages):
    """The string-check warnings in the oracle's order, as `policy:kind` entries joined by `;`."""
    entries = []
    for message in messages:
        kind = classify_string_check(message)
        if kind:
            policy = re.match(r"^for policy `([^`]*)`", message).group(1)
            entries.append(f"{policy}:{kind}")
    return ";".join(entries) or "-"


def parse_links(text):
    links = []
    for entry in text.split(";"):
        template, link, slots = entry.split(":", 2)
        arguments = dict(slot.split("=", 1) for slot in slots.split("+"))
        links.append({"template_id": template, "link_id": link, "args": arguments})
    return links


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--oracle", required=True, help="the reference `cedar` CLI")
    parser.add_argument("--cases", required=True, help="the cases table")
    parser.add_argument("--work-directory", required=True, help="where to write the policy files")
    parser.add_argument("--output", required=True, help="the expectations table to write")
    parser.add_argument("--string-warnings", action="store_true",
                        help="add a `strings` column: the string-check warnings in the oracle's order")
    arguments = parser.parse_args()

    cases_directory = os.path.dirname(os.path.abspath(arguments.cases))
    os.makedirs(arguments.work_directory, exist_ok=True)
    rows = ["case\tverdict\tkinds" + ("\tstrings" if arguments.string_warnings else "")]
    with open(arguments.cases) as cases:
        lines = cases.read().splitlines()
    columns = lines[0].split("\t")
    for line in lines[1:]:
        if not line.strip():
            continue
        fields = dict(zip(columns, line.split("\t")))
        name, schema, mode, policy = fields["case"], fields["schema"], fields["mode"], fields["policy"]
        policy_path = os.path.join(arguments.work_directory, name + ".cedar")
        with open(policy_path, "w") as policy_file:
            policy_file.write(policy + "\n")
        command = [arguments.oracle, "validate", "--schema", os.path.join(cases_directory, schema), "--policies",
                   policy_path, "--validation-mode", mode, "--error-format", "json"]
        links = fields.get("links", "-")
        if links != "-":
            links_path = os.path.join(arguments.work_directory, name + ".links.json")
            with open(links_path, "w") as links_file:
                json.dump(parse_links(links), links_file)
            command += ["--template-linked", links_path]
        level = fields.get("level", "-")
        if level != "-":
            command += ["--level", level]
        completed = subprocess.run(command, capture_output=True, text=True)
        if completed.returncode not in (0, 3):
            raise SystemExit(f"{name}: oracle exited {completed.returncode}: {completed.stdout}{completed.stderr}")
        messages = messages_of(completed.stdout)
        kinds = {classify(message) for message in messages}
        verdict = "pass" if completed.returncode == 0 else "fail"
        row = [name, verdict, ",".join(sorted(kinds)) or "-"]
        if arguments.string_warnings:
            row.append(string_warnings_of(messages))
        rows.append("\t".join(row))
    with open(arguments.output, "w") as output:
        output.write("\n".join(rows) + "\n")
    print(f"wrote {len(rows) - 1} expectations to {arguments.output}")


if __name__ == "__main__":
    sys.exit(main())
