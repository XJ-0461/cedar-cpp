#!/usr/bin/env python3
"""Writes the partial-evaluation sweep: manifests for cedar-partial-evaluation-oracle and
cedar-cpp-partial-evaluation-runner, derived from the integration corpus.

    python3 -I generate-partial-evaluation-sweep.py --integration-tests-directory <dir>
        --output-directory <dir>

The corpus is walked as tests/parity/CorpusRunner.cpp walks it: every `*.json` under `tests/` that
reads as a test, and every `*.json` under `corpus-tests/` but the `*.entities.json` files. A test
whose policies are in the JSON format is left out, because the oracle reads Cedar policy syntax only.

For every request of every test with `shouldValidate`, each variant writes one case to
`<Variant>.jsonl` in the output directory:

    PrincipalUnknown    partial-authorize, the principal id unknown
    ResourceUnknown     partial-authorize, the resource id unknown
    ContextUnknown      partial-authorize, the context unknown
    AllUnknown          partial-authorize, all three unknown
    AttributesUnknown   partial-authorize, every entity's attributes unknown
    AncestorsUnknown    partial-authorize, every entity's ancestors unknown
    TagsUnknown         partial-authorize, every entity's tags unknown
    NoEntities          partial-authorize, no entities
    QueryPrincipal      query-principal over the concrete entities
    QueryResource       query-resource over the concrete entities
    QueryAction         query-action, the principal, resource and context known

A test without `shouldValidate` writes its AllUnknown cases only. Every partial-authorize case
reauthorizes with the original request and entities as its one completion. The partial-authorize
variants without unknown entity data read the test's entities with attributes, ancestors and tags all
known.

Partial entities documents go under `partial-entities/`, each distinct one written once per test
file. They leave out the action entities, which the partial entities reader refuses and takes from
the schema instead. Schemas, policies and concrete entities are named by absolute path into the
corpus.
"""

import argparse
import json
import os
import sys

VARIANTS = (
    "PrincipalUnknown",
    "ResourceUnknown",
    "ContextUnknown",
    "AllUnknown",
    "AttributesUnknown",
    "AncestorsUnknown",
    "TagsUnknown",
    "NoEntities",
    "QueryPrincipal",
    "QueryResource",
    "QueryAction",
)

TEST_KEYS = {"policies", "policyFormat", "entities", "schema", "schemaFormat", "shouldValidate", "requests"}


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--integration-tests-directory", required=True)
    parser.add_argument("--output-directory", required=True)
    return parser.parse_args()


def sorted_files(directory):
    files = []
    for root, _, names in os.walk(directory):
        files.extend(os.path.join(root, name) for name in names)
    return sorted(files, key=os.fsencode)


def read_test(path):
    """The test descriptor at `path`, or None when the file does not read as one."""
    try:
        with open(path, encoding="utf-8") as stream:
            test = json.load(stream)
    except (OSError, ValueError):
        return None
    if not isinstance(test, dict) or not set(test) <= TEST_KEYS:
        return None
    if not all(key in test for key in ("policies", "entities", "schema", "shouldValidate", "requests")):
        return None
    if not isinstance(test["requests"], list):
        return None
    return test


def scan_tests(root):
    tests = []
    for path in sorted_files(os.path.join(root, "tests")):
        if not path.lower().endswith(".json"):
            continue
        test = read_test(path)
        if test is not None:
            tests.append((os.path.relpath(path, root), test))
    for path in sorted_files(os.path.join(root, "corpus-tests")):
        name = os.path.basename(path)
        if not name.endswith(".json") or name.endswith(".entities.json"):
            continue
        test = read_test(path)
        if test is not None:
            tests.append((os.path.relpath(path, root), test))
    return tests


def is_action_type(type_name):
    return isinstance(type_name, str) and type_name.split("::")[-1] == "Action"


def partial_entities_documents(entities):
    """The known, attributes-unknown, ancestors-unknown and tags-unknown partial entities documents,
    or None when the concrete entities are not a JSON array of entity objects."""
    if not isinstance(entities, list):
        return None
    known = []
    for entity in entities:
        if not isinstance(entity, dict) or not isinstance(entity.get("uid"), dict):
            return None
        if is_action_type(entity["uid"].get("type")):
            continue
        complete = dict(entity)
        complete.setdefault("attrs", {})
        complete.setdefault("parents", [])
        complete.setdefault("tags", {})
        known.append(complete)

    def without(key):
        return [{name: value for name, value in entity.items() if name != key} for entity in known]

    return {
        "known": known,
        "attributes-unknown": without("attrs"),
        "ancestors-unknown": without("parents"),
        "tags-unknown": without("tags"),
    }


