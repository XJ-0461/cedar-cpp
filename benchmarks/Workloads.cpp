// The cedar-cpp benchmark: the workloads of benchmarks/rust-reference/benches/workloads.rs, with the
// same benchmark names and the same timed regions, through cedar-cpp.
//
// A build with CedarCpp_BuildBenchmarks generates the workloads into <build>/benchmarks/workloads.
// Run:
//
//   nice -n 10 <build>/benchmarks/cedar-cpp-benchmarks \
//       --workloads-directory=<build>/benchmarks/workloads \
//       --expected-decisions=benchmarks/workloads/expected-decisions.json \
//       --benchmark_min_warmup_time=2 --benchmark_min_time=5s
//
// Google Benchmark's own flags (--benchmark_filter and the rest) are accepted as well.
//
// Every workload file is read and every input not under measurement is built before the timed
// region. The timed regions are exactly these library calls:
//   authorization/<application>/<N>     IsAuthorized with PolicySelection::EvaluateEveryPolicy once
//                                       per request, over the whole request list;
//                                       one iteration is the whole list (items_per_second is the
//                                       call rate)
//   authorization-index/<application>/<N>
//                                       the same with PolicySelection::ScopeIndex, the index built
//                                       before the timed region (by the decision check)
//   entity-loading/<application>/without-schema
//   entity-loading/<application>/with-schema
//                                       EntitiesFromJson on `entities-large.json`, text already in
//                                       memory, schema already parsed
//   policy-set-parsing/<application>    ParsePolicySet on `policies-1000.cedar`
//   validation/<application>            Validator::Validate in strict mode on the 1,000-policy set
//                                       with its links, validator already built

#include <cedar/Authorizer.hpp>
#include <cedar/Entities.hpp>
#include <cedar/Json.hpp>
#include <cedar/PolicySet.hpp>
#include <cedar/Request.hpp>
#include <cedar/Schema.hpp>
#include <cedar/Syntax.hpp>
#include <cedar/Validator.hpp>

#include <benchmark/benchmark.h>
#include <simdjson.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

constexpr std::array<std::string_view, 8> Applications{
    "document_cloud", "github_example", "hotel_chains", "sales_orgs",
    "streaming_service", "tags_n_roles", "tax_preparer", "tinytodo",
};
constexpr std::array<std::size_t, 3> PolicySetSizes{10, 100, 1000};
constexpr std::array<std::string_view, 2> LargeEntityApplications{"document_cloud", "github_example"};
// Its 1,000 policies are template links; its policy text is three policies.
constexpr std::string_view LinkOnlyApplication{"tax_preparer"};

[[noreturn]] auto Fail(const std::string_view message) -> void
{
    std::println(stderr, "cedar-cpp-benchmarks: {}", message);
    std::exit(EXIT_FAILURE);
}

auto ReadFile(const std::filesystem::path& path) -> std::string
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) { Fail(std::format("{}: cannot be read", path.string())); }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return std::move(contents).str();
}

/// The value, or the program stops with the error's diagnostics and `what`.
template <typename Value, typename Error>
auto Require(std::expected<Value, Error> result, const std::string_view what) -> Value
{
    if (!result) { Fail(std::format("{}:\n{}", what, cedar::RenderDiagnostics(result.error().Diagnostics()))); }
    if constexpr (!std::is_void_v<Value>) { return std::move(*result); }
}

/// A parsed JSON document. Elements are views into the parser, so it is kept with them.
class JsonDocument final
{
public:
    explicit JsonDocument(const std::filesystem::path& path) : path_{path.string()}
    {
        const simdjson::padded_string text{ReadFile(path)};
        if (parser_.parse(text).get(root_) != simdjson::SUCCESS) { Fail(std::format("{}: not JSON", path_)); }
    }

    [[nodiscard]] auto Root() const noexcept -> simdjson::dom::element { return root_; }
    [[nodiscard]] auto Path() const noexcept -> std::string_view { return path_; }

private:
    std::string path_;
    simdjson::dom::parser parser_;
    simdjson::dom::element root_;
};

