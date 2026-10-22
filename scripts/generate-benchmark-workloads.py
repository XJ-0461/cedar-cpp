#!/usr/bin/env python3
"""Generates the benchmark workloads that the C++ suite and the Rust reference crate both read.

For each of the eight cedar-examples applications under --cedar-examples-directory, writes
`<output-directory>/<application>/`:
  schema.cedarschema       the application's schema, copied;
  policies-<N>.cedar       a policy set of N policies, for N in 10, 100 and 1000;
  links-<N>.json           the template links of that set, in the CLI's `--template-linked` format
                           (an empty list for applications without templates);
  entities.json            a few hundred entities conforming to the schema, with the deepest
                           hierarchy the schema allows (up to five levels);
  requests.json            a list of requests valid per the schema, each
                           `{"principal", "action", "resource", "context"}` with entity uids written
                           as Cedar uid strings and the context in the schema-based JSON form;
Two applications also get `entities-large.json`, about ten thousand entities, for entity loading.

Policy sets: every `@id` annotation is removed, so policy ids are the parser's positional
`policy<K>` under both `PolicySet::from_str` and the CLI. A set of N policies holds the
application's static policies (when there are more than N, all forbid policies and then permits in
source order) and N minus that many generated policies. An application with a template gets
template links, each binding its slots to uids drawn from the entity set. An application without
one gets replicas of its static policies: replica r of static policy i is policy i with one scope
variable narrowed to a uid from the entity set (a bare `principal` or `principal is T` becomes
`principal == T::"uid"`; otherwise the resource is narrowed the same way), each (policy, uid) pair
used once.

Everything is drawn from a fixed seed, so the output is identical on every run. With --oracle
(the reference CLI), every entity file is loaded with and without the schema, every policy set is
validated against its schema (warnings fail), every request is authorized against every set, and
the decisions are written to --expected-decisions-output as
`{"<application>": {"<N>": ["allow" | "deny", ...]}}`, one entry per request in order.
"""

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile

APPLICATIONS = [
    "document_cloud",
    "github_example",
    "hotel_chains",
    "sales_orgs",
    "streaming_service",
    "tags_n_roles",
    "tax_preparer",
    "tinytodo",
]
POLICY_SET_SIZES = [10, 100, 1000]
REQUEST_COUNT = 50
LARGE_ENTITY_APPLICATIONS = {"document_cloud": 33, "github_example": 40}
SEED = 20261022


def uid(entity_type, entity_id):
    return {"type": entity_type, "id": entity_id}


def reference(entity_type, entity_id):
    return {"__entity": uid(entity_type, entity_id)}


def extension(function, argument):
    return {"__extn": {"fn": function, "arg": argument}}


def uid_text(entity_type, entity_id):
    return entity_type + "::" + json.dumps(entity_id)


class EntitySet:
    """Entities in insertion order, plus the ids of each type for drawing uids."""

    def __init__(self):
        self.entities = []
        self.pools = {}
        self.attributes = {}

    def add(self, entity_type, entity_id, attributes=None, parents=()):
        attributes = attributes or {}
        self.entities.append(
            {
                "uid": uid(entity_type, entity_id),
                "attrs": attributes,
                "parents": [uid(t, i) for t, i in parents],
            }
        )
        self.pools.setdefault(entity_type, []).append(entity_id)
        self.attributes[(entity_type, entity_id)] = attributes
        return (entity_type, entity_id)

    def ids(self, entity_type):
        return self.pools[entity_type]


def sample(generator, population, low, high):
    count = generator.randint(low, min(high, len(population)))
    return generator.sample(population, count)


# ---------------------------------------------------------------------------------------------
# Applications. Each has an entity generator (scale multiplies the counts), a request generator,
# and the replica narrowing defaults: the principal type used for a bare `principal`, and the
# resource type used when the principal scope cannot be narrowed.


