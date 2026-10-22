#!/usr/bin/env python3
"""Prints the diagnostics of one `validate --error-format json` report, one per line.

Each line is `severity<TAB>policy<TAB>kind`, sorted and without repeats: severity is `error` or
`warning`, policy the id the diagnostic is about (`-` when it names none), kind cedar-cpp's
CodeName (`validation.unexpected-type`). A report of a passed validation with nothing to say
prints nothing.

`--format reference` reads the Rust CLI's miette report: the first diagnostic is the report's
first cause, with the report's severity, the others are its `related` entries. That JSON names no
kinds, so each message is classified by its wording with the patterns of
`generate-validation-parity-table.py` (beside this script); an unrecognized message exits 1. The
policy id is the `for policy `id`` in the message, else in the help text. `--format cedar-cpp`
reads cedar-cpp-cli's report, whose diagnostics carry their policy, severity and code.
"""

import argparse
import importlib.util
import json
import os
import re
import sys

POLICY_PATTERN = re.compile(r"for policy `([^`]*)`")


def load_classifier():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "generate-validation-parity-table.py")
    specification = importlib.util.spec_from_file_location("generate_validation_parity_table", path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module.classify


def policy_of(entry):
    for text in (entry.get("message", ""), entry.get("help", "")):
        found = POLICY_PATTERN.search(text or "")
        if found:
            return found.group(1)
    return "-"


def reference_lines(report):
    classify = load_classifier()
    severity = report.get("severity")
    if severity == "advice":
        return set()
    causes = report.get("causes", [])
    if not causes:
        raise SystemExit(f"report has no causes: {json.dumps(report)}")
    first = {"message": causes[0], "help": report.get("help", ""), "severity": severity}
    entries = [first] + list(report.get("related", []))
    return {(entry["severity"], policy_of(entry), classify(entry["message"])) for entry in entries}


def cedar_cpp_lines(report):
    return {(entry["severity"], entry["policy"], entry["code"]) for entry in report.get("diagnostics", [])}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--format", required=True, choices=["reference", "cedar-cpp"], help="whose report it is")
    parser.add_argument("--report", required=True, help="the file holding the report (standard output of validate)")
    arguments = parser.parse_args()
    with open(arguments.report) as report_file:
        text = report_file.read()
    try:
        report = json.loads(text)
    except json.JSONDecodeError:
        raise SystemExit(f"not a JSON report: {text.strip()}")
    if report.get("message") not in ("policy set validation failed", "policy set validation passed"):
        raise SystemExit(f"not a validation report: {text.strip()}")
    lines = reference_lines(report) if arguments.format == "reference" else cedar_cpp_lines(report)
    for line in sorted(lines):
        print("\t".join(line))


if __name__ == "__main__":
    sys.exit(main())