auto JsonString(const simdjson::dom::element element, const std::string_view where) -> std::string_view
{
    std::string_view text;
    if (element.get_string().get(text) != simdjson::SUCCESS) { Fail(std::format("{}: expected a string", where)); }
    return text;
}

auto JsonArray(const simdjson::dom::element element, const std::string_view where) -> simdjson::dom::array
{
    simdjson::dom::array array;
    if (element.get_array().get(array) != simdjson::SUCCESS) { Fail(std::format("{}: expected a list", where)); }
    return array;
}

auto JsonField(const simdjson::dom::element element, const std::string_view field, const std::string_view where)
    -> simdjson::dom::element
{
    simdjson::dom::element value;
    if (element.at_key(field).get(value) != simdjson::SUCCESS) { Fail(std::format("{}: no `{}`", where, field)); }
    return value;
}

auto Uid(const std::string_view text) -> cedar::EntityUid
{
    return Require(cedar::ParseEntityUid(text), std::format("entity uid {}", text));
}

auto LoadSchema(const std::filesystem::path& directory) -> cedar::Schema
{
    const std::filesystem::path path = directory / "schema.cedarschema";
    return Require(cedar::ParseSchema(ReadFile(path), path.string()), path.string()).value;
}

auto LoadPolicySet(const std::filesystem::path& directory, const std::size_t size) -> cedar::PolicySet
{
    const std::filesystem::path path = directory / std::format("policies-{}.cedar", size);
    cedar::PolicySet policies = Require(cedar::ParsePolicySet(ReadFile(path), path.string()), path.string());
    const JsonDocument links{directory / std::format("links-{}.json", size)};
    for (const simdjson::dom::element link : JsonArray(links.Root(), links.Path()))
    {
        cedar::SlotBindings bindings;
        simdjson::dom::object arguments;
        if (JsonField(link, "args", links.Path()).get_object().get(arguments) != simdjson::SUCCESS)
        {
            Fail(std::format("{}: link arguments are not an object", links.Path()));
        }
        for (const auto [slot, value] : arguments)
        {
            const cedar::EntityUid uid = Uid(JsonString(value, links.Path()));
            if (slot == "?principal") { bindings.Bind(cedar::SlotId::Principal, uid); }
            else if (slot == "?resource") { bindings.Bind(cedar::SlotId::Resource, uid); }
            else { Fail(std::format("{}: unknown slot {}", links.Path(), slot)); }
        }
        Require(policies.Link(JsonString(JsonField(link, "template_id", links.Path()), links.Path()),
                              cedar::PolicyId{JsonString(JsonField(link, "link_id", links.Path()), links.Path())},
                              std::move(bindings)),
                links.Path());
    }
    return policies;
}

auto LoadRequests(const std::filesystem::path& directory, const cedar::Schema& schema) -> std::vector<cedar::Request>
{
    const JsonDocument requests{directory / "requests.json"};
    const std::string_view where = requests.Path();
    std::vector<cedar::Request> loaded;
    for (const simdjson::dom::element request : JsonArray(requests.Root(), where))
    {
        cedar::EntityUid action = Uid(JsonString(JsonField(request, "action", where), where));
        const std::string contextText = simdjson::minify(JsonField(request, "context", where));
        cedar::Context context = Require(cedar::ContextFromJson(contextText, schema, action), where);
        loaded.push_back(Require(cedar::Request::Create(Uid(JsonString(JsonField(request, "principal", where), where)),
                                                        std::move(action),
                                                        Uid(JsonString(JsonField(request, "resource", where), where)),
                                                        std::move(context), schema),
                                 where));
    }
    return loaded;
}

auto DecisionName(const cedar::Decision decision) -> std::string_view
{
    return decision == cedar::Decision::Allow ? "allow" : "deny";
}

constexpr std::array<cedar::PolicySelection, 2> Selections{cedar::PolicySelection::EvaluateEveryPolicy,
                                                           cedar::PolicySelection::ScopeIndex};