def document_cloud_entities(generator, scale):
    entities = EntitySet()
    shares = [entities.add("DocumentShare", f"share-{i}")[1] for i in range(20 * scale)]
    entities.add("Public", "public", {}, [("DocumentShare", generator.choice(shares))])
    entities.add("Drive", "drive")
    user_ids = [f"user-{i}" for i in range(100 * scale)]
    group_ids = [f"group-{i}" for i in range(30 * scale)]
    for group in group_ids:
        entities.add(
            "Group",
            group,
            {"owner": reference("User", generator.choice(user_ids))},
            [("DocumentShare", s) for s in sample(generator, shares, 1, 2)],
        )
    for user in user_ids:
        blocked = sample(generator, user_ids, 0, 2) if generator.random() < 0.3 else []
        entities.add(
            "User",
            user,
            {
                "blocked": [reference("User", b) for b in blocked if b != user],
                "personalGroup": reference("Group", generator.choice(group_ids)),
            },
            [("Group", g) for g in sample(generator, group_ids, 1, 3)],
        )
    for i in range(150 * scale):
        entities.add(
            "Document",
            f"document-{i}",
            {
                "isPrivate": generator.random() < 0.2,
                "manageACL": reference("DocumentShare", generator.choice(shares)),
                "modifyACL": reference("DocumentShare", generator.choice(shares)),
                "owner": reference("User", generator.choice(user_ids)),
                "publicAccess": generator.choice(["none", "view", "edit"]),
                "viewACL": reference("DocumentShare", generator.choice(shares)),
            },
        )
    return entities


def document_cloud_requests(generator, entities):
    document_actions = [
        "ViewDocument", "ModifyDocument", "EditIsPrivate", "AddToShareACL", "EditPublicAccess",
        "DeleteDocument",
    ]
    requests = []
    for _ in range(REQUEST_COUNT):
        context = {"is_authenticated": generator.random() < 0.9}
        roll = generator.random()
        if roll < 0.1:
            action = generator.choice(["CreateDocument", "CreateGroup"])
            resource = ("Drive", "drive")
            principal = ("User", generator.choice(entities.ids("User")))
        elif roll < 0.2:
            action = generator.choice(["ModifyGroup", "DeleteGroup"])
            group = generator.choice(entities.ids("Group"))
            resource = ("Group", group)
            if generator.random() < 0.5:
                principal = ("User", entities.attributes[resource]["owner"]["__entity"]["id"])
            else:
                principal = ("User", generator.choice(entities.ids("User")))
        else:
            action = generator.choice(document_actions)
            document = generator.choice(entities.ids("Document"))
            resource = ("Document", document)
            if action == "ViewDocument" and generator.random() < 0.15:
                principal = ("Public", "public")
            elif generator.random() < 0.4:
                principal = ("User", entities.attributes[resource]["owner"]["__entity"]["id"])
            else:
                principal = ("User", generator.choice(entities.ids("User")))
        requests.append((principal, ("Action", action), resource, context))
    return requests


def github_entities(generator, scale):
    entities = EntitySet()
    levels = [[], [], []]
    for depth, count in enumerate([10, 15, 15]):
        for i in range(count * scale):
            group = f"group-{depth}-{i}"
            parents = [] if depth == 0 else sample(generator, levels[depth - 1], 1, 2)
            entities.add("UserGroup", group, {}, [("UserGroup", p) for p in parents])
            levels[depth].append(group)
    every_group = levels[0] + levels[1] + levels[2]
    teams = []
    for i in range(20 * scale):
        team = f"team-{i}"
        entities.add("Team", team, {}, [("UserGroup", p) for p in sample(generator, levels[2], 1, 2)])
        teams.append(team)
    user_ids = []
    for i in range(120 * scale):
        user = f"user-{i}"
        parents = [("Team", t) for t in sample(generator, teams, 1, 2)]
        if generator.random() < 0.3:
            parents.append(("UserGroup", generator.choice(every_group)))
        entities.add("User", user, {}, parents)
        user_ids.append(user)
    repositories = []
    for i in range(30 * scale):
        repository = f"repository-{i}"
        entities.add(
            "Repository",
            repository,
            {
                role: reference("UserGroup", generator.choice(every_group))
                for role in ["admins", "maintainers", "readers", "triagers", "writers"]
            },
        )
        repositories.append(repository)
    for i in range(80 * scale):
        entities.add(
            "Issue",
            f"issue-{i}",
            {
                "repo": reference("Repository", generator.choice(repositories)),
                "reporter": reference("User", generator.choice(user_ids)),
            },
        )
    for i in range(3 * scale):
        entities.add(
            "Org",
            f"org-{i}",
            {
                "members": reference("UserGroup", generator.choice(levels[0])),
                "owners": reference("UserGroup", generator.choice(levels[1])),
            },
        )
    return entities


