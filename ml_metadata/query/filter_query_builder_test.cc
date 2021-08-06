/* Copyright 2020 Google LLC

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "ml_metadata/query/filter_query_builder.h"

#include <vector>

#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "ml_metadata/metadata_store/test_util.h"
#include "ml_metadata/proto/metadata_store.pb.h"
#include "ml_metadata/query/filter_query_ast_resolver.h"

namespace ml_metadata {
namespace {

using ::testing::ValuesIn;

// A property mention consists of a tuple (base table alias, property name).
using PropertyMention = std::pair<absl::string_view, absl::string_view>;

// A tuple of (user-query, from clause, where clause)
struct QueryTupleTestCase {
  const std::string user_query;
  // The from clause depends on the base_table of the template Node type
  // (Artifact/Execution/Context). The `join_mentions` describes the expected
  // table alias of related neighbors.
  // Use GetFromClause<T> to test the resolved from_clause with the test case.
  struct MentionedNeighbors {
    std::vector<absl::string_view> types;
    std::vector<absl::string_view> contexts;
    std::vector<PropertyMention> properties;
    std::vector<PropertyMention> custom_properties;
    std::vector<absl::string_view> parent_contexts;
    std::vector<absl::string_view> child_contexts;
  };
  const MentionedNeighbors join_mentions;
  const std::string where_clause;

  // Note gtest has limitation to support parametrized type and value together.
  // We use a test_case_nodes in QueryTupleTestCase to implement parameterized
  // tests and share test cases for both types {Artifact, Execution, Context}
  // and query tuple values.
  // Each {`user_query`, `from_clause`, `where_clause`} is tested on all three
  // node types unless that node type is set to false in `test_case_nodes`.
  struct TestOnNodes {
    const bool artifact = true;
    const bool execution = true;
    const bool context = true;
  };
  const TestOnNodes test_case_nodes;

  // Utility method to test the resolved from clause with the testcase instance.
  template <typename Node>
  std::string GetFromClause() const {
    const absl::string_view base_alias =
        FilterQueryBuilder<Node>::kBaseTableAlias;
    std::string from_clause =
        FilterQueryBuilder<Node>::GetBaseNodeTable(base_alias);
    for (absl::string_view type_alias : join_mentions.types) {
      from_clause +=
          FilterQueryBuilder<Node>::GetTypeJoinTable(base_alias, type_alias);
    }
    for (absl::string_view context_alias : join_mentions.contexts) {
      from_clause += FilterQueryBuilder<Node>::GetContextJoinTable(
          base_alias, context_alias);
    }
    for (const PropertyMention& property_mention : join_mentions.properties) {
      from_clause += FilterQueryBuilder<Node>::GetPropertyJoinTable(
          base_alias, property_mention.first, property_mention.second);
    }
    for (const PropertyMention& property_mention :
         join_mentions.custom_properties) {
      from_clause += FilterQueryBuilder<Node>::GetCustomPropertyJoinTable(
          base_alias, property_mention.first, property_mention.second);
    }
    for (absl::string_view parent_context_alias :
         join_mentions.parent_contexts) {
      from_clause += FilterQueryBuilder<Node>::GetParentContextJoinTable(
          base_alias, parent_context_alias);
    }
    for (absl::string_view child_context_alias : join_mentions.child_contexts) {
      from_clause += FilterQueryBuilder<Node>::GetChildContextJoinTable(
          base_alias, child_context_alias);
    }

    return from_clause;
  }
};

constexpr QueryTupleTestCase::TestOnNodes artifact_only = {true, false, false};
constexpr QueryTupleTestCase::TestOnNodes exclude_context = {true, true, false};
constexpr QueryTupleTestCase::TestOnNodes context_only = {false, false, true};

// A list of utilities to write the mentioned tables for the test cases.
QueryTupleTestCase::MentionedNeighbors NoJoin() {
  return QueryTupleTestCase::MentionedNeighbors();
}

QueryTupleTestCase::MentionedNeighbors JoinWith(
    std::vector<absl::string_view> types = {},
    std::vector<absl::string_view> contexts = {},
    std::vector<PropertyMention> properties = {},
    std::vector<PropertyMention> custom_properties = {},
    std::vector<absl::string_view> parent_contexts = {},
    std::vector<absl::string_view> child_contexts = {}) {
  return {types, contexts, properties, custom_properties,
          parent_contexts, child_contexts};
}

QueryTupleTestCase::MentionedNeighbors JoinWithType(
    absl::string_view type_table_alias) {
  return JoinWith(/*types=*/{type_table_alias}, /*contexts=*/{});
}

