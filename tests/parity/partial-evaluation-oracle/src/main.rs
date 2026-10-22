//! cedar-partial-evaluation-oracle: Rust Cedar's type-aware partial evaluation (the `tpe` feature),
//! driven by a manifest of cases and answered as canonical JSON lines. cedar-cpp's
//! `cedar-cpp-partial-evaluation-runner` reads the same manifests and must write the same lines,
//! byte for byte.
//!
//! # Invocation
//!
//! `cedar-partial-evaluation-oracle <manifest.jsonl>`
//!
//! Every output line goes to standard output. The exit status is 0 when the manifest was read, even
//! when cases fail; it is 2 for a usage error or an unreadable manifest, with nothing written.
//!
//! # Manifest
//!
//! UTF-8, one case per line. A line that is empty or only whitespace is skipped and writes nothing.
//! Every other line is one case and writes exactly one output line, in manifest order.
//!
//! A case is a JSON object. File paths are relative to the directory that holds the manifest. A key
//! not listed for the case's operation is an `input.manifest` error.
//!
//! | Key | Operations | Value |
//! |---|---|---|
//! | `name` | all, required | string, echoed in the output |
//! | `operation` | all, required | `partial-authorize`, `batched`, `query-principal`, `query-resource` or `query-action` |
//! | `schema` | all, required | path; a name ending in `.json` is a JSON schema, any other is Cedar schema syntax |
//! | `policies` | all, required | path to a Cedar policy file, read with `PolicySet::from_str` (ids `policy0`, `policy1`, ... in source order; `@id` is not consulted) |
//! | `links` | all, optional | array of `{"template": id, "id": new id, "slots": {"?principal": UID, "?resource": UID}}`, linked in order with `PolicySet::link`; `slots` may omit either key |
//! | `entities` | all, required | path to an entities JSON file; its form depends on the operation (below) |
//! | `request` | all, required | object; its keys depend on the operation (below) |
//! | `completions` | `partial-authorize`, optional | array of `{"request": concrete request, "entities": path to concrete entities}` |
//! | `maximum-iterations` | `batched`, required | integer from 0 to 4294967295 |
//!
//! A UID is `{"type": "Namespace::Type", "id": "eid"}`. `type` is parsed with
//! `EntityTypeName::from_str`; `id` is the raw (unescaped) entity id. A partial UID omits `id`.
//!
//! Request objects, per operation. `context` is a Cedar JSON context record.
//!
//! | Operation | `principal` | `action` | `resource` | `context` |
//! |---|---|---|---|---|
//! | `partial-authorize` | UID, `id` optional | UID | UID, `id` optional | optional; absent means unknown |
//! | `batched`, and every completion | UID | UID | UID | optional; absent means `{}` |
//! | `query-principal` | UID without `id` | UID | UID | optional; absent means `{}` |
//! | `query-resource` | UID | UID | UID without `id` | optional; absent means `{}` |
//! | `query-action` | UID, `id` optional | no key | UID, `id` optional | optional; absent means unknown |
//!
//! Except for `query-action`, the context is parsed with
//! `Context::from_json_value(value, Some((&schema, &action)))`. `query-action` has no single action,
//! so its context is parsed with `Context::from_json_value(value, None)`.
//!
//! Entities files, per operation:
//! - `partial-authorize` and `query-action`: partial entities, read with
//!   `PartialEntities::from_json_value(json, &schema)`. An entity without `attrs`, `parents` or
//!   `tags` has those unknown.
//! - `batched`, `query-principal`, `query-resource` and every completion: concrete entities, read with
//!   `Entities::from_json_value(json, Some(&schema))`, which adds the schema's action entities.
//!
//! # Steps, and which error a case reports
//!
//! A case stops at its first failing step and reports that step's error. The steps, in order:
//! 1. the manifest line: JSON object, `name`, `operation`, the allowed keys, the shape of
//!    `completions`;
//! 2. the schema; 3. the policies; 4. the links;
//! 5. the request: its UIDs, then its context, then the request object of the Cedar API
//!    (`PartialRequest::new`, `PrincipalQueryRequest::new`, `ResourceQueryRequest::new`,
//!    `ActionQueryRequest::new`, or `Request::new(.., None)` for a concrete request);
//! 6. the entities;
//! 7. the operation itself.
//!
//! `maximum-iterations` is checked just before the batched request, and the shape of `links` and
//! of each link during step 4; both fail as `input.manifest`.
//!
//! For `partial-authorize`, each completion runs after step 7 in order: its request (UIDs, context,
//! `Request::new(.., None)`), then its entities, then `TpeResponse::reauthorize`. A completion's
//! failure is reported in its own entry and does not stop the case.
//!
//! # Output
//!
//! One line per case: canonical JSON, meaning object keys sorted by their UTF-8 bytes, no
//! whitespace anywhere, integers in decimal, and strings escaped exactly as `serde_json` escapes them
//! (`\"`, `\\`, `\b`, `\f`, `\n`, `\r`, `\t`, other characters below U+0020 as `\u00xx` with
//! lowercase hex, everything else, `/` and non-ASCII included, written raw).
//!
//! A case line is `{"error": PATH, "name": N, "operation": O}` when it stopped at a step, and
//! `{"name": N, "operation": O, "result": RESULT}` when it reached the operation. `N` and `O` are
//! `null` when the manifest line did not supply them as strings.
//!
//! A decision is `"allow"`, `"deny"`, or `null` for unknown. Policy-id lists are sorted by UTF-8
//! bytes. A UID is written `{"id": raw id, "type": type name}`; UID lists are sorted by type, then
//! id, both by UTF-8 bytes.
//!
//! `partial-authorize` RESULT:
//! - `decision`: the decision;
//! - `reason`: the sorted ids of `TpeResponse::reason`, or `null` when the decision is unknown;
//! - `true-permits`, `false-permits`, `error-permits`, `residual-permits`, `true-forbids`,
//!   `false-forbids`, `error-forbids`, `residual-forbids`: sorted id lists;
//! - `policies`: an object keyed by policy id, holding every policy of the response (residual or
//!   not) as `{"annotations": {key: value}, "effect": "permit" | "forbid", "residual": TREE}`; an
//!   annotation without a value has the value `""`;
//! - `reauthorizations`: an array, one entry per completion in manifest order (empty without
//!   completions): `{"decision": "allow" | "deny", "determining": ids, "erroring": ids}`, where the
//!   ids are the response's sorted reason and error policy ids, or `{"error": PATH}`.
//!
//! `batched` RESULT: `{"decision": "allow" | "deny", "rounds": ROUNDS}` or
//! `{"error": PATH, "rounds": ROUNDS}`. ROUNDS has one entry per call to the entity loader, in call
//! order; each entry is the sorted UID list the call asked for (a call may ask for nothing, `[]`).
//!
//! `query-principal` and `query-resource` RESULT: `{"uids": sorted UID list}`.
//!
//! `query-action` RESULT: `{"actions": [{"action": UID, "decision": "allow" | null}]}`, sorted by
//! action UID.
//!
//! # Residual trees
//!
//! A tree is written one-to-one with core's `tpe::residual::Residual` and `ResidualKind`. Every
//! node is an object with a `kind`. Types and source locations are not written.
//!
//! | Rust | JSON |
//! |---|---|
//! | `Residual::Concrete { value }` | `{"kind": "concrete", "value": VALUE}` |
//! | `Residual::Error(_)` | `{"kind": "error"}` |
//! | `Var(v)` | `{"kind": "var", "var": "principal" \| "action" \| "resource" \| "context"}` |
//! | `If` | `{"kind": "if", "test_expr": T, "then_expr": T, "else_expr": T}` |
//! | `And`, `Or` | `{"kind": "and" \| "or", "left": T, "right": T}` |
//! | `UnaryApp` | `{"kind": "unary-app", "op": UNARY, "arg": T}` |
//! | `BinaryApp` | `{"kind": "binary-app", "op": BINARY, "arg1": T, "arg2": T}` |
//! | `ExtensionFunctionApp` | `{"kind": "extension-function-app", "fn_name": name, "args": [T, ...]}` |
//! | `GetAttr` | `{"kind": "get-attribute", "expr": T, "attr": string}` |
//! | `HasAttr` | `{"kind": "has-attribute", "expr": T, "attr": string}` |
//! | `ExtHasAttr` | `{"kind": "extension-has-attribute", "expr": T, "attrs": [string, ...]}` in path order |
//! | `Like` | `{"kind": "like", "expr": T, "pattern": [ELEMENT, ...]}` |
//! | `Is` | `{"kind": "is", "expr": T, "entity_type": type name}` |
//! | `Set` | `{"kind": "set", "elements": [T, ...]}` in order |
//! | `Record` | `{"kind": "record", "fields": {key: T}}` |
//!
//! - UNARY is `not`, `neg` or `is-empty`. BINARY is `eq`, `less`, `less-eq`, `add`, `sub`, `mul`,
//!   `in`, `contains`, `contains-all`, `contains-any`, `get-tag` or `has-tag`.
//! - `fn_name` is the extension function's name as Cedar prints it (`ip`, `decimal`, `isInRange`,
//!   `lessThan`, `datetime`, `offset`, ...). A type name is printed as Cedar prints it
//!   (`Namespace::Type`).
//! - A pattern ELEMENT is a one-character string for `PatternElem::Char`, and `null` for
//!   `PatternElem::Wildcard`. Characters are not merged.
//! - VALUE is the Cedar JSON value form, `CedarValueJson::from_value`: booleans, integers, strings,
//!   arrays for sets (in the order `from_value` yields, which is Rust's `Value` order), objects for
//!   records, `{"__entity": {"id": .., "type": ..}}` for entities, and
//!   `{"__extn": {"arg": .., "fn": ..}}` for extension values in their canonical form. Keys are
//!   sorted like every other object.
//!
//! # Error paths
//!
//! An error is the path of its Rust enum variants, never message text. Each segment is a Rust
//! identifier split into words at case boundaries (`NoMatchingReqEnv` becomes
//! `no`, `matching`, `req`, `env`; `TCComputation` becomes `tc`, `computation`), lowercased, with
//! these words spelled out: `attr` attribute, `attrs` attributes, `env` environment, `eval`
//! evaluation, `expr` expression, `ext` extension, `req` request, `tc` transitive-closure; then
//! joined with `-`. Segments are joined with `.`.
//!
//! The first segment names the error's enum, without its `Error` suffix:
//!
//! | First segment | Rust type | Raised by |
//! |---|---|---|
//! | `input` | none: a problem with the case itself | steps 1 to 6 |
//! | `partial-request-creation` | `PartialRequestCreationError` | `PartialRequest::new`, the two query request constructors, `ActionQueryRequest::new` |
//! | `entities` | `tpe_err::EntitiesError` | `PartialEntities::from_json_value` |
//! | `tpe` | `tpe_err::TpeError` | `PolicySet::tpe` |
//! | `tpe-reauthorization` | `TpeReauthorizationError` | `TpeResponse::reauthorize` |
//! | `batched-evaluation` | `BatchedEvalError` | `PolicySet::is_authorized_batched` |
//! | `permission-query` | `PermissionQueryError` | `query_principal`, `query_resource`, `query_action` |
//!
//! `input` paths: `input.manifest` (the line is not a JSON object; a key is missing, unknown or of
//! the wrong JSON type; a UID has an `id` where the operation forbids one or lacks one it needs;
//! `maximum-iterations` is out of range), `input.file` (a named file cannot be read),
//! `input.schema`, `input.policies`, `input.links`, `input.uid` (a type name or UID does not parse),
//! `input.context`, `input.request` (`Request::new` refused a concrete request), and
//! `input.entities` (an entities file is not JSON, or `Entities::from_json_value` refused it).
//!
//! After the first segment come the variant names, descending into a variant's payload when that
//! payload is itself an enum, and stopping at a struct or a unit variant. These payload enums are
//! descended:
//! - `TpeError::ExprToResidualError` into `ExprToResidualError`
//!   (`tpe.expression-to-residual-error.unlinked-slot-error`);
//! - `EntitiesError::Deserialization` into tpe's `JsonDeserializationError`, and stops there
//!   (`entities.deserialization.concrete`, `entities.deserialization.unexpected-action`,
//!   `entities.deserialization.restricted-expression-evaluation`);
//! - `EntitiesError::Validation` into `EntityValidationError`, and its `Concrete` into
//!   `EntitySchemaConformanceError` (`entities.validation.concrete.type-mismatch`,
//!   `entities.validation.mismatched-action-ancestors`);
//! - `EntitiesError::TCComputation` into `TcError`
//!   (`entities.transitive-closure-computation.has-cycle`);
//! - every `PartialValueToValue` into `PartialValueToValueError`;
//! - every `RequestValidation` or `Validation` holding a `RequestValidationError` into it
//!   (`partial-request-creation.validation.undeclared-action`);
//! - `TpeReauthorizationError::EntityValidation` into `EntitySchemaConformanceError`;
//! - `TpeReauthorizationError::InconsistentEntities` into `EntitiesConsistencyError`, and its
//!   `InconsistentEntity` into `EntityConsistencyError`
//!   (`tpe-reauthorization.inconsistent-entities.inconsistent-entity.mismatched-attribute`);
//! - `TpeReauthorizationError::InconsistentRequests` into `RequestConsistencyError`
//!   (`tpe-reauthorization.inconsistent-requests.inconsistent-context`);
//! - `BatchedEvalError::TPE` and `PermissionQueryError::TPE` into `TpeError`, and
//!   `BatchedEvalError::Entities` and `PermissionQueryError::Entities` into `EntitiesError`, each
//!   continuing as above (`batched-evaluation.tpe.incomplete-schema`,
//!   `permission-query.entities.duplicate`).

