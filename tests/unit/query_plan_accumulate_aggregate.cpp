#include <algorithm>
#include <iterator>
#include <memory>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "query/context.hpp"
#include "query/exceptions.hpp"
#include "query/plan/operator.hpp"
#include "query_plan_common.hpp"

using namespace query;
using namespace query::plan;
using query::test_common::ToIntList;
using query::test_common::ToIntMap;
using testing::UnorderedElementsAre;

TEST(QueryPlan, Accumulate) {
  // simulate the following two query execution on an empty db
  // CREATE ({x:0})-[:T]->({x:0})
  // MATCH (n)--(m) SET n.x = n.x + 1, m.x = m.x + 1 RETURN n.x, m.x
  // without accumulation we expected results to be [[1, 1], [2, 2]]
  // with accumulation we expect them to be [[2, 2], [2, 2]]

  auto check = [&](bool accumulate) {
    storage::Storage db;
    auto storage_dba = db.Access();
    query::DbAccessor dba(&storage_dba);
    auto prop = dba.NameToProperty("x");

    auto v1 = dba.InsertVertex();
    ASSERT_TRUE(v1.SetProperty(prop, storage::PropertyValue(0)).HasValue());
    auto v2 = dba.InsertVertex();
    ASSERT_TRUE(v2.SetProperty(prop, storage::PropertyValue(0)).HasValue());
    ASSERT_TRUE(dba.InsertEdge(&v1, &v2, dba.NameToEdgeType("T")).HasValue());
    dba.AdvanceCommand();

    AstStorage storage;
    SymbolTable symbol_table;

    auto n = MakeScanAll(storage, symbol_table, "n");
    auto r_m = MakeExpand(storage, symbol_table, n.op_, n.sym_, "r", EdgeAtom::Direction::BOTH, {}, "m", false,
                          storage::View::OLD);

    auto one = LITERAL(1);
    auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);
    auto set_n_p = std::make_shared<plan::SetProperty>(r_m.op_, prop, n_p, ADD(n_p, one));
    auto m_p = PROPERTY_LOOKUP(IDENT("m")->MapTo(r_m.node_sym_), prop);
    auto set_m_p = std::make_shared<plan::SetProperty>(set_n_p, prop, m_p, ADD(m_p, one));

    std::shared_ptr<LogicalOperator> last_op = set_m_p;
    if (accumulate) {
      last_op = std::make_shared<Accumulate>(last_op, std::vector<Symbol>{n.sym_, r_m.node_sym_});
    }

    auto n_p_ne = NEXPR("n.p", n_p)->MapTo(symbol_table.CreateSymbol("n_p_ne", true));
    auto m_p_ne = NEXPR("m.p", m_p)->MapTo(symbol_table.CreateSymbol("m_p_ne", true));
    auto produce = MakeProduce(last_op, n_p_ne, m_p_ne);
    auto context = MakeContext(storage, symbol_table, &dba);
    auto results = CollectProduce(*produce, &context);
    std::vector<int> results_data;
    for (const auto &row : results)
      for (const auto &column : row) results_data.emplace_back(column.ValueInt());
    if (accumulate)
      EXPECT_THAT(results_data, testing::ElementsAre(2, 2, 2, 2));
    else
      EXPECT_THAT(results_data, testing::ElementsAre(1, 1, 2, 2));
  };

  check(false);
  check(true);
}

TEST(QueryPlan, AccumulateAdvance) {
  // we simulate 'CREATE (n) WITH n AS n MATCH (m) RETURN m'
  // to get correct results we need to advance the command
  auto check = [&](bool advance) {
    storage::Storage db;
    auto storage_dba = db.Access();
    query::DbAccessor dba(&storage_dba);
    AstStorage storage;
    SymbolTable symbol_table;
    NodeCreationInfo node;
    node.symbol = symbol_table.CreateSymbol("n", true);
    auto create = std::make_shared<CreateNode>(nullptr, node);
    auto accumulate = std::make_shared<Accumulate>(create, std::vector<Symbol>{node.symbol}, advance);
    auto match = MakeScanAll(storage, symbol_table, "m", accumulate);
    auto context = MakeContext(storage, symbol_table, &dba);
    EXPECT_EQ(advance ? 1 : 0, PullAll(*match.op_, &context));
  };
  check(false);
  check(true);
}