auto BenchmarkPrefix(const cedar::PolicySelection selection) -> std::string_view
{
    return selection == cedar::PolicySelection::ScopeIndex ? "authorization-index" : "authorization";
}

struct AuthorizationWorkload
{
    cedar::Entities entities;
    std::vector<cedar::Request> requests;
    std::vector<cedar::PolicySet> policySets;
};

auto RegisterAuthorization(const std::filesystem::path& workloads, const JsonDocument& expected) -> void
{
    for (const std::string_view application : Applications)
    {
        const std::filesystem::path directory = workloads / application;
        const cedar::Schema schema = LoadSchema(directory);
        const std::filesystem::path entitiesPath = directory / "entities.json";
        auto workload = std::make_shared<AuthorizationWorkload>(AuthorizationWorkload{
            .entities = Require(cedar::EntitiesFromJson(ReadFile(entitiesPath), schema, entitiesPath.string()),
                                entitiesPath.string()),
            .requests = LoadRequests(directory, schema),
            .policySets = {},
        });
        for (const std::size_t size : PolicySetSizes)
        {
            const std::string name = std::format("{} at {}", application, size);
            cedar::PolicySet policies = LoadPolicySet(directory, size);
            if (policies.PolicyCount() != size)
            {
                Fail(std::format("{}: {} policies, expected {}", name, policies.PolicyCount(), size));
            }
            simdjson::dom::array wanted;
            if (expected.Root().at_key(application).at_key(std::to_string(size)).get_array().get(wanted)
                != simdjson::SUCCESS)
            {
                Fail(std::format("no expected decisions for {}", name));
            }
            if (wanted.size() != workload->requests.size())
            {
                Fail(std::format("{}: {} expected decisions for {} requests", name, wanted.size(),
                                 workload->requests.size()));
            }
            for (const cedar::PolicySelection selection : Selections)
            {
                std::size_t index = 0;
                for (const simdjson::dom::element decision : wanted)
                {
                    const cedar::Response response = cedar::IsAuthorized(
                        workload->requests[index], policies, workload->entities, {.policySelection = selection});
                    if (DecisionName(response.decision) != JsonString(decision, expected.Path()))
                    {
                        Fail(std::format("{} ({}): request {} decides {}, expected {}", name, BenchmarkPrefix(selection),
                                         index, DecisionName(response.decision), JsonString(decision, expected.Path())));
                    }
                    ++index;
                }
            }
            workload->policySets.push_back(std::move(policies));
            const std::size_t position = workload->policySets.size() - 1;
            for (const cedar::PolicySelection selection : Selections)
            {
                benchmark::RegisterBenchmark(
                    std::format("{}/{}/{}", BenchmarkPrefix(selection), application, size),
                    [workload, position, selection](benchmark::State& state) {
                        const cedar::PolicySet& set = workload->policySets[position];
                        const cedar::AuthorizerOptions options{.policySelection = selection};
                        for (auto _ : state)
                        {
                            for (const cedar::Request& request : workload->requests)
                            {
                                benchmark::DoNotOptimize(cedar::IsAuthorized(request, set, workload->entities, options));
                            }
                        }
                        state.SetItemsProcessed(state.iterations()
                                                * static_cast<std::int64_t>(workload->requests.size()));
                    })
                    ->Unit(benchmark::kMicrosecond);
            }
        }
    }
}

struct EntityLoadingWorkload
{
    cedar::Schema schema;
    std::string text;
};

