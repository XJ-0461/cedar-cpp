#!/usr/bin/env bash
# Runs cedar-cpp-partial-evaluation-runner on a partial-evaluation manifest and compares its output
# line by line, byte for byte, with the oracle's: either the live cedar-partial-evaluation-oracle
# run on the same manifest, or a checked-in expectation file the oracle wrote. Names each case that
# differs. Exits 0 when every line agrees, 1 when any differs or a binary fails, 2 on a malformed
# command line.
#
#   PartialEvaluationDiff.sh --runner <runner> --oracle <oracle> --manifest <cases.jsonl>
#   PartialEvaluationDiff.sh --runner <runner> --expected <expected.jsonl> --manifest <cases.jsonl>
#
# A runner that stops early (a crash, or an abort) is a failure, and every case it did not answer
# is named as missing.

set -u

usage() {
    echo "usage: PartialEvaluationDiff.sh --runner <runner> (--oracle <oracle> | --expected <expected.jsonl>)" \
        "--manifest <cases.jsonl>" >&2
    exit 2
}

runner=""
oracle=""
expected=""
manifest=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --runner) [[ $# -ge 2 ]] || usage; runner="$2"; shift 2 ;;
        --oracle) [[ $# -ge 2 ]] || usage; oracle="$2"; shift 2 ;;
        --expected) [[ $# -ge 2 ]] || usage; expected="$2"; shift 2 ;;
        --manifest) [[ $# -ge 2 ]] || usage; manifest="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[[ -n "$runner" && -n "$manifest" ]] || usage
[[ -n "$oracle" && -z "$expected" || -z "$oracle" && -n "$expected" ]] || usage
[[ -f "$manifest" ]] || { echo "no manifest at $manifest" >&2; exit 2; }

scratch="$(mktemp -d "${TMPDIR:-/var/tmp}/partial-evaluation-diff.XXXXXX")" || exit 2
trap 'rm -rf -- "$scratch"' EXIT

failed=0

# Runs one binary on the manifest into $scratch/<label>.out; reports a non-zero exit.
run() {
    local label="$1" binary="$2"
    "$binary" "$manifest" >"$scratch/$label.out" 2>"$scratch/$label.err"
    local status=$?
    if [[ $status -ne 0 ]]; then
        echo "$label exited with status $status after $(wc -l <"$scratch/$label.out") lines" >&2
        sed -n '1,20p' "$scratch/$label.err" >&2
        failed=1
    elif [[ -s "$scratch/$label.err" ]]; then
        echo "$label wrote to standard error:" >&2
        sed -n '1,20p' "$scratch/$label.err" >&2
        failed=1
    fi
}

run runner "$runner"
if [[ -n "$oracle" ]]; then
    run oracle "$oracle"
    reference="$scratch/oracle.out"
    referenceLabel="oracle"
else
    [[ -f "$expected" ]] || { echo "no expectation file at $expected" >&2; exit 2; }
    reference="$expected"
    referenceLabel="expected"
fi

# The case name of an output line: its "name" field, which canonical key order puts first after an
# optional "error".
namePattern='^\{("error":"[^"]*",)?"name":("(([^"\\]|\\.)*)"|null)'
caseName() {
    if [[ "$1" =~ $namePattern ]]; then
        if [[ "${BASH_REMATCH[2]}" == "null" ]]; then echo "<no name>"; else echo "${BASH_REMATCH[3]}"; fi
    else
        echo "<unreadable line>"
    fi
}

mapfile -t runnerLines <"$scratch/runner.out"
mapfile -t referenceLines <"$reference"
cases=${#referenceLines[@]}
[[ ${#runnerLines[@]} -gt $cases ]] && cases=${#runnerLines[@]}

differences=0
for ((index = 0; index < cases; ++index)); do
    ours="${runnerLines[index]-}"
    theirs="${referenceLines[index]-}"
    [[ "$ours" == "$theirs" ]] && continue
    differences=$((differences + 1))
    line=$((index + 1))
    if [[ $index -ge ${#runnerLines[@]} ]]; then
        echo "line $line, case \"$(caseName "$theirs")\": missing from the runner's output"
    elif [[ $index -ge ${#referenceLines[@]} ]]; then
        echo "line $line, case \"$(caseName "$ours")\": missing from the $referenceLabel output"
    else
        echo "line $line, case \"$(caseName "$theirs")\" differs"
        echo "  runner:   $ours"
        echo "  $referenceLabel: $theirs"
    fi
done

echo "$differences of $cases cases differ ($(basename "$(dirname "$manifest")")/$(basename "$manifest"))"
[[ $differences -eq 0 && $failed -eq 0 ]] || exit 1
exit 0