use std::collections::{HashMap, HashSet};
use std::fmt::Debug;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::str::FromStr;

use cedar_policy::{
    tpe_err, ActionQueryRequest, AuthorizationError, Context, Decision, Effect, Entities, Entity, EntityId,
    EntityLoader, EntityTypeName, EntityUid, PartialEntities, PartialEntityUid, PartialRequest,
    PartialRequestCreationError, PermissionQueryError, PolicyId, PolicySet,
    PrincipalQueryRequest, Request, ResourceQueryRequest, Schema, SlotId,
    TpeReauthorizationError, TpeResponse,
};
use cedar_policy_core::ast::{PatternElem, PolicyID};
use cedar_policy_core::batched_evaluator::err::BatchedEvalError;
use cedar_policy_core::entities::json::CedarValueJson;
use cedar_policy_core::tpe::residual::{Residual, ResidualKind};
use serde_json::{json, Map, Value};

type Outcome<T> = Result<T, String>;

fn main() -> ExitCode {
    let arguments: Vec<String> = std::env::args().collect();
    if arguments.len() != 2 {
        eprintln!("usage: cedar-partial-evaluation-oracle <manifest.jsonl>");
        return ExitCode::from(2);
    }
    let manifest_path = PathBuf::from(&arguments[1]);
    let text = match std::fs::read_to_string(&manifest_path) {
        Ok(text) => text,
        Err(error) => {
            eprintln!("cannot read {}: {error}", manifest_path.display());
            return ExitCode::from(2);
        }
    };
    let directory = manifest_path
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_default();
    let mut output = String::new();
    for line in text.lines() {
        if line.trim().is_empty() {
            continue;
        }
        write_canonical(&run_case(line, &directory), &mut output);
        output.push('\n');
    }
    print!("{output}");
    ExitCode::SUCCESS
}