std::shared_ptr<Produce> MakeAggregationProduce(std::shared_ptr<LogicalOperator> input, SymbolTable &symbol_table,
                                                AstStorage &storage, const std::vector<Expression *> aggr_inputs,
                                                const std::vector<Aggregation::Op> aggr_ops,
                                                const std::vector<Expression *> group_by_exprs,
                                                const std::vector<Symbol> remember) {
  // prepare all the aggregations
  std::vector<Aggregate::Element> aggregates;
  std::vector<NamedExpression *> named_expressions;

  auto aggr_inputs_it = aggr_inputs.begin();
  for (auto aggr_op : aggr_ops) {
    // TODO change this from using IDENT to using AGGREGATION
    // once AGGREGATION is handled properly in ExpressionEvaluation
    auto aggr_sym = symbol_table.CreateSymbol("aggregation", true);
    auto named_expr =
        NEXPR("", IDENT("aggregation")->MapTo(aggr_sym))->MapTo(symbol_table.CreateSymbol("named_expression", true));
    named_expressions.push_back(named_expr);
    // the key expression is only used in COLLECT_MAP
    Expression *key_expr_ptr = aggr_op == Aggregation::Op::COLLECT_MAP ? LITERAL("key") : nullptr;
    aggregates.emplace_back(Aggregate::Element{*aggr_inputs_it++, key_expr_ptr, aggr_op, aggr_sym});
  }

  // Produce will also evaluate group_by expressions and return them after the
  // aggregations.
  for (auto group_by_expr : group_by_exprs) {
    auto named_expr = NEXPR("", group_by_expr)->MapTo(symbol_table.CreateSymbol("named_expression", true));
    named_expressions.push_back(named_expr);
  }
  auto aggregation = std::make_shared<Aggregate>(input, aggregates, group_by_exprs, remember);
  return std::make_shared<Produce>(aggregation, named_expressions);
}

/** Test fixture for all the aggregation ops in one return. */
class QueryPlanAggregateOps : public ::testing::Test {
 protected:
  storage::Storage db;
  storage::Storage::Accessor storage_dba{db.Access()};
  query::DbAccessor dba{&storage_dba};
  storage::PropertyId prop = db.NameToProperty("prop");

  AstStorage storage;
  SymbolTable symbol_table;

  void AddData() {
    // setup is several nodes most of which have an int property set
    // we will take the sum, avg, min, max and count
    // we won't group by anything
    ASSERT_TRUE(dba.InsertVertex().SetProperty(prop, storage::PropertyValue(5)).HasValue());
    ASSERT_TRUE(dba.InsertVertex().SetProperty(prop, storage::PropertyValue(7)).HasValue());
    ASSERT_TRUE(dba.InsertVertex().SetProperty(prop, storage::PropertyValue(12)).HasValue());
    // a missing property (null) gets ignored by all aggregations except
    // COUNT(*)
    dba.InsertVertex();
    dba.AdvanceCommand();
  }

  auto AggregationResults(bool with_group_by, std::vector<Aggregation::Op> ops = {
                                                  Aggregation::Op::COUNT, Aggregation::Op::COUNT, Aggregation::Op::MIN,
                                                  Aggregation::Op::MAX, Aggregation::Op::SUM, Aggregation::Op::AVG,
                                                  Aggregation::Op::COLLECT_LIST, Aggregation::Op::COLLECT_MAP}) {
    // match all nodes and perform aggregations
    auto n = MakeScanAll(storage, symbol_table, "n");
    auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);

    std::vector<Expression *> aggregation_expressions(ops.size(), n_p);
    std::vector<Expression *> group_bys;
    if (with_group_by) group_bys.push_back(n_p);
    aggregation_expressions[0] = nullptr;
    auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, aggregation_expressions, ops, group_bys, {});
    auto context = MakeContext(storage, symbol_table, &dba);
    return CollectProduce(*produce, &context);
  }
};