def github_requests(generator, entities):
    repository_actions = [
        "push", "pull", "fork", "add_reader", "add_writer", "add_maintainer", "add_admin",
        "add_triager",
    ]
    issue_actions = ["assign_issue", "delete_issue", "edit_issue"]
    requests = []
    for _ in range(REQUEST_COUNT):
        if generator.random() < 0.6:
            action = generator.choice(repository_actions)
            resource = ("Repository", generator.choice(entities.ids("Repository")))
            principal = ("User", generator.choice(entities.ids("User")))
        else:
            action = generator.choice(issue_actions)
            resource = ("Issue", generator.choice(entities.ids("Issue")))
            if generator.random() < 0.4:
                principal = ("User", entities.attributes[resource]["reporter"]["__entity"]["id"])
            else:
                principal = ("User", generator.choice(entities.ids("User")))
        requests.append((principal, ("Action", action), resource, {}))
    return requests


def hotel_chains_entities(generator, scale):
    entities = EntitySet()
    levels = [[], [], []]
    for depth, count in enumerate([5, 10, 25]):
        for i in range(count * scale):
            hotel = f"hotel-{depth}-{i}"
            parents = [] if depth == 0 else [generator.choice(levels[depth - 1])]
            entities.add("Hotel", hotel, {}, [("Hotel", p) for p in parents])
            levels[depth].append(hotel)
    hotels = levels[0] + levels[1] + levels[2]
    properties = []
    for i in range(60 * scale):
        identifier = f"property-{i}"
        entities.add("Property", identifier, {}, [("Hotel", generator.choice(levels[2]))])
        properties.append(identifier)
    for i in range(150 * scale):
        entities.add("Reservation", f"reservation-{i}", {}, [("Property", generator.choice(properties))])

    def permissions():
        return {
            "hotelReservations": [reference("Hotel", h) for h in sample(generator, hotels, 0, 3)],
            "propertyReservations": [
                reference("Property", p) for p in sample(generator, properties, 0, 3)
            ],
        }

    for i in range(170 * scale):
        entities.add(
            "User",
            f"user-{i}",
            {
                "viewPermissions": permissions(),
                "memberPermissions": permissions(),
                "hotelAdminPermissions": [
                    reference("Hotel", h) for h in sample(generator, hotels, 0, 2)
                ],
                "propertyAdminPermissions": [
                    reference("Property", p) for p in sample(generator, properties, 0, 2)
                ],
            },
        )
    return entities


def hotel_chains_requests(generator, entities):
    groups = [
        (["viewReservation", "updateReservation", "grantAccessReservation"], "Reservation"),
        (["createReservation", "viewProperty", "updateProperty", "grantAccessProperty"], "Property"),
        (["createProperty", "createHotel", "viewHotel", "updateHotel", "grantAccessHotel"], "Hotel"),
    ]
    requests = []
    for _ in range(REQUEST_COUNT):
        actions, resource_type = generator.choice(groups)
        principal = ("User", generator.choice(entities.ids("User")))
        resource = (resource_type, generator.choice(entities.ids(resource_type)))
        requests.append((principal, ("Action", generator.choice(actions)), resource, {}))
    return requests


def sales_orgs_entities(generator, scale):
    entities = EntitySet()
    jobs = ["internal", "distributor", "customer", "other"]
    for job in jobs:
        entities.add("Job", job)
    markets = [entities.add("Market", f"market-{i}")[1] for i in range(15 * scale)]
    user_ids = []
    for i in range(120 * scale):
        user = f"user-{i}"
        entities.add(
            "User",
            user,
            {
                "job": reference("Job", generator.choices(jobs, weights=[4, 2, 3, 1])[0]),
                "customerId": f"customer-{generator.randrange(10)}",
            },
            [("Market", m) for m in sample(generator, markets, 1, 2)],
        )
        user_ids.append(user)
    for i in range(100 * scale):
        entities.add(
            "Presentation",
            f"presentation-{i}",
            {
                "owner": reference("User", generator.choice(user_ids)),
                "viewers": [reference("User", u) for u in sample(generator, user_ids, 0, 5)],
                "editors": [reference("User", u) for u in sample(generator, user_ids, 0, 3)],
            },
        )
    for i in range(80 * scale):
        entities.add(
            "Template",
            f"template-{i}",
            {
                "owner": reference("User", generator.choice(user_ids)),
                "viewers": [reference("User", u) for u in sample(generator, user_ids, 0, 5)],
                "editors": [reference("User", u) for u in sample(generator, user_ids, 0, 3)],
                "viewerMarkets": [reference("Market", m) for m in sample(generator, markets, 0, 3)],
                "editorMarkets": [reference("Market", m) for m in sample(generator, markets, 0, 2)],
            },
        )
    return entities