fn run_case(line: &str, directory: &Path) -> Value {
    let parsed: Option<Map<String, Value>> = match serde_json::from_str::<Value>(line) {
        Ok(Value::Object(object)) => Some(object),
        _ => None,
    };
    let name = parsed
        .as_ref()
        .and_then(|object| object.get("name"))
        .and_then(Value::as_str)
        .map_or(Value::Null, |name| Value::String(name.to_owned()));
    let operation = parsed
        .as_ref()
        .and_then(|object| object.get("operation"))
        .and_then(Value::as_str)
        .map_or(Value::Null, |operation| Value::String(operation.to_owned()));
    let outcome = match &parsed {
        Some(object) => run_operation(object, directory),
        None => Err("input.manifest".to_owned()),
    };
    match outcome {
        Ok(result) => json!({"name": name, "operation": operation, "result": result}),
        Err(path) => json!({"error": path, "name": name, "operation": operation}),
    }
}

fn run_operation(case: &Map<String, Value>, directory: &Path) -> Outcome<Value> {
    if !case.get("name").is_some_and(Value::is_string) {
        return Err("input.manifest".to_owned());
    }
    let operation = case
        .get("operation")
        .and_then(Value::as_str)
        .ok_or_else(|| "input.manifest".to_owned())?;
    let common = ["name", "operation", "schema", "policies", "links", "entities", "request"];
    let extra: &[&str] = match operation {
        "partial-authorize" => &["completions"],
        "batched" => &["maximum-iterations"],
        "query-principal" | "query-resource" | "query-action" => &[],
        _ => return Err("input.manifest".to_owned()),
    };
    for key in case.keys() {
        if !common.contains(&key.as_str()) && !extra.contains(&key.as_str()) {
            return Err("input.manifest".to_owned());
        }
    }
    if let Some(completions) = case.get("completions") {
        let completions = completions.as_array().ok_or_else(|| "input.manifest".to_owned())?;
        for completion in completions {
            object_with_keys(completion, &["request", "entities"])?;
        }
    }
    let inputs = Inputs { case, directory };
    let schema = inputs.schema()?;
    let policies = inputs.policies()?;
    match operation {
        "partial-authorize" => partial_authorize(&inputs, &schema, &policies),
        "batched" => batched(&inputs, &schema, &policies),
        "query-principal" => query_principal(&inputs, &schema, &policies),
        "query-resource" => query_resource(&inputs, &schema, &policies),
        _ => query_action(&inputs, &schema, &policies),
    }
}