TEST_F(QueryPlanAggregateOps, WithData) {
  AddData();
  auto results = AggregationResults(false);

  ASSERT_EQ(results.size(), 1);
  ASSERT_EQ(results[0].size(), 8);
  // count(*)
  ASSERT_EQ(results[0][0].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][0].ValueInt(), 4);
  // count
  ASSERT_EQ(results[0][1].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][1].ValueInt(), 3);
  // min
  ASSERT_EQ(results[0][2].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][2].ValueInt(), 5);
  // max
  ASSERT_EQ(results[0][3].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][3].ValueInt(), 12);
  // sum
  ASSERT_EQ(results[0][4].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][4].ValueInt(), 24);
  // avg
  ASSERT_EQ(results[0][5].type(), TypedValue::Type::Double);
  EXPECT_FLOAT_EQ(results[0][5].ValueDouble(), 24 / 3.0);
  // collect list
  ASSERT_EQ(results[0][6].type(), TypedValue::Type::List);
  EXPECT_THAT(ToIntList(results[0][6]), UnorderedElementsAre(5, 7, 12));
  // collect map
  ASSERT_EQ(results[0][7].type(), TypedValue::Type::Map);
  auto map = ToIntMap(results[0][7]);
  ASSERT_EQ(map.size(), 1);
  EXPECT_EQ(map.begin()->first, "key");
  EXPECT_FALSE(std::set<int>({5, 7, 12}).insert(map.begin()->second).second);
}

TEST_F(QueryPlanAggregateOps, WithoutDataWithGroupBy) {
  {
    auto results = AggregationResults(true, {Aggregation::Op::COUNT});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::SUM});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::AVG});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::MIN});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::MAX});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::COLLECT_LIST});
    EXPECT_EQ(results.size(), 0);
  }
  {
    auto results = AggregationResults(true, {Aggregation::Op::COLLECT_MAP});
    EXPECT_EQ(results.size(), 0);
  }
}

TEST_F(QueryPlanAggregateOps, WithoutDataWithoutGroupBy) {
  auto results = AggregationResults(false);
  ASSERT_EQ(results.size(), 1);
  ASSERT_EQ(results[0].size(), 8);
  // count(*)
  ASSERT_EQ(results[0][0].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][0].ValueInt(), 0);
  // count
  ASSERT_EQ(results[0][1].type(), TypedValue::Type::Int);
  EXPECT_EQ(results[0][1].ValueInt(), 0);
  // min
  EXPECT_TRUE(results[0][2].IsNull());
  // max
  EXPECT_TRUE(results[0][3].IsNull());
  // sum
  EXPECT_TRUE(results[0][4].IsNull());
  // avg
  EXPECT_TRUE(results[0][5].IsNull());
  // collect list
  ASSERT_EQ(results[0][6].type(), TypedValue::Type::List);
  EXPECT_EQ(ToIntList(results[0][6]).size(), 0);
  // collect map
  ASSERT_EQ(results[0][7].type(), TypedValue::Type::Map);
  EXPECT_EQ(ToIntMap(results[0][7]).size(), 0);
}

TEST(QueryPlan, AggregateGroupByValues) {
  // Tests that distinct groups are aggregated properly for values of all types.
  // Also test the "remember" part of the Aggregation API as final results are
  // obtained via a property lookup of a remembered node.
  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);

  // a vector of storage::PropertyValue to be set as property values on vertices
  // most of them should result in a distinct group (commented where not)
  std::vector<storage::PropertyValue> group_by_vals;
  group_by_vals.emplace_back(4);
  group_by_vals.emplace_back(7);
  group_by_vals.emplace_back(7.3);
  group_by_vals.emplace_back(7.2);
  group_by_vals.emplace_back("Johhny");
  group_by_vals.emplace_back("Jane");
  group_by_vals.emplace_back("1");
  group_by_vals.emplace_back(true);
  group_by_vals.emplace_back(false);
  group_by_vals.emplace_back(std::vector<storage::PropertyValue>{storage::PropertyValue(1)});
  group_by_vals.emplace_back(std::vector<storage::PropertyValue>{storage::PropertyValue(1), storage::PropertyValue(2)});
  group_by_vals.emplace_back(std::vector<storage::PropertyValue>{storage::PropertyValue(2), storage::PropertyValue(1)});
  group_by_vals.emplace_back(storage::PropertyValue());
  // should NOT result in another group because 7.0 == 7
  group_by_vals.emplace_back(7.0);
  // should NOT result in another group
  group_by_vals.emplace_back(
      std::vector<storage::PropertyValue>{storage::PropertyValue(1), storage::PropertyValue(2.0)});

  // generate a lot of vertices and set props on them
  auto prop = dba.NameToProperty("prop");
  for (int i = 0; i < 1000; ++i)
    ASSERT_TRUE(dba.InsertVertex().SetProperty(prop, group_by_vals[i % group_by_vals.size()]).HasValue());
  dba.AdvanceCommand();

  AstStorage storage;
  SymbolTable symbol_table;

  // match all nodes and perform aggregations
  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);

  auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, {n_p}, {Aggregation::Op::COUNT}, {n_p}, {n.sym_});

  auto context = MakeContext(storage, symbol_table, &dba);
  auto results = CollectProduce(*produce, &context);
  ASSERT_EQ(results.size(), group_by_vals.size() - 2);
  std::unordered_set<TypedValue, TypedValue::Hash, TypedValue::BoolEqual> result_group_bys;
  for (const auto &row : results) {
    ASSERT_EQ(2, row.size());
    result_group_bys.insert(row[1]);
  }
  ASSERT_EQ(result_group_bys.size(), group_by_vals.size() - 2);
  std::vector<TypedValue> group_by_tvals;
  group_by_tvals.reserve(group_by_vals.size());
  for (const auto &v : group_by_vals) group_by_tvals.emplace_back(v);
  EXPECT_TRUE(std::is_permutation(group_by_tvals.begin(), group_by_tvals.end() - 2, result_group_bys.begin(),
                                  TypedValue::BoolEqual{}));
}

