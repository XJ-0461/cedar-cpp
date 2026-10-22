#!/usr/bin/env python3
"""Builds a corpus of JSON schema fragments for sweeping the JSON schema reader against the oracle.

Writes three directories under --output-directory:
  reference/  every `*.json` file under the --input-directory trees that is a JSON schema (its path
              contains `schema` or it has an `entityTypes` key), copied with its path flattened;
  converted/  every distinct `*.cedarschema` under those trees, translated to JSON by the oracle
              (`translate-schema --direction cedar-to-json`), named by a hash of its text;
  mutants/    --mutants files, each a converted schema with one random structural change (a field
              removed, nulled, added, repeated, renamed, retyped, or a list emptied), from --seed.
Then `scripts/generate-schema-json-parity-table.sh` over the output directory writes the table that
`SchemaJson.MatchesTheOracleOnTheSweepTable` reads (CEDAR_SCHEMA_JSON_SWEEP_TABLE).
"""

import argparse
import hashlib
import json
import os
import random
import subprocess
import sys


def is_json_schema(path):
    if "schema" in path:
        return True
    try:
        with open(path, encoding="utf-8") as handle:
            return '"entityTypes"' in handle.read()
    except (OSError, UnicodeDecodeError):
        return False


def nodes(value, found):
    found.append(value)
    if isinstance(value, dict):
        for child in value.values():
            nodes(child, found)
    elif isinstance(value, list):
        for child in value:
            nodes(child, found)
    return found


def dump(value, repeated):
    """JSON text of `value`, writing the first member of the object `repeated` twice."""
    if isinstance(value, dict):
        members = [json.dumps(key) + ": " + dump(child, repeated) for key, child in value.items()]
        if value is repeated and members:
            members.append(members[0])
        return "{" + ", ".join(members) + "}"
    if isinstance(value, list):
        return "[" + ", ".join(dump(child, repeated) for child in value) + "]"
    return json.dumps(value)


def mutate(document, generator):
    """Applies one random change to `document`; returns the text, or None when nothing applies."""
    everything = nodes(document, [])
    objects = [node for node in everything if isinstance(node, dict) and node]
    arrays = [node for node in everything if isinstance(node, list) and node]
    if not objects:
        return None
    target = generator.choice(objects)
    key = generator.choice(list(target.keys()))
    kind = generator.randrange(8)
    repeated = None
    if kind == 0:
        del target[key]
    elif kind == 1:
        target[key] = None
    elif kind == 2:
        target["zz"] = {}
    elif kind == 3:
        repeated = target
    elif kind == 4:
        places = [(node, k) for node in objects for k, v in node.items() if isinstance(v, str)]
        places += [(node, i) for node in arrays for i, v in enumerate(node) if isinstance(v, str)]
        if not places:
            return None
        node, place = generator.choice(places)
        node[place] = generator.choice(["", "A::", "__cedar", "if", "Bool", "Long", "__cedar::String", "Entity"])
    elif kind == 5:
        target[generator.choice(["", "__cedar", "A::B", "Long", "Action", "if", "type"])] = target.pop(key)
    elif kind == 6:
        target[key] = generator.choice([1, True, [], "x", {}])
    else:
        if not arrays:
            return None
        generator.choice(arrays).clear()
    return dump(document, repeated)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--oracle", required=True, help="the reference cedar binary")
    parser.add_argument("--input-directory", required=True, action="append", help="a tree to search; repeatable")
    parser.add_argument("--output-directory", required=True)
    parser.add_argument("--mutants", type=int, required=True)
    parser.add_argument("--seed", type=int, required=True)
    arguments = parser.parse_args()

    reference = os.path.join(arguments.output_directory, "reference")
    converted = os.path.join(arguments.output_directory, "converted")
    mutants = os.path.join(arguments.output_directory, "mutants")
    for directory in (reference, converted, mutants):
        os.makedirs(directory, exist_ok=True)

    seen = set()
    counts = {"reference": 0, "converted": 0, "unconvertible": 0, "mutants": 0}
    for tree in arguments.input_directory:
        for root, _, files in os.walk(tree):
            for name in sorted(files):
                path = os.path.join(root, name)
                if name.endswith(".json") and is_json_schema(path):
                    flat = os.path.relpath(path, tree).replace(os.sep, "__")
                    with open(path, "rb") as source, open(os.path.join(reference, flat), "wb") as target:
                        target.write(source.read())
                    counts["reference"] += 1
                elif name.endswith(".cedarschema"):
                    with open(path, "rb") as source:
                        digest = hashlib.sha256(source.read()).hexdigest()[:16]
                    if digest in seen:
                        continue
                    seen.add(digest)
                    result = subprocess.run([arguments.oracle, "translate-schema", "--direction", "cedar-to-json",
                                             "--schema", path], capture_output=True, check=False)
                    if result.returncode != 0:
                        counts["unconvertible"] += 1
                        continue
                    with open(os.path.join(converted, digest + ".json"), "wb") as target:
                        target.write(result.stdout)
                    counts["converted"] += 1

    generator = random.Random(arguments.seed)
    sources = sorted(os.listdir(converted))
    for index in range(arguments.mutants if sources else 0):
        name = generator.choice(sources)
        with open(os.path.join(converted, name), encoding="utf-8") as source:
            text = mutate(json.load(source), generator)
        if text is None:
            continue
        with open(os.path.join(mutants, f"mutant-{index:05d}-{name}"), "w", encoding="utf-8") as target:
            target.write(text)
        counts["mutants"] += 1
    print(", ".join(f"{key} {value}" for key, value in counts.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
