#!/usr/bin/env bash
# Runs the reference Rust `cedar` binary and cedar-cpp-cli on identical argument lists, one run per
# input file, and reports where they disagree. Exits 0 when every input agrees, 1 when any
# disagrees (or no input was found), 2 on a malformed command line.
#
#   OracleDiff.sh --reference-cedar <cedar> --cedar-cpp-cli <cedar-cpp-cli>
#                 --input-directory <dir> [--input-directory <dir> ...] --command <mode>
#
# Modes:
#   check-parse  every `*.cedar` file under each directory, run as `check-parse --policies <file>`;
#                the two agree when their exit statuses are equal.
#   authorize    every directory (at any depth) holding `entities.json` or `entity.json`. Each policy
#                file in it (`*.cedar`; `*.cedar.json` with `--policy-format json`), alone and with
#                each `linked*` template-linked file, is run as `authorize --verbose` on each
#                `request*.json` (`--request-json`) and on every principal, action and resource
#                drawn from the entities file and the policy text (an action is a uid of type
#                `Action` in any namespace; principals and resources are all other uids), without a
#                context and, when the policy file mentions `context`, with each `context*.json`.
#                The two agree when their exit statuses are equal and, unless both failed, they
#                print the same decision, the same set of determining policy ids and the same set
#                of erroring policy ids.
#   authorize-schema
#                as authorize, for the directories that hold a schema: each input once with each
#                `*.cedarschema` file as `--schema` (and again with `--request-validation false`)
#                and once with each `*.cedarschema.json` file (`--schema-format json`).
#   run-tests    every `test*.json` file under each directory, run as `run-tests --tests <file>`
#                with each `*.cedar` file beside it as `--policies`, without a schema and with each
#                schema file beside it (`*.cedarschema`; `*.cedarschema.json` with
#                `--schema-format json`); the two agree when their exit statuses are equal and they
#                give each test the same verdict (`ok`, `error`, or `fail` with the same decision,
#                missing reasons and error counts).
#   translate-schema
#                every `*.cedarschema` file under each directory (duplicates by content skipped), run
#                as `translate-schema --direction cedar-to-json` and `... cedar-to-json-with-resolved-types`,
#                and every `*schema*.json` file, run as `--direction json-to-cedar`; each with
#                `--schema <file>`. The two agree when their exit statuses are equal, their standard
#                output is byte-identical, and both or neither wrote a warning to standard error.
#   translate-schema-json
#                as translate-schema, but every `*.json` file is a JSON schema.
#   translate-policy
#                every `*.cedar` file under each directory (duplicates by content skipped), run as
#                `translate-policy --direction cedar-to-json --policies <file>`, and every
#                `*.cedar.json` file, run as `--direction json-to-cedar`. The two agree when their
#                exit statuses are equal and, on success, Cedar output is byte-identical; JSON output
#                agrees in meaning: both name the same policies, and each JSON output, read back by
#                both binaries with `json-to-cedar`, prints the same Cedar text.
#   link         every directory whose README.md has `cargo run link` commands (the CLI's sample
#                sandboxes), the commands run in order; and every directory (at depth one) holding
#                `step1.args`, whose `step<N>.args` files (one argument per line, after `link`) run
#                in order. Each binary runs in its own copy of the directory's files, so a
#                `--template-linked` file is created or updated there. The two agree when each
#                step's exit status is equal, each step that succeeded printed the new policy, and
#                the directories end with the same files, a JSON file (the template-linked file)
#                compared by meaning (`jq -S`: key order aside, the same entries in the same
#                order) and any other byte for byte.
#   validate     every directory holding a schema (`*.cedarschema`; `*.cedarschema.json` with
#                `--schema-format json`): each policy file beside it (`*.cedar`; `*.cedar.json` with
#                `--policy-format json`), alone and with each `linked*` template-linked file, run as
#                `validate --schema <schema> --policies <file> --error-format json`; a schema with
#                no policy file beside it is run with `permit(principal, action, resource);` and with
#                policies generated from its entity types, attributes, tags, enums, actions and
#                contexts. And every row of each `*Cases.tsv` validation parity table (its strict
#                rows) with the row's schema, policy, links, mode and level (`--level`). The two agree when their exit
#                statuses are equal and, when both validated (exit 0 or 3), they report the same set
#                of (severity, policy id, kind); the reference CLI's messages are classified by
#                `scripts/classify-validation-report.py`.
#   validate-permissive
#                as validate with `--validation-mode permissive` (table rows keep their own mode,
#                permissive rows included), against a reference CLI built with the
#                `permissive-validate` feature.
#   validate-level
#                the directory inputs of validate (no tables), each run at `--level` 0 through 4.
#   tpe          against a reference CLI built with the `tpe` feature: every `*.args` file under each
#                directory (one argument per line after `tpe`, run in the file's directory); and
#                every directory holding `entities.json` and a schema (`*.cedarschema`;
#                `*.cedarschema.json` with `--schema-format json`): each policy file beside it run
#                with each `request*.json` (`--request-json`), and with every request environment
#                of the schema (`--principal-type`, `--action`, `--resource-type`), each id unknown,
#                one of the entities file's ids of that type or an id of no entity
#                (`--principal-eid`, `--resource-eid`), and the context unknown or each
#                `context*.json` (`--context`). The two agree when their exit statuses are equal
#                and, on 0, 2 or 4, standard output is byte-identical (the timing line's number
#                aside), except that the residual policies printed after an unknown decision,
#                which Rust prints in HashMap order and on one line each, are compared as a
#                sorted list of policies read back by the reference CLI into its JSON policy
#                format; on 1, both printed the leading blank line.
#
# A mode is three functions named after it (`-` spelled `_`): `<mode>_inputs <dir>` lists the
# inputs, NUL-separated (a file, or for authorize the argument list itself, tab-separated);
# `<mode>_arguments <input>` sets the `arguments` array both binaries run with;
# `<mode>_compare` compares the two runs from `reference_status`/`reference_output` and
# `candidate_status`/`candidate_output`. On agreement it sets `outcome` (the summary counts each
# outcome) and returns 0; otherwise it sets `reason` and returns 1. A mode that runs the binaries
# itself defines `<mode>_run <input>` too, which the driver calls instead of running them once.
set -uo pipefail