TEST(QueryPlan, AggregateMultipleGroupBy) {
  // in this test we have 3 different properties that have different values
  // for different records and assert that we get the correct combination
  // of values in our groups
  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);

  auto prop1 = dba.NameToProperty("prop1");
  auto prop2 = dba.NameToProperty("prop2");
  auto prop3 = dba.NameToProperty("prop3");
  for (int i = 0; i < 2 * 3 * 5; ++i) {
    auto v = dba.InsertVertex();
    ASSERT_TRUE(v.SetProperty(prop1, storage::PropertyValue(static_cast<bool>(i % 2))).HasValue());
    ASSERT_TRUE(v.SetProperty(prop2, storage::PropertyValue(i % 3)).HasValue());
    ASSERT_TRUE(v.SetProperty(prop3, storage::PropertyValue("value" + std::to_string(i % 5))).HasValue());
  }
  dba.AdvanceCommand();

  AstStorage storage;
  SymbolTable symbol_table;

  // match all nodes and perform aggregations
  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_p1 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop1);
  auto n_p2 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop2);
  auto n_p3 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop3);

  auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, {n_p1}, {Aggregation::Op::COUNT},
                                        {n_p1, n_p2, n_p3}, {n.sym_});

  auto context = MakeContext(storage, symbol_table, &dba);
  auto results = CollectProduce(*produce, &context);
  EXPECT_EQ(results.size(), 2 * 3 * 5);
}

TEST(QueryPlan, AggregateNoInput) {
  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);
  AstStorage storage;
  SymbolTable symbol_table;

  auto two = LITERAL(2);
  auto produce = MakeAggregationProduce(nullptr, symbol_table, storage, {two}, {Aggregation::Op::COUNT}, {}, {});
  auto context = MakeContext(storage, symbol_table, &dba);
  auto results = CollectProduce(*produce, &context);
  EXPECT_EQ(1, results.size());
  EXPECT_EQ(1, results[0].size());
  EXPECT_EQ(TypedValue::Type::Int, results[0][0].type());
  EXPECT_EQ(1, results[0][0].ValueInt());
}

