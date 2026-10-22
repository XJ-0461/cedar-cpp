// The Rust reference side of the cedar-cpp benchmark: the same workloads, through `cedar-policy`.
//
// Generate the workloads, then run (from this directory):
//
//   python3 -I ../../scripts/generate-benchmark-workloads.py \
//       --cedar-examples-directory <cedar>/cedar-policy-symcc/tests/data/cedar-examples \
//       --output-directory /var/tmp/cedar-bench-workloads
//   CEDAR_BENCHMARK_WORKLOADS_DIRECTORY=/var/tmp/cedar-bench-workloads \
//   CEDAR_BENCHMARK_EXPECTED_DECISIONS=../workloads/expected-decisions.json \
//   CARGO_TARGET_DIR=/var/tmp/cedar-bench-rust-target \
//       nice -n 10 cargo bench --offline -j 4 --bench workloads
//
// Criterion arguments go after `--`, for example `-- authorization/tinytodo`.
//
// Every workload file is read and every input not under measurement is built before the timed
// region. The timed regions are exactly these library calls:
//   authorization/<application>/<N>     `Authorizer::is_authorized` once per request, over the
//                                       whole request list; one iteration is the whole list, so
//                                       the time per call is the iteration time over the request
//                                       count (reported as the element throughput)
//   entity-loading/<application>/without-schema
//   entity-loading/<application>/with-schema
//                                       `Entities::from_json_str` on `entities-large.json`, text
//                                       already in memory, schema already parsed
//   policy-set-parsing/<application>    `PolicySet::from_str` on `policies-1000.cedar`
//   validation/<application>            `Validator::validate` in strict mode on the 1,000-policy
//                                       set with its links, validator already built

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::time::Duration;

use cedar_policy::{
    Authorizer, Context, Decision, Entities, EntityUid, PolicyId, PolicySet, Request, Schema,
    SlotId, ValidationMode, Validator,
};
use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion, Throughput};

const APPLICATIONS: [&str; 8] = [
    "document_cloud",
    "github_example",
    "hotel_chains",
    "sales_orgs",
    "streaming_service",
    "tags_n_roles",
    "tax_preparer",
    "tinytodo",
];
const POLICY_SET_SIZES: [usize; 3] = [10, 100, 1000];
const LARGE_ENTITY_APPLICATIONS: [&str; 2] = ["document_cloud", "github_example"];
// Its 1,000 policies are template links; its policy text is three policies.
const LINK_ONLY_APPLICATION: &str = "tax_preparer";

fn required_path(variable: &str) -> PathBuf {
    match std::env::var_os(variable) {
        Some(value) => PathBuf::from(value),
        None => panic!("set {variable}"),
    }
}

fn read(path: &Path) -> String {
    std::fs::read_to_string(path).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
}

fn read_json(path: &Path) -> serde_json::Value {
    serde_json::from_str(&read(path)).unwrap_or_else(|error| panic!("{}: {error}", path.display()))
}

fn load_schema(directory: &Path) -> Schema {
    let path = directory.join("schema.cedarschema");
    match Schema::from_cedarschema_str(&read(&path)) {
        Ok((schema, _warnings)) => schema,
        Err(error) => panic!("{}: {error}", path.display()),
    }
}

fn uid(text: &str) -> EntityUid {
    EntityUid::from_str(text).unwrap_or_else(|error| panic!("{text}: {error}"))
}

fn load_policy_set(directory: &Path, size: usize) -> PolicySet {
    let path = directory.join(format!("policies-{size}.cedar"));
    let mut policies =
        PolicySet::from_str(&read(&path)).unwrap_or_else(|error| panic!("{}: {error}", path.display()));
    let links_path = directory.join(format!("links-{size}.json"));
    let links = read_json(&links_path);
    for link in links.as_array().expect("links are a list") {
        let mut arguments = HashMap::new();
        for (slot, value) in link["args"].as_object().expect("link arguments are an object") {
            let slot_id = match slot.as_str() {
                "?principal" => SlotId::principal(),
                "?resource" => SlotId::resource(),
                other => panic!("unknown slot {other}"),
            };
            arguments.insert(slot_id, uid(value.as_str().expect("slot values are strings")));
        }
        policies
            .link(
                PolicyId::new(link["template_id"].as_str().expect("template id")),
                PolicyId::new(link["link_id"].as_str().expect("link id")),
                arguments,
            )
            .unwrap_or_else(|error| panic!("{}: {error}", links_path.display()));
    }
    policies
}