QueryTupleTestCase::MentionedNeighbors JoinWithContexts(
    std::vector<absl::string_view> context_table_alias) {
  return JoinWith(/*types=*/{}, context_table_alias);
}

QueryTupleTestCase::MentionedNeighbors JoinWithProperty(
    absl::string_view property_table_alias, absl::string_view property_name) {
  return JoinWith(/*types=*/{}, /*contexts=*/{},
                  {{property_table_alias, property_name}});
}

QueryTupleTestCase::MentionedNeighbors JoinWithCustomProperty(
    absl::string_view property_table_alias, absl::string_view property_name) {
  return JoinWith(/*types=*/{}, /*contexts=*/{},
                  /*properties=*/{}, {{property_table_alias, property_name}});
}

QueryTupleTestCase::MentionedNeighbors JoinWithProperties(
    std::vector<PropertyMention> properties,
    std::vector<PropertyMention> custom_properties) {
  return JoinWith(/*types=*/{}, /*contexts=*/{}, properties, custom_properties);
}

QueryTupleTestCase::MentionedNeighbors JoinWithParentContexts(
    std::vector<absl::string_view> parent_context_table_alias) {
  return JoinWith(/*types=*/{}, /*contexts=*/{}, /*properties=*/{},
                  /*custom_properties=*/{}, parent_context_table_alias,
                  /*child_contexts=*/{});
}

QueryTupleTestCase::MentionedNeighbors JoinWithChildContexts(
    std::vector<absl::string_view> child_context_table_alias) {
  return JoinWith(/*types=*/{}, /*contexts=*/{}, /*properties=*/{},
                  /*custom_properties=*/{}, /*parent_contexts=*/{},
                  child_context_table_alias);
}