TEST(QueryPlan, AggregateCountEdgeCases) {
  // tests for detected bugs in the COUNT aggregation behavior
  // ensure that COUNT returns correctly for
  //  - 0 vertices in database
  //  - 1 vertex in database, property not set
  //  - 1 vertex in database, property set
  //  - 2 vertices in database, property set on one
  //  - 2 vertices in database, property set on both

  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);
  auto prop = dba.NameToProperty("prop");

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_p = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop);

  // returns -1 when there are no results
  // otherwise returns MATCH (n) RETURN count(n.prop)
  auto count = [&]() {
    auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, {n_p}, {Aggregation::Op::COUNT}, {}, {});
    auto context = MakeContext(storage, symbol_table, &dba);
    auto results = CollectProduce(*produce, &context);
    if (results.size() == 0) return -1L;
    EXPECT_EQ(1, results.size());
    EXPECT_EQ(1, results[0].size());
    EXPECT_EQ(TypedValue::Type::Int, results[0][0].type());
    return results[0][0].ValueInt();
  };

  // no vertices yet in database
  EXPECT_EQ(0, count());

  // one vertex, no property set
  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(0, count());

  // one vertex, property set
  for (auto va : dba.Vertices(storage::View::OLD))
    ASSERT_TRUE(va.SetProperty(prop, storage::PropertyValue(42)).HasValue());
  dba.AdvanceCommand();
  EXPECT_EQ(1, count());

  // two vertices, one with property set
  dba.InsertVertex();
  dba.AdvanceCommand();
  EXPECT_EQ(1, count());

  // two vertices, both with property set
  for (auto va : dba.Vertices(storage::View::OLD))
    ASSERT_TRUE(va.SetProperty(prop, storage::PropertyValue(42)).HasValue());
  dba.AdvanceCommand();
  EXPECT_EQ(2, count());
}

TEST(QueryPlan, AggregateFirstValueTypes) {
  // testing exceptions that get emitted by the first-value
  // type check

  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);

  auto v1 = dba.InsertVertex();
  auto prop_string = dba.NameToProperty("string");
  ASSERT_TRUE(v1.SetProperty(prop_string, storage::PropertyValue("johhny")).HasValue());
  auto prop_int = dba.NameToProperty("int");
  ASSERT_TRUE(v1.SetProperty(prop_int, storage::PropertyValue(12)).HasValue());
  dba.AdvanceCommand();

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_prop_string = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop_string);
  auto n_prop_int = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), prop_int);
  auto n_id = n_prop_string->expression_;

  auto aggregate = [&](Expression *expression, Aggregation::Op aggr_op) {
    auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, {expression}, {aggr_op}, {}, {});
    auto context = MakeContext(storage, symbol_table, &dba);
    CollectProduce(*produce, &context);
  };

  // everything except for COUNT and COLLECT fails on a Vertex
  aggregate(n_id, Aggregation::Op::COUNT);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::MIN), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::MAX), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::AVG), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::SUM), QueryRuntimeException);

  // on strings AVG and SUM fail
  aggregate(n_prop_string, Aggregation::Op::COUNT);
  aggregate(n_prop_string, Aggregation::Op::MIN);
  aggregate(n_prop_string, Aggregation::Op::MAX);
  EXPECT_THROW(aggregate(n_prop_string, Aggregation::Op::AVG), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_prop_string, Aggregation::Op::SUM), QueryRuntimeException);

  // on ints nothing fails
  aggregate(n_prop_int, Aggregation::Op::COUNT);
  aggregate(n_prop_int, Aggregation::Op::MIN);
  aggregate(n_prop_int, Aggregation::Op::MAX);
  aggregate(n_prop_int, Aggregation::Op::AVG);
  aggregate(n_prop_int, Aggregation::Op::SUM);
  aggregate(n_prop_int, Aggregation::Op::COLLECT_LIST);
  aggregate(n_prop_int, Aggregation::Op::COLLECT_MAP);
}