def sales_orgs_requests(generator, entities):
    presentation_actions = [
        "viewPresentation", "removeSelfAccessFromPresentation", "duplicatePresentation",
        "editPresentation", "grantViewAccessToPresentation", "grantEditAccessToPresentation",
    ]
    template_actions = [
        "viewTemplate", "duplicateTemplate", "removeSelfAccessFromTemplate", "editTemplate",
        "removeOthersAccessToTemplate", "grantViewAccessToTemplate", "grantEditAccessToTemplate",
    ]
    users = entities.ids("User")
    requests = []
    for _ in range(REQUEST_COUNT):
        if generator.random() < 0.5:
            action = generator.choice(presentation_actions)
            resource = ("Presentation", generator.choice(entities.ids("Presentation")))
        else:
            action = generator.choice(template_actions)
            resource = ("Template", generator.choice(entities.ids("Template")))
        attributes = entities.attributes[resource]
        roll = generator.random()
        if roll < 0.3:
            principal = ("User", attributes["owner"]["__entity"]["id"])
        elif roll < 0.6 and attributes["viewers"]:
            principal = ("User", generator.choice(attributes["viewers"])["__entity"]["id"])
        else:
            principal = ("User", generator.choice(users))
        context = {}
        if action in ("grantViewAccessToPresentation", "grantEditAccessToPresentation"):
            context = {"target": reference("User", generator.choice(users))}
        elif action in ("grantViewAccessToTemplate", "grantEditAccessToTemplate"):
            if generator.random() < 0.6:
                context["targetUser"] = reference("User", generator.choice(users))
            if generator.random() < 0.4:
                context["targetMarket"] = reference("Market", generator.choice(entities.ids("Market")))
        requests.append((principal, ("Action", action), resource, context))
    return requests


def random_datetime(generator):
    day = generator.randint(1, 28)
    month = generator.choice([1, 2, 3])
    return f"2025-{month:02d}-{day:02d}T{generator.randrange(24):02d}:{generator.randrange(60):02d}:00Z"


def streaming_entities(generator, scale):
    entities = EntitySet()
    for i in range(170 * scale):
        entities.add("FreeMember", f"free-member-{i}")
    for i in range(170 * scale):
        entities.add(
            "Subscriber",
            f"subscriber-{i}",
            {
                "subscription": {"tier": generator.choice(["standard", "premium"])},
                "profile": {"isKid": generator.random() < 0.25},
            },
        )
    for i in range(80 * scale):
        entities.add(
            "Movie",
            f"movie-{i}",
            {
                "isFree": generator.random() < 0.3,
                "needsRentOrBuy": generator.random() < 0.4,
                "isOscarNominated": generator.random() < 0.3,
            },
        )
    for i in range(70 * scale):
        entities.add(
            "Show",
            f"show-{i}",
            {
                "isFree": generator.random() < 0.3,
                "releaseDate": extension("datetime", random_datetime(generator)),
                "isEarlyAccess": generator.random() < 0.4,
            },
        )
    return entities


def streaming_requests(generator, entities):
    offsets = ["-8h", "-5h", "-3h30m", "0h", "1h", "5h30m", "9h"]
    requests = []
    for _ in range(REQUEST_COUNT):
        if generator.random() < 0.7:
            principal = ("Subscriber", generator.choice(entities.ids("Subscriber")))
        else:
            principal = ("FreeMember", generator.choice(entities.ids("FreeMember")))
        now = extension("datetime", random_datetime(generator))
        if generator.random() < 0.7:
            action = "watch"
            resource_type = generator.choice(["Movie", "Show"])
            context = {"now": {"datetime": now, "localTimeOffset": extension("duration", generator.choice(offsets))}}
        else:
            action = generator.choice(["rent", "buy"])
            resource_type = "Movie"
            context = {"now": {"datetime": now}}
        resource = (resource_type, generator.choice(entities.ids(resource_type)))
        requests.append((principal, ("Action", action), resource, context))
    return requests


TAG_VOCABULARY = {
    "production_status": ["ALL", "draft", "review", "released", "archived"],
    "country": ["ALL", "us", "ca", "mx", "fr", "de", "jp"],
    "stage": ["ALL", "alpha", "beta", "gamma", "prod"],
}


def tag_record(generator):
    record = {}
    for name, vocabulary in TAG_VOCABULARY.items():
        if generator.random() < 0.6:
            values = sample(generator, vocabulary[1:], 1, 3)
            if generator.random() < 0.1:
                values.append("ALL")
            record[name] = values
    return record