def write_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")


def manifest_line(case):
    return json.dumps(case, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n"


def uid_without_id(uid):
    if not isinstance(uid, dict):
        return uid
    return {key: value for key, value in uid.items() if key != "id"}


def main():
    arguments = parse_arguments()
    root = os.path.abspath(arguments.integration_tests_directory)
    output = os.path.abspath(arguments.output_directory)
    os.makedirs(output, exist_ok=True)

    empty_entities = "partial-entities/empty.json"
    write_json(os.path.join(output, empty_entities), [])

    manifests = {variant: open(os.path.join(output, f"{variant}.jsonl"), "w", encoding="utf-8")
                 for variant in VARIANTS}
    counts = dict.fromkeys(VARIANTS, 0)
    skipped = 0
    for relative, test in scan_tests(root):
        if test.get("policyFormat", "cedar") != "cedar":
            skipped += 1
            continue
        schema = os.path.join(root, test["schema"])
        if test.get("schemaFormat", "cedar") == "json" and not schema.endswith(".json"):
            skipped += 1
            continue
        policies = os.path.join(root, test["policies"])
        concrete_entities = os.path.join(root, test["entities"])
        validating = test["shouldValidate"] is True

        stem = os.path.splitext(relative)[0]
        try:
            with open(concrete_entities, encoding="utf-8") as stream:
                documents = partial_entities_documents(json.load(stream))
        except (OSError, ValueError):
            documents = None
        written = {}

        def partial_entities(kind):
            if documents is None:
                return concrete_entities
            if kind not in written:
                path = f"partial-entities/{stem}.{kind}.json"
                write_json(os.path.join(output, path), documents[kind])
                written[kind] = path
            return written[kind]

        for index, request in enumerate(test["requests"]):
            if not isinstance(request, dict):
                continue
            name = f"{relative}#{index}"
            principal = request.get("principal")
            action = request.get("action")
            resource = request.get("resource")
            concrete_request = {"principal": principal, "action": action, "resource": resource}
            if "context" in request:
                concrete_request["context"] = request["context"]
            completions = [{"request": concrete_request, "entities": concrete_entities}]

            def emit(variant, operation, case_request, entities, with_completions=True):
                case = {
                    "name": name,
                    "operation": operation,
                    "schema": schema,
                    "policies": policies,
                    "entities": entities,
                    "request": case_request,
                }
                if with_completions:
                    case["completions"] = completions
                manifests[variant].write(manifest_line(case))
                counts[variant] += 1

            def partial_request(principal_known, resource_known, context_known):
                result = {
                    "principal": principal if principal_known else uid_without_id(principal),
                    "action": action,
                    "resource": resource if resource_known else uid_without_id(resource),
                }
                if context_known and "context" in request:
                    result["context"] = request["context"]
                return result

            emit("AllUnknown", "partial-authorize", partial_request(False, False, False), partial_entities("known"))
            if not validating:
                continue
            known = partial_entities("known")
            emit("PrincipalUnknown", "partial-authorize", partial_request(False, True, True), known)
            emit("ResourceUnknown", "partial-authorize", partial_request(True, False, True), known)
            emit("ContextUnknown", "partial-authorize", partial_request(True, True, False), known)
            emit("AttributesUnknown", "partial-authorize", partial_request(True, True, True),
                 partial_entities("attributes-unknown"))
            emit("AncestorsUnknown", "partial-authorize", partial_request(True, True, True),
                 partial_entities("ancestors-unknown"))
            emit("TagsUnknown", "partial-authorize", partial_request(True, True, True),
                 partial_entities("tags-unknown"))
            emit("NoEntities", "partial-authorize", partial_request(True, True, True), empty_entities)
            query = {"principal": uid_without_id(principal), "action": action, "resource": resource}
            if "context" in request:
                query["context"] = request["context"]
            emit("QueryPrincipal", "query-principal", query, concrete_entities, with_completions=False)
            query = {"principal": principal, "action": action, "resource": uid_without_id(resource)}
            if "context" in request:
                query["context"] = request["context"]
            emit("QueryResource", "query-resource", query, concrete_entities, with_completions=False)
            query = {"principal": principal, "resource": resource}
            if "context" in request:
                query["context"] = request["context"]
            emit("QueryAction", "query-action", query, known, with_completions=False)

    for stream in manifests.values():
        stream.close()
    summary = ", ".join(f"{variant} {counts[variant]}" for variant in VARIANTS)
    print(f"partial-evaluation sweep: {summary}; {skipped} tests left out", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