struct Inputs<'a> {
    case: &'a Map<String, Value>,
    directory: &'a Path,
}

impl Inputs<'_> {
    fn required(&self, key: &str) -> Outcome<&Value> {
        self.case.get(key).ok_or_else(|| "input.manifest".to_owned())
    }

    fn read(&self, value: &Value) -> Outcome<String> {
        let relative = value.as_str().ok_or_else(|| "input.manifest".to_owned())?;
        std::fs::read_to_string(self.directory.join(relative)).map_err(|_| "input.file".to_owned())
    }

    fn schema(&self) -> Outcome<Schema> {
        let value = self.required("schema")?;
        let text = self.read(value)?;
        let is_json = value.as_str().is_some_and(|path| path.ends_with(".json"));
        let schema = if is_json {
            Schema::from_json_str(&text).ok()
        } else {
            Schema::from_cedarschema_str(&text).ok().map(|(schema, _)| schema)
        };
        schema.ok_or_else(|| "input.schema".to_owned())
    }

    fn policies(&self) -> Outcome<PolicySet> {
        let text = self.read(self.required("policies")?)?;
        let mut policies = PolicySet::from_str(&text).map_err(|_| "input.policies".to_owned())?;
        let Some(links) = self.case.get("links") else {
            return Ok(policies);
        };
        let links = links.as_array().ok_or_else(|| "input.manifest".to_owned())?;
        for link in links {
            let link = object_with_keys(link, &["template", "id", "slots"])?;
            let template = string_field(link, "template")?;
            let id = string_field(link, "id")?;
            let slots = link
                .get("slots")
                .ok_or_else(|| "input.manifest".to_owned())?;
            let slots = object_with_keys(slots, &["?principal", "?resource"])?;
            let mut values = HashMap::new();
            for (key, slot) in [("?principal", SlotId::principal()), ("?resource", SlotId::resource())] {
                if let Some(uid) = slots.get(key) {
                    values.insert(slot, concrete_uid(uid)?);
                }
            }
            policies
                .link(PolicyId::new(template), PolicyId::new(id), values)
                .map_err(|_| "input.links".to_owned())?;
        }
        Ok(policies)
    }

    fn entities_json(&self, value: &Value) -> Outcome<Value> {
        let text = self.read(value)?;
        serde_json::from_str(&text).map_err(|_| "input.entities".to_owned())
    }

    fn concrete_entities(&self, value: &Value, schema: &Schema) -> Outcome<Entities> {
        let json = self.entities_json(value)?;
        Entities::from_json_value(json, Some(schema)).map_err(|_| "input.entities".to_owned())
    }

    fn partial_entities(&self, schema: &Schema) -> Outcome<PartialEntities> {
        let json = self.entities_json(self.required("entities")?)?;
        PartialEntities::from_json_value(json, schema)
            .map_err(|error| format!("entities.{}", entities_error(&error)))
    }
}

/// How a request object's principal or resource may carry its id.
#[derive(Clone, Copy, PartialEq)]
enum IdRule {
    Required,
    Optional,
    Forbidden,
}

fn object_with_keys<'a>(value: &'a Value, allowed: &[&str]) -> Outcome<&'a Map<String, Value>> {
    let object = value.as_object().ok_or_else(|| "input.manifest".to_owned())?;
    if object.keys().any(|key| !allowed.contains(&key.as_str())) {
        return Err("input.manifest".to_owned());
    }
    Ok(object)
}

fn string_field<'a>(object: &'a Map<String, Value>, key: &str) -> Outcome<&'a str> {
    object
        .get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| "input.manifest".to_owned())
}

fn uid_parts(value: &Value, rule: IdRule) -> Outcome<(EntityTypeName, Option<EntityId>)> {
    let object = object_with_keys(value, &["type", "id"])?;
    let type_name = string_field(object, "type")?;
    let id = match object.get("id") {
        None => None,
        Some(id) => Some(id.as_str().ok_or_else(|| "input.manifest".to_owned())?),
    };
    match (rule, id.is_some()) {
        (IdRule::Required, false) | (IdRule::Forbidden, true) => {
            return Err("input.manifest".to_owned())
        }
        _ => {}
    }
    let type_name = EntityTypeName::from_str(type_name).map_err(|_| "input.uid".to_owned())?;
    Ok((type_name, id.map(EntityId::new)))
}