usage() {
  echo "usage: $0 --reference-cedar <path> --cedar-cpp-cli <path> --input-directory <dir> [--input-directory <dir> ...] --command check-parse|authorize|authorize-schema|run-tests|translate-schema|translate-schema-json|translate-policy|link|validate|validate-permissive|validate-level|tpe" >&2
  exit 2
}

reference=""
candidate=""
mode=""
directories=()
while [ $# -gt 0 ]; do
  case "$1" in
    --reference-cedar) [ $# -ge 2 ] || usage; reference="$2"; shift 2 ;;
    --cedar-cpp-cli) [ $# -ge 2 ] || usage; candidate="$2"; shift 2 ;;
    --input-directory) [ $# -ge 2 ] || usage; directories+=("$2"); shift 2 ;;
    --command) [ $# -ge 2 ] || usage; mode="$2"; shift 2 ;;
    *) usage ;;
  esac
done
[ -n "$reference" ] && [ -n "$candidate" ] && [ -n "$mode" ] && [ ${#directories[@]} -gt 0 ] || usage
[ -x "$reference" ] || { echo "not executable: $reference" >&2; exit 2; }
[ -x "$candidate" ] || { echo "not executable: $candidate" >&2; exit 2; }
for directory in "${directories[@]}"; do
  [ -d "$directory" ] || { echo "not a directory: $directory" >&2; exit 2; }
done

if { [ "$mode" = authorize ] || [ "$mode" = authorize-schema ] || [ "$mode" = translate-policy ] || [ "$mode" = link ] || [ "$mode" = validate ] || [ "$mode" = validate-permissive ] || [ "$mode" = validate-level ] || [ "$mode" = tpe ]; } && ! command -v jq >/dev/null; then
  echo "the $mode mode reads JSON files with jq, which is not on PATH" >&2
  exit 2
fi

# ---- check-parse ----

check_parse_inputs() {
  find "$1" -type f -name '*.cedar' -print0 | sort -z
}

check_parse_arguments() {
  arguments=(check-parse --policies "$1")
}

check_parse_compare() {
  if [ "$reference_status" -eq "$candidate_status" ]; then
    if [ "$reference_status" -eq 0 ]; then outcome="accepted"; else outcome="rejected"; fi
    return 0
  fi
  reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
  return 1
}

# ---- authorize ----

# The uids written in `$1` (an entities file) and in the Cedar policy files `$2...`, one per line,
# as Cedar text.
authorize_uids() {
  local entities="$1"
  shift
  {
    jq -r '.[] | .uid | (.__entity // .) | "\(.type)::\(.id | tojson)"' "$entities" 2>/dev/null
    [ $# -gt 0 ] && grep -oh '[A-Za-z_][A-Za-z0-9_]*\(::[A-Za-z_][A-Za-z0-9_]*\)*::"[^"]*"' "$@"
  } | LC_ALL=C sort -u
}

# Prints one NUL-terminated input, the arguments `$@` joined by tabs.
authorize_emit() {
  local IFS=$'\t'
  printf '%s\0' "$*"
}

# The authorize inputs of one directory; arguments after the entities file are added to each.
authorize_directory_inputs() {
  local directory="$1" entities="$2"
  shift 2
  local -a extra=("$@")
  local -a policy_files cedar_files link_options request_files context_files uids actions others contexts
  mapfile -t policy_files < <(find "$directory" -maxdepth 1 -type f \( -name '*.cedar' -o -name '*.cedar.json' \) | LC_ALL=C sort)
  mapfile -t cedar_files < <(find "$directory" -maxdepth 1 -type f -name '*.cedar' | LC_ALL=C sort)
  mapfile -t request_files < <(find "$directory" -maxdepth 1 -type f -name 'request*.json' | LC_ALL=C sort)
  mapfile -t context_files < <(find "$directory" -maxdepth 1 -type f -name 'context*.json' | LC_ALL=C sort)
  link_options=("")
  while IFS= read -r file; do link_options+=("$file"); done < <(find "$directory" -maxdepth 1 -type f -name 'linked*' | LC_ALL=C sort)
  mapfile -t uids < <(authorize_uids "$entities" "${cedar_files[@]}")
  actions=()
  others=()
  local uid
  for uid in "${uids[@]}"; do
    if [[ "$uid" =~ ^([A-Za-z_][A-Za-z0-9_]*::)*Action:: ]]; then actions+=("$uid"); else others+=("$uid"); fi
  done

  local policies links request principal action resource context
  local -a policy_arguments
  for policies in "${policy_files[@]}"; do
    policy_arguments=(--policies "$policies")
    [[ "$policies" == *.cedar.json ]] && policy_arguments+=(--policy-format json)
    contexts=("")
    grep -q context "$policies" && contexts+=("${context_files[@]}")
    for links in "${link_options[@]}"; do
      local -a link_arguments=()
      [ -n "$links" ] && link_arguments=(--template-linked "$links")
      for request in "${request_files[@]}"; do
        authorize_emit authorize --verbose "${policy_arguments[@]}" "${link_arguments[@]}" --entities "$entities" \
          --request-json "$request" "${extra[@]}"
      done
      for principal in "${others[@]}"; do
        for action in "${actions[@]}"; do
          for resource in "${others[@]}"; do
            for context in "${contexts[@]}"; do
              local -a context_arguments=()
              [ -n "$context" ] && context_arguments=(--context "$context")
              authorize_emit authorize --verbose "${policy_arguments[@]}" "${link_arguments[@]}" \
                --entities "$entities" --principal "$principal" --action "$action" --resource "$resource" \
                "${context_arguments[@]}" "${extra[@]}"
            done
          done
        done
      done
    done
  done
}

authorize_inputs() {
  local directory name entities
  while IFS= read -r -d '' directory; do
    entities=""
    for name in entities.json entity.json; do
      if [ -f "$directory/$name" ]; then entities="$directory/$name"; break; fi
    done
    [ -n "$entities" ] && authorize_directory_inputs "$directory" "$entities"
  done < <(find "$1" -type d -print0 | LC_ALL=C sort -z)
}

# As authorize, but only directories with a schema, each input run with every schema beside it:
# a `*.cedarschema` file (also with `--request-validation false`) and a `*.cedarschema.json` file
# (`--schema-format json`).
authorize_schema_inputs() {
  local directory name entities schema
  while IFS= read -r -d '' directory; do
    entities=""
    for name in entities.json entity.json; do
      if [ -f "$directory/$name" ]; then entities="$directory/$name"; break; fi
    done
    [ -n "$entities" ] || continue
    while IFS= read -r schema; do
      authorize_directory_inputs "$directory" "$entities" --schema "$schema"
      authorize_directory_inputs "$directory" "$entities" --schema "$schema" --request-validation false
    done < <(find "$directory" -maxdepth 1 -type f -name '*.cedarschema' | LC_ALL=C sort)
    while IFS= read -r schema; do
      authorize_directory_inputs "$directory" "$entities" --schema "$schema" --schema-format json
    done < <(find "$directory" -maxdepth 1 -type f -name '*.cedarschema.json' | LC_ALL=C sort)
  done < <(find "$1" -type d -print0 | LC_ALL=C sort -z)
}

authorize_schema_arguments() {
  authorize_arguments "$1"
}

authorize_schema_compare() {
  authorize_compare
}

authorize_arguments() {
  IFS=$'\t' read -r -a arguments <<<"$1"
}

# The answer printed by `authorize --verbose`, one item per line: the decision, then each
# determining policy id and each erroring policy id, sorted.
authorize_answer() {
  printf '%s\n' "$1" | awk '
    /^(ALLOW|DENY)$/ && !decided { print "0 " $0; decided = 1; next }
    /^error while evaluating policy `/ {
      text = substr($0, length("error while evaluating policy `") + 1)
      print "2 error " substr(text, 1, index(text, "`: ") - 1)
      next
    }
    /^note: this decision was due to the following policies:$/ { listing = 1; next }
    listing && /^  / { print "1 reason " substr($0, 3); next }
    { listing = 0 }
  ' | LC_ALL=C sort | cut -d' ' -f2- | paste -sd';'
}

authorize_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
    return 1
  fi
  if [ "$reference_status" -ne 0 ] && [ "$reference_status" -ne 2 ]; then
    outcome="failed"
    return 0
  fi
  local reference_answer candidate_answer
  reference_answer="$(authorize_answer "$reference_output")"
  candidate_answer="$(authorize_answer "$candidate_output")"
  # Exit 2 without a decision is a refused command line, not a Deny.
  if [ -z "$reference_answer" ] && [ -z "$candidate_answer" ]; then
    outcome="refused"
    return 0
  fi
  if [ "$reference_answer" != "$candidate_answer" ]; then
    reason="reference [$reference_answer], cedar-cpp-cli [$candidate_answer]"
    return 1
  fi
  if [ "$reference_status" -eq 0 ]; then outcome="allowed"; else outcome="denied"; fi
  return 0
}

# ---- run-tests ----

run_tests_inputs() {
  local tests policies schema
  while IFS= read -r -d '' tests; do
    while IFS= read -r -d '' policies; do
      authorize_emit run-tests --policies "$policies" --tests "$tests"
      while IFS= read -r -d '' schema; do
        if [[ "$schema" == *.json ]]; then
          authorize_emit run-tests --policies "$policies" --tests "$tests" --schema "$schema" --schema-format json
        else
          authorize_emit run-tests --policies "$policies" --tests "$tests" --schema "$schema"
        fi
      done < <(find "$(dirname "$tests")" -maxdepth 1 -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) -print0 | LC_ALL=C sort -z)
    done < <(find "$(dirname "$tests")" -maxdepth 1 -type f -name '*.cedar' -print0 | LC_ALL=C sort -z)
  done < <(find "$1" -type f -name 'test*.json' -print0 | LC_ALL=C sort -z)
}