def tags_n_roles_entities(generator, scale):
    entities = EntitySet()
    roles = ["Role-A", "Role-B"]
    for role in roles:
        entities.add("Role", role)
    for i in range(150 * scale):
        member_of = sample(generator, roles, 1, 2)
        allowed = {}
        for role in roles:
            if role in member_of or generator.random() < 0.2:
                allowed[role] = tag_record(generator)
        entities.add("User", f"user-{i}", {"allowedTagsForRole": allowed}, [("Role", r) for r in member_of])
    for i in range(500 * scale):
        entities.add("Workspace", f"workspace-{i}", {"tags": tag_record(generator)})
    return entities


def tags_n_roles_requests(generator, entities):
    requests = []
    for _ in range(REQUEST_COUNT):
        principal = ("User", generator.choice(entities.ids("User")))
        resource = ("Workspace", generator.choice(entities.ids("Workspace")))
        action = generator.choice(["UpdateWorkspace", "DeleteWorkspace", "ReadWorkspace"])
        requests.append((principal, ("Action", action), resource, {}))
    return requests


TAX_ORGANIZATIONS = [f"organization-{i}" for i in range(10)]
TAX_SERVICELINES = ["individual", "corporate", "estate", "nonprofit"]
TAX_LOCATIONS = ["north", "south", "east", "west", "central"]


def tax_preparer_entities(generator, scale):
    entities = EntitySet()
    clients = []
    for i in range(60 * scale):
        client = f"client-{i}"
        entities.add("Taxpreparer::Client", client, {"organization": generator.choice(TAX_ORGANIZATIONS)})
        clients.append(client)
    documents = []
    for i in range(160 * scale):
        document = f"document-{i}"
        attributes = {
            "serviceline": generator.choice(TAX_SERVICELINES),
            "location": generator.choice(TAX_LOCATIONS),
            "owner": reference("Taxpreparer::Client", generator.choice(clients)),
        }
        entities.add("Taxpreparer::Document", document, attributes)
        documents.append(attributes)
    entities.assigned_documents = {}
    for i in range(80 * scale):
        assigned = []
        chosen = sample(generator, range(len(documents)), 1, 3)
        entities.assigned_documents[f"professional-{i}"] = [f"document-{d}" for d in chosen]
        for document in (documents[d] for d in chosen):
            owner = ("Taxpreparer::Client", document["owner"]["__entity"]["id"])
            assigned.append(
                {
                    "organization": entities.attributes[owner]["organization"],
                    "serviceline": document["serviceline"],
                    "location": document["location"],
                }
            )
        entities.add(
            "Taxpreparer::Professional",
            f"professional-{i}",
            {"assigned_orgs": assigned, "location": generator.choice(TAX_LOCATIONS)},
        )
    return entities


def tax_preparer_link_pairs(entities):
    """The (professional, document) pairs that template links bind, in link order."""
    generator = random.Random(f"{SEED}-links")
    professionals = entities.ids("Taxpreparer::Professional")
    documents = entities.ids("Taxpreparer::Document")
    pairs = []
    seen = set()
    while len(pairs) < max(POLICY_SET_SIZES):
        pair = (generator.choice(professionals), generator.choice(documents))
        if pair not in seen:
            seen.add(pair)
            pairs.append(pair)
    return pairs


def tax_preparer_requests(generator, entities):
    linked = tax_preparer_link_pairs(entities)[:8]
    requests = []
    for _ in range(REQUEST_COUNT):
        roll = generator.random()
        if roll < 0.2:
            professional, document = generator.choice(linked)
        else:
            professional = generator.choice(entities.ids("Taxpreparer::Professional"))
            if roll < 0.6:
                document = generator.choice(entities.assigned_documents[professional])
            else:
                document = generator.choice(entities.ids("Taxpreparer::Document"))
        principal = ("Taxpreparer::Professional", professional)
        resource = ("Taxpreparer::Document", document)
        owner = entities.attributes[resource]["owner"]["__entity"]["id"]
        if generator.random() >= 0.8:
            owner = generator.choice(entities.ids("Taxpreparer::Client"))
        regions = sample(generator, TAX_LOCATIONS, 1, 3)
        if generator.random() < 0.7:
            regions.append(entities.attributes[principal]["location"])
        context = {
            "consent": {
                "client": reference("Taxpreparer::Client", owner),
                "team_region_list": sorted(set(regions)),
            }
        }
        requests.append((principal, ("Taxpreparer::Action", "viewDocument"), resource, context))
    return requests