TEST(QueryPlan, AggregateTypes) {
  // testing exceptions that can get emitted by an aggregation
  // does not check all combinations that can result in an exception
  // (that logic is defined and tested by TypedValue)

  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);

  auto p1 = dba.NameToProperty("p1");  // has only string props
  ASSERT_TRUE(dba.InsertVertex().SetProperty(p1, storage::PropertyValue("string")).HasValue());
  ASSERT_TRUE(dba.InsertVertex().SetProperty(p1, storage::PropertyValue("str2")).HasValue());
  auto p2 = dba.NameToProperty("p2");  // combines int and bool
  ASSERT_TRUE(dba.InsertVertex().SetProperty(p2, storage::PropertyValue(42)).HasValue());
  ASSERT_TRUE(dba.InsertVertex().SetProperty(p2, storage::PropertyValue(true)).HasValue());
  dba.AdvanceCommand();

  AstStorage storage;
  SymbolTable symbol_table;

  auto n = MakeScanAll(storage, symbol_table, "n");
  auto n_p1 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), p1);
  auto n_p2 = PROPERTY_LOOKUP(IDENT("n")->MapTo(n.sym_), p2);

  auto aggregate = [&](Expression *expression, Aggregation::Op aggr_op) {
    auto produce = MakeAggregationProduce(n.op_, symbol_table, storage, {expression}, {aggr_op}, {}, {});
    auto context = MakeContext(storage, symbol_table, &dba);
    CollectProduce(*produce, &context);
  };

  // everything except for COUNT and COLLECT fails on a Vertex
  auto n_id = n_p1->expression_;
  aggregate(n_id, Aggregation::Op::COUNT);
  aggregate(n_id, Aggregation::Op::COLLECT_LIST);
  aggregate(n_id, Aggregation::Op::COLLECT_MAP);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::MIN), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::MAX), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::AVG), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_id, Aggregation::Op::SUM), QueryRuntimeException);

  // on strings AVG and SUM fail
  aggregate(n_p1, Aggregation::Op::COUNT);
  aggregate(n_p1, Aggregation::Op::COLLECT_LIST);
  aggregate(n_p1, Aggregation::Op::COLLECT_MAP);
  aggregate(n_p1, Aggregation::Op::MIN);
  aggregate(n_p1, Aggregation::Op::MAX);
  EXPECT_THROW(aggregate(n_p1, Aggregation::Op::AVG), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_p1, Aggregation::Op::SUM), QueryRuntimeException);

  // combination of int and bool, everything except COUNT and COLLECT fails
  aggregate(n_p2, Aggregation::Op::COUNT);
  aggregate(n_p2, Aggregation::Op::COLLECT_LIST);
  aggregate(n_p2, Aggregation::Op::COLLECT_MAP);
  EXPECT_THROW(aggregate(n_p2, Aggregation::Op::MIN), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_p2, Aggregation::Op::MAX), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_p2, Aggregation::Op::AVG), QueryRuntimeException);
  EXPECT_THROW(aggregate(n_p2, Aggregation::Op::SUM), QueryRuntimeException);
}

TEST(QueryPlan, Unwind) {
  storage::Storage db;
  auto storage_dba = db.Access();
  query::DbAccessor dba(&storage_dba);
  AstStorage storage;
  SymbolTable symbol_table;

  // UNWIND [ [1, true, "x"], [], ["bla"] ] AS x UNWIND x as y RETURN x, y
  auto input_expr = storage.Create<PrimitiveLiteral>(std::vector<storage::PropertyValue>{
      storage::PropertyValue(std::vector<storage::PropertyValue>{
          storage::PropertyValue(1), storage::PropertyValue(true), storage::PropertyValue("x")}),
      storage::PropertyValue(std::vector<storage::PropertyValue>{}),
      storage::PropertyValue(std::vector<storage::PropertyValue>{storage::PropertyValue("bla")})});

  auto x = symbol_table.CreateSymbol("x", true);
  auto unwind_0 = std::make_shared<plan::Unwind>(nullptr, input_expr, x);
  auto x_expr = IDENT("x")->MapTo(x);
  auto y = symbol_table.CreateSymbol("y", true);
  auto unwind_1 = std::make_shared<plan::Unwind>(unwind_0, x_expr, y);

  auto x_ne = NEXPR("x", x_expr)->MapTo(symbol_table.CreateSymbol("x_ne", true));
  auto y_ne = NEXPR("y", IDENT("y")->MapTo(y))->MapTo(symbol_table.CreateSymbol("y_ne", true));
  auto produce = MakeProduce(unwind_1, x_ne, y_ne);

  auto context = MakeContext(storage, symbol_table, &dba);
  auto results = CollectProduce(*produce, &context);
  ASSERT_EQ(4, results.size());
  const std::vector<int> expected_x_card{3, 3, 3, 1};
  auto expected_x_card_it = expected_x_card.begin();
  const std::vector<TypedValue> expected_y{TypedValue(1), TypedValue(true), TypedValue("x"), TypedValue("bla")};
  auto expected_y_it = expected_y.begin();
  for (const auto &row : results) {
    ASSERT_EQ(2, row.size());
    ASSERT_EQ(row[0].type(), TypedValue::Type::List);
    EXPECT_EQ(row[0].ValueList().size(), *expected_x_card_it);
    EXPECT_EQ(row[1].type(), expected_y_it->type());
    expected_x_card_it++;
    expected_y_it++;
  }
}