run_tests_arguments() {
  authorize_arguments "$1"
}

# The verdicts printed by `run-tests`: the `running`, per-test and `results` lines, with the
# runtime error messages after an error count cut off.
run_tests_verdicts() {
  printf '%s\n' "$1" | grep -E '^(running |  test |results: )' | sed 's/\(runtime error(s)\).*/\1/'
}

run_tests_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
    return 1
  fi
  local reference_verdicts candidate_verdicts
  reference_verdicts="$(run_tests_verdicts "$reference_output")"
  candidate_verdicts="$(run_tests_verdicts "$candidate_output")"
  if [ "$reference_verdicts" != "$candidate_verdicts" ]; then
    reason="verdicts differ: reference [$(printf '%s' "$reference_verdicts" | paste -sd';')], cedar-cpp-cli [$(printf '%s' "$candidate_verdicts" | paste -sd';')]"
    return 1
  fi
  if [ "$reference_status" -eq 0 ]; then outcome="all-passed"; else outcome="some-failed"; fi
  return 0
}

# ---- translate-schema ----

# Prints each file in `$1` matching the `find` expression `$2...`, once per distinct content.
translate_schema_distinct() {
  local directory="$1"
  shift
  find "$directory" -type f "$@" -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum -z |
    awk 'BEGIN { RS = "\0"; ORS = "\0" } !seen[substr($0, 1, 64)]++ { print substr($0, 67) }'
}

# One NUL-terminated input per file and direction, the arguments joined by tabs.
translate_schema_inputs() {
  local file
  while IFS= read -r -d '' file; do
    authorize_emit translate-schema --direction cedar-to-json --schema "$file"
    authorize_emit translate-schema --direction cedar-to-json-with-resolved-types --schema "$file"
  done < <(translate_schema_distinct "$1" -name '*.cedarschema')
  while IFS= read -r -d '' file; do
    authorize_emit translate-schema --direction json-to-cedar --schema "$file"
  done < <(translate_schema_distinct "$1" -name '*schema*.json')
}

translate_schema_json_inputs() {
  local file
  while IFS= read -r -d '' file; do
    authorize_emit translate-schema --direction json-to-cedar --schema "$file"
  done < <(translate_schema_distinct "$1" -name '*.json')
}

translate_schema_arguments() {
  authorize_arguments "$1"
}

translate_schema_json_arguments() {
  authorize_arguments "$1"
}