auto RegisterEntityLoading(const std::filesystem::path& workloads) -> void
{
    for (const std::string_view application : LargeEntityApplications)
    {
        const std::filesystem::path directory = workloads / application;
        auto workload = std::make_shared<EntityLoadingWorkload>(
            EntityLoadingWorkload{.schema = LoadSchema(directory), .text = ReadFile(directory / "entities-large.json")});
        Require(cedar::EntitiesFromJson(workload->text), std::format("{} entities", application));
        Require(cedar::EntitiesFromJson(workload->text, workload->schema), std::format("{} entities", application));
        benchmark::RegisterBenchmark(std::format("entity-loading/{}/without-schema", application),
                                     [workload](benchmark::State& state) {
                                         for (auto _ : state)
                                         {
                                             benchmark::DoNotOptimize(cedar::EntitiesFromJson(workload->text));
                                         }
                                     })
            ->Unit(benchmark::kMillisecond);
        benchmark::RegisterBenchmark(std::format("entity-loading/{}/with-schema", application),
                                     [workload](benchmark::State& state) {
                                         for (auto _ : state)
                                         {
                                             benchmark::DoNotOptimize(
                                                 cedar::EntitiesFromJson(workload->text, workload->schema));
                                         }
                                     })
            ->Unit(benchmark::kMillisecond);
    }
}

auto RegisterPolicySetParsing(const std::filesystem::path& workloads) -> void
{
    for (const std::string_view application : Applications)
    {
        if (application == LinkOnlyApplication) { continue; }
        auto text = std::make_shared<const std::string>(ReadFile(workloads / application / "policies-1000.cedar"));
        Require(cedar::ParsePolicySet(*text), std::format("{} policies", application));
        benchmark::RegisterBenchmark(std::format("policy-set-parsing/{}", application),
                                     [text](benchmark::State& state) {
                                         for (auto _ : state) { benchmark::DoNotOptimize(cedar::ParsePolicySet(*text)); }
                                     })
            ->Unit(benchmark::kMillisecond);
    }
}

struct ValidationWorkload
{
    cedar::Validator validator;
    cedar::PolicySet policies;
};

auto RegisterValidation(const std::filesystem::path& workloads) -> void
{
    for (const std::string_view application : Applications)
    {
        const std::filesystem::path directory = workloads / application;
        auto workload = std::make_shared<ValidationWorkload>(
            ValidationWorkload{.validator = cedar::Validator{LoadSchema(directory)},
                               .policies = LoadPolicySet(directory, 1000)});
        if (!workload->validator.Validate(workload->policies, cedar::ValidationMode::Strict).Passed())
        {
            Fail(std::format("{}: the 1,000-policy set does not validate", application));
        }
        benchmark::RegisterBenchmark(std::format("validation/{}", application),
                                     [workload](benchmark::State& state) {
                                         for (auto _ : state)
                                         {
                                             benchmark::DoNotOptimize(workload->validator.Validate(
                                                 workload->policies, cedar::ValidationMode::Strict));
                                         }
                                     })
            ->Unit(benchmark::kMillisecond);
    }
}

/// Removes `--<name>=<value>` from the arguments and returns the value.
auto TakeFlag(int& argumentCount, char** arguments, const std::string_view name) -> std::optional<std::string>
{
    const std::string prefix = std::format("--{}=", name);
    std::optional<std::string> value;
    int kept = 1;
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::string_view argument{arguments[index]};
        if (argument.starts_with(prefix)) { value = std::string{argument.substr(prefix.size())}; }
        else { arguments[kept++] = arguments[index]; }
    }
    argumentCount = kept;
    return value;
}

} // namespace

auto main(int argumentCount, char** arguments) -> int
{
    benchmark::Initialize(&argumentCount, arguments);
    const std::optional<std::string> workloads = TakeFlag(argumentCount, arguments, "workloads-directory");
    const std::optional<std::string> expected = TakeFlag(argumentCount, arguments, "expected-decisions");
    if (benchmark::ReportUnrecognizedArguments(argumentCount, arguments)) { return EXIT_FAILURE; }
    if (!workloads || !expected) { Fail("pass --workloads-directory=<directory> and --expected-decisions=<file>"); }

    const JsonDocument expectedDecisions{*expected};
    RegisterAuthorization(*workloads, expectedDecisions);
    RegisterEntityLoading(*workloads);
    RegisterPolicySetParsing(*workloads);
    RegisterValidation(*workloads);

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return EXIT_SUCCESS;
}