def tinytodo_entities(generator, scale):
    entities = EntitySet()
    application = ("Application", "TinyTodo")
    entities.add(*application)
    levels = [[], [], []]
    for depth, count in enumerate([5, 10, 20]):
        for i in range(count * scale):
            team = f"team-{depth}-{i}"
            parents = [application] if depth == 0 else [("Team", p) for p in sample(generator, levels[depth - 1], 1, 2)]
            entities.add("Team", team, {}, parents)
            levels[depth].append(team)
    teams = levels[0] + levels[1] + levels[2]
    user_ids = []
    for i in range(250 * scale):
        user = f"user-{i}"
        parents = [("Team", t) for t in sample(generator, levels[2], 1, 2)] + [application]
        entities.add(
            "User",
            user,
            {"joblevel": generator.randint(1, 10), "location": generator.choice(["ABC", "DEF", "GHI"])},
            parents,
        )
        user_ids.append(user)
    for i in range(150 * scale):
        tasks = [
            {"id": t, "name": f"task-{t}", "state": generator.choice(["checked", "unchecked"])}
            for t in range(generator.randint(0, 5))
        ]
        entities.add(
            "List",
            f"list-{i}",
            {
                "editors": reference("Team", generator.choice(teams)),
                "name": f"list-{i}",
                "owner": reference("User", generator.choice(user_ids)),
                "readers": reference("Team", generator.choice(teams)),
                "tasks": tasks,
            },
            [application],
        )
    return entities


def tinytodo_requests(generator, entities):
    list_actions = ["DeleteList", "GetList", "UpdateList", "CreateTask", "UpdateTask", "DeleteTask", "EditShare"]
    requests = []
    for _ in range(REQUEST_COUNT):
        if generator.random() < 0.15:
            action = generator.choice(["CreateList", "GetLists"])
            resource = ("Application", "TinyTodo")
            principal = ("User", generator.choice(entities.ids("User")))
        else:
            action = generator.choice(list_actions)
            resource = ("List", generator.choice(entities.ids("List")))
            if generator.random() < 0.25:
                principal = ("User", entities.attributes[resource]["owner"]["__entity"]["id"])
            else:
                principal = ("User", generator.choice(entities.ids("User")))
        requests.append((principal, ("Action", action), resource, {}))
    return requests


APPLICATION_GENERATORS = {
    "document_cloud": (document_cloud_entities, document_cloud_requests, "User", "Document"),
    "github_example": (github_entities, github_requests, "User", "Repository"),
    "hotel_chains": (hotel_chains_entities, hotel_chains_requests, "User", None),
    "sales_orgs": (sales_orgs_entities, sales_orgs_requests, "User", None),
    "streaming_service": (streaming_entities, streaming_requests, "Subscriber", None),
    "tags_n_roles": (tags_n_roles_entities, tags_n_roles_requests, "User", "Workspace"),
    "tax_preparer": (tax_preparer_entities, tax_preparer_requests, None, None),
    "tinytodo": (tinytodo_entities, tinytodo_requests, "User", None),
}


# ---------------------------------------------------------------------------------------------
# Policy text handling.


def strip_comments(text):
    """Removes `//` comments outside string literals."""
    result = []
    index = 0
    in_string = False
    while index < len(text):
        character = text[index]
        if in_string:
            result.append(character)
            if character == "\\":
                result.append(text[index + 1])
                index += 1
            elif character == '"':
                in_string = False
        elif character == '"':
            in_string = True
            result.append(character)
        elif text.startswith("//", index):
            while index < len(text) and text[index] != "\n":
                index += 1
            continue
        else:
            result.append(character)
        index += 1
    return "".join(result)


def split_policies(text):
    """Splits policy text at `;` outside string literals; returns each policy with its `;`."""
    policies = []
    start = 0
    in_string = False
    index = 0
    while index < len(text):
        character = text[index]
        if in_string:
            if character == "\\":
                index += 1
            elif character == '"':
                in_string = False
        elif character == '"':
            in_string = True
        elif character == ";":
            policies.append(text[start : index + 1].strip())
            start = index + 1
        index += 1
    if text[start:].strip():
        raise SystemExit("policy text ends without `;`")
    return policies


ANNOTATION_ID = re.compile(r'@id\s*\(\s*"(?:[^"\\]|\\.)*"\s*\)\s*')
HEAD = re.compile(r"^((?:@\w+\s*\(\s*\"(?:[^\"\\]|\\.)*\"\s*\)\s*)*)(permit|forbid)\s*\(", re.S)