translate_schema_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
    return 1
  fi
  if ! cmp -s "$reference_stdout" "$candidate_stdout"; then
    reason="standard output differs (exit $reference_status): $(diff "$reference_stdout" "$candidate_stdout" | head -c 300 | tr '\n' ' ')"
    return 1
  fi
  if [ -s "$reference_stderr" ] && [ "$reference_status" -eq 0 ] && [ ! -s "$candidate_stderr" ]; then
    reason="reference warned, cedar-cpp-cli did not"
    return 1
  fi
  if [ ! -s "$reference_stderr" ] && [ -s "$candidate_stderr" ] && [ "$reference_status" -eq 0 ]; then
    reason="cedar-cpp-cli warned, reference did not"
    return 1
  fi
  if [ "$reference_status" -ne 0 ]; then outcome="rejected"
  elif [ -s "$reference_stderr" ]; then outcome="warned"
  else outcome="translated"; fi
  return 0
}

translate_schema_json_compare() {
  translate_schema_compare
}

# ---- translate-policy ----

# One NUL-terminated input per distinct file: each `*.cedar` file to JSON, each `*.cedar.json`
# file to Cedar text.
translate_policy_inputs() {
  local file
  while IFS= read -r -d '' file; do
    authorize_emit translate-policy --direction cedar-to-json --policies "$file"
  done < <(translate_schema_distinct "$1" -name '*.cedar')
  while IFS= read -r -d '' file; do
    authorize_emit translate-policy --direction json-to-cedar --policies "$file"
  done < <(translate_schema_distinct "$1" -name '*.cedar.json')
}

translate_policy_arguments() {
  authorize_arguments "$1"
}

# Prints the Cedar text `$1` translates the JSON file `$2` to, then its exit status.
translate_policy_to_cedar() {
  "$1" translate-policy --direction json-to-cedar --policies "$2" </dev/null 2>/dev/null
  echo "exit $?"
}

# The ids of a JSON policy set: templates, static policies, and the link count.
translate_policy_ids() {
  jq -c '[(.templates | keys), (.staticPolicies | keys), (.templateLinks | length)]' "$1" 2>&1
}

translate_policy_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
    return 1
  fi
  if [ "$reference_status" -ne 0 ]; then
    outcome="rejected"
    return 0
  fi
  if [ "${arguments[2]}" = json-to-cedar ]; then
    if ! cmp -s "$reference_stdout" "$candidate_stdout"; then
      reason="Cedar text differs: $(diff "$reference_stdout" "$candidate_stdout" | head -c 300 | tr '\n' ' ')"
      return 1
    fi
    outcome="translated-to-cedar"
    return 0
  fi
  # Rust's JSON keeps the text's sugar (`!=`, `>`, separate clauses) and ours is built from the
  # model, so the two are compared by meaning: each is read back by both binaries and printed as
  # canonical Cedar text, which must agree four ways, and both name the same policies.
  local reference_ids candidate_ids from_reference from_candidate rust_from_reference rust_from_candidate
  reference_ids="$(translate_policy_ids "$reference_stdout")"
  candidate_ids="$(translate_policy_ids "$candidate_stdout")"
  if [ "$reference_ids" != "$candidate_ids" ]; then
    reason="policy ids differ: reference $reference_ids, cedar-cpp-cli $candidate_ids"
    return 1
  fi
  from_reference="$(translate_policy_to_cedar "$candidate" "$reference_stdout")"
  from_candidate="$(translate_policy_to_cedar "$candidate" "$candidate_stdout")"
  rust_from_reference="$(translate_policy_to_cedar "$reference" "$reference_stdout")"
  rust_from_candidate="$(translate_policy_to_cedar "$reference" "$candidate_stdout")"
  if [ "$from_reference" != "$rust_from_reference" ]; then
    reason="Rust's JSON reads back differently: $(diff <(printf '%s\n' "$rust_from_reference") <(printf '%s\n' "$from_reference") | head -c 300 | tr '\n' ' ')"
    return 1
  fi
  if [ "$from_candidate" != "$from_reference" ] || [ "$rust_from_candidate" != "$from_reference" ]; then
    reason="the JSON outputs mean different policies: $(diff <(printf '%s\n' "$from_reference") <(printf '%s\n' "$from_candidate") | head -c 300 | tr '\n' ' ')"
    return 1
  fi
  outcome="translated-to-json"
  return 0
}

# ---- link ----

link_inputs() {
  local directory
  if [ -f "$1/README.md" ] && grep -q '^cargo run link' "$1/README.md"; then
    printf '%s\0' "$1"
  fi
  while IFS= read -r -d '' directory; do
    printf '%s\0' "${directory%/step1.args}"
  done < <(find "$1" -mindepth 2 -maxdepth 2 -type f -name step1.args -print0 | sort -z)
}