fn concrete_uid(value: &Value) -> Outcome<EntityUid> {
    Ok(known_uid(uid_parts(value, IdRule::Required)?))
}

fn partial_uid(value: &Value) -> Outcome<PartialEntityUid> {
    let (type_name, id) = uid_parts(value, IdRule::Optional)?;
    Ok(PartialEntityUid::new(type_name, id))
}

fn context_for(
    request: &Map<String, Value>,
    schema_and_action: Option<(&Schema, &EntityUid)>,
) -> Outcome<Option<Context>> {
    match request.get("context") {
        None => Ok(None),
        Some(value) => Context::from_json_value(value.clone(), schema_and_action)
            .map(Some)
            .map_err(|_| "input.context".to_owned()),
    }
}

/// A concrete request: the batched request, and every completion's.
fn concrete_request(value: &Value, schema: &Schema) -> Outcome<Request> {
    let request = object_with_keys(value, &["principal", "action", "resource", "context"])?;
    let principal = concrete_uid(field(request, "principal")?)?;
    let action = concrete_uid(field(request, "action")?)?;
    let resource = concrete_uid(field(request, "resource")?)?;
    let context = context_for(request, Some((schema, &action)))?.unwrap_or_else(Context::empty);
    Request::new(principal, action, resource, context, None).map_err(|_| "input.request".to_owned())
}

fn field<'a>(object: &'a Map<String, Value>, key: &str) -> Outcome<&'a Value> {
    object.get(key).ok_or_else(|| "input.manifest".to_owned())
}

fn partial_authorize(inputs: &Inputs, schema: &Schema, policies: &PolicySet) -> Outcome<Value> {
    let request = object_with_keys(
        inputs.required("request")?,
        &["principal", "action", "resource", "context"],
    )?;
    let principal = partial_uid(field(request, "principal")?)?;
    let action = concrete_uid(field(request, "action")?)?;
    let resource = partial_uid(field(request, "resource")?)?;
    let context = context_for(request, Some((schema, &action)))?;
    let partial_request = PartialRequest::new(principal, action, resource, context, schema)
        .map_err(|error| format!("partial-request-creation.{}", partial_request_creation_error(&error)))?;
    let completions: Vec<&Map<String, Value>> = inputs
        .case
        .get("completions")
        .and_then(Value::as_array)
        .map(|completions| completions.iter().filter_map(Value::as_object).collect())
        .unwrap_or_default();
    let entities = inputs.partial_entities(schema)?;
    let response = policies
        .tpe(&partial_request, &entities, schema)
        .map_err(|error| format!("tpe.{}", tpe_error(&error)))?;

    let mut result = Map::new();
    result.insert("decision".to_owned(), decision_json(response.decision()));
    result.insert(
        "reason".to_owned(),
        match response.reason() {
            Some(reason) => sorted_ids(reason),
            None => Value::Null,
        },
    );
    result.insert("true-permits".to_owned(), sorted_ids(response.true_permits()));
    result.insert("false-permits".to_owned(), sorted_ids(response.false_permits()));
    result.insert("error-permits".to_owned(), sorted_ids(response.error_permits()));
    result.insert("residual-permits".to_owned(), sorted_ids(response.residual_permits()));
    result.insert("true-forbids".to_owned(), sorted_ids(response.true_forbids()));
    result.insert("false-forbids".to_owned(), sorted_ids(response.false_forbids()));
    result.insert("error-forbids".to_owned(), sorted_ids(response.error_forbids()));
    result.insert("residual-forbids".to_owned(), sorted_ids(response.residual_forbids()));
    result.insert("policies".to_owned(), policies_json(&response));

    let mut reauthorizations = Vec::new();
    for completion in completions {
        let entry = reauthorize(inputs, schema, &response, completion);
        reauthorizations.push(match entry {
            Ok(value) => value,
            Err(path) => json!({ "error": path }),
        });
    }
    result.insert("reauthorizations".to_owned(), Value::Array(reauthorizations));
    Ok(Value::Object(result))
}

fn policies_json(response: &TpeResponse<'_>) -> Value {
    let core: &cedar_policy_core::tpe::response::Response<'_> = response.as_ref();
    let mut policies = Map::new();
    for policy in response.policies() {
        let id = <PolicyId as AsRef<PolicyID>>::as_ref(policy.id());
        #[expect(clippy::expect_used, reason = "every policy of the response has a residual")]
        let residual = core
            .get_residual_policy(id)
            .expect("the response holds a residual for each of its policies");
        let annotations: Map<String, Value> = policy
            .annotations()
            .map(|(key, value)| (key.to_owned(), Value::String(value.to_owned())))
            .collect();
        let effect = match policy.effect() {
            Effect::Permit => "permit",
            Effect::Forbid => "forbid",
        };
        policies.insert(
            <PolicyId as AsRef<str>>::as_ref(policy.id()).to_owned(),
            json!({
                "annotations": annotations,
                "effect": effect,
                "residual": residual_json(residual.as_residual()),
            }),
        );
    }
    Value::Object(policies)
}