def scope_components(policy):
    """Returns (prefix, [principal, action, resource], suffix) of a policy's scope."""
    match = HEAD.match(policy)
    if not match:
        raise SystemExit("unrecognized policy head: " + policy[:60])
    depth = 0
    in_string = False
    components = []
    start = match.end()
    for index in range(match.end(), len(policy)):
        character = policy[index]
        if in_string:
            if character == '"' and policy[index - 1] != "\\":
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character in "([":
            depth += 1
        elif character in ")]":
            if depth == 0:
                components.append(policy[start:index].strip())
                if len(components) != 3:
                    raise SystemExit("scope without three components: " + policy[:60])
                return policy[: match.end()], components, policy[index:]
            depth -= 1
        elif character == "," and depth == 0:
            components.append(policy[start:index].strip())
            start = index + 1
    raise SystemExit("unterminated scope: " + policy[:60])


def narrowing(component, variable, default_type):
    """The entity type to narrow a scope component to, or None when it cannot be narrowed."""
    if component == variable:
        return default_type
    match = re.fullmatch(variable + r"\s+is\s+([A-Za-z_][A-Za-z0-9_:]*)", component)
    if match:
        return match.group(1)
    return None


def replica(policy, round_index, policy_index, entities, principal_type, resource_type):
    prefix, (principal, action, resource), suffix = scope_components(policy)
    narrowed_principal = narrowing(principal, "principal", principal_type)
    if narrowed_principal is not None:
        pool = entities.ids(narrowed_principal)
        identifier = pool[(round_index + policy_index * 13) % len(pool)]
        principal = "principal == " + uid_text(narrowed_principal, identifier)
    else:
        narrowed_resource = narrowing(resource, "resource", resource_type)
        if narrowed_resource is None:
            raise SystemExit("cannot narrow policy: " + policy[:60])
        pool = entities.ids(narrowed_resource)
        identifier = pool[(round_index + policy_index * 13) % len(pool)]
        resource = "resource == " + uid_text(narrowed_resource, identifier)
    if round_index >= len(pool):
        raise SystemExit(f"pool of {len(pool)} too small for replica round {round_index}")
    return prefix + "\n  " + principal + ",\n  " + action + ",\n  " + resource + "\n" + suffix


def is_template(policy):
    return "?principal" in policy or "?resource" in policy


def is_forbid(policy):
    return scope_components(policy)[0].rstrip().rstrip("(").rstrip().endswith("forbid")


def policy_sets(policies, entities, principal_type, resource_type):
    """Yields (size, policy text, links) for each size."""
    templates = [p for p in policies if is_template(p)]
    statics = [p for p in policies if not is_template(p)]
    for size in POLICY_SET_SIZES:
        if len(statics) > size:
            forbids = [p for p in statics if is_forbid(p)]
            permits = [p for p in statics if not is_forbid(p)]
            chosen = forbids + permits[: size - len(forbids)]
            chosen = [p for p in statics if p in chosen]
        else:
            chosen = list(statics)
        links = []
        generated = []
        remaining = size - len(chosen)
        if templates:
            text_policies = chosen + templates
            template_ids = [f"policy{len(chosen) + i}" for i in range(len(templates))]
            if len(templates) != 1 or not ("?principal" in templates[0] and "?resource" in templates[0]):
                raise SystemExit("only one template with both slots is supported")
            for professional, document in tax_preparer_link_pairs(entities)[:remaining]:
                arguments = {
                    "?principal": uid_text("Taxpreparer::Professional", professional),
                    "?resource": uid_text("Taxpreparer::Document", document),
                }
                links.append({"template_id": template_ids[0], "link_id": f"link{len(links)}", "args": arguments})
        else:
            for replica_index in range(remaining):
                policy_index = replica_index % len(statics)
                round_index = replica_index // len(statics)
                generated.append(
                    replica(statics[policy_index], round_index, policy_index, entities, principal_type, resource_type)
                )
            text_policies = chosen + generated
        yield size, "\n\n".join(text_policies) + "\n", links


def request_json(request):
    (principal_type, principal_id), (action_type, action_id), (resource_type, resource_id), context = request
    return {
        "principal": uid_text(principal_type, principal_id),
        "action": uid_text(action_type, action_id),
        "resource": uid_text(resource_type, resource_id),
        "context": context,
    }


def write_json(path, value):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=None, separators=(",", ":"))
        handle.write("\n")


def run_oracle(arguments):
    return subprocess.run(arguments, capture_output=True, text=True, check=False)