# The steps of the case `$1`, NUL-separated, each step its arguments joined by the unit separator
# (0x1f, so that an empty argument survives): the README's `cargo run link` commands (continuation
# lines joined, then split as the shell would, by xargs), or `link` and the lines of each
# `step<N>.args` file, in order.
link_steps() {
  local step command
  if [ -f "$1/step1.args" ]; then
    for step in $(find "$1" -maxdepth 1 -name 'step*.args' -printf '%f\n' | sed 's/^step\([0-9]*\)\.args$/\1/' | sort -n); do
      local IFS=$'\x1f'
      local -a words=()
      mapfile -t words <"$1/step$step.args"
      words=(link "${words[@]}")
      printf '%s\0' "${words[*]}"
    done
    return
  fi
  while IFS= read -r -d '' command; do
    local IFS=$'\x1f'
    local -a words=()
    mapfile -d '' -t words < <(printf '%s\n' "${command#cargo run }" | xargs printf '%s\0')
    printf '%s\0' "${words[*]}"
  done < <(awk '
    /^cargo run link/ { joining = 1; line = "" }
    joining { text = $0; continued = sub(/\\[[:space:]]*$/, "", text); line = line text
              if (!continued) { printf "%s%c", line, 0; joining = 0 } }' "$1/README.md")
}

# Runs every step of the case `$1` with the binary `$2` in a fresh copy `$3` of the case's files.
# Sets `link_statuses` (one status per step) and `link_printed` (whether every successful step
# printed the new policy).
link_run_one() {
  local case_directory="$1" binary="$2" copy="$3" step
  rm -rf "$copy"
  mkdir -p "$copy"
  find "$case_directory" -maxdepth 1 -type f -exec cp {} "$copy/" \;
  link_statuses=""
  link_printed=1
  while IFS= read -r -d '' step; do
    local IFS=$'\x1f'
    local -a words=()
    read -r -d '' -a words < <(printf '%s' "$step")
    (cd "$copy" && "$binary" "${words[@]}" </dev/null >"$copy.out" 2>"$copy.err")
    local status=$?
    link_statuses+="${link_statuses:+ }$status"
    if [ "$status" -eq 0 ] && ! grep -q '^Template-linked policy added: ' "$copy.out"; then link_printed=0; fi
  done < <(link_steps "$case_directory")
}

link_run() {
  link_run_one "$1" "$reference" "$scratch/reference-case"
  reference_statuses="$link_statuses"
  reference_printed="$link_printed"
  link_run_one "$1" "$candidate" "$scratch/candidate-case"
  candidate_statuses="$link_statuses"
  candidate_printed="$link_printed"
  reference_status="${reference_statuses##* }"
  candidate_status="${candidate_statuses##* }"
}

link_arguments() {
  arguments=(link "$1")
}

link_compare() {
  if [ "$reference_statuses" != "$candidate_statuses" ]; then
    reason="step exit statuses differ: reference $reference_statuses, cedar-cpp-cli $candidate_statuses"
    return 1
  fi
  if [ "$reference_printed" != "$candidate_printed" ]; then
    reason="the new policy was printed by only one binary"
    return 1
  fi
  local reference_files candidate_files file
  reference_files="$(cd "$scratch/reference-case" && find . -type f | LC_ALL=C sort)"
  candidate_files="$(cd "$scratch/candidate-case" && find . -type f | LC_ALL=C sort)"
  if [ "$reference_files" != "$candidate_files" ]; then
    reason="the files differ: reference $(echo $reference_files), cedar-cpp-cli $(echo $candidate_files)"
    return 1
  fi
  while IFS= read -r file; do
    if cmp -s "$scratch/reference-case/$file" "$scratch/candidate-case/$file"; then continue; fi
    local reference_json candidate_json
    if reference_json="$(jq -S -c . "$scratch/reference-case/$file" 2>/dev/null)" &&
      candidate_json="$(jq -S -c . "$scratch/candidate-case/$file" 2>/dev/null)" &&
      [ "$reference_json" = "$candidate_json" ]; then
      continue
    fi
    reason="$file differs: reference $(head -c 300 "$scratch/reference-case/$file"), cedar-cpp-cli $(head -c 300 "$scratch/candidate-case/$file")"
    return 1
  done <<<"$reference_files"
  if [ "$reference_status" -eq 0 ]; then outcome="linked"; else outcome="rejected"; fi
  return 0
}

# ---- validate ----

# Policies for a schema that has none beside it: for each entity type, a policy scoped to it as
# principal and as resource, attribute accesses with and without `has`, tag accesses with and
# without `hasTag`, a declared and an undeclared enum id; for each action, a policy scoped to it
# and context attribute accesses. `$1` is the schema as JSON (`translate-schema` output).
validate_generated_policies() {
  jq -r '
    to_entries[] | .key as $namespace | (if $namespace == "" then "" else $namespace + "::" end) as $prefix |
    ( (.value.entityTypes // {}) | to_entries[] | ($prefix + .key) as $type |
        "permit(principal is \($type), action, resource);",
        "permit(principal, action, resource is \($type));",
        ( (.value.shape.attributes // {}) | keys[] | tojson as $attribute |
            "permit(principal is \($type), action, resource) when { principal[\($attribute)] == principal[\($attribute)] };",
            "permit(principal is \($type), action, resource) when { principal has \($attribute) && principal[\($attribute)] == principal[\($attribute)] };" ),
        ( if .value.tags then
            "permit(principal is \($type), action, resource) when { principal.hasTag(\"k\") && principal.getTag(\"k\") == principal.getTag(\"k\") };",
            "permit(principal is \($type), action, resource) when { principal.getTag(\"k\") == principal.getTag(\"k\") };"
          else empty end ),
        ( if .value.enum then
            "permit(principal == \($type)::\(.value.enum[0] | tojson), action, resource);",
            "permit(principal == \($type)::\"undeclared-enum-id\", action, resource);"
          else empty end ) ),
    ( (.value.actions // {}) | to_entries[] | "\($prefix)Action::\(.key | tojson)" as $action |
        "permit(principal, action == \($action), resource);",
        ( (.value.appliesTo.context.attributes // {}) | keys[] | tojson as $attribute |
            "permit(principal, action == \($action), resource) when { context[\($attribute)] == context[\($attribute)] };" ) )
  ' "$1"
}

# The inputs of one directory that holds a schema: each policy file beside it (with each
# `linked*` file) with each schema file, or when there is no policy file, a trivial policy set and
# the generated one.
validate_directory_inputs() {
  local directory="$1"
  local -a schemas policy_files link_options
  mapfile -t schemas < <(find "$directory" -maxdepth 1 -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) | LC_ALL=C sort)
  mapfile -t policy_files < <(find "$directory" -maxdepth 1 -type f \( -name '*.cedar' -o -name '*.cedar.json' \) | LC_ALL=C sort)
  link_options=("")
  while IFS= read -r file; do link_options+=("$file"); done < <(find "$directory" -maxdepth 1 -type f -name 'linked*' | LC_ALL=C sort)
  local schema policies link generated json_schema
  local -a schema_options policy_options
  for schema in "${schemas[@]}"; do
    schema_options=(--schema "$schema")
    [[ "$schema" == *.json ]] && schema_options+=(--schema-format json)
    if [ ${#policy_files[@]} -eq 0 ]; then
      generated="$scratch/validate/$(printf '%s' "$schema" | md5sum | cut -c1-16)"
      printf 'permit(principal, action, resource);\n' >"$generated.trivial.cedar"
      authorize_emit validate "${schema_options[@]}" --policies "$generated.trivial.cedar"
      if [[ "$schema" == *.json ]]; then json_schema="$schema"
      else
        json_schema="$generated.json"
        "$reference" translate-schema --direction cedar-to-json-with-resolved-types --schema "$schema" >"$json_schema" 2>/dev/null || continue
      fi
      validate_generated_policies "$json_schema" >"$generated.generated.cedar" 2>/dev/null || continue
      authorize_emit validate "${schema_options[@]}" --policies "$generated.generated.cedar"
      continue
    fi
    for policies in "${policy_files[@]}"; do
      policy_options=(--policies "$policies")
      [[ "$policies" == *.json ]] && policy_options+=(--policy-format json)
      for link in "${link_options[@]}"; do
        if [ -n "$link" ]; then
          authorize_emit validate "${schema_options[@]}" "${policy_options[@]}" --template-linked "$link"
        else
          authorize_emit validate "${schema_options[@]}" "${policy_options[@]}"
        fi
      done
    done
  done
}

# The rows of a validation parity table (`*Cases.tsv`, columns named by its header: `case`,
# `schema`, `mode`, `policy`, optional `links` and `level` as `scripts/generate-validation-parity-table.py`
# reads them), each written out as a policy file (and a template-linked file). `$2` is 1 to run
# every row in its own mode, 0 to skip the permissive ones.
validate_table_inputs() {
  local table="$1" permissive="$2"
  local base header line
  base="$scratch/validate/$(printf '%s' "$table" | md5sum | cut -c1-16)"
  local -a columns fields
  local -A column=()
  {
    IFS= read -r header
    IFS=$'\t' read -r -a columns <<<"$header"
    local index
    for index in "${!columns[@]}"; do column[${columns[$index]}]=$index; done
    while IFS= read -r line; do
      [ -n "$line" ] || continue
      IFS=$'\t' read -r -a fields <<<"$line"
      local name="${fields[${column[case]}]}" schema="${fields[${column[schema]}]}" mode="${fields[${column[mode]}]}"
      local policy="${fields[${column[policy]}]}" links="-"
      [ -n "${column[links]:-}" ] && links="${fields[${column[links]}]}"
      local level="-"
      [ -n "${column[level]:-}" ] && level="${fields[${column[level]}]}"
      [ "$mode" = permissive ] && [ "$permissive" -eq 0 ] && continue
      printf '%s\n' "$policy" >"$base.$name.cedar"
      local -a extra=()
      if [ "$links" != "-" ]; then
        jq -n --arg links "$links" '
          $links | split(";") | map(capture("^(?<template>[^:]*):(?<link>[^:]*):(?<slots>.*)$")
            | {template_id: .template, link_id: .link,
               args: (.slots | split("+") | map(capture("^(?<slot>[^=]*)=(?<uid>.*)$") | {(.slot): .uid}) | add)})
        ' >"$base.$name.links.json"
        extra=(--template-linked "$base.$name.links.json")
      fi
      [ "$level" != "-" ] && extra+=(--level "$level")
      authorize_emit validate --schema "$(dirname "$table")/$schema" --policies "$base.$name.cedar" "${extra[@]}" --validation-mode "$mode"
    done
  } <"$table"
}

# `$2` is 1 for validate-permissive.
validate_all_inputs() {
  mkdir -p "$scratch/validate"
  local directory table
  while IFS= read -r -d '' directory; do
    validate_directory_inputs "$directory"
  done < <(find "$1" -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) -printf '%h\0' | LC_ALL=C sort -zu)
  while IFS= read -r -d '' table; do
    validate_table_inputs "$table" "$2"
  done < <(find "$1" -type f -name '*Cases.tsv' -print0 | LC_ALL=C sort -z)
}

validate_inputs() {
  validate_all_inputs "$1" 0
}

validate_permissive_inputs() {
  validate_all_inputs "$1" 1
}

# Each directory input of validate once per level.
validate_level_inputs() {
  mkdir -p "$scratch/validate"
  local directory input level
  while IFS= read -r -d '' directory; do
    while IFS= read -r -d '' input; do
      for level in 0 1 2 3 4; do printf '%s\t--level\t%s\0' "$input" "$level"; done
    done < <(validate_directory_inputs "$directory")
  done < <(find "$1" -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) -printf '%h\0' | LC_ALL=C sort -zu)
}

validate_arguments() {
  authorize_arguments "$1"
  arguments+=(--error-format json)
}

# Every argument list gets `--validation-mode permissive` unless it names a mode (a table row).
validate_permissive_arguments() {
  validate_arguments "$1"
  [[ " ${arguments[*]} " == *" --validation-mode "* ]] || arguments+=(--validation-mode permissive)
}

validate_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status: $(head -c 300 "$reference_stdout" "$reference_stderr" | tr '\n' ' ')"
    return 1
  fi
  if [ "$reference_status" -ne 0 ] && [ "$reference_status" -ne 3 ]; then
    outcome="refused"
    return 0
  fi
  local reference_kinds candidate_kinds
  if ! reference_kinds="$(python3 -I "$classifier" --format reference --report "$reference_stdout" 2>&1)"; then
    reason="reference report not classified: $reference_kinds"
    return 1
  fi
  if ! candidate_kinds="$(python3 -I "$classifier" --format cedar-cpp --report "$candidate_stdout" 2>&1)"; then
    reason="cedar-cpp-cli report not read: $candidate_kinds"
    return 1
  fi
  if [ "$reference_kinds" != "$candidate_kinds" ]; then
    reason="diagnostics differ (exit $reference_status): reference [$(printf '%s' "$reference_kinds" | tr '\t' ' ' | paste -sd';')], cedar-cpp-cli [$(printf '%s' "$candidate_kinds" | tr '\t' ' ' | paste -sd';')]"
    return 1
  fi
  if [ "$reference_status" -eq 3 ]; then outcome="failed"
  elif [ -n "$reference_kinds" ]; then outcome="passed-with-warnings"
  else outcome="passed"; fi
  return 0
}

validate_permissive_compare() {
  validate_compare
}

validate_level_arguments() {
  validate_arguments "$1"
}

validate_level_compare() {
  validate_compare
}

# ---- tpe ----

# The generated `tpe` inputs of one directory holding `entities.json` and a schema. Each input is
# the directory, then the arguments, tab-separated.
tpe_directory_inputs() {
  local directory="$1" entities="$1/entities.json"
  local -a policy_files request_files context_files schema_files
  mapfile -t policy_files < <(find "$directory" -maxdepth 1 -type f \( -name '*.cedar' -o -name '*.cedar.json' \) | LC_ALL=C sort)
  mapfile -t request_files < <(find "$directory" -maxdepth 1 -type f -name 'request*.json' | LC_ALL=C sort)
  mapfile -t context_files < <(find "$directory" -maxdepth 1 -type f -name 'context*.json' | LC_ALL=C sort)
  mapfile -t schema_files < <(find "$directory" -maxdepth 1 -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) | LC_ALL=C sort)
  local schema policies request environment action principal_type resource_type principal_eid resource_eid context
  local -a schema_arguments policy_arguments environments principal_eids resource_eids contexts
  contexts=("" "${context_files[@]}")
  for schema in "${schema_files[@]}"; do
    schema_arguments=(--schema "$schema")
    local resolved
    if [[ "$schema" == *.json ]]; then
      schema_arguments+=(--schema-format json)
      resolved="$("$reference" translate-schema --direction json-to-cedar --schema "$schema" 2>/dev/null >"$scratch/tpe-schema.cedarschema" &&
        "$reference" translate-schema --direction cedar-to-json-with-resolved-types --schema "$scratch/tpe-schema.cedarschema" 2>/dev/null)"
    else
      resolved="$("$reference" translate-schema --direction cedar-to-json-with-resolved-types --schema "$schema" 2>/dev/null)"
    fi
    # One line per request environment: the action uid, a principal type and a resource type.
    mapfile -t environments < <(printf '%s' "$resolved" | jq -r '
      to_entries[] | .key as $namespace | (.value.actions // {}) | to_entries[]
      | select(.value.appliesTo != null)
      | ((if $namespace == "" then "" else $namespace + "::" end) + "Action::" + (.key | tojson)) as $action
      | (.value.appliesTo.principalTypes // [] | unique[]) as $principal
      | (.value.appliesTo.resourceTypes // [] | unique[]) as $resource
      | [$action, $principal, $resource] | @tsv' 2>/dev/null)
    for policies in "${policy_files[@]}"; do
      policy_arguments=(--policies "$policies")
      [[ "$policies" == *.cedar.json ]] && policy_arguments+=(--policy-format json)
      for request in "${request_files[@]}"; do
        authorize_emit "$directory" tpe "${policy_arguments[@]}" "${schema_arguments[@]}" --entities "$entities" \
          --request-json "$request"
      done
      for environment in "${environments[@]}"; do
        IFS=$'\t' read -r action principal_type resource_type <<<"$environment"
        # Each id unknown, each id of that type in the entities file, and an id of no entity.
        mapfile -t principal_eids < <(jq -r --arg type "$principal_type" '.[] | .uid | (.__entity // .) | select(.type == $type) | .id' "$entities" 2>/dev/null | head -n 3)
        mapfile -t resource_eids < <(jq -r --arg type "$resource_type" '.[] | .uid | (.__entity // .) | select(.type == $type) | .id' "$entities" 2>/dev/null | head -n 3)
        principal_eids=("" "${principal_eids[@]}" "no-such-entity")
        resource_eids=("" "${resource_eids[@]}" "no-such-entity")
        for principal_eid in "${principal_eids[@]}"; do
          for resource_eid in "${resource_eids[@]}"; do
            for context in "${contexts[@]}"; do
              local -a request_arguments=(--principal-type "$principal_type" --action "$action" --resource-type "$resource_type")
              [ -n "$principal_eid" ] && request_arguments+=(--principal-eid "$principal_eid")
              [ -n "$resource_eid" ] && request_arguments+=(--resource-eid "$resource_eid")
              [ -n "$context" ] && request_arguments+=(--context "$context")
              authorize_emit "$directory" tpe "${policy_arguments[@]}" "${schema_arguments[@]}" --entities "$entities" \
                "${request_arguments[@]}"
            done
          done
        done
      done
    done
  done
}

# Every `*.args` file under the directory (a crafted command line, one argument per line after
# `tpe`, run in the file's directory), then the generated inputs of every directory holding
# `entities.json` and a schema.
tpe_inputs() {
  local directory
  find "$1" -type f -name '*.args' -print0 | LC_ALL=C sort -z
  while IFS= read -r -d '' directory; do
    [ -f "$directory/entities.json" ] || continue
    [ -n "$(find "$directory" -maxdepth 1 -type f \( -name '*.cedarschema' -o -name '*.cedarschema.json' \) -print -quit)" ] || continue
    tpe_directory_inputs "$directory"
  done < <(find "$(realpath "$1")" -type d -print0 | LC_ALL=C sort -z)
}

tpe_reference="$(realpath "$reference")"
tpe_candidate="$(realpath "$candidate")"

tpe_arguments() {
  if [[ "$1" == *.args && "$1" != *$'\t'* ]]; then
    tpe_directory="$(dirname "$1")"
    local -a words
    mapfile -t words <"$1"
    arguments=(tpe "${words[@]}")
  else
    local -a fields
    IFS=$'\t' read -r -a fields <<<"$1"
    tpe_directory="${fields[0]}"
    arguments=("${fields[@]:1}")
  fi
}

tpe_run() {
  # Each command runs in its input's directory, so the binaries are named by absolute path.
  local reference="$tpe_reference" candidate="$tpe_candidate"
  (cd "$tpe_directory" && "$reference" "${arguments[@]}" </dev/null >"$reference_stdout" 2>"$reference_stderr")
  reference_status=$?
  (cd "$tpe_directory" && "$candidate" "${arguments[@]}" </dev/null >"$candidate_stdout" 2>"$candidate_stderr")
  candidate_status=$?
}

# The residual policies printed after `All policy residuals:` in `$1`, as a sorted list of JSON
# policies: the text is read back by the reference CLI (`translate-policy --direction
# cedar-to-json`) and each policy written as compact JSON with sorted keys. A residual `error()`
# names no real function, so it is read as a stand-in attribute access.
tpe_residuals() {
  sed '1,/^All policy residuals:$/d' "$1" |
    sed 's/\(^\|[^A-Za-z0-9_]\)error()/\1context["cedar-cpp residual error"]/g' >"$scratch/tpe-residuals.cedar"
  "$reference" translate-policy --direction cedar-to-json --policies "$scratch/tpe-residuals.cedar" 2>/dev/null |
    jq -c '[.staticPolicies[]] | map(tojson) | sort | .[]' 2>/dev/null
  return "${PIPESTATUS[0]}"
}

# Standard output with the timing line's number removed.
tpe_output() {
  sed 's/^\(Authorization Time (micro seconds) : \)[0-9]*$/\1N/' "$1"
}

tpe_compare() {
  if [ "$reference_status" -ne "$candidate_status" ]; then
    reason="reference exit $reference_status, cedar-cpp-cli exit $candidate_status"
    return 1
  fi
  case "$reference_status" in
    0 | 2 | 4) ;;
    1)
      # Both read their inputs and failed: each printed the leading blank line first.
      if [ "$(head -c 1 "$reference_stdout" | od -An -c | tr -d ' ')" != '\n' ] ||
        [ "$(head -c 1 "$candidate_stdout" | od -An -c | tr -d ' ')" != '\n' ]; then
        reason="exit 1 without the leading blank line"
        return 1
      fi
      outcome="failed"
      return 0
      ;;
    *) outcome="exited $reference_status"; return 0 ;;
  esac
  if [ "$reference_status" -eq 4 ]; then
    local reference_head candidate_head reference_list candidate_list
    reference_head="$(tpe_output "$reference_stdout" | sed '/^All policy residuals:$/q')"
    candidate_head="$(tpe_output "$candidate_stdout" | sed '/^All policy residuals:$/q')"
    if [ "$reference_head" != "$candidate_head" ]; then
      reason="output differs before the residuals"
      return 1
    fi
    if ! reference_list="$(tpe_residuals "$reference_stdout")"; then
      reason="the reference's residuals do not read back"
      return 1
    fi
    if ! candidate_list="$(tpe_residuals "$candidate_stdout")"; then
      reason="cedar-cpp-cli's residuals do not read back"
      return 1
    fi
    if [ "$reference_list" != "$candidate_list" ]; then
      reason="residuals differ: reference [$(sed '1,/^All policy residuals:$/d' "$reference_stdout" | paste -sd'|')], cedar-cpp-cli [$(sed '1,/^All policy residuals:$/d' "$candidate_stdout" | tr -s ' \n' ' ')]"
      return 1
    fi
    outcome="unknown"
    return 0
  fi
  if [ "$(tpe_output "$reference_stdout")" != "$(tpe_output "$candidate_stdout")" ]; then
    reason="standard output differs"
    return 1
  fi
  if ! [ -s "$reference_stdout" ]; then outcome="refused"
  elif [ "$reference_status" -eq 0 ]; then outcome="allowed"
  else outcome="denied"; fi
  return 0
}

# ---- driver ----

case "$mode" in
  check-parse) prefix=check_parse ;;
  authorize) prefix=authorize ;;
  authorize-schema) prefix=authorize_schema ;;
  run-tests) prefix=run_tests ;;
  translate-schema) prefix=translate_schema ;;
  translate-schema-json) prefix=translate_schema_json ;;
  translate-policy) prefix=translate_policy ;;
  link) prefix=link ;;
  validate) prefix=validate ;;
  validate-permissive) prefix=validate_permissive ;;
  validate-level) prefix=validate_level ;;
  tpe) prefix=tpe ;;
  *) echo "unknown --command: $mode" >&2; usage ;;