std::vector<QueryTupleTestCase> GetTestQueryTuples() {
  return {
      // basic type attributes conditions
      {"type_id = 1", NoJoin(), "(table_0.type_id) = 1"},
      {"NOT(type_id = 1)", NoJoin(), "NOT ((table_0.type_id) = 1)"},
      {"type = 'foo'", JoinWithType("table_1"), "(table_1.type) = (\"foo\")"},
      // artifact-only attributes
      {"uri like 'abc'", NoJoin(), "(table_0.uri) LIKE (\"abc\")",
       artifact_only},
      // mention context (the neighbor only applies to artifact/execution)
      {"contexts_0.id = 1", JoinWithContexts({"table_1"}), "(table_1.id) = 1",
       exclude_context},
      // use multiple conditions on the same context
      {"contexts_0.id = 1 AND contexts_0.name LIKE 'foo%'",
       JoinWithContexts({"table_1"}),
       "((table_1.id) = 1) AND ((table_1.name) LIKE (\"foo%\"))",
       exclude_context},
      // use multiple conditions(including date fields) on the same context
      {"contexts_0.id = 1 AND contexts_0.create_time_since_epoch > 1",
       JoinWithContexts({"table_1"}),
       "((table_1.id) = 1) AND ((table_1.create_time_since_epoch) > 1)",
       exclude_context},
      // use multiple conditions on different contexts
      {"contexts_0.id = 1 AND contexts_1.id != 2",
       JoinWithContexts({"table_1", "table_2"}),
       "((table_1.id) = 1) AND ((table_2.id) != 2)", exclude_context},
      // use multiple conditions on different contexts
      {"contexts_0.id = 1 AND contexts_0.last_update_time_since_epoch < 1 AND "
       "contexts_1.id != 2",
       JoinWithContexts({"table_1", "table_2"}),
       "((table_1.id) = 1) AND ((table_1.last_update_time_since_epoch) < 1) "
       "AND ((table_2.id) != 2)",
       exclude_context},
      // mix attributes and context together
      {"type_id = 1 AND contexts_0.id = 1", JoinWithContexts({"table_1"}),
       "((table_0.type_id) = 1) AND ((table_1.id) = 1)", exclude_context},
      // mix attributes (including type) and context together
      {"(type_id = 1 OR type != 'foo') AND contexts_0.id = 1",
       JoinWith(/*types=*/{"table_1"}, /*contexts=*/{"table_2"}),
       "(((table_0.type_id) = 1) OR ((table_1.type) != (\"foo\"))) AND "
       "((table_2.id) = 1)",
       exclude_context},
      // mention properties
      {"properties.p0.int_value = 1", JoinWithProperty("table_1", "p0"),
       "(table_1.int_value) = 1"},
      // properties with backquoted names
      {"properties.`0:b`.int_value = 1", JoinWithProperty("table_1", "0:b"),
       "(table_1.int_value) = 1"},
      {"custom_properties.`0 b`.string_value != '1'",
       JoinWithCustomProperty("table_1", "0 b"),
       "(table_1.string_value) != (\"1\")"},
      {"properties.`0:b`.int_value = 1 AND "
       "properties.foo.double_value > 1 AND "
       "custom_properties.`0 b`.string_value != '1'",
       JoinWithProperties(
           /*properties=*/{{"table_1", "0:b"}, {"table_2", "foo"}},
           /*custom_properties=*/{{"table_3", "0 b"}}),
       "((table_1.int_value) = 1) AND ((table_2.double_value) > (1.0)) AND "
       "((table_3.string_value) != (\"1\"))"},
      // use multiple conditions on the same property
      {"properties.p0.int_value = 1 OR properties.p0.string_value = '1' ",
       JoinWithProperty("table_1", "p0"),
       "((table_1.int_value) = 1) OR ((table_1.string_value) = (\"1\"))"},
      // mention property and custom property with the same property name
      {"properties.p0.int_value > 1 OR custom_properties.p0.int_value > 1",
       JoinWithProperties(/*properties=*/{{"table_1", "p0"}},
                          /*custom_properties=*/{{"table_2", "p0"}}),
       "((table_1.int_value) > 1) OR ((table_2.int_value) > 1)"},
      // use multiple properties and custom properties
      {"(properties.p0.int_value > 1 OR custom_properties.p0.int_value > 1) "
       "AND "
       "properties.p1.double_value > 0.95 AND "
       "custom_properties.p2.string_value = 'name'",
       JoinWithProperties(
           /*properties=*/{{"table_1", "p0"}, {"table_3", "p1"}},
           /*custom_properties=*/{{"table_2", "p0"}, {"table_4", "p2"}}),
       "(((table_1.int_value) > 1) OR ((table_2.int_value) > 1)) AND "
       "((table_3.double_value) > (0.95)) AND "
       "((table_4.string_value) = (\"name\"))"},
      // use attributes, contexts, properties and custom properties
      {"type = 'dataset' AND "
       "(contexts_0.name = 'my_run' AND contexts_0.type = 'exp') AND "
       "(properties.p0.int_value > 1 OR custom_properties.p1.double_value > "
       "0.9)",
       JoinWith(/*types=*/{"table_1"},
                /*contexts=*/{"table_2"},
                /*properties=*/{{"table_3", "p0"}},
                /*custom_properties=*/{{"table_4", "p1"}}),
       "((table_1.type) = (\"dataset\")) AND (((table_2.name) = (\"my_run\")) "
       "AND ((table_2.type) = (\"exp\"))) AND (((table_3.int_value) > 1) OR "
       "((table_4.double_value) > (0.9)))",
       exclude_context},
      // Parent context queries.
      // mention context (the neighbor only applies to contexts)
      {"parent_contexts_0.id = 1", JoinWithParentContexts({"table_1"}),
       "(table_1.id) = 1", context_only},
      // use multiple conditions on the same parent context
      {"parent_contexts_0.id = 1 AND parent_contexts_0.name LIKE 'foo%'",
       JoinWithParentContexts({"table_1"}),
       "((table_1.id) = 1) AND ((table_1.name) LIKE (\"foo%\"))", context_only},
      // use multiple conditions on different parent contexts
      {"parent_contexts_0.id = 1 AND parent_contexts_1.id != 2",
       JoinWithParentContexts({"table_1", "table_2"}),
       "((table_1.id) = 1) AND ((table_2.id) != 2)", context_only},
      // // mix attributes and parent context together
      {"type_id = 1 AND parent_contexts_0.id = 1",
       JoinWithParentContexts({"table_1"}),
       "((table_0.type_id) = 1) AND ((table_1.id) = 1)", context_only},
      // mix attributes (including type) and parent context together
      {"(type_id = 1 OR type != 'foo') AND parent_contexts_0.id = 1",
       JoinWith(/*types=*/{"table_1"}, /*contexts=*/{}, /*properties=*/{},
                /*custom_properties=*/{},
                /*parent_contexts=*/{"table_2"}),
       "(((table_0.type_id) = 1) OR ((table_1.type) != (\"foo\"))) AND "
       "((table_2.id) = 1)",
       context_only},
      // use attributes, parent contexts, properties and custom properties
      {"type = 'pipeline_run' AND (properties.p0.int_value > 1 OR "
       "custom_properties.p1.double_value > 0.9) AND (parent_contexts_0.name = "
       "'pipeline_context' AND parent_contexts_0.type = 'pipeline')",
       JoinWith(/*types=*/{"table_1"},
                /*contexts=*/{},
                /*properties=*/{{"table_2", "p0"}},
                /*custom_properties=*/{{"table_3", "p1"}},
                /*parent_contexts=*/{"table_4"}),
       "((table_1.type) = (\"pipeline_run\")) AND (((table_2.int_value) > 1) "
       "OR ((table_3.double_value) > (0.9))) AND (((table_4.name) = "
       "(\"pipeline_context\")) AND ((table_4.type) = (\"pipeline\")))",
       context_only},
      // Child context queries.
      // mention context (the neighbor only applies to contexts)
      {"child_contexts_0.id = 1", JoinWithChildContexts({"table_1"}),
       "(table_1.id) = 1", context_only},
      // use multiple conditions on the same child context
      {"child_contexts_0.id = 1 AND child_contexts_0.name LIKE 'foo%'",
       JoinWithChildContexts({"table_1"}),
       "((table_1.id) = 1) AND ((table_1.name) LIKE (\"foo%\"))", context_only},
      // use multiple conditions on different child contexts
      {"child_contexts_0.id = 1 AND child_contexts_1.id != 2",
       JoinWithChildContexts({"table_1", "table_2"}),
       "((table_1.id) = 1) AND ((table_2.id) != 2)", context_only},
      // // mix attributes and child context together
      {"type_id = 1 AND child_contexts_0.id = 1",
       JoinWithChildContexts({"table_1"}),
       "((table_0.type_id) = 1) AND ((table_1.id) = 1)", context_only},
      // mix attributes (including type) and child context together
      {"(type_id = 1 OR type != 'foo') AND child_contexts_0.id = 1",
       JoinWith(/*types=*/{"table_1"}, /*contexts=*/{}, /*properties=*/{},
                /*custom_properties=*/{}, /*parent_contexts=*/{},
                /*child_contexts=*/{"table_2"}),
       "(((table_0.type_id) = 1) OR ((table_1.type) != (\"foo\"))) AND "
       "((table_2.id) = 1)",
       context_only},
      // use attributes, child contexts, properties and custom properties
      {"type = 'pipeline' AND (properties.p0.int_value > 1 OR "
       "custom_properties.p1.double_value > 0.9) AND (child_contexts_0.name = "
       "'pipeline_run' AND child_contexts_0.type = 'runs')",
       JoinWith(/*types=*/{"table_1"},
                /*contexts=*/{},
                /*properties=*/{{"table_2", "p0"}},
                /*custom_properties=*/{{"table_3", "p1"}},
                /*parent_contexts=*/{},
                /*child_contexts=*/{"table_4"}),
       "((table_1.type) = (\"pipeline\")) AND (((table_2.int_value) > 1) "
       "OR ((table_3.double_value) > (0.9))) AND (((table_4.name) = "
       "(\"pipeline_run\")) AND ((table_4.type) = (\"runs\")))",
       context_only},
      // use attributes, parent context, child contexts, properties and custom
      // properties
      {"type = 'pipeline' AND (properties.p0.int_value > 1 OR "
       "custom_properties.p1.double_value > 0.9) AND (parent_contexts_0.name = "
       "'parent_context1' AND parent_contexts_0.type = 'parent_context_type') "
       "AND (child_contexts_0.name = 'pipeline_run' AND child_contexts_0.type "
       "= 'runs')",
       JoinWith(/*types=*/{"table_1"},
                /*contexts=*/{},
                /*properties=*/{{"table_2", "p0"}},
                /*custom_properties=*/{{"table_3", "p1"}},
                /*parent_contexts=*/{"table_4"},
                /*child_contexts=*/{"table_5"}),
       "((table_1.type) = (\"pipeline\")) AND (((table_2.int_value) > 1) "
       "OR ((table_3.double_value) > (0.9))) AND (((table_4.name) = "
       "(\"parent_context1\")) AND ((table_4.type) = "
       "(\"parent_context_type\"))) AND (((table_5.name) = (\"pipeline_run\")) "
       "AND ((table_5.type) = (\"runs\")))",
       context_only},
  };
}