fn reauthorize(
    inputs: &Inputs,
    schema: &Schema,
    response: &TpeResponse<'_>,
    completion: &Map<String, Value>,
) -> Outcome<Value> {
    let request = concrete_request(field(completion, "request")?, schema)?;
    let entities = inputs.concrete_entities(field(completion, "entities")?, schema)?;
    let answer = response
        .reauthorize(&request, &entities)
        .map_err(|error| format!("tpe-reauthorization.{}", reauthorization_error(&error)))?;
    Ok(json!({
        "decision": decision_json(Some(answer.decision())),
        "determining": sorted_ids(answer.diagnostics().reason()),
        "erroring": sorted_ids(answer.diagnostics().errors().map(|error| match error {
            AuthorizationError::PolicyEvaluationError(error) => error.policy_id(),
        })),
    }))
}

/// Answers the batched loop from concrete entities, and records the uids of every call.
struct RecordingLoader<'a> {
    entities: &'a Entities,
    rounds: Vec<Value>,
}

impl EntityLoader for RecordingLoader<'_> {
    fn load_entities(&mut self, uids: &HashSet<EntityUid>) -> HashMap<EntityUid, Option<Entity>> {
        self.rounds.push(sorted_uids(uids.iter().cloned()));
        uids.iter()
            .map(|uid| (uid.clone(), self.entities.get(uid).cloned()))
            .collect()
    }
}

fn batched(inputs: &Inputs, schema: &Schema, policies: &PolicySet) -> Outcome<Value> {
    let maximum_iterations = inputs
        .required("maximum-iterations")?
        .as_u64()
        .and_then(|value| u32::try_from(value).ok())
        .ok_or_else(|| "input.manifest".to_owned())?;
    let request = concrete_request(inputs.required("request")?, schema)?;
    let entities = inputs.concrete_entities(inputs.required("entities")?, schema)?;
    let mut loader = RecordingLoader {
        entities: &entities,
        rounds: Vec::new(),
    };
    let answer = policies.is_authorized_batched(&request, schema, &mut loader, maximum_iterations);
    let rounds = Value::Array(loader.rounds);
    Ok(match answer {
        Ok(decision) => json!({"decision": decision_json(Some(decision)), "rounds": rounds}),
        Err(error) => json!({
            "error": format!("batched-evaluation.{}", batched_error(&error)),
            "rounds": rounds,
        }),
    })
}

/// The request of a principal or resource query: the queried side is a type only.
struct QueryParts {
    principal: (EntityTypeName, Option<EntityId>),
    action: EntityUid,
    resource: (EntityTypeName, Option<EntityId>),
    context: Context,
}

fn query_parts(
    inputs: &Inputs,
    schema: &Schema,
    principal_rule: IdRule,
    resource_rule: IdRule,
) -> Outcome<QueryParts> {
    let request = object_with_keys(
        inputs.required("request")?,
        &["principal", "action", "resource", "context"],
    )?;
    let principal = uid_parts(field(request, "principal")?, principal_rule)?;
    let action = concrete_uid(field(request, "action")?)?;
    let resource = uid_parts(field(request, "resource")?, resource_rule)?;
    let context = context_for(request, Some((schema, &action)))?.unwrap_or_else(Context::empty);
    Ok(QueryParts {
        principal,
        action,
        resource,
        context,
    })
}

fn known_uid((type_name, id): (EntityTypeName, Option<EntityId>)) -> EntityUid {
    #[expect(clippy::unwrap_used, reason = "IdRule::Required guarantees an id")]
    EntityUid::from_type_name_and_id(type_name, id.unwrap())
}

fn query_principal(inputs: &Inputs, schema: &Schema, policies: &PolicySet) -> Outcome<Value> {
    let parts = query_parts(inputs, schema, IdRule::Forbidden, IdRule::Required)?;
    let query = PrincipalQueryRequest::new(
        parts.principal.0,
        parts.action,
        known_uid(parts.resource),
        parts.context,
        schema,
    )
    .map_err(|error| format!("partial-request-creation.{}", partial_request_creation_error(&error)))?;
    let entities = inputs.concrete_entities(inputs.required("entities")?, schema)?;
    let uids = policies
        .query_principal(&query, &entities, schema)
        .map_err(|error| format!("permission-query.{}", permission_query_error(&error)))?;
    Ok(json!({ "uids": sorted_uids(uids) }))
}

fn query_resource(inputs: &Inputs, schema: &Schema, policies: &PolicySet) -> Outcome<Value> {
    let parts = query_parts(inputs, schema, IdRule::Required, IdRule::Forbidden)?;
    let query = ResourceQueryRequest::new(
        known_uid(parts.principal),
        parts.action,
        parts.resource.0,
        parts.context,
        schema,
    )
    .map_err(|error| format!("partial-request-creation.{}", partial_request_creation_error(&error)))?;
    let entities = inputs.concrete_entities(inputs.required("entities")?, schema)?;
    let uids = policies
        .query_resource(&query, &entities, schema)
        .map_err(|error| format!("permission-query.{}", permission_query_error(&error)))?;
    Ok(json!({ "uids": sorted_uids(uids) }))
}

fn query_action(inputs: &Inputs, schema: &Schema, policies: &PolicySet) -> Outcome<Value> {
    let request = object_with_keys(inputs.required("request")?, &["principal", "resource", "context"])?;
    let principal = partial_uid(field(request, "principal")?)?;
    let resource = partial_uid(field(request, "resource")?)?;
    let context = context_for(request, None)?;
    let query = ActionQueryRequest::new(principal, resource, context, schema.clone())
        .map_err(|error| format!("partial-request-creation.{}", partial_request_creation_error(&error)))?;
    let entities = inputs.partial_entities(schema)?;
    let answers = policies
        .query_action(&query, &entities)
        .map_err(|error| format!("permission-query.{}", permission_query_error(&error)))?;
    let mut actions: Vec<((String, String), Value)> = answers
        .map(|(action, decision)| {
            (
                uid_key(action),
                json!({"action": uid_json(action), "decision": decision_json(decision)}),
            )
        })
        .collect();
    actions.sort_by(|left, right| left.0.cmp(&right.0));
    Ok(json!({ "actions": actions.into_iter().map(|(_, value)| value).collect::<Vec<_>>() }))
}