def check_with_oracle(oracle, directory, requests, scratch):
    schema = os.path.join(directory, "schema.cedarschema")
    for large in ["entities.json", "entities-large.json"]:
        path = os.path.join(directory, large)
        if not os.path.exists(path):
            continue
        empty_policies = os.path.join(scratch, "empty.cedar")
        with open(empty_policies, "w", encoding="utf-8"):
            pass
        request_path = os.path.join(scratch, "request.json")
        write_json(request_path, requests[0])
        for schema_arguments in (["--schema", schema], ["--request-validation", "false"]):
            result = run_oracle(
                [oracle, "authorize", *schema_arguments, "--policies", empty_policies,
                 "--entities", path, "--request-json", request_path]
            )
            if result.stdout.strip() != "DENY":
                raise SystemExit(f"{path} does not load ({schema_arguments}):\n{result.stdout}{result.stderr}")
    decisions = {}
    for size in POLICY_SET_SIZES:
        policies = os.path.join(directory, f"policies-{size}.cedar")
        links = os.path.join(directory, f"links-{size}.json")
        result = run_oracle(
            [oracle, "validate", "--schema", schema, "--policies", policies,
             "--template-linked", links, "--deny-warnings"]
        )
        if result.returncode != 0:
            raise SystemExit(f"{policies} does not validate:\n{result.stdout}{result.stderr}")
        verdicts = []
        for request in requests:
            request_path = os.path.join(scratch, "request.json")
            write_json(request_path, request)
            result = run_oracle(
                [oracle, "authorize", "--schema", schema, "--policies", policies,
                 "--template-linked", links, "--entities", os.path.join(directory, "entities.json"),
                 "--request-json", request_path]
            )
            if result.stdout.strip() == "ALLOW":
                verdicts.append("allow")
            elif result.stdout.strip() == "DENY":
                verdicts.append("deny")
            else:
                raise SystemExit(f"{policies}: request {request} failed:\n{result.stdout}{result.stderr}")
        decisions[str(size)] = verdicts
    return decisions


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cedar-examples-directory", required=True)
    parser.add_argument("--output-directory", required=True)
    parser.add_argument("--oracle", help="the reference CLI; when given, every workload is checked with it")
    parser.add_argument("--expected-decisions-output", help="with --oracle, where to write the decisions")
    arguments = parser.parse_args()
    if bool(arguments.oracle) != bool(arguments.expected_decisions_output):
        parser.error("--oracle and --expected-decisions-output go together")
    expected_decisions = {}

    for application in APPLICATIONS:
        entity_generator, request_generator, principal_type, resource_type = APPLICATION_GENERATORS[application]
        source = os.path.join(arguments.cedar_examples_directory, application)
        directory = os.path.join(arguments.output_directory, application)
        os.makedirs(directory, exist_ok=True)
        shutil.copyfile(os.path.join(source, "policies.cedarschema"), os.path.join(directory, "schema.cedarschema"))

        generator = random.Random(f"{SEED}-{application}")
        entities = entity_generator(generator, 1)
        write_json(os.path.join(directory, "entities.json"), entities.entities)
        requests = [request_json(r) for r in request_generator(random.Random(f"{SEED}-{application}-requests"), entities)]
        with open(os.path.join(directory, "requests.json"), "w", encoding="utf-8") as handle:
            json.dump(requests, handle, indent=1)
            handle.write("\n")

        if application in LARGE_ENTITY_APPLICATIONS:
            large = entity_generator(random.Random(f"{SEED}-{application}-large"), LARGE_ENTITY_APPLICATIONS[application])
            write_json(os.path.join(directory, "entities-large.json"), large.entities)

        with open(os.path.join(source, "policies.cedar"), encoding="utf-8") as handle:
            text = strip_comments(handle.read())
        policies = [ANNOTATION_ID.sub("", p) for p in split_policies(text)]
        for size, policy_text, links in policy_sets(policies, entities, principal_type, resource_type):
            with open(os.path.join(directory, f"policies-{size}.cedar"), "w", encoding="utf-8") as handle:
                handle.write(policy_text)
            with open(os.path.join(directory, f"links-{size}.json"), "w", encoding="utf-8") as handle:
                json.dump(links, handle, indent=1)
                handle.write("\n")

        summary = f"{application}: {len(entities.entities)} entities, {len(requests)} requests"
        if arguments.oracle:
            with tempfile.TemporaryDirectory(dir=arguments.output_directory) as scratch:
                decisions = check_with_oracle(arguments.oracle, directory, requests, scratch)
            expected_decisions[application] = decisions
            summary += ", allows per size " + ", ".join(
                f"{size}: {verdicts.count('allow')}" for size, verdicts in decisions.items()
            )
        print(summary)
    if arguments.oracle:
        with open(arguments.expected_decisions_output, "w", encoding="utf-8") as handle:
            json.dump(expected_decisions, handle, indent=1)
            handle.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