esac

# The translate modes compare standard output byte for byte and the validate modes read it as
# JSON, so they keep the two streams apart.
split_streams=0
case "$mode" in translate-* | validate* | tpe) split_streams=1 ;; esac
# The validate modes read both reports with this script, found beside this one in the source tree.
classifier="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../scripts" && pwd)/classify-validation-report.py"
scratch="$(mktemp -d "${TMPDIR:-/var/tmp}/oracle-diff.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
reference_stdout="$scratch/reference.out"
reference_stderr="$scratch/reference.err"
candidate_stdout="$scratch/candidate.out"
candidate_stderr="$scratch/candidate.err"

total=0
agreed=0
declare -A outcomes=()
disagreements=()
for directory in "${directories[@]}"; do
  while IFS= read -r -d '' input; do
    "${prefix}_arguments" "$input"
    if declare -F "${prefix}_run" >/dev/null; then
      "${prefix}_run" "$input"
    elif [ "$split_streams" -eq 1 ]; then
      "$reference" "${arguments[@]}" </dev/null >"$reference_stdout" 2>"$reference_stderr"
      reference_status=$?
      "$candidate" "${arguments[@]}" </dev/null >"$candidate_stdout" 2>"$candidate_stderr"
      candidate_status=$?
    else
      reference_output="$("$reference" "${arguments[@]}" </dev/null 2>&1)"
      reference_status=$?
      candidate_output="$("$candidate" "${arguments[@]}" </dev/null 2>&1)"
      candidate_status=$?
    fi
    total=$((total + 1))
    outcome=""
    reason=""
    if "${prefix}_compare"; then
      agreed=$((agreed + 1))
      outcomes[$outcome]=$(( ${outcomes[$outcome]:-0} + 1 ))
    else
      disagreements+=("${input//$'\t'/ }: $reason")
    fi
  done < <("${prefix}_inputs" "$directory")
done

breakdown=""
for key in $([ ${#outcomes[@]} -gt 0 ] && printf '%s\n' "${!outcomes[@]}" | sort); do
  breakdown+="${breakdown:+, }${outcomes[$key]} $key by both"
done
echo "$mode: $total inputs, $agreed agree${breakdown:+ ($breakdown)}, ${#disagreements[@]} disagree"
for line in "${disagreements[@]}"; do
  echo "  DISAGREE $line"
done
[ "$total" -gt 0 ] || { echo "no inputs found" >&2; exit 1; }
[ ${#disagreements[@]} -eq 0 ]