fn decision_json(decision: Option<Decision>) -> Value {
    match decision {
        Some(Decision::Allow) => Value::from("allow"),
        Some(Decision::Deny) => Value::from("deny"),
        None => Value::Null,
    }
}

fn sorted_ids<'a>(ids: impl Iterator<Item = &'a PolicyId>) -> Value {
    let mut ids: Vec<String> = ids
        .map(|id| <PolicyId as AsRef<str>>::as_ref(id).to_owned())
        .collect();
    ids.sort();
    Value::from(ids)
}

fn uid_key(uid: &EntityUid) -> (String, String) {
    (uid.type_name().to_string(), uid.id().unescaped().to_owned())
}

fn uid_json(uid: &EntityUid) -> Value {
    let (type_name, id) = uid_key(uid);
    json!({"id": id, "type": type_name})
}

fn sorted_uids(uids: impl IntoIterator<Item = EntityUid>) -> Value {
    let mut keyed: Vec<((String, String), Value)> = uids
        .into_iter()
        .map(|uid| (uid_key(&uid), uid_json(&uid)))
        .collect();
    keyed.sort_by(|left, right| left.0.cmp(&right.0));
    Value::Array(keyed.into_iter().map(|(_, value)| value).collect())
}

// Residual trees.

fn residual_json(residual: &Residual) -> Value {
    match residual {
        Residual::Concrete { value, .. } => {
            #[expect(clippy::expect_used, reason = "a residual's concrete value holds no unknowns")]
            let value = CedarValueJson::from_value(value.clone())
                .expect("a concrete residual value converts to Cedar JSON");
            #[expect(clippy::expect_used, reason = "Cedar JSON values serialize")]
            let value = serde_json::to_value(value).expect("Cedar JSON values serialize");
            json!({"kind": "concrete", "value": value})
        }
        Residual::Error(_) => json!({"kind": "error"}),
        Residual::Partial { kind, .. } => kind_json(kind),
    }
}

fn kind_json(kind: &ResidualKind) -> Value {
    match kind {
        ResidualKind::Var(var) => json!({"kind": "var", "var": debug_variant(var)}),
        ResidualKind::If {
            test_expr,
            then_expr,
            else_expr,
        } => json!({
            "kind": "if",
            "test_expr": residual_json(test_expr),
            "then_expr": residual_json(then_expr),
            "else_expr": residual_json(else_expr),
        }),
        ResidualKind::And { left, right } => json!({
            "kind": "and", "left": residual_json(left), "right": residual_json(right),
        }),
        ResidualKind::Or { left, right } => json!({
            "kind": "or", "left": residual_json(left), "right": residual_json(right),
        }),
        ResidualKind::UnaryApp { op, arg } => json!({
            "kind": "unary-app", "op": debug_variant(op), "arg": residual_json(arg),
        }),
        ResidualKind::BinaryApp { op, arg1, arg2 } => json!({
            "kind": "binary-app",
            "op": debug_variant(op),
            "arg1": residual_json(arg1),
            "arg2": residual_json(arg2),
        }),
        ResidualKind::ExtensionFunctionApp { fn_name, args } => json!({
            "kind": "extension-function-app",
            "fn_name": fn_name.to_string(),
            "args": args.iter().map(residual_json).collect::<Vec<_>>(),
        }),
        ResidualKind::GetAttr { expr, attr } => json!({
            "kind": "get-attribute", "expr": residual_json(expr), "attr": attr.as_str(),
        }),
        ResidualKind::HasAttr { expr, attr } => json!({
            "kind": "has-attribute", "expr": residual_json(expr), "attr": attr.as_str(),
        }),
        ResidualKind::ExtHasAttr { expr, attrs } => json!({
            "kind": "extension-has-attribute",
            "expr": residual_json(expr),
            "attrs": attrs.iter().map(|attr| attr.as_str()).collect::<Vec<_>>(),
        }),
        ResidualKind::Like { expr, pattern } => json!({
            "kind": "like",
            "expr": residual_json(expr),
            "pattern": pattern
                .iter()
                .map(|element| match element {
                    PatternElem::Char(character) => Value::String(character.to_string()),
                    PatternElem::Wildcard => Value::Null,
                })
                .collect::<Vec<_>>(),
        }),
        ResidualKind::Is { expr, entity_type } => json!({
            "kind": "is", "expr": residual_json(expr), "entity_type": entity_type.to_string(),
        }),
        ResidualKind::Set(elements) => json!({
            "kind": "set",
            "elements": elements.iter().map(residual_json).collect::<Vec<_>>(),
        }),
        ResidualKind::Record(fields) => json!({
            "kind": "record",
            "fields": fields
                .iter()
                .map(|(key, value)| (key.to_string(), residual_json(value)))
                .collect::<Map<String, Value>>(),
        }),
    }
}

// Error paths.