class SQLGenerationTest : public ::testing::TestWithParam<QueryTupleTestCase> {
 protected:
  template <typename T>
  void VerifyQueryTuple() {
    LOG(INFO) << "Testing valid query string: " << GetParam().user_query;
    FilterQueryAstResolver<T> ast_resolver(GetParam().user_query);
    ASSERT_EQ(absl::OkStatus(), ast_resolver.Resolve());
    ASSERT_NE(ast_resolver.GetAst(), nullptr);
    FilterQueryBuilder<T> query_builder;
    ASSERT_EQ(absl::OkStatus(), ast_resolver.GetAst()->Accept(&query_builder));
    // Ensures the base table alias constant does not violate the test strings
    // used in the expected where clause.
    ASSERT_EQ(FilterQueryBuilder<T>::kBaseTableAlias, "table_0");
    EXPECT_EQ(query_builder.GetFromClause(), GetParam().GetFromClause<T>());
    EXPECT_EQ(query_builder.GetWhereClause(), GetParam().where_clause);
  }
};

TEST_P(SQLGenerationTest, Artifact) {
  if (GetParam().test_case_nodes.artifact) {
    VerifyQueryTuple<Artifact>();
  }
}

TEST_P(SQLGenerationTest, Execution) {
  if (GetParam().test_case_nodes.execution) {
    VerifyQueryTuple<Execution>();
  }
}

TEST_P(SQLGenerationTest, Context) {
  if (GetParam().test_case_nodes.context) {
    VerifyQueryTuple<Context>();
  }
}

INSTANTIATE_TEST_SUITE_P(FilterQueryBuilderTest, SQLGenerationTest,
                         ValuesIn(GetTestQueryTuples()));

}  // namespace
}  // namespace ml_metadata