fn load_requests(directory: &Path, schema: &Schema) -> Vec<Request> {
    let path = directory.join("requests.json");
    let requests = read_json(&path);
    requests
        .as_array()
        .expect("requests are a list")
        .iter()
        .map(|request| {
            let text = |field: &str| request[field].as_str().expect("uids are strings").to_owned();
            let action = uid(&text("action"));
            let context = Context::from_json_value(request["context"].clone(), Some((schema, &action)))
                .unwrap_or_else(|error| panic!("{}: {error}", path.display()));
            Request::new(uid(&text("principal")), action, uid(&text("resource")), context, Some(schema))
                .unwrap_or_else(|error| panic!("{}: {error}", path.display()))
        })
        .collect()
}

fn decision_name(decision: Decision) -> &'static str {
    match decision {
        Decision::Allow => "allow",
        Decision::Deny => "deny",
    }
}

fn authorization(criterion: &mut Criterion, workloads: &Path, expected: &serde_json::Value) {
    let authorizer = Authorizer::new();
    for application in APPLICATIONS {
        let directory = workloads.join(application);
        let schema = load_schema(&directory);
        let entities = Entities::from_json_str(&read(&directory.join("entities.json")), Some(&schema))
            .unwrap_or_else(|error| panic!("{application} entities: {error}"));
        let requests = load_requests(&directory, &schema);
        let mut group = criterion.benchmark_group(format!("authorization/{application}"));
        group.throughput(Throughput::Elements(requests.len() as u64));
        for size in POLICY_SET_SIZES {
            let policies = load_policy_set(&directory, size);
            assert_eq!(policies.policies().count(), size, "{application} at {size}");
            let decisions: Vec<&str> = requests
                .iter()
                .map(|request| decision_name(authorizer.is_authorized(request, &policies, &entities).decision()))
                .collect();
            let wanted: Vec<&str> = expected[application][size.to_string()]
                .as_array()
                .unwrap_or_else(|| panic!("no expected decisions for {application} at {size}"))
                .iter()
                .map(|decision| decision.as_str().expect("decisions are strings"))
                .collect();
            assert_eq!(decisions, wanted, "{application} at {size}");
            group.bench_with_input(BenchmarkId::from_parameter(size), &policies, |bencher, policies| {
                bencher.iter(|| {
                    for request in &requests {
                        std::hint::black_box(authorizer.is_authorized(request, policies, &entities));
                    }
                });
            });
        }
        group.finish();
    }
}

fn entity_loading(criterion: &mut Criterion, workloads: &Path) {
    for application in LARGE_ENTITY_APPLICATIONS {
        let directory = workloads.join(application);
        let schema = load_schema(&directory);
        let text = read(&directory.join("entities-large.json"));
        let mut group = criterion.benchmark_group(format!("entity-loading/{application}"));
        group.bench_function("without-schema", |bencher| {
            bencher.iter(|| Entities::from_json_str(std::hint::black_box(&text), None).expect("entities load"));
        });
        group.bench_function("with-schema", |bencher| {
            bencher.iter(|| {
                Entities::from_json_str(std::hint::black_box(&text), Some(&schema)).expect("entities load")
            });
        });
        group.finish();
    }
}

fn policy_set_parsing(criterion: &mut Criterion, workloads: &Path) {
    let mut group = criterion.benchmark_group("policy-set-parsing");
    for application in APPLICATIONS {
        if application == LINK_ONLY_APPLICATION {
            continue;
        }
        let text = read(&workloads.join(application).join("policies-1000.cedar"));
        group.bench_function(application, |bencher| {
            bencher.iter(|| PolicySet::from_str(std::hint::black_box(&text)).expect("policies parse"));
        });
    }
    group.finish();
}

fn validation(criterion: &mut Criterion, workloads: &Path) {
    let mut group = criterion.benchmark_group("validation");
    for application in APPLICATIONS {
        let directory = workloads.join(application);
        let validator = Validator::new(load_schema(&directory));
        let policies = load_policy_set(&directory, 1000);
        assert!(validator.validate(&policies, ValidationMode::Strict).validation_passed(), "{application}");
        group.bench_function(application, |bencher| {
            bencher.iter(|| validator.validate(std::hint::black_box(&policies), ValidationMode::Strict));
        });
    }
    group.finish();
}

fn workloads(criterion: &mut Criterion) {
    let workloads = required_path("CEDAR_BENCHMARK_WORKLOADS_DIRECTORY");
    let expected = read_json(&required_path("CEDAR_BENCHMARK_EXPECTED_DECISIONS"));
    authorization(criterion, &workloads, &expected);
    entity_loading(criterion, &workloads);
    policy_set_parsing(criterion, &workloads);
    validation(criterion, &workloads);
}

criterion_group! {
    name = benches;
    config = Criterion::default().warm_up_time(Duration::from_secs(2)).measurement_time(Duration::from_secs(5)).without_plots();
    targets = workloads
}
criterion_main!(benches);