/// Splits a Rust identifier into the path-segment form described at the top of this file.
fn segment(identifier: &str) -> String {
    let characters: Vec<char> = identifier.chars().collect();
    let mut words: Vec<String> = Vec::new();
    let mut current = String::new();
    for (index, &character) in characters.iter().enumerate() {
        if character.is_ascii_uppercase() && !current.is_empty() {
            let previous = characters[index - 1];
            let next_is_lowercase = characters
                .get(index + 1)
                .is_some_and(char::is_ascii_lowercase);
            if previous.is_ascii_lowercase()
                || previous.is_ascii_digit()
                || (previous.is_ascii_uppercase() && next_is_lowercase)
            {
                words.push(std::mem::take(&mut current));
            }
        }
        current.push(character.to_ascii_lowercase());
    }
    if !current.is_empty() {
        words.push(current);
    }
    words
        .iter()
        .map(|word| match word.as_str() {
            "attr" => "attribute",
            "attrs" => "attributes",
            "env" => "environment",
            "eval" => "evaluation",
            "expr" => "expression",
            "ext" => "extension",
            "req" => "request",
            "tc" => "transitive-closure",
            other => other,
        })
        .collect::<Vec<_>>()
        .join("-")
}

/// The segment of an enum value's variant, read from its derived `Debug` output, which begins with
/// the variant's identifier.
fn debug_variant<T: Debug>(value: &T) -> String {
    let text = format!("{value:?}");
    let identifier: String = text
        .chars()
        .take_while(|character| character.is_ascii_alphanumeric() || *character == '_')
        .collect();
    segment(&identifier)
}

fn tpe_error(error: &tpe_err::TpeError) -> String {
    match error {
        tpe_err::TpeError::ExprToResidualError(inner) => {
            format!("{}.{}", debug_variant(error), debug_variant(inner))
        }
        _ => debug_variant(error),
    }
}

fn entities_error(error: &tpe_err::EntitiesError) -> String {
    let head = debug_variant(error);
    match error {
        tpe_err::EntitiesError::Deserialization(inner) => format!("{head}.{}", debug_variant(inner)),
        tpe_err::EntitiesError::Validation(inner) => match inner {
            tpe_err::EntityValidationError::Concrete(conformance) => {
                format!("{head}.{}.{}", debug_variant(inner), debug_variant(conformance))
            }
            _ => format!("{head}.{}", debug_variant(inner)),
        },
        tpe_err::EntitiesError::TCComputation(inner) => format!("{head}.{}", debug_variant(inner)),
        tpe_err::EntitiesError::PartialValueToValue(inner) => {
            format!("{head}.{}", debug_variant(inner))
        }
        _ => head,
    }
}

fn partial_request_creation_error(error: &PartialRequestCreationError) -> String {
    match error {
        PartialRequestCreationError::Validation(inner) => {
            format!("{}.{}", debug_variant(error), debug_variant(inner))
        }
        _ => debug_variant(error),
    }
}

fn reauthorization_error(error: &TpeReauthorizationError) -> String {
    let head = debug_variant(error);
    match error {
        TpeReauthorizationError::RequestValidation(inner) => format!("{head}.{}", debug_variant(inner)),
        TpeReauthorizationError::EntityValidation(inner) => format!("{head}.{}", debug_variant(inner)),
        TpeReauthorizationError::InconsistentEntities(inner) => match inner {
            tpe_err::EntitiesConsistencyError::InconsistentEntity(entity) => {
                format!("{head}.{}.{}", debug_variant(inner), debug_variant(entity))
            }
            _ => format!("{head}.{}", debug_variant(inner)),
        },
        TpeReauthorizationError::InconsistentRequests(inner) => {
            format!("{head}.{}", debug_variant(inner))
        }
    }
}

fn batched_error(error: &BatchedEvalError) -> String {
    let head = debug_variant(error);
    match error {
        BatchedEvalError::TPE(inner) => format!("{head}.{}", tpe_error(inner)),
        BatchedEvalError::RequestValidation(inner) => format!("{head}.{}", debug_variant(inner)),
        BatchedEvalError::Entities(inner) => format!("{head}.{}", entities_error(inner)),
        BatchedEvalError::PartialValueToValue(inner) => format!("{head}.{}", debug_variant(inner)),
        _ => head,
    }
}

fn permission_query_error(error: &PermissionQueryError) -> String {
    let head = debug_variant(error);
    match error {
        PermissionQueryError::Entities(inner) => format!("{head}.{}", entities_error(inner)),
        PermissionQueryError::TPE(inner) => format!("{head}.{}", tpe_error(inner)),
    }
}

// Canonical JSON.

fn write_canonical(value: &Value, output: &mut String) {
    match value {
        Value::Null => output.push_str("null"),
        Value::Bool(flag) => output.push_str(if *flag { "true" } else { "false" }),
        Value::Number(number) => output.push_str(&number.to_string()),
        Value::String(text) => write_string(text, output),
        Value::Array(elements) => {
            output.push('[');
            for (index, element) in elements.iter().enumerate() {
                if index > 0 {
                    output.push(',');
                }
                write_canonical(element, output);
            }
            output.push(']');
        }
        Value::Object(object) => {
            let mut entries: Vec<(&String, &Value)> = object.iter().collect();
            entries.sort_by(|left, right| left.0.as_bytes().cmp(right.0.as_bytes()));
            output.push('{');
            for (index, (key, element)) in entries.into_iter().enumerate() {
                if index > 0 {
                    output.push(',');
                }
                write_string(key, output);
                output.push(':');
                write_canonical(element, output);
            }
            output.push('}');
        }
    }
}

fn write_string(text: &str, output: &mut String) {
    #[expect(clippy::expect_used, reason = "a string always serializes")]
    output.push_str(&serde_json::to_string(text).expect("a string serializes"));
}
