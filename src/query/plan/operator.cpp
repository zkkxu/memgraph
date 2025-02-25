// Copyright 2021 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include "query/plan/operator.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <cppitertools/chain.hpp>
#include <cppitertools/imap.hpp>

#include "query/context.hpp"
#include "query/db_accessor.hpp"
#include "query/exceptions.hpp"
#include "query/frontend/ast/ast.hpp"
#include "query/frontend/semantic/symbol_table.hpp"
#include "query/interpret/eval.hpp"
#include "query/path.hpp"
#include "query/plan/scoped_profile.hpp"
#include "query/procedure/cypher_types.hpp"
#include "query/procedure/mg_procedure_impl.hpp"
#include "query/procedure/module.hpp"
#include "storage/v2/property_value.hpp"
#include "utils/algorithm.hpp"
#include "utils/csv_parsing.hpp"
#include "utils/event_counter.hpp"
#include "utils/exceptions.hpp"
#include "utils/fnv.hpp"
#include "utils/likely.hpp"
#include "utils/logging.hpp"
#include "utils/pmr/unordered_map.hpp"
#include "utils/pmr/unordered_set.hpp"
#include "utils/pmr/vector.hpp"
#include "utils/readable_size.hpp"
#include "utils/string.hpp"

// macro for the default implementation of LogicalOperator::Accept
// that accepts the visitor and visits it's input_ operator
#define ACCEPT_WITH_INPUT(class_name)                                    \
  bool class_name::Accept(HierarchicalLogicalOperatorVisitor &visitor) { \
    if (visitor.PreVisit(*this)) {                                       \
      input_->Accept(visitor);                                           \
    }                                                                    \
    return visitor.PostVisit(*this);                                     \
  }

#define WITHOUT_SINGLE_INPUT(class_name)                         \
  bool class_name::HasSingleInput() const { return false; }      \
  std::shared_ptr<LogicalOperator> class_name::input() const {   \
    LOG_FATAL("Operator " #class_name " has no single input!");  \
  }                                                              \
  void class_name::set_input(std::shared_ptr<LogicalOperator>) { \
    LOG_FATAL("Operator " #class_name " has no single input!");  \
  }

namespace EventCounter {
extern const Event OnceOperator;
extern const Event CreateNodeOperator;
extern const Event CreateExpandOperator;
extern const Event ScanAllOperator;
extern const Event ScanAllByLabelOperator;
extern const Event ScanAllByLabelPropertyRangeOperator;
extern const Event ScanAllByLabelPropertyValueOperator;
extern const Event ScanAllByLabelPropertyOperator;
extern const Event ScanAllByIdOperator;
extern const Event ExpandOperator;
extern const Event ExpandVariableOperator;
extern const Event ConstructNamedPathOperator;
extern const Event FilterOperator;
extern const Event ProduceOperator;
extern const Event DeleteOperator;
extern const Event SetPropertyOperator;
extern const Event SetPropertiesOperator;
extern const Event SetLabelsOperator;
extern const Event RemovePropertyOperator;
extern const Event RemoveLabelsOperator;
extern const Event EdgeUniquenessFilterOperator;
extern const Event AccumulateOperator;
extern const Event AggregateOperator;
extern const Event SkipOperator;
extern const Event LimitOperator;
extern const Event OrderByOperator;
extern const Event MergeOperator;
extern const Event OptionalOperator;
extern const Event UnwindOperator;
extern const Event DistinctOperator;
extern const Event UnionOperator;
extern const Event CartesianOperator;
extern const Event CallProcedureOperator;
}  // namespace EventCounter

namespace query::plan {

namespace {

// Custom equality function for a vector of typed values.
// Used in unordered_maps in Aggregate and Distinct operators.
struct TypedValueVectorEqual {
  template <class TAllocator>
  bool operator()(const std::vector<TypedValue, TAllocator> &left,
                  const std::vector<TypedValue, TAllocator> &right) const {
    MG_ASSERT(left.size() == right.size(),
              "TypedValueVector comparison should only be done over vectors "
              "of the same size");
    return std::equal(left.begin(), left.end(), right.begin(), TypedValue::BoolEqual{});
  }
};

// Returns boolean result of evaluating filter expression. Null is treated as
// false. Other non boolean values raise a QueryRuntimeException.
bool EvaluateFilter(ExpressionEvaluator &evaluator, Expression *filter) {
  TypedValue result = filter->Accept(evaluator);
  // Null is treated like false.
  if (result.IsNull()) return false;
  if (result.type() != TypedValue::Type::Bool)
    throw QueryRuntimeException("Filter expression must evaluate to bool or null, got {}.", result.type());
  return result.ValueBool();
}

template <typename T>
uint64_t ComputeProfilingKey(const T *obj) {
  static_assert(sizeof(T *) == sizeof(uint64_t));
  return reinterpret_cast<uint64_t>(obj);
}

}  // namespace

#define SCOPED_PROFILE_OP(name) ScopedProfile profile{ComputeProfilingKey(this), name, &context};

bool Once::OnceCursor::Pull(Frame &, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Once");

  if (!did_pull_) {
    did_pull_ = true;
    return true;
  }
  return false;
}

UniqueCursorPtr Once::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OnceOperator);

  return MakeUniqueCursorPtr<OnceCursor>(mem);
}

WITHOUT_SINGLE_INPUT(Once);

void Once::OnceCursor::Shutdown() {}

void Once::OnceCursor::Reset() { did_pull_ = false; }

CreateNode::CreateNode(const std::shared_ptr<LogicalOperator> &input, const NodeCreationInfo &node_info)
    : input_(input ? input : std::make_shared<Once>()), node_info_(node_info) {}

// Creates a vertex on this GraphDb. Returns a reference to vertex placed on the
// frame.
VertexAccessor &CreateLocalVertex(const NodeCreationInfo &node_info, Frame *frame, const ExecutionContext &context) {
  auto &dba = *context.db_accessor;
  auto new_node = dba.InsertVertex();
  for (auto label : node_info.labels) {
    auto maybe_error = new_node.AddLabel(label);
    if (maybe_error.HasError()) {
      switch (maybe_error.GetError()) {
        case storage::Error::SERIALIZATION_ERROR:
          throw QueryRuntimeException(kSerializationErrorMessage);
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to set a label on a deleted node.");
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::PROPERTIES_DISABLED:
        case storage::Error::NONEXISTENT_OBJECT:
          throw QueryRuntimeException("Unexpected error when setting a label.");
      }
    }
  }
  // Evaluator should use the latest accessors, as modified in this query, when
  // setting properties on new nodes.
  ExpressionEvaluator evaluator(frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);
  // TODO: PropsSetChecked allocates a PropertyValue, make it use context.memory
  // when we update PropertyValue with custom allocator.
  if (const auto *node_info_properties = std::get_if<PropertiesMapList>(&node_info.properties)) {
    for (const auto &[key, value_expression] : *node_info_properties) {
      PropsSetChecked(&new_node, key, value_expression->Accept(evaluator));
    }
  } else {
    auto property_map = evaluator.Visit(*std::get<ParameterLookup *>(node_info.properties));
    for (const auto &[key, value] : property_map.ValueMap()) {
      auto property_id = dba.NameToProperty(key);
      PropsSetChecked(&new_node, property_id, value);
    }
  }

  (*frame)[node_info.symbol] = new_node;
  return (*frame)[node_info.symbol].ValueVertex();
}

ACCEPT_WITH_INPUT(CreateNode)

UniqueCursorPtr CreateNode::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CreateNodeOperator);

  return MakeUniqueCursorPtr<CreateNodeCursor>(mem, *this, mem);
}

std::vector<Symbol> CreateNode::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(node_info_.symbol);
  return symbols;
}

CreateNode::CreateNodeCursor::CreateNodeCursor(const CreateNode &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool CreateNode::CreateNodeCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("CreateNode");

  if (input_cursor_->Pull(frame, context)) {
    auto created_vertex = CreateLocalVertex(self_.node_info_, &frame, context);
    if (context.trigger_context_collector) {
      context.trigger_context_collector->RegisterCreatedObject(created_vertex);
    }
    return true;
  }

  return false;
}

void CreateNode::CreateNodeCursor::Shutdown() { input_cursor_->Shutdown(); }

void CreateNode::CreateNodeCursor::Reset() { input_cursor_->Reset(); }

CreateExpand::CreateExpand(const NodeCreationInfo &node_info, const EdgeCreationInfo &edge_info,
                           const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, bool existing_node)
    : node_info_(node_info),
      edge_info_(edge_info),
      input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      existing_node_(existing_node) {}

ACCEPT_WITH_INPUT(CreateExpand)

UniqueCursorPtr CreateExpand::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CreateNodeOperator);

  return MakeUniqueCursorPtr<CreateExpandCursor>(mem, *this, mem);
}

std::vector<Symbol> CreateExpand::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(node_info_.symbol);
  symbols.emplace_back(edge_info_.symbol);
  return symbols;
}

CreateExpand::CreateExpandCursor::CreateExpandCursor(const CreateExpand &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

namespace {

EdgeAccessor CreateEdge(const EdgeCreationInfo &edge_info, DbAccessor *dba, VertexAccessor *from, VertexAccessor *to,
                        Frame *frame, ExpressionEvaluator *evaluator) {
  auto maybe_edge = dba->InsertEdge(from, to, edge_info.edge_type);
  if (maybe_edge.HasValue()) {
    auto &edge = *maybe_edge;
    if (const auto *properties = std::get_if<PropertiesMapList>(&edge_info.properties)) {
      for (const auto &[key, value_expression] : *properties) {
        PropsSetChecked(&edge, key, value_expression->Accept(*evaluator));
      }
    } else {
      auto property_map = evaluator->Visit(*std::get<ParameterLookup *>(edge_info.properties));
      for (const auto &[key, value] : property_map.ValueMap()) {
        auto property_id = dba->NameToProperty(key);
        PropsSetChecked(&edge, property_id, value);
      }
    }

    (*frame)[edge_info.symbol] = edge;
  } else {
    switch (maybe_edge.GetError()) {
      case storage::Error::SERIALIZATION_ERROR:
        throw QueryRuntimeException(kSerializationErrorMessage);
      case storage::Error::DELETED_OBJECT:
        throw QueryRuntimeException("Trying to create an edge on a deleted node.");
      case storage::Error::VERTEX_HAS_EDGES:
      case storage::Error::PROPERTIES_DISABLED:
      case storage::Error::NONEXISTENT_OBJECT:
        throw QueryRuntimeException("Unexpected error when creating an edge.");
    }
  }

  return *maybe_edge;
}

}  // namespace

bool CreateExpand::CreateExpandCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("CreateExpand");

  if (!input_cursor_->Pull(frame, context)) return false;

  // get the origin vertex
  TypedValue &vertex_value = frame[self_.input_symbol_];
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &v1 = vertex_value.ValueVertex();

  // Similarly to CreateNode, newly created edges and nodes should use the
  // storage::View::NEW.
  // E.g. we pickup new properties: `CREATE (n {p: 42}) -[:r {ep: n.p}]-> ()`
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);

  // get the destination vertex (possibly an existing node)
  auto &v2 = OtherVertex(frame, context);

  // create an edge between the two nodes
  auto *dba = context.db_accessor;

  auto created_edge = [&] {
    switch (self_.edge_info_.direction) {
      case EdgeAtom::Direction::IN:
        return CreateEdge(self_.edge_info_, dba, &v2, &v1, &frame, &evaluator);
      case EdgeAtom::Direction::OUT:
      // in the case of an undirected CreateExpand we choose an arbitrary
      // direction. this is used in the MERGE clause
      // it is not allowed in the CREATE clause, and the semantic
      // checker needs to ensure it doesn't reach this point
      case EdgeAtom::Direction::BOTH:
        return CreateEdge(self_.edge_info_, dba, &v1, &v2, &frame, &evaluator);
    }
  }();

  if (context.trigger_context_collector) {
    context.trigger_context_collector->RegisterCreatedObject(created_edge);
  }

  return true;
}

void CreateExpand::CreateExpandCursor::Shutdown() { input_cursor_->Shutdown(); }

void CreateExpand::CreateExpandCursor::Reset() { input_cursor_->Reset(); }

VertexAccessor &CreateExpand::CreateExpandCursor::OtherVertex(Frame &frame, ExecutionContext &context) {
  if (self_.existing_node_) {
    TypedValue &dest_node_value = frame[self_.node_info_.symbol];
    ExpectType(self_.node_info_.symbol, dest_node_value, TypedValue::Type::Vertex);
    return dest_node_value.ValueVertex();
  } else {
    auto &created_vertex = CreateLocalVertex(self_.node_info_, &frame, context);
    if (context.trigger_context_collector) {
      context.trigger_context_collector->RegisterCreatedObject(created_vertex);
    }
    return created_vertex;
  }
}

template <class TVerticesFun>
class ScanAllCursor : public Cursor {
 public:
  explicit ScanAllCursor(Symbol output_symbol, UniqueCursorPtr input_cursor, TVerticesFun get_vertices,
                         const char *op_name)
      : output_symbol_(output_symbol),
        input_cursor_(std::move(input_cursor)),
        get_vertices_(std::move(get_vertices)),
        op_name_(op_name) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP(op_name_);

    if (MustAbort(context)) throw HintedAbortError();

    while (!vertices_ || vertices_it_.value() == vertices_.value().end()) {
      if (!input_cursor_->Pull(frame, context)) return false;
      // We need a getter function, because in case of exhausting a lazy
      // iterable, we cannot simply reset it by calling begin().
      auto next_vertices = get_vertices_(frame, context);
      if (!next_vertices) continue;
      // Since vertices iterator isn't nothrow_move_assignable, we have to use
      // the roundabout assignment + emplace, instead of simple:
      // vertices _ = get_vertices_(frame, context);
      vertices_.emplace(std::move(next_vertices.value()));
      vertices_it_.emplace(vertices_.value().begin());
    }

    frame[output_symbol_] = *vertices_it_.value();
    ++vertices_it_.value();
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    vertices_ = std::nullopt;
    vertices_it_ = std::nullopt;
  }

 private:
  const Symbol output_symbol_;
  const UniqueCursorPtr input_cursor_;
  TVerticesFun get_vertices_;
  std::optional<typename std::result_of<TVerticesFun(Frame &, ExecutionContext &)>::type::value_type> vertices_;
  std::optional<decltype(vertices_.value().begin())> vertices_it_;
  const char *op_name_;
};

ScanAll::ScanAll(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol, storage::View view)
    : input_(input ? input : std::make_shared<Once>()), output_symbol_(output_symbol), view_(view) {}

ACCEPT_WITH_INPUT(ScanAll)

UniqueCursorPtr ScanAll::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllOperator);

  auto vertices = [this](Frame &, ExecutionContext &context) {
    auto *db = context.db_accessor;
    return std::make_optional(db->Vertices(view_));
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAll");
}

std::vector<Symbol> ScanAll::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(output_symbol_);
  return symbols;
}

ScanAllByLabel::ScanAllByLabel(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
                               storage::LabelId label, storage::View view)
    : ScanAll(input, output_symbol, view), label_(label) {}

ACCEPT_WITH_INPUT(ScanAllByLabel)

UniqueCursorPtr ScanAllByLabel::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelOperator);

  auto vertices = [this](Frame &, ExecutionContext &context) {
    auto *db = context.db_accessor;
    return std::make_optional(db->Vertices(view_, label_));
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAllByLabel");
}

// TODO(buda): Implement ScanAllByLabelProperty operator to iterate over
// vertices that have the label and some value for the given property.

ScanAllByLabelPropertyRange::ScanAllByLabelPropertyRange(const std::shared_ptr<LogicalOperator> &input,
                                                         Symbol output_symbol, storage::LabelId label,
                                                         storage::PropertyId property, const std::string &property_name,
                                                         std::optional<Bound> lower_bound,
                                                         std::optional<Bound> upper_bound, storage::View view)
    : ScanAll(input, output_symbol, view),
      label_(label),
      property_(property),
      property_name_(property_name),
      lower_bound_(lower_bound),
      upper_bound_(upper_bound) {
  MG_ASSERT(lower_bound_ || upper_bound_, "Only one bound can be left out");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyRange)

UniqueCursorPtr ScanAllByLabelPropertyRange::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyRangeOperator);

  auto vertices = [this](Frame &frame, ExecutionContext &context)
      -> std::optional<decltype(context.db_accessor->Vertices(view_, label_, property_, std::nullopt, std::nullopt))> {
    auto *db = context.db_accessor;
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor, view_);
    auto convert = [&evaluator](const auto &bound) -> std::optional<utils::Bound<storage::PropertyValue>> {
      if (!bound) return std::nullopt;
      const auto &value = bound->value()->Accept(evaluator);
      try {
        const auto &property_value = storage::PropertyValue(value);
        switch (property_value.type()) {
          case storage::PropertyValue::Type::Bool:
          case storage::PropertyValue::Type::List:
          case storage::PropertyValue::Type::Map:
            // Prevent indexed lookup with something that would fail if we did
            // the original filter with `operator<`. Note, for some reason,
            // Cypher does not support comparing boolean values.
            throw QueryRuntimeException("Invalid type {} for '<'.", value.type());
          case storage::PropertyValue::Type::Null:
          case storage::PropertyValue::Type::Int:
          case storage::PropertyValue::Type::Double:
          case storage::PropertyValue::Type::String:
          case storage::PropertyValue::Type::TemporalData:
            // These are all fine, there's also Point, Date and Time data types
            // which were added to Cypher, but we don't have support for those
            // yet.
            return std::make_optional(utils::Bound<storage::PropertyValue>(property_value, bound->type()));
        }
      } catch (const TypedValueException &) {
        throw QueryRuntimeException("'{}' cannot be used as a property value.", value.type());
      }
    };
    auto maybe_lower = convert(lower_bound_);
    auto maybe_upper = convert(upper_bound_);
    // If any bound is null, then the comparison would result in nulls. This
    // is treated as not satisfying the filter, so return no vertices.
    if (maybe_lower && maybe_lower->value().IsNull()) return std::nullopt;
    if (maybe_upper && maybe_upper->value().IsNull()) return std::nullopt;
    return std::make_optional(db->Vertices(view_, label_, property_, maybe_lower, maybe_upper));
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAllByLabelPropertyRange");
}

ScanAllByLabelPropertyValue::ScanAllByLabelPropertyValue(const std::shared_ptr<LogicalOperator> &input,
                                                         Symbol output_symbol, storage::LabelId label,
                                                         storage::PropertyId property, const std::string &property_name,
                                                         Expression *expression, storage::View view)
    : ScanAll(input, output_symbol, view),
      label_(label),
      property_(property),
      property_name_(property_name),
      expression_(expression) {
  DMG_ASSERT(expression, "Expression is not optional.");
}

ACCEPT_WITH_INPUT(ScanAllByLabelPropertyValue)

UniqueCursorPtr ScanAllByLabelPropertyValue::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyValueOperator);

  auto vertices = [this](Frame &frame, ExecutionContext &context)
      -> std::optional<decltype(context.db_accessor->Vertices(view_, label_, property_, storage::PropertyValue()))> {
    auto *db = context.db_accessor;
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor, view_);
    auto value = expression_->Accept(evaluator);
    if (value.IsNull()) return std::nullopt;
    if (!value.IsPropertyValue()) {
      throw QueryRuntimeException("'{}' cannot be used as a property value.", value.type());
    }
    return std::make_optional(db->Vertices(view_, label_, property_, storage::PropertyValue(value)));
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAllByLabelPropertyValue");
}

ScanAllByLabelProperty::ScanAllByLabelProperty(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol,
                                               storage::LabelId label, storage::PropertyId property,
                                               const std::string &property_name, storage::View view)
    : ScanAll(input, output_symbol, view), label_(label), property_(property), property_name_(property_name) {}

ACCEPT_WITH_INPUT(ScanAllByLabelProperty)

UniqueCursorPtr ScanAllByLabelProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByLabelPropertyOperator);

  auto vertices = [this](Frame &frame, ExecutionContext &context) {
    auto *db = context.db_accessor;
    return std::make_optional(db->Vertices(view_, label_, property_));
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAllByLabelProperty");
}

ScanAllById::ScanAllById(const std::shared_ptr<LogicalOperator> &input, Symbol output_symbol, Expression *expression,
                         storage::View view)
    : ScanAll(input, output_symbol, view), expression_(expression) {
  MG_ASSERT(expression);
}

ACCEPT_WITH_INPUT(ScanAllById)

UniqueCursorPtr ScanAllById::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ScanAllByIdOperator);

  auto vertices = [this](Frame &frame, ExecutionContext &context) -> std::optional<std::vector<VertexAccessor>> {
    auto *db = context.db_accessor;
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor, view_);
    auto value = expression_->Accept(evaluator);
    if (!value.IsNumeric()) return std::nullopt;
    int64_t id = value.IsInt() ? value.ValueInt() : value.ValueDouble();
    if (value.IsDouble() && id != value.ValueDouble()) return std::nullopt;
    auto maybe_vertex = db->FindVertex(storage::Gid::FromInt(id), view_);
    if (!maybe_vertex) return std::nullopt;
    return std::vector<VertexAccessor>{*maybe_vertex};
  };
  return MakeUniqueCursorPtr<ScanAllCursor<decltype(vertices)>>(mem, output_symbol_, input_->MakeCursor(mem),
                                                                std::move(vertices), "ScanAllById");
}

namespace {
bool CheckExistingNode(const VertexAccessor &new_node, const Symbol &existing_node_sym, Frame &frame) {
  const TypedValue &existing_node = frame[existing_node_sym];
  if (existing_node.IsNull()) return false;
  ExpectType(existing_node_sym, existing_node, TypedValue::Type::Vertex);
  return existing_node.ValueVertex() == new_node;
}

template <class TEdges>
auto UnwrapEdgesResult(storage::Result<TEdges> &&result) {
  if (result.HasError()) {
    switch (result.GetError()) {
      case storage::Error::DELETED_OBJECT:
        throw QueryRuntimeException("Trying to get relationships of a deleted node.");
      case storage::Error::NONEXISTENT_OBJECT:
        throw query::QueryRuntimeException("Trying to get relationships from a node that doesn't exist.");
      case storage::Error::VERTEX_HAS_EDGES:
      case storage::Error::SERIALIZATION_ERROR:
      case storage::Error::PROPERTIES_DISABLED:
        throw QueryRuntimeException("Unexpected error when accessing relationships.");
    }
  }
  return std::move(*result);
}

}  // namespace

Expand::Expand(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Symbol node_symbol,
               Symbol edge_symbol, EdgeAtom::Direction direction, const std::vector<storage::EdgeTypeId> &edge_types,
               bool existing_node, storage::View view)
    : input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      common_{node_symbol, edge_symbol, direction, edge_types, existing_node},
      view_(view) {}

ACCEPT_WITH_INPUT(Expand)

UniqueCursorPtr Expand::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ExpandOperator);

  return MakeUniqueCursorPtr<ExpandCursor>(mem, *this, mem);
}

std::vector<Symbol> Expand::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(common_.node_symbol);
  symbols.emplace_back(common_.edge_symbol);
  return symbols;
}

Expand::ExpandCursor::ExpandCursor(const Expand &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool Expand::ExpandCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Expand");

  // A helper function for expanding a node from an edge.
  auto pull_node = [this, &frame](const EdgeAccessor &new_edge, EdgeAtom::Direction direction) {
    if (self_.common_.existing_node) return;
    switch (direction) {
      case EdgeAtom::Direction::IN:
        frame[self_.common_.node_symbol] = new_edge.From();
        break;
      case EdgeAtom::Direction::OUT:
        frame[self_.common_.node_symbol] = new_edge.To();
        break;
      case EdgeAtom::Direction::BOTH:
        LOG_FATAL("Must indicate exact expansion direction here");
    }
  };

  while (true) {
    if (MustAbort(context)) throw HintedAbortError();
    // attempt to get a value from the incoming edges
    if (in_edges_ && *in_edges_it_ != in_edges_->end()) {
      auto edge = *(*in_edges_it_)++;
      frame[self_.common_.edge_symbol] = edge;
      pull_node(edge, EdgeAtom::Direction::IN);
      return true;
    }

    // attempt to get a value from the outgoing edges
    if (out_edges_ && *out_edges_it_ != out_edges_->end()) {
      auto edge = *(*out_edges_it_)++;
      // when expanding in EdgeAtom::Direction::BOTH directions
      // we should do only one expansion for cycles, and it was
      // already done in the block above
      if (self_.common_.direction == EdgeAtom::Direction::BOTH && edge.IsCycle()) continue;
      frame[self_.common_.edge_symbol] = edge;
      pull_node(edge, EdgeAtom::Direction::OUT);
      return true;
    }

    // If we are here, either the edges have not been initialized,
    // or they have been exhausted. Attempt to initialize the edges.
    if (!InitEdges(frame, context)) return false;

    // we have re-initialized the edges, continue with the loop
  }
}

void Expand::ExpandCursor::Shutdown() { input_cursor_->Shutdown(); }

void Expand::ExpandCursor::Reset() {
  input_cursor_->Reset();
  in_edges_ = std::nullopt;
  in_edges_it_ = std::nullopt;
  out_edges_ = std::nullopt;
  out_edges_it_ = std::nullopt;
}

bool Expand::ExpandCursor::InitEdges(Frame &frame, ExecutionContext &context) {
  // Input Vertex could be null if it is created by a failed optional match. In
  // those cases we skip that input pull and continue with the next.
  while (true) {
    if (!input_cursor_->Pull(frame, context)) return false;
    TypedValue &vertex_value = frame[self_.input_symbol_];

    // Null check due to possible failed optional match.
    if (vertex_value.IsNull()) continue;

    ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
    auto &vertex = vertex_value.ValueVertex();

    auto direction = self_.common_.direction;
    if (direction == EdgeAtom::Direction::IN || direction == EdgeAtom::Direction::BOTH) {
      if (self_.common_.existing_node) {
        TypedValue &existing_node = frame[self_.common_.node_symbol];
        // old_node_value may be Null when using optional matching
        if (!existing_node.IsNull()) {
          ExpectType(self_.common_.node_symbol, existing_node, TypedValue::Type::Vertex);
          in_edges_.emplace(
              UnwrapEdgesResult(vertex.InEdges(self_.view_, self_.common_.edge_types, existing_node.ValueVertex())));
        }
      } else {
        in_edges_.emplace(UnwrapEdgesResult(vertex.InEdges(self_.view_, self_.common_.edge_types)));
      }
      if (in_edges_) {
        in_edges_it_.emplace(in_edges_->begin());
      }
    }

    if (direction == EdgeAtom::Direction::OUT || direction == EdgeAtom::Direction::BOTH) {
      if (self_.common_.existing_node) {
        TypedValue &existing_node = frame[self_.common_.node_symbol];
        // old_node_value may be Null when using optional matching
        if (!existing_node.IsNull()) {
          ExpectType(self_.common_.node_symbol, existing_node, TypedValue::Type::Vertex);
          out_edges_.emplace(
              UnwrapEdgesResult(vertex.OutEdges(self_.view_, self_.common_.edge_types, existing_node.ValueVertex())));
        }
      } else {
        out_edges_.emplace(UnwrapEdgesResult(vertex.OutEdges(self_.view_, self_.common_.edge_types)));
      }
      if (out_edges_) {
        out_edges_it_.emplace(out_edges_->begin());
      }
    }

    return true;
  }
}

ExpandVariable::ExpandVariable(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Symbol node_symbol,
                               Symbol edge_symbol, EdgeAtom::Type type, EdgeAtom::Direction direction,
                               const std::vector<storage::EdgeTypeId> &edge_types, bool is_reverse,
                               Expression *lower_bound, Expression *upper_bound, bool existing_node,
                               ExpansionLambda filter_lambda, std::optional<ExpansionLambda> weight_lambda,
                               std::optional<Symbol> total_weight)
    : input_(input ? input : std::make_shared<Once>()),
      input_symbol_(input_symbol),
      common_{node_symbol, edge_symbol, direction, edge_types, existing_node},
      type_(type),
      is_reverse_(is_reverse),
      lower_bound_(lower_bound),
      upper_bound_(upper_bound),
      filter_lambda_(filter_lambda),
      weight_lambda_(weight_lambda),
      total_weight_(total_weight) {
  DMG_ASSERT(type_ == EdgeAtom::Type::DEPTH_FIRST || type_ == EdgeAtom::Type::BREADTH_FIRST ||
                 type_ == EdgeAtom::Type::WEIGHTED_SHORTEST_PATH,
             "ExpandVariable can only be used with breadth first, depth first or "
             "weighted shortest path type");
  DMG_ASSERT(!(type_ == EdgeAtom::Type::BREADTH_FIRST && is_reverse), "Breadth first expansion can't be reversed");
}

ACCEPT_WITH_INPUT(ExpandVariable)

std::vector<Symbol> ExpandVariable::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(common_.node_symbol);
  symbols.emplace_back(common_.edge_symbol);
  return symbols;
}

namespace {

/**
 * Helper function that returns an iterable over
 * <EdgeAtom::Direction, EdgeAccessor> pairs
 * for the given params.
 *
 * @param vertex - The vertex to expand from.
 * @param direction - Expansion direction. All directions (IN, OUT, BOTH)
 *    are supported.
 * @param memory - Used to allocate the result.
 * @return See above.
 */
auto ExpandFromVertex(const VertexAccessor &vertex, EdgeAtom::Direction direction,
                      const std::vector<storage::EdgeTypeId> &edge_types, utils::MemoryResource *memory) {
  // wraps an EdgeAccessor into a pair <accessor, direction>
  auto wrapper = [](EdgeAtom::Direction direction, auto &&edges) {
    return iter::imap([direction](const auto &edge) { return std::make_pair(edge, direction); },
                      std::forward<decltype(edges)>(edges));
  };

  storage::View view = storage::View::OLD;
  utils::pmr::vector<decltype(wrapper(direction, *vertex.InEdges(view, edge_types)))> chain_elements(memory);

  if (direction != EdgeAtom::Direction::OUT) {
    auto edges = UnwrapEdgesResult(vertex.InEdges(view, edge_types));
    if (edges.begin() != edges.end()) {
      chain_elements.emplace_back(wrapper(EdgeAtom::Direction::IN, std::move(edges)));
    }
  }
  if (direction != EdgeAtom::Direction::IN) {
    auto edges = UnwrapEdgesResult(vertex.OutEdges(view, edge_types));
    if (edges.begin() != edges.end()) {
      chain_elements.emplace_back(wrapper(EdgeAtom::Direction::OUT, std::move(edges)));
    }
  }

  // TODO: Investigate whether itertools perform heap allocation?
  return iter::chain.from_iterable(std::move(chain_elements));
}

}  // namespace

class ExpandVariableCursor : public Cursor {
 public:
  ExpandVariableCursor(const ExpandVariable &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), edges_(mem), edges_it_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("ExpandVariable");

    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);
    while (true) {
      if (Expand(frame, context)) return true;

      if (PullInput(frame, context)) {
        // if lower bound is zero we also yield empty paths
        if (lower_bound_ == 0) {
          auto &start_vertex = frame[self_.input_symbol_].ValueVertex();
          if (!self_.common_.existing_node) {
            frame[self_.common_.node_symbol] = start_vertex;
            return true;
          } else if (CheckExistingNode(start_vertex, self_.common_.node_symbol, frame)) {
            return true;
          }
        }
        // if lower bound is not zero, we just continue, the next
        // loop iteration will attempt to expand and we're good
      } else
        return false;
      // else continue with the loop, try to expand again
      // because we succesfully pulled from the input
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    edges_.clear();
    edges_it_.clear();
  }

 private:
  const ExpandVariable &self_;
  const UniqueCursorPtr input_cursor_;
  // bounds. in the cursor they are not optional but set to
  // default values if missing in the ExpandVariable operator
  // initialize to arbitrary values, they should only be used
  // after a successful pull from the input
  int64_t upper_bound_{-1};
  int64_t lower_bound_{-1};

  // a stack of edge iterables corresponding to the level/depth of
  // the expansion currently being Pulled
  using ExpandEdges = decltype(ExpandFromVertex(std::declval<VertexAccessor>(), EdgeAtom::Direction::IN,
                                                self_.common_.edge_types, utils::NewDeleteResource()));

  utils::pmr::vector<ExpandEdges> edges_;
  // an iterator indicating the position in the corresponding edges_ element
  utils::pmr::vector<decltype(edges_.begin()->begin())> edges_it_;

  /**
   * Helper function that Pulls from the input vertex and
   * makes iteration over it's edges possible.
   *
   * @return If the Pull succeeded. If not, this VariableExpandCursor
   * is exhausted.
   */
  bool PullInput(Frame &frame, ExecutionContext &context) {
    // Input Vertex could be null if it is created by a failed optional match.
    // In those cases we skip that input pull and continue with the next.
    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      if (!input_cursor_->Pull(frame, context)) return false;
      TypedValue &vertex_value = frame[self_.input_symbol_];

      // Null check due to possible failed optional match.
      if (vertex_value.IsNull()) continue;

      ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
      auto &vertex = vertex_value.ValueVertex();

      // Evaluate the upper and lower bounds.
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                    storage::View::OLD);
      auto calc_bound = [&evaluator](auto &bound) {
        auto value = EvaluateInt(&evaluator, bound, "Variable expansion bound");
        if (value < 0) throw QueryRuntimeException("Variable expansion bound must be a non-negative integer.");
        return value;
      };

      lower_bound_ = self_.lower_bound_ ? calc_bound(self_.lower_bound_) : 1;
      upper_bound_ = self_.upper_bound_ ? calc_bound(self_.upper_bound_) : std::numeric_limits<int64_t>::max();

      if (upper_bound_ > 0) {
        auto *memory = edges_.get_allocator().GetMemoryResource();
        edges_.emplace_back(ExpandFromVertex(vertex, self_.common_.direction, self_.common_.edge_types, memory));
        edges_it_.emplace_back(edges_.back().begin());
      }

      // reset the frame value to an empty edge list
      auto *pull_memory = context.evaluation_context.memory;
      frame[self_.common_.edge_symbol] = TypedValue::TVector(pull_memory);

      return true;
    }
  }

  // Helper function for appending an edge to the list on the frame.
  void AppendEdge(const EdgeAccessor &new_edge, utils::pmr::vector<TypedValue> *edges_on_frame) {
    // We are placing an edge on the frame. It is possible that there already
    // exists an edge on the frame for this level. If so first remove it.
    DMG_ASSERT(edges_.size() > 0, "Edges are empty");
    if (self_.is_reverse_) {
      // TODO: This is innefficient, we should look into replacing
      // vector with something else for TypedValue::List.
      size_t diff = edges_on_frame->size() - std::min(edges_on_frame->size(), edges_.size() - 1U);
      if (diff > 0U) edges_on_frame->erase(edges_on_frame->begin(), edges_on_frame->begin() + diff);
      edges_on_frame->emplace(edges_on_frame->begin(), new_edge);
    } else {
      edges_on_frame->resize(std::min(edges_on_frame->size(), edges_.size() - 1U));
      edges_on_frame->emplace_back(new_edge);
    }
  }

  /**
   * Performs a single expansion for the current state of this
   * VariableExpansionCursor.
   *
   * @return True if the expansion was a success and this Cursor's
   * consumer can consume it. False if the expansion failed. In that
   * case no more expansions are available from the current input
   * vertex and another Pull from the input cursor should be performed.
   */
  bool Expand(Frame &frame, ExecutionContext &context) {
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);
    // Some expansions might not be valid due to edge uniqueness and
    // existing_node criterions, so expand in a loop until either the input
    // vertex is exhausted or a valid variable-length expansion is available.
    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // pop from the stack while there is stuff to pop and the current
      // level is exhausted
      while (!edges_.empty() && edges_it_.back() == edges_.back().end()) {
        edges_.pop_back();
        edges_it_.pop_back();
      }

      // check if we exhausted everything, if so return false
      if (edges_.empty()) return false;

      // we use this a lot
      auto &edges_on_frame = frame[self_.common_.edge_symbol].ValueList();

      // it is possible that edges_on_frame does not contain as many
      // elements as edges_ due to edge-uniqueness (when a whole layer
      // gets exhausted but no edges are valid). for that reason only
      // pop from edges_on_frame if they contain enough elements
      if (self_.is_reverse_) {
        auto diff = edges_on_frame.size() - std::min(edges_on_frame.size(), edges_.size());
        if (diff > 0) {
          edges_on_frame.erase(edges_on_frame.begin(), edges_on_frame.begin() + diff);
        }
      } else {
        edges_on_frame.resize(std::min(edges_on_frame.size(), edges_.size()));
      }

      // if we are here, we have a valid stack,
      // get the edge, increase the relevant iterator
      auto current_edge = *edges_it_.back()++;

      // Check edge-uniqueness.
      bool found_existing =
          std::any_of(edges_on_frame.begin(), edges_on_frame.end(),
                      [&current_edge](const TypedValue &edge) { return current_edge.first == edge.ValueEdge(); });
      if (found_existing) continue;

      AppendEdge(current_edge.first, &edges_on_frame);
      VertexAccessor current_vertex =
          current_edge.second == EdgeAtom::Direction::IN ? current_edge.first.From() : current_edge.first.To();

      if (!self_.common_.existing_node) {
        frame[self_.common_.node_symbol] = current_vertex;
      }

      // Skip expanding out of filtered expansion.
      frame[self_.filter_lambda_.inner_edge_symbol] = current_edge.first;
      frame[self_.filter_lambda_.inner_node_symbol] = current_vertex;
      if (self_.filter_lambda_.expression && !EvaluateFilter(evaluator, self_.filter_lambda_.expression)) continue;

      // we are doing depth-first search, so place the current
      // edge's expansions onto the stack, if we should continue to expand
      if (upper_bound_ > static_cast<int64_t>(edges_.size())) {
        auto *memory = edges_.get_allocator().GetMemoryResource();
        edges_.emplace_back(
            ExpandFromVertex(current_vertex, self_.common_.direction, self_.common_.edge_types, memory));
        edges_it_.emplace_back(edges_.back().begin());
      }

      if (self_.common_.existing_node && !CheckExistingNode(current_vertex, self_.common_.node_symbol, frame)) continue;

      // We only yield true if we satisfy the lower bound.
      if (static_cast<int64_t>(edges_on_frame.size()) >= lower_bound_)
        return true;
      else
        continue;
    }
  }
};

class STShortestPathCursor : public query::plan::Cursor {
 public:
  STShortestPathCursor(const ExpandVariable &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input()->MakeCursor(mem)) {
    MG_ASSERT(self_.common_.existing_node,
              "s-t shortest path algorithm should only "
              "be used when `existing_node` flag is "
              "set!");
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("STShortestPath");

    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);
    while (input_cursor_->Pull(frame, context)) {
      const auto &source_tv = frame[self_.input_symbol_];
      const auto &sink_tv = frame[self_.common_.node_symbol];

      // It is possible that source or sink vertex is Null due to optional
      // matching.
      if (source_tv.IsNull() || sink_tv.IsNull()) continue;

      const auto &source = source_tv.ValueVertex();
      const auto &sink = sink_tv.ValueVertex();

      int64_t lower_bound =
          self_.lower_bound_ ? EvaluateInt(&evaluator, self_.lower_bound_, "Min depth in breadth-first expansion") : 1;
      int64_t upper_bound = self_.upper_bound_
                                ? EvaluateInt(&evaluator, self_.upper_bound_, "Max depth in breadth-first expansion")
                                : std::numeric_limits<int64_t>::max();

      if (upper_bound < 1 || lower_bound > upper_bound) continue;

      if (FindPath(*context.db_accessor, source, sink, lower_bound, upper_bound, &frame, &evaluator, context)) {
        return true;
      }
    }
    return false;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override { input_cursor_->Reset(); }

 private:
  const ExpandVariable &self_;
  UniqueCursorPtr input_cursor_;

  using VertexEdgeMapT = utils::pmr::unordered_map<VertexAccessor, std::optional<EdgeAccessor>>;

  void ReconstructPath(const VertexAccessor &midpoint, const VertexEdgeMapT &in_edge, const VertexEdgeMapT &out_edge,
                       Frame *frame, utils::MemoryResource *pull_memory) {
    utils::pmr::vector<TypedValue> result(pull_memory);
    auto last_vertex = midpoint;
    while (true) {
      const auto &last_edge = in_edge.at(last_vertex);
      if (!last_edge) break;
      last_vertex = last_edge->From() == last_vertex ? last_edge->To() : last_edge->From();
      result.emplace_back(*last_edge);
    }
    std::reverse(result.begin(), result.end());
    last_vertex = midpoint;
    while (true) {
      const auto &last_edge = out_edge.at(last_vertex);
      if (!last_edge) break;
      last_vertex = last_edge->From() == last_vertex ? last_edge->To() : last_edge->From();
      result.emplace_back(*last_edge);
    }
    frame->at(self_.common_.edge_symbol) = std::move(result);
  }

  bool ShouldExpand(const VertexAccessor &vertex, const EdgeAccessor &edge, Frame *frame,
                    ExpressionEvaluator *evaluator) {
    if (!self_.filter_lambda_.expression) return true;

    frame->at(self_.filter_lambda_.inner_node_symbol) = vertex;
    frame->at(self_.filter_lambda_.inner_edge_symbol) = edge;

    TypedValue result = self_.filter_lambda_.expression->Accept(*evaluator);
    if (result.IsNull()) return false;
    if (result.IsBool()) return result.ValueBool();

    throw QueryRuntimeException("Expansion condition must evaluate to boolean or null");
  }

  bool FindPath(const DbAccessor &dba, const VertexAccessor &source, const VertexAccessor &sink, int64_t lower_bound,
                int64_t upper_bound, Frame *frame, ExpressionEvaluator *evaluator, const ExecutionContext &context) {
    using utils::Contains;

    if (source == sink) return false;

    // We expand from both directions, both from the source and the sink.
    // Expansions meet at the middle of the path if it exists. This should
    // perform better for real-world like graphs where the expansion front
    // grows exponentially, effectively reducing the exponent by half.

    auto *pull_memory = evaluator->GetMemoryResource();
    // Holds vertices at the current level of expansion from the source
    // (sink).
    utils::pmr::vector<VertexAccessor> source_frontier(pull_memory);
    utils::pmr::vector<VertexAccessor> sink_frontier(pull_memory);

    // Holds vertices we can expand to from `source_frontier`
    // (`sink_frontier`).
    utils::pmr::vector<VertexAccessor> source_next(pull_memory);
    utils::pmr::vector<VertexAccessor> sink_next(pull_memory);

    // Maps each vertex we visited expanding from the source (sink) to the
    // edge used. Necessary for path reconstruction.
    VertexEdgeMapT in_edge(pull_memory);
    VertexEdgeMapT out_edge(pull_memory);

    size_t current_length = 0;

    source_frontier.emplace_back(source);
    in_edge[source] = std::nullopt;
    sink_frontier.emplace_back(sink);
    out_edge[sink] = std::nullopt;

    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // Top-down step (expansion from the source).
      ++current_length;
      if (current_length > upper_bound) return false;

      for (const auto &vertex : source_frontier) {
        if (self_.common_.direction != EdgeAtom::Direction::IN) {
          auto out_edges = UnwrapEdgesResult(vertex.OutEdges(storage::View::OLD, self_.common_.edge_types));
          for (const auto &edge : out_edges) {
            if (ShouldExpand(edge.To(), edge, frame, evaluator) && !Contains(in_edge, edge.To())) {
              in_edge.emplace(edge.To(), edge);
              if (Contains(out_edge, edge.To())) {
                if (current_length >= lower_bound) {
                  ReconstructPath(edge.To(), in_edge, out_edge, frame, pull_memory);
                  return true;
                } else {
                  return false;
                }
              }
              source_next.push_back(edge.To());
            }
          }
        }
        if (self_.common_.direction != EdgeAtom::Direction::OUT) {
          auto in_edges = UnwrapEdgesResult(vertex.InEdges(storage::View::OLD, self_.common_.edge_types));
          for (const auto &edge : in_edges) {
            if (ShouldExpand(edge.From(), edge, frame, evaluator) && !Contains(in_edge, edge.From())) {
              in_edge.emplace(edge.From(), edge);
              if (Contains(out_edge, edge.From())) {
                if (current_length >= lower_bound) {
                  ReconstructPath(edge.From(), in_edge, out_edge, frame, pull_memory);
                  return true;
                } else {
                  return false;
                }
              }
              source_next.push_back(edge.From());
            }
          }
        }
      }

      if (source_next.empty()) return false;
      source_frontier.clear();
      std::swap(source_frontier, source_next);

      // Bottom-up step (expansion from the sink).
      ++current_length;
      if (current_length > upper_bound) return false;

      // When expanding from the sink we have to be careful which edge
      // endpoint we pass to `should_expand`, because everything is
      // reversed.
      for (const auto &vertex : sink_frontier) {
        if (self_.common_.direction != EdgeAtom::Direction::OUT) {
          auto out_edges = UnwrapEdgesResult(vertex.OutEdges(storage::View::OLD, self_.common_.edge_types));
          for (const auto &edge : out_edges) {
            if (ShouldExpand(vertex, edge, frame, evaluator) && !Contains(out_edge, edge.To())) {
              out_edge.emplace(edge.To(), edge);
              if (Contains(in_edge, edge.To())) {
                if (current_length >= lower_bound) {
                  ReconstructPath(edge.To(), in_edge, out_edge, frame, pull_memory);
                  return true;
                } else {
                  return false;
                }
              }
              sink_next.push_back(edge.To());
            }
          }
        }
        if (self_.common_.direction != EdgeAtom::Direction::IN) {
          auto in_edges = UnwrapEdgesResult(vertex.InEdges(storage::View::OLD, self_.common_.edge_types));
          for (const auto &edge : in_edges) {
            if (ShouldExpand(vertex, edge, frame, evaluator) && !Contains(out_edge, edge.From())) {
              out_edge.emplace(edge.From(), edge);
              if (Contains(in_edge, edge.From())) {
                if (current_length >= lower_bound) {
                  ReconstructPath(edge.From(), in_edge, out_edge, frame, pull_memory);
                  return true;
                } else {
                  return false;
                }
              }
              sink_next.push_back(edge.From());
            }
          }
        }
      }

      if (sink_next.empty()) return false;
      sink_frontier.clear();
      std::swap(sink_frontier, sink_next);
    }
  }
};

class SingleSourceShortestPathCursor : public query::plan::Cursor {
 public:
  SingleSourceShortestPathCursor(const ExpandVariable &self, utils::MemoryResource *mem)
      : self_(self),
        input_cursor_(self_.input()->MakeCursor(mem)),
        processed_(mem),
        to_visit_current_(mem),
        to_visit_next_(mem) {
    MG_ASSERT(!self_.common_.existing_node,
              "Single source shortest path algorithm "
              "should not be used when `existing_node` "
              "flag is set, s-t shortest path algorithm "
              "should be used instead!");
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("SingleSourceShortestPath");

    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);

    // for the given (edge, vertex) pair checks if they satisfy the
    // "where" condition. if so, places them in the to_visit_ structure.
    auto expand_pair = [this, &evaluator, &frame](EdgeAccessor edge, VertexAccessor vertex) {
      // if we already processed the given vertex it doesn't get expanded
      if (processed_.find(vertex) != processed_.end()) return;

      frame[self_.filter_lambda_.inner_edge_symbol] = edge;
      frame[self_.filter_lambda_.inner_node_symbol] = vertex;

      if (self_.filter_lambda_.expression) {
        TypedValue result = self_.filter_lambda_.expression->Accept(evaluator);
        switch (result.type()) {
          case TypedValue::Type::Null:
            return;
          case TypedValue::Type::Bool:
            if (!result.ValueBool()) return;
            break;
          default:
            throw QueryRuntimeException("Expansion condition must evaluate to boolean or null.");
        }
      }
      to_visit_next_.emplace_back(edge, vertex);
      processed_.emplace(vertex, edge);
    };

    // populates the to_visit_next_ structure with expansions
    // from the given vertex. skips expansions that don't satisfy
    // the "where" condition.
    auto expand_from_vertex = [this, &expand_pair](const auto &vertex) {
      if (self_.common_.direction != EdgeAtom::Direction::IN) {
        auto out_edges = UnwrapEdgesResult(vertex.OutEdges(storage::View::OLD, self_.common_.edge_types));
        for (const auto &edge : out_edges) expand_pair(edge, edge.To());
      }
      if (self_.common_.direction != EdgeAtom::Direction::OUT) {
        auto in_edges = UnwrapEdgesResult(vertex.InEdges(storage::View::OLD, self_.common_.edge_types));
        for (const auto &edge : in_edges) expand_pair(edge, edge.From());
      }
    };

    // do it all in a loop because we skip some elements
    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // if we have nothing to visit on the current depth, switch to next
      if (to_visit_current_.empty()) to_visit_current_.swap(to_visit_next_);

      // if current is still empty, it means both are empty, so pull from
      // input
      if (to_visit_current_.empty()) {
        if (!input_cursor_->Pull(frame, context)) return false;

        to_visit_current_.clear();
        to_visit_next_.clear();
        processed_.clear();

        const auto &vertex_value = frame[self_.input_symbol_];
        // it is possible that the vertex is Null due to optional matching
        if (vertex_value.IsNull()) continue;
        lower_bound_ = self_.lower_bound_
                           ? EvaluateInt(&evaluator, self_.lower_bound_, "Min depth in breadth-first expansion")
                           : 1;
        upper_bound_ = self_.upper_bound_
                           ? EvaluateInt(&evaluator, self_.upper_bound_, "Max depth in breadth-first expansion")
                           : std::numeric_limits<int64_t>::max();

        if (upper_bound_ < 1 || lower_bound_ > upper_bound_) continue;

        const auto &vertex = vertex_value.ValueVertex();
        processed_.emplace(vertex, std::nullopt);
        expand_from_vertex(vertex);

        // go back to loop start and see if we expanded anything
        continue;
      }

      // take the next expansion from the queue
      auto expansion = to_visit_current_.back();
      to_visit_current_.pop_back();

      // create the frame value for the edges
      auto *pull_memory = context.evaluation_context.memory;
      utils::pmr::vector<TypedValue> edge_list(pull_memory);
      edge_list.emplace_back(expansion.first);
      auto last_vertex = expansion.second;
      while (true) {
        const EdgeAccessor &last_edge = edge_list.back().ValueEdge();
        last_vertex = last_edge.From() == last_vertex ? last_edge.To() : last_edge.From();
        // origin_vertex must be in processed
        const auto &previous_edge = processed_.find(last_vertex)->second;
        if (!previous_edge) break;

        edge_list.emplace_back(previous_edge.value());
      }

      // expand only if what we've just expanded is less then max depth
      if (static_cast<int64_t>(edge_list.size()) < upper_bound_) expand_from_vertex(expansion.second);

      if (static_cast<int64_t>(edge_list.size()) < lower_bound_) continue;

      frame[self_.common_.node_symbol] = expansion.second;

      // place edges on the frame in the correct order
      std::reverse(edge_list.begin(), edge_list.end());
      frame[self_.common_.edge_symbol] = std::move(edge_list);

      return true;
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    processed_.clear();
    to_visit_next_.clear();
    to_visit_current_.clear();
  }

 private:
  const ExpandVariable &self_;
  const UniqueCursorPtr input_cursor_;

  // Depth bounds. Calculated on each pull from the input, the initial value
  // is irrelevant.
  int64_t lower_bound_{-1};
  int64_t upper_bound_{-1};

  // maps vertices to the edge they got expanded from. it is an optional
  // edge because the root does not get expanded from anything.
  // contains visited vertices as well as those scheduled to be visited.
  utils::pmr::unordered_map<VertexAccessor, std::optional<EdgeAccessor>> processed_;
  // edge/vertex pairs we have yet to visit, for current and next depth
  utils::pmr::vector<std::pair<EdgeAccessor, VertexAccessor>> to_visit_current_;
  utils::pmr::vector<std::pair<EdgeAccessor, VertexAccessor>> to_visit_next_;
};

class ExpandWeightedShortestPathCursor : public query::plan::Cursor {
 public:
  ExpandWeightedShortestPathCursor(const ExpandVariable &self, utils::MemoryResource *mem)
      : self_(self),
        input_cursor_(self_.input_->MakeCursor(mem)),
        total_cost_(mem),
        previous_(mem),
        yielded_vertices_(mem),
        pq_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("ExpandWeightedShortestPath");

    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);
    auto create_state = [this](const VertexAccessor &vertex, int64_t depth) {
      return std::make_pair(vertex, upper_bound_set_ ? depth : 0);
    };

    // For the given (edge, vertex, weight, depth) tuple checks if they
    // satisfy the "where" condition. if so, places them in the priority
    // queue.
    auto expand_pair = [this, &evaluator, &frame, &create_state](const EdgeAccessor &edge, const VertexAccessor &vertex,
                                                                 double weight, int64_t depth) {
      auto *memory = evaluator.GetMemoryResource();
      if (self_.filter_lambda_.expression) {
        frame[self_.filter_lambda_.inner_edge_symbol] = edge;
        frame[self_.filter_lambda_.inner_node_symbol] = vertex;

        if (!EvaluateFilter(evaluator, self_.filter_lambda_.expression)) return;
      }

      frame[self_.weight_lambda_->inner_edge_symbol] = edge;
      frame[self_.weight_lambda_->inner_node_symbol] = vertex;

      TypedValue typed_weight = self_.weight_lambda_->expression->Accept(evaluator);

      if (!typed_weight.IsNumeric()) {
        throw QueryRuntimeException("Calculated weight must be numeric, got {}.", typed_weight.type());
      }
      if ((typed_weight < TypedValue(0, memory)).ValueBool()) {
        throw QueryRuntimeException("Calculated weight must be non-negative!");
      }

      auto next_state = create_state(vertex, depth);
      auto next_weight = TypedValue(weight, memory) + typed_weight;
      auto found_it = total_cost_.find(next_state);
      if (found_it != total_cost_.end() && found_it->second.ValueDouble() <= next_weight.ValueDouble()) return;

      pq_.push({next_weight.ValueDouble(), depth + 1, vertex, edge});
    };

    // Populates the priority queue structure with expansions
    // from the given vertex. skips expansions that don't satisfy
    // the "where" condition.
    auto expand_from_vertex = [this, &expand_pair](const VertexAccessor &vertex, double weight, int64_t depth) {
      if (self_.common_.direction != EdgeAtom::Direction::IN) {
        auto out_edges = UnwrapEdgesResult(vertex.OutEdges(storage::View::OLD, self_.common_.edge_types));
        for (const auto &edge : out_edges) {
          expand_pair(edge, edge.To(), weight, depth);
        }
      }
      if (self_.common_.direction != EdgeAtom::Direction::OUT) {
        auto in_edges = UnwrapEdgesResult(vertex.InEdges(storage::View::OLD, self_.common_.edge_types));
        for (const auto &edge : in_edges) {
          expand_pair(edge, edge.From(), weight, depth);
        }
      }
    };

    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      if (pq_.empty()) {
        if (!input_cursor_->Pull(frame, context)) return false;
        const auto &vertex_value = frame[self_.input_symbol_];
        if (vertex_value.IsNull()) continue;
        auto vertex = vertex_value.ValueVertex();
        if (self_.common_.existing_node) {
          const auto &node = frame[self_.common_.node_symbol];
          // Due to optional matching the existing node could be null.
          // Skip expansion for such nodes.
          if (node.IsNull()) continue;
        }
        if (self_.upper_bound_) {
          upper_bound_ = EvaluateInt(&evaluator, self_.upper_bound_, "Max depth in weighted shortest path expansion");
          upper_bound_set_ = true;
        } else {
          upper_bound_ = std::numeric_limits<int64_t>::max();
          upper_bound_set_ = false;
        }
        if (upper_bound_ < 1)
          throw QueryRuntimeException(
              "Maximum depth in weighted shortest path expansion must be at "
              "least 1.");

        // Clear existing data structures.
        previous_.clear();
        total_cost_.clear();
        yielded_vertices_.clear();

        pq_.push({0.0, 0, vertex, std::nullopt});
        // We are adding the starting vertex to the set of yielded vertices
        // because we don't want to yield paths that end with the starting
        // vertex.
        yielded_vertices_.insert(vertex);
      }

      while (!pq_.empty()) {
        if (MustAbort(context)) throw HintedAbortError();
        auto [current_weight, current_depth, current_vertex, current_edge] = pq_.top();
        pq_.pop();

        auto current_state = create_state(current_vertex, current_depth);

        // Check if the vertex has already been processed.
        if (total_cost_.find(current_state) != total_cost_.end()) {
          continue;
        }
        previous_.emplace(current_state, current_edge);
        total_cost_.emplace(current_state, current_weight);

        // Expand only if what we've just expanded is less than max depth.
        if (current_depth < upper_bound_) expand_from_vertex(current_vertex, current_weight, current_depth);

        // If we yielded a path for a vertex already, make the expansion but
        // don't return the path again.
        if (yielded_vertices_.find(current_vertex) != yielded_vertices_.end()) continue;

        // Reconstruct the path.
        auto last_vertex = current_vertex;
        auto last_depth = current_depth;
        auto *pull_memory = context.evaluation_context.memory;
        utils::pmr::vector<TypedValue> edge_list(pull_memory);
        while (true) {
          // Origin_vertex must be in previous.
          const auto &previous_edge = previous_.find(create_state(last_vertex, last_depth))->second;
          if (!previous_edge) break;
          last_vertex = previous_edge->From() == last_vertex ? previous_edge->To() : previous_edge->From();
          last_depth--;
          edge_list.emplace_back(previous_edge.value());
        }

        // Place destination node on the frame, handle existence flag.
        if (self_.common_.existing_node) {
          const auto &node = frame[self_.common_.node_symbol];
          if ((node != TypedValue(current_vertex, pull_memory)).ValueBool())
            continue;
          else
            // Prevent expanding other paths, because we found the
            // shortest to existing node.
            ClearQueue();
        } else {
          frame[self_.common_.node_symbol] = current_vertex;
        }

        if (!self_.is_reverse_) {
          // Place edges on the frame in the correct order.
          std::reverse(edge_list.begin(), edge_list.end());
        }
        frame[self_.common_.edge_symbol] = std::move(edge_list);
        frame[self_.total_weight_.value()] = current_weight;
        yielded_vertices_.insert(current_vertex);
        return true;
      }
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    previous_.clear();
    total_cost_.clear();
    yielded_vertices_.clear();
    ClearQueue();
  }

 private:
  const ExpandVariable &self_;
  const UniqueCursorPtr input_cursor_;

  // Upper bound on the path length.
  int64_t upper_bound_{-1};
  bool upper_bound_set_{false};

  struct WspStateHash {
    size_t operator()(const std::pair<VertexAccessor, int64_t> &key) const {
      return utils::HashCombine<VertexAccessor, int64_t>{}(key.first, key.second);
    }
  };

  // Maps vertices to weights they got in expansion.
  utils::pmr::unordered_map<std::pair<VertexAccessor, int64_t>, TypedValue, WspStateHash> total_cost_;

  // Maps vertices to edges used to reach them.
  utils::pmr::unordered_map<std::pair<VertexAccessor, int64_t>, std::optional<EdgeAccessor>, WspStateHash> previous_;

  // Keeps track of vertices for which we yielded a path already.
  utils::pmr::unordered_set<VertexAccessor> yielded_vertices_;

  // Priority queue comparator. Keep lowest weight on top of the queue.
  class PriorityQueueComparator {
   public:
    bool operator()(const std::tuple<double, int64_t, VertexAccessor, std::optional<EdgeAccessor>> &lhs,
                    const std::tuple<double, int64_t, VertexAccessor, std::optional<EdgeAccessor>> &rhs) {
      return std::get<0>(lhs) > std::get<0>(rhs);
    }
  };

  std::priority_queue<std::tuple<double, int64_t, VertexAccessor, std::optional<EdgeAccessor>>,
                      utils::pmr::vector<std::tuple<double, int64_t, VertexAccessor, std::optional<EdgeAccessor>>>,
                      PriorityQueueComparator>
      pq_;

  void ClearQueue() {
    while (!pq_.empty()) pq_.pop();
  }
};

UniqueCursorPtr ExpandVariable::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ExpandVariableOperator);

  switch (type_) {
    case EdgeAtom::Type::BREADTH_FIRST:
      if (common_.existing_node) {
        return MakeUniqueCursorPtr<STShortestPathCursor>(mem, *this, mem);
      } else {
        return MakeUniqueCursorPtr<SingleSourceShortestPathCursor>(mem, *this, mem);
      }
    case EdgeAtom::Type::DEPTH_FIRST:
      return MakeUniqueCursorPtr<ExpandVariableCursor>(mem, *this, mem);
    case EdgeAtom::Type::WEIGHTED_SHORTEST_PATH:
      return MakeUniqueCursorPtr<ExpandWeightedShortestPathCursor>(mem, *this, mem);
    case EdgeAtom::Type::SINGLE:
      LOG_FATAL("ExpandVariable should not be planned for a single expansion!");
  }
}

class ConstructNamedPathCursor : public Cursor {
 public:
  ConstructNamedPathCursor(const ConstructNamedPath &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input()->MakeCursor(mem)) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("ConstructNamedPath");

    if (!input_cursor_->Pull(frame, context)) return false;

    auto symbol_it = self_.path_elements_.begin();
    DMG_ASSERT(symbol_it != self_.path_elements_.end(), "Named path must contain at least one node");

    const auto &start_vertex = frame[*symbol_it++];
    auto *pull_memory = context.evaluation_context.memory;
    // In an OPTIONAL MATCH everything could be Null.
    if (start_vertex.IsNull()) {
      frame[self_.path_symbol_] = TypedValue(pull_memory);
      return true;
    }

    DMG_ASSERT(start_vertex.IsVertex(), "First named path element must be a vertex");
    query::Path path(start_vertex.ValueVertex(), pull_memory);

    // If the last path element symbol was for an edge list, then
    // the next symbol is a vertex and it should not append to the path
    // because
    // expansion already did it.
    bool last_was_edge_list = false;

    for (; symbol_it != self_.path_elements_.end(); symbol_it++) {
      const auto &expansion = frame[*symbol_it];
      //  We can have Null (OPTIONAL MATCH), a vertex, an edge, or an edge
      //  list (variable expand or BFS).
      switch (expansion.type()) {
        case TypedValue::Type::Null:
          frame[self_.path_symbol_] = TypedValue(pull_memory);
          return true;
        case TypedValue::Type::Vertex:
          if (!last_was_edge_list) path.Expand(expansion.ValueVertex());
          last_was_edge_list = false;
          break;
        case TypedValue::Type::Edge:
          path.Expand(expansion.ValueEdge());
          break;
        case TypedValue::Type::List: {
          last_was_edge_list = true;
          // We need to expand all edges in the list and intermediary
          // vertices.
          const auto &edges = expansion.ValueList();
          for (const auto &edge_value : edges) {
            const auto &edge = edge_value.ValueEdge();
            const auto &from = edge.From();
            if (path.vertices().back() == from)
              path.Expand(edge, edge.To());
            else
              path.Expand(edge, from);
          }
          break;
        }
        default:
          LOG_FATAL("Unsupported type in named path construction");

          break;
      }
    }

    frame[self_.path_symbol_] = path;
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override { input_cursor_->Reset(); }

 private:
  const ConstructNamedPath self_;
  const UniqueCursorPtr input_cursor_;
};

ACCEPT_WITH_INPUT(ConstructNamedPath)

UniqueCursorPtr ConstructNamedPath::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ConstructNamedPathOperator);

  return MakeUniqueCursorPtr<ConstructNamedPathCursor>(mem, *this, mem);
}

std::vector<Symbol> ConstructNamedPath::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(path_symbol_);
  return symbols;
}

Filter::Filter(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input ? input : std::make_shared<Once>()), expression_(expression) {}

ACCEPT_WITH_INPUT(Filter)

UniqueCursorPtr Filter::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::FilterOperator);

  return MakeUniqueCursorPtr<FilterCursor>(mem, *this, mem);
}

std::vector<Symbol> Filter::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Filter::FilterCursor::FilterCursor(const Filter &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Filter::FilterCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Filter");

  // Like all filters, newly set values should not affect filtering of old
  // nodes and edges.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::OLD);
  while (input_cursor_->Pull(frame, context)) {
    if (EvaluateFilter(evaluator, self_.expression_)) return true;
  }
  return false;
}

void Filter::FilterCursor::Shutdown() { input_cursor_->Shutdown(); }

void Filter::FilterCursor::Reset() { input_cursor_->Reset(); }

Produce::Produce(const std::shared_ptr<LogicalOperator> &input, const std::vector<NamedExpression *> &named_expressions)
    : input_(input ? input : std::make_shared<Once>()), named_expressions_(named_expressions) {}

ACCEPT_WITH_INPUT(Produce)

UniqueCursorPtr Produce::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::ProduceOperator);

  return MakeUniqueCursorPtr<ProduceCursor>(mem, *this, mem);
}

std::vector<Symbol> Produce::OutputSymbols(const SymbolTable &symbol_table) const {
  std::vector<Symbol> symbols;
  for (const auto &named_expr : named_expressions_) {
    symbols.emplace_back(symbol_table.at(*named_expr));
  }
  return symbols;
}

std::vector<Symbol> Produce::ModifiedSymbols(const SymbolTable &table) const { return OutputSymbols(table); }

Produce::ProduceCursor::ProduceCursor(const Produce &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Produce::ProduceCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Produce");

  if (input_cursor_->Pull(frame, context)) {
    // Produce should always yield the latest results.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::NEW);
    for (auto named_expr : self_.named_expressions_) named_expr->Accept(evaluator);

    return true;
  }
  return false;
}

void Produce::ProduceCursor::Shutdown() { input_cursor_->Shutdown(); }

void Produce::ProduceCursor::Reset() { input_cursor_->Reset(); }

Delete::Delete(const std::shared_ptr<LogicalOperator> &input_, const std::vector<Expression *> &expressions,
               bool detach_)
    : input_(input_), expressions_(expressions), detach_(detach_) {}

ACCEPT_WITH_INPUT(Delete)

UniqueCursorPtr Delete::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::DeleteOperator);

  return MakeUniqueCursorPtr<DeleteCursor>(mem, *this, mem);
}

std::vector<Symbol> Delete::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Delete::DeleteCursor::DeleteCursor(const Delete &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Delete::DeleteCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Delete");

  if (!input_cursor_->Pull(frame, context)) return false;

  // Delete should get the latest information, this way it is also possible
  // to delete newly added nodes and edges.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);
  auto *pull_memory = context.evaluation_context.memory;
  // collect expressions results so edges can get deleted before vertices
  // this is necessary because an edge that gets deleted could block vertex
  // deletion
  utils::pmr::vector<TypedValue> expression_results(pull_memory);
  expression_results.reserve(self_.expressions_.size());
  for (Expression *expression : self_.expressions_) {
    expression_results.emplace_back(expression->Accept(evaluator));
  }

  auto &dba = *context.db_accessor;
  // delete edges first
  for (TypedValue &expression_result : expression_results) {
    if (MustAbort(context)) throw HintedAbortError();
    if (expression_result.type() == TypedValue::Type::Edge) {
      auto maybe_value = dba.RemoveEdge(&expression_result.ValueEdge());
      if (maybe_value.HasError()) {
        switch (maybe_value.GetError()) {
          case storage::Error::SERIALIZATION_ERROR:
            throw QueryRuntimeException(kSerializationErrorMessage);
          case storage::Error::DELETED_OBJECT:
          case storage::Error::VERTEX_HAS_EDGES:
          case storage::Error::PROPERTIES_DISABLED:
          case storage::Error::NONEXISTENT_OBJECT:
            throw QueryRuntimeException("Unexpected error when deleting an edge.");
        }
      }

      if (context.trigger_context_collector && maybe_value.GetValue()) {
        context.trigger_context_collector->RegisterDeletedObject(*maybe_value.GetValue());
      }
    }
  }

  // delete vertices
  for (TypedValue &expression_result : expression_results) {
    if (MustAbort(context)) throw HintedAbortError();
    switch (expression_result.type()) {
      case TypedValue::Type::Vertex: {
        auto &va = expression_result.ValueVertex();
        if (self_.detach_) {
          auto res = dba.DetachRemoveVertex(&va);
          if (res.HasError()) {
            switch (res.GetError()) {
              case storage::Error::SERIALIZATION_ERROR:
                throw QueryRuntimeException(kSerializationErrorMessage);
              case storage::Error::DELETED_OBJECT:
              case storage::Error::VERTEX_HAS_EDGES:
              case storage::Error::PROPERTIES_DISABLED:
              case storage::Error::NONEXISTENT_OBJECT:
                throw QueryRuntimeException("Unexpected error when deleting a node.");
            }
          }

          std::invoke([&] {
            if (!context.trigger_context_collector || !*res) {
              return;
            }

            context.trigger_context_collector->RegisterDeletedObject((*res)->first);
            if (!context.trigger_context_collector->ShouldRegisterDeletedObject<query::EdgeAccessor>()) {
              return;
            }
            for (const auto &edge : (*res)->second) {
              context.trigger_context_collector->RegisterDeletedObject(edge);
            }
          });
        } else {
          auto res = dba.RemoveVertex(&va);
          if (res.HasError()) {
            switch (res.GetError()) {
              case storage::Error::SERIALIZATION_ERROR:
                throw QueryRuntimeException(kSerializationErrorMessage);
              case storage::Error::VERTEX_HAS_EDGES:
                throw RemoveAttachedVertexException();
              case storage::Error::DELETED_OBJECT:
              case storage::Error::PROPERTIES_DISABLED:
              case storage::Error::NONEXISTENT_OBJECT:
                throw QueryRuntimeException("Unexpected error when deleting a node.");
            }
          }

          if (context.trigger_context_collector && res.GetValue()) {
            context.trigger_context_collector->RegisterDeletedObject(*res.GetValue());
          }
        }
        break;
      }

      // skip Edges (already deleted) and Nulls (can occur in optional
      // match)
      case TypedValue::Type::Edge:
      case TypedValue::Type::Null:
        break;
      // check we're not trying to delete anything except vertices and edges
      default:
        throw QueryRuntimeException("Only edges and vertices can be deleted.");
    }
  }

  return true;
}

void Delete::DeleteCursor::Shutdown() { input_cursor_->Shutdown(); }

void Delete::DeleteCursor::Reset() { input_cursor_->Reset(); }

SetProperty::SetProperty(const std::shared_ptr<LogicalOperator> &input, storage::PropertyId property,
                         PropertyLookup *lhs, Expression *rhs)
    : input_(input), property_(property), lhs_(lhs), rhs_(rhs) {}

ACCEPT_WITH_INPUT(SetProperty)

UniqueCursorPtr SetProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetPropertyOperator);

  return MakeUniqueCursorPtr<SetPropertyCursor>(mem, *this, mem);
}

std::vector<Symbol> SetProperty::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetProperty::SetPropertyCursor::SetPropertyCursor(const SetProperty &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool SetProperty::SetPropertyCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("SetProperty");

  if (!input_cursor_->Pull(frame, context)) return false;

  // Set, just like Create needs to see the latest changes.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);
  TypedValue lhs = self_.lhs_->expression_->Accept(evaluator);
  TypedValue rhs = self_.rhs_->Accept(evaluator);

  switch (lhs.type()) {
    case TypedValue::Type::Vertex: {
      auto old_value = PropsSetChecked(&lhs.ValueVertex(), self_.property_, rhs);

      if (context.trigger_context_collector) {
        // rhs cannot be moved because it was created with the allocator that is only valid during current pull
        context.trigger_context_collector->RegisterSetObjectProperty(lhs.ValueVertex(), self_.property_,
                                                                     TypedValue{std::move(old_value)}, TypedValue{rhs});
      }
      break;
    }
    case TypedValue::Type::Edge: {
      auto old_value = PropsSetChecked(&lhs.ValueEdge(), self_.property_, rhs);

      if (context.trigger_context_collector) {
        // rhs cannot be moved because it was created with the allocator that is only valid during current pull
        context.trigger_context_collector->RegisterSetObjectProperty(lhs.ValueEdge(), self_.property_,
                                                                     TypedValue{std::move(old_value)}, TypedValue{rhs});
      }
      break;
    }
    case TypedValue::Type::Null:
      // Skip setting properties on Null (can occur in optional match).
      break;
    case TypedValue::Type::Map:
    // Semantically modifying a map makes sense, but it's not supported due
    // to all the copying we do (when PropertyValue -> TypedValue and in
    // ExpressionEvaluator). So even though we set a map property here, that
    // is never visible to the user and it's not stored.
    // TODO: fix above described bug
    default:
      throw QueryRuntimeException("Properties can only be set on edges and vertices.");
  }
  return true;
}

void SetProperty::SetPropertyCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetProperty::SetPropertyCursor::Reset() { input_cursor_->Reset(); }

SetProperties::SetProperties(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol, Expression *rhs, Op op)
    : input_(input), input_symbol_(input_symbol), rhs_(rhs), op_(op) {}

ACCEPT_WITH_INPUT(SetProperties)

UniqueCursorPtr SetProperties::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetPropertiesOperator);

  return MakeUniqueCursorPtr<SetPropertiesCursor>(mem, *this, mem);
}

std::vector<Symbol> SetProperties::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetProperties::SetPropertiesCursor::SetPropertiesCursor(const SetProperties &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

namespace {

template <typename T>
concept AccessorWithProperties = requires(T value, storage::PropertyId property_id,
                                          storage::PropertyValue property_value) {
  { value.ClearProperties() } -> std::same_as<storage::Result<std::map<storage::PropertyId, storage::PropertyValue>>>;
  { value.SetProperty(property_id, property_value) };
};

/// Helper function that sets the given values on either a Vertex or an Edge.
///
/// @tparam TRecordAccessor Either RecordAccessor<Vertex> or
///     RecordAccessor<Edge>
template <AccessorWithProperties TRecordAccessor>
void SetPropertiesOnRecord(TRecordAccessor *record, const TypedValue &rhs, SetProperties::Op op,
                           ExecutionContext *context) {
  std::optional<std::map<storage::PropertyId, storage::PropertyValue>> old_values;
  const bool should_register_change =
      context->trigger_context_collector &&
      context->trigger_context_collector->ShouldRegisterObjectPropertyChange<TRecordAccessor>();
  if (op == SetProperties::Op::REPLACE) {
    auto maybe_value = record->ClearProperties();
    if (maybe_value.HasError()) {
      switch (maybe_value.GetError()) {
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to set properties on a deleted graph element.");
        case storage::Error::SERIALIZATION_ERROR:
          throw QueryRuntimeException(kSerializationErrorMessage);
        case storage::Error::PROPERTIES_DISABLED:
          throw QueryRuntimeException("Can't set property because properties on edges are disabled.");
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::NONEXISTENT_OBJECT:
          throw QueryRuntimeException("Unexpected error when setting properties.");
      }
    }

    if (should_register_change) {
      old_values.emplace(std::move(*maybe_value));
    }
  }

  auto get_props = [](const auto &record) {
    auto maybe_props = record.Properties(storage::View::NEW);
    if (maybe_props.HasError()) {
      switch (maybe_props.GetError()) {
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to get properties from a deleted object.");
        case storage::Error::NONEXISTENT_OBJECT:
          throw query::QueryRuntimeException("Trying to get properties from an object that doesn't exist.");
        case storage::Error::SERIALIZATION_ERROR:
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::PROPERTIES_DISABLED:
          throw QueryRuntimeException("Unexpected error when getting properties.");
      }
    }
    return *maybe_props;
  };

  auto register_set_property = [&](auto &&returned_old_value, auto key, auto &&new_value) {
    auto old_value = [&]() -> storage::PropertyValue {
      if (!old_values) {
        return std::forward<decltype(returned_old_value)>(returned_old_value);
      }

      if (auto it = old_values->find(key); it != old_values->end()) {
        return std::move(it->second);
      }

      return {};
    }();

    context->trigger_context_collector->RegisterSetObjectProperty(
        *record, key, TypedValue(std::move(old_value)), TypedValue(std::forward<decltype(new_value)>(new_value)));
  };

  auto set_props = [&, record](auto properties) {
    for (auto &kv : properties) {
      auto maybe_error = record->SetProperty(kv.first, kv.second);
      if (maybe_error.HasError()) {
        switch (maybe_error.GetError()) {
          case storage::Error::DELETED_OBJECT:
            throw QueryRuntimeException("Trying to set properties on a deleted graph element.");
          case storage::Error::SERIALIZATION_ERROR:
            throw QueryRuntimeException(kSerializationErrorMessage);
          case storage::Error::PROPERTIES_DISABLED:
            throw QueryRuntimeException("Can't set property because properties on edges are disabled.");
          case storage::Error::VERTEX_HAS_EDGES:
          case storage::Error::NONEXISTENT_OBJECT:
            throw QueryRuntimeException("Unexpected error when setting properties.");
        }
      }

      if (should_register_change) {
        register_set_property(std::move(*maybe_error), kv.first, std::move(kv.second));
      }
    }
  };

  switch (rhs.type()) {
    case TypedValue::Type::Edge:
      set_props(get_props(rhs.ValueEdge()));
      break;
    case TypedValue::Type::Vertex:
      set_props(get_props(rhs.ValueVertex()));
      break;
    case TypedValue::Type::Map: {
      for (const auto &kv : rhs.ValueMap()) {
        auto key = context->db_accessor->NameToProperty(kv.first);
        auto old_value = PropsSetChecked(record, key, kv.second);
        if (should_register_change) {
          register_set_property(std::move(old_value), key, kv.second);
        }
      }
      break;
    }
    default:
      throw QueryRuntimeException(
          "Right-hand side in SET expression must be a node, an edge or a "
          "map.");
  }

  if (should_register_change && old_values) {
    // register removed properties
    for (auto &[property_id, property_value] : *old_values) {
      context->trigger_context_collector->RegisterRemovedObjectProperty(*record, property_id,
                                                                        TypedValue(std::move(property_value)));
    }
  }
}

}  // namespace

bool SetProperties::SetPropertiesCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("SetProperties");

  if (!input_cursor_->Pull(frame, context)) return false;

  TypedValue &lhs = frame[self_.input_symbol_];

  // Set, just like Create needs to see the latest changes.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);
  TypedValue rhs = self_.rhs_->Accept(evaluator);

  switch (lhs.type()) {
    case TypedValue::Type::Vertex:
      SetPropertiesOnRecord(&lhs.ValueVertex(), rhs, self_.op_, &context);
      break;
    case TypedValue::Type::Edge:
      SetPropertiesOnRecord(&lhs.ValueEdge(), rhs, self_.op_, &context);
      break;
    case TypedValue::Type::Null:
      // Skip setting properties on Null (can occur in optional match).
      break;
    default:
      throw QueryRuntimeException("Properties can only be set on edges and vertices.");
  }
  return true;
}

void SetProperties::SetPropertiesCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetProperties::SetPropertiesCursor::Reset() { input_cursor_->Reset(); }

SetLabels::SetLabels(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol,
                     const std::vector<storage::LabelId> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(SetLabels)

UniqueCursorPtr SetLabels::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SetLabelsOperator);

  return MakeUniqueCursorPtr<SetLabelsCursor>(mem, *this, mem);
}

std::vector<Symbol> SetLabels::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

SetLabels::SetLabelsCursor::SetLabelsCursor(const SetLabels &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool SetLabels::SetLabelsCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("SetLabels");

  if (!input_cursor_->Pull(frame, context)) return false;

  TypedValue &vertex_value = frame[self_.input_symbol_];
  // Skip setting labels on Null (can occur in optional match).
  if (vertex_value.IsNull()) return true;
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &vertex = vertex_value.ValueVertex();
  for (auto label : self_.labels_) {
    auto maybe_value = vertex.AddLabel(label);
    if (maybe_value.HasError()) {
      switch (maybe_value.GetError()) {
        case storage::Error::SERIALIZATION_ERROR:
          throw QueryRuntimeException(kSerializationErrorMessage);
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to set a label on a deleted node.");
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::PROPERTIES_DISABLED:
        case storage::Error::NONEXISTENT_OBJECT:
          throw QueryRuntimeException("Unexpected error when setting a label.");
      }
    }

    if (context.trigger_context_collector && *maybe_value) {
      context.trigger_context_collector->RegisterSetVertexLabel(vertex, label);
    }
  }

  return true;
}

void SetLabels::SetLabelsCursor::Shutdown() { input_cursor_->Shutdown(); }

void SetLabels::SetLabelsCursor::Reset() { input_cursor_->Reset(); }

RemoveProperty::RemoveProperty(const std::shared_ptr<LogicalOperator> &input, storage::PropertyId property,
                               PropertyLookup *lhs)
    : input_(input), property_(property), lhs_(lhs) {}

ACCEPT_WITH_INPUT(RemoveProperty)

UniqueCursorPtr RemoveProperty::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::RemovePropertyOperator);

  return MakeUniqueCursorPtr<RemovePropertyCursor>(mem, *this, mem);
}

std::vector<Symbol> RemoveProperty::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

RemoveProperty::RemovePropertyCursor::RemovePropertyCursor(const RemoveProperty &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool RemoveProperty::RemovePropertyCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("RemoveProperty");

  if (!input_cursor_->Pull(frame, context)) return false;

  // Remove, just like Delete needs to see the latest changes.
  ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                storage::View::NEW);
  TypedValue lhs = self_.lhs_->expression_->Accept(evaluator);

  auto remove_prop = [property = self_.property_, &context](auto *record) {
    auto maybe_old_value = record->RemoveProperty(property);
    if (maybe_old_value.HasError()) {
      switch (maybe_old_value.GetError()) {
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to remove a property on a deleted graph element.");
        case storage::Error::SERIALIZATION_ERROR:
          throw QueryRuntimeException(kSerializationErrorMessage);
        case storage::Error::PROPERTIES_DISABLED:
          throw QueryRuntimeException(
              "Can't remove property because properties on edges are "
              "disabled.");
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::NONEXISTENT_OBJECT:
          throw QueryRuntimeException("Unexpected error when removing property.");
      }
    }

    if (context.trigger_context_collector) {
      context.trigger_context_collector->RegisterRemovedObjectProperty(*record, property,
                                                                       TypedValue(std::move(*maybe_old_value)));
    }
  };

  switch (lhs.type()) {
    case TypedValue::Type::Vertex:
      remove_prop(&lhs.ValueVertex());
      break;
    case TypedValue::Type::Edge:
      remove_prop(&lhs.ValueEdge());
      break;
    case TypedValue::Type::Null:
      // Skip removing properties on Null (can occur in optional match).
      break;
    default:
      throw QueryRuntimeException("Properties can only be removed from vertices and edges.");
  }
  return true;
}

void RemoveProperty::RemovePropertyCursor::Shutdown() { input_cursor_->Shutdown(); }

void RemoveProperty::RemovePropertyCursor::Reset() { input_cursor_->Reset(); }

RemoveLabels::RemoveLabels(const std::shared_ptr<LogicalOperator> &input, Symbol input_symbol,
                           const std::vector<storage::LabelId> &labels)
    : input_(input), input_symbol_(input_symbol), labels_(labels) {}

ACCEPT_WITH_INPUT(RemoveLabels)

UniqueCursorPtr RemoveLabels::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::RemoveLabelsOperator);

  return MakeUniqueCursorPtr<RemoveLabelsCursor>(mem, *this, mem);
}

std::vector<Symbol> RemoveLabels::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

RemoveLabels::RemoveLabelsCursor::RemoveLabelsCursor(const RemoveLabels &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

bool RemoveLabels::RemoveLabelsCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("RemoveLabels");

  if (!input_cursor_->Pull(frame, context)) return false;

  TypedValue &vertex_value = frame[self_.input_symbol_];
  // Skip removing labels on Null (can occur in optional match).
  if (vertex_value.IsNull()) return true;
  ExpectType(self_.input_symbol_, vertex_value, TypedValue::Type::Vertex);
  auto &vertex = vertex_value.ValueVertex();
  for (auto label : self_.labels_) {
    auto maybe_value = vertex.RemoveLabel(label);
    if (maybe_value.HasError()) {
      switch (maybe_value.GetError()) {
        case storage::Error::SERIALIZATION_ERROR:
          throw QueryRuntimeException(kSerializationErrorMessage);
        case storage::Error::DELETED_OBJECT:
          throw QueryRuntimeException("Trying to remove labels from a deleted node.");
        case storage::Error::VERTEX_HAS_EDGES:
        case storage::Error::PROPERTIES_DISABLED:
        case storage::Error::NONEXISTENT_OBJECT:
          throw QueryRuntimeException("Unexpected error when removing labels from a node.");
      }
    }

    if (context.trigger_context_collector && *maybe_value) {
      context.trigger_context_collector->RegisterRemovedVertexLabel(vertex, label);
    }
  }

  return true;
}

void RemoveLabels::RemoveLabelsCursor::Shutdown() { input_cursor_->Shutdown(); }

void RemoveLabels::RemoveLabelsCursor::Reset() { input_cursor_->Reset(); }

EdgeUniquenessFilter::EdgeUniquenessFilter(const std::shared_ptr<LogicalOperator> &input, Symbol expand_symbol,
                                           const std::vector<Symbol> &previous_symbols)
    : input_(input), expand_symbol_(expand_symbol), previous_symbols_(previous_symbols) {}

ACCEPT_WITH_INPUT(EdgeUniquenessFilter)

UniqueCursorPtr EdgeUniquenessFilter::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::EdgeUniquenessFilterOperator);

  return MakeUniqueCursorPtr<EdgeUniquenessFilterCursor>(mem, *this, mem);
}

std::vector<Symbol> EdgeUniquenessFilter::ModifiedSymbols(const SymbolTable &table) const {
  return input_->ModifiedSymbols(table);
}

EdgeUniquenessFilter::EdgeUniquenessFilterCursor::EdgeUniquenessFilterCursor(const EdgeUniquenessFilter &self,
                                                                             utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)) {}

namespace {
/**
 * Returns true if:
 *    - a and b are either edge or edge-list values, and there
 *    is at least one matching edge in the two values
 */
bool ContainsSameEdge(const TypedValue &a, const TypedValue &b) {
  auto compare_to_list = [](const TypedValue &list, const TypedValue &other) {
    for (const TypedValue &list_elem : list.ValueList())
      if (ContainsSameEdge(list_elem, other)) return true;
    return false;
  };

  if (a.type() == TypedValue::Type::List) return compare_to_list(a, b);
  if (b.type() == TypedValue::Type::List) return compare_to_list(b, a);

  return a.ValueEdge() == b.ValueEdge();
}
}  // namespace

bool EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("EdgeUniquenessFilter");

  auto expansion_ok = [&]() {
    const auto &expand_value = frame[self_.expand_symbol_];
    for (const auto &previous_symbol : self_.previous_symbols_) {
      const auto &previous_value = frame[previous_symbol];
      // This shouldn't raise a TypedValueException, because the planner
      // makes sure these are all of the expected type. In case they are not
      // an error should be raised long before this code is executed.
      if (ContainsSameEdge(previous_value, expand_value)) return false;
    }
    return true;
  };

  while (input_cursor_->Pull(frame, context))
    if (expansion_ok()) return true;
  return false;
}

void EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Shutdown() { input_cursor_->Shutdown(); }

void EdgeUniquenessFilter::EdgeUniquenessFilterCursor::Reset() { input_cursor_->Reset(); }

Accumulate::Accumulate(const std::shared_ptr<LogicalOperator> &input, const std::vector<Symbol> &symbols,
                       bool advance_command)
    : input_(input), symbols_(symbols), advance_command_(advance_command) {}

ACCEPT_WITH_INPUT(Accumulate)

std::vector<Symbol> Accumulate::ModifiedSymbols(const SymbolTable &) const { return symbols_; }

class AccumulateCursor : public Cursor {
 public:
  AccumulateCursor(const Accumulate &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), cache_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Accumulate");

    auto &dba = *context.db_accessor;
    // cache all the input
    if (!pulled_all_input_) {
      while (input_cursor_->Pull(frame, context)) {
        utils::pmr::vector<TypedValue> row(cache_.get_allocator().GetMemoryResource());
        row.reserve(self_.symbols_.size());
        for (const Symbol &symbol : self_.symbols_) row.emplace_back(frame[symbol]);
        cache_.emplace_back(std::move(row));
      }
      pulled_all_input_ = true;
      cache_it_ = cache_.begin();

      if (self_.advance_command_) dba.AdvanceCommand();
    }

    if (MustAbort(context)) throw HintedAbortError();
    if (cache_it_ == cache_.end()) return false;
    auto row_it = (cache_it_++)->begin();
    for (const Symbol &symbol : self_.symbols_) frame[symbol] = *row_it++;
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    cache_.clear();
    cache_it_ = cache_.begin();
    pulled_all_input_ = false;
  }

 private:
  const Accumulate &self_;
  const UniqueCursorPtr input_cursor_;
  utils::pmr::vector<utils::pmr::vector<TypedValue>> cache_;
  decltype(cache_.begin()) cache_it_ = cache_.begin();
  bool pulled_all_input_{false};
};

UniqueCursorPtr Accumulate::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::AccumulateOperator);

  return MakeUniqueCursorPtr<AccumulateCursor>(mem, *this, mem);
}

Aggregate::Aggregate(const std::shared_ptr<LogicalOperator> &input, const std::vector<Aggregate::Element> &aggregations,
                     const std::vector<Expression *> &group_by, const std::vector<Symbol> &remember)
    : input_(input ? input : std::make_shared<Once>()),
      aggregations_(aggregations),
      group_by_(group_by),
      remember_(remember) {}

ACCEPT_WITH_INPUT(Aggregate)

std::vector<Symbol> Aggregate::ModifiedSymbols(const SymbolTable &) const {
  auto symbols = remember_;
  for (const auto &elem : aggregations_) symbols.push_back(elem.output_sym);
  return symbols;
}

namespace {
/** Returns the default TypedValue for an Aggregation element.
 * This value is valid both for returning when where are no inputs
 * to the aggregation op, and for initializing an aggregation result
 * when there are */
TypedValue DefaultAggregationOpValue(const Aggregate::Element &element, utils::MemoryResource *memory) {
  switch (element.op) {
    case Aggregation::Op::COUNT:
      return TypedValue(0, memory);
    case Aggregation::Op::SUM:
    case Aggregation::Op::MIN:
    case Aggregation::Op::MAX:
    case Aggregation::Op::AVG:
      return TypedValue(memory);
    case Aggregation::Op::COLLECT_LIST:
      return TypedValue(TypedValue::TVector(memory));
    case Aggregation::Op::COLLECT_MAP:
      return TypedValue(TypedValue::TMap(memory));
  }
}
}  // namespace

class AggregateCursor : public Cursor {
 public:
  AggregateCursor(const Aggregate &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input_->MakeCursor(mem)), aggregation_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Aggregate");

    if (!pulled_all_input_) {
      ProcessAll(&frame, &context);
      pulled_all_input_ = true;
      aggregation_it_ = aggregation_.begin();

      // in case there is no input and no group_bys we need to return true
      // just this once
      if (aggregation_.empty() && self_.group_by_.empty()) {
        auto *pull_memory = context.evaluation_context.memory;
        // place default aggregation values on the frame
        for (const auto &elem : self_.aggregations_)
          frame[elem.output_sym] = DefaultAggregationOpValue(elem, pull_memory);
        // place null as remember values on the frame
        for (const Symbol &remember_sym : self_.remember_) frame[remember_sym] = TypedValue(pull_memory);
        return true;
      }
    }

    if (aggregation_it_ == aggregation_.end()) return false;

    // place aggregation values on the frame
    auto aggregation_values_it = aggregation_it_->second.values_.begin();
    for (const auto &aggregation_elem : self_.aggregations_)
      frame[aggregation_elem.output_sym] = *aggregation_values_it++;

    // place remember values on the frame
    auto remember_values_it = aggregation_it_->second.remember_.begin();
    for (const Symbol &remember_sym : self_.remember_) frame[remember_sym] = *remember_values_it++;

    aggregation_it_++;
    return true;
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    aggregation_.clear();
    aggregation_it_ = aggregation_.begin();
    pulled_all_input_ = false;
  }

 private:
  // Data structure for a single aggregation cache.
  // Does NOT include the group-by values since those are a key in the
  // aggregation map. The vectors in an AggregationValue contain one element for
  // each aggregation in this LogicalOp.
  struct AggregationValue {
    explicit AggregationValue(utils::MemoryResource *mem) : counts_(mem), values_(mem), remember_(mem) {}

    // how many input rows have been aggregated in respective values_ element so
    // far
    // TODO: The counting value type should be changed to an unsigned type once
    // TypedValue can support signed integer values larger than 64bits so that
    // precision isn't lost.
    utils::pmr::vector<int64_t> counts_;
    // aggregated values. Initially Null (until at least one input row with a
    // valid value gets processed)
    utils::pmr::vector<TypedValue> values_;
    // remember values.
    utils::pmr::vector<TypedValue> remember_;
  };

  const Aggregate &self_;
  const UniqueCursorPtr input_cursor_;
  // storage for aggregated data
  // map key is the vector of group-by values
  // map value is an AggregationValue struct
  utils::pmr::unordered_map<utils::pmr::vector<TypedValue>, AggregationValue,
                            // use FNV collection hashing specialized for a
                            // vector of TypedValues
                            utils::FnvCollection<utils::pmr::vector<TypedValue>, TypedValue, TypedValue::Hash>,
                            // custom equality
                            TypedValueVectorEqual>
      aggregation_;
  // iterator over the accumulated cache
  decltype(aggregation_.begin()) aggregation_it_ = aggregation_.begin();
  // this LogicalOp pulls all from the input on it's first pull
  // this switch tracks if this has been performed
  bool pulled_all_input_{false};

  /**
   * Pulls from the input operator until exhausted and aggregates the
   * results. If the input operator is not provided, a single call
   * to ProcessOne is issued.
   *
   * Accumulation automatically groups the results so that `aggregation_`
   * cache cardinality depends on number of
   * aggregation results, and not on the number of inputs.
   */
  void ProcessAll(Frame *frame, ExecutionContext *context) {
    ExpressionEvaluator evaluator(frame, context->symbol_table, context->evaluation_context, context->db_accessor,
                                  storage::View::NEW);
    while (input_cursor_->Pull(*frame, *context)) {
      ProcessOne(*frame, &evaluator);
    }

    // calculate AVG aggregations (so far they have only been summed)
    for (size_t pos = 0; pos < self_.aggregations_.size(); ++pos) {
      if (self_.aggregations_[pos].op != Aggregation::Op::AVG) continue;
      for (auto &kv : aggregation_) {
        AggregationValue &agg_value = kv.second;
        auto count = agg_value.counts_[pos];
        auto *pull_memory = context->evaluation_context.memory;
        if (count > 0) {
          agg_value.values_[pos] = agg_value.values_[pos] / TypedValue(static_cast<double>(count), pull_memory);
        }
      }
    }
  }

  /**
   * Performs a single accumulation.
   */
  void ProcessOne(const Frame &frame, ExpressionEvaluator *evaluator) {
    auto *mem = aggregation_.get_allocator().GetMemoryResource();
    utils::pmr::vector<TypedValue> group_by(mem);
    group_by.reserve(self_.group_by_.size());
    for (Expression *expression : self_.group_by_) {
      group_by.emplace_back(expression->Accept(*evaluator));
    }
    auto &agg_value = aggregation_.try_emplace(std::move(group_by), mem).first->second;
    EnsureInitialized(frame, &agg_value);
    Update(evaluator, &agg_value);
  }

  /** Ensures the new AggregationValue has been initialized. This means
   * that the value vectors are filled with an appropriate number of Nulls,
   * counts are set to 0 and remember values are remembered.
   */
  void EnsureInitialized(const Frame &frame, AggregateCursor::AggregationValue *agg_value) const {
    if (!agg_value->values_.empty()) return;

    for (const auto &agg_elem : self_.aggregations_) {
      auto *mem = agg_value->values_.get_allocator().GetMemoryResource();
      agg_value->values_.emplace_back(DefaultAggregationOpValue(agg_elem, mem));
    }
    agg_value->counts_.resize(self_.aggregations_.size(), 0);

    for (const Symbol &remember_sym : self_.remember_) agg_value->remember_.push_back(frame[remember_sym]);
  }

  /** Updates the given AggregationValue with new data. Assumes that
   * the AggregationValue has been initialized */
  void Update(ExpressionEvaluator *evaluator, AggregateCursor::AggregationValue *agg_value) {
    DMG_ASSERT(self_.aggregations_.size() == agg_value->values_.size(),
               "Expected as much AggregationValue.values_ as there are "
               "aggregations.");
    DMG_ASSERT(self_.aggregations_.size() == agg_value->counts_.size(),
               "Expected as much AggregationValue.counts_ as there are "
               "aggregations.");

    // we iterate over counts, values and aggregation info at the same time
    auto count_it = agg_value->counts_.begin();
    auto value_it = agg_value->values_.begin();
    auto agg_elem_it = self_.aggregations_.begin();
    for (; count_it < agg_value->counts_.end(); count_it++, value_it++, agg_elem_it++) {
      // COUNT(*) is the only case where input expression is optional
      // handle it here
      auto input_expr_ptr = agg_elem_it->value;
      if (!input_expr_ptr) {
        *count_it += 1;
        *value_it = *count_it;
        continue;
      }

      TypedValue input_value = input_expr_ptr->Accept(*evaluator);

      // Aggregations skip Null input values.
      if (input_value.IsNull()) continue;
      const auto &agg_op = agg_elem_it->op;
      *count_it += 1;
      if (*count_it == 1) {
        // first value, nothing to aggregate. check type, set and continue.
        switch (agg_op) {
          case Aggregation::Op::MIN:
          case Aggregation::Op::MAX:
            *value_it = input_value;
            EnsureOkForMinMax(input_value);
            break;
          case Aggregation::Op::SUM:
          case Aggregation::Op::AVG:
            *value_it = input_value;
            EnsureOkForAvgSum(input_value);
            break;
          case Aggregation::Op::COUNT:
            *value_it = 1;
            break;
          case Aggregation::Op::COLLECT_LIST:
            value_it->ValueList().push_back(input_value);
            break;
          case Aggregation::Op::COLLECT_MAP:
            auto key = agg_elem_it->key->Accept(*evaluator);
            if (key.type() != TypedValue::Type::String) throw QueryRuntimeException("Map key must be a string.");
            value_it->ValueMap().emplace(key.ValueString(), input_value);
            break;
        }
        continue;
      }

      // aggregation of existing values
      switch (agg_op) {
        case Aggregation::Op::COUNT:
          *value_it = *count_it;
          break;
        case Aggregation::Op::MIN: {
          EnsureOkForMinMax(input_value);
          try {
            TypedValue comparison_result = input_value < *value_it;
            // since we skip nulls we either have a valid comparison, or
            // an exception was just thrown above
            // safe to assume a bool TypedValue
            if (comparison_result.ValueBool()) *value_it = input_value;
          } catch (const TypedValueException &) {
            throw QueryRuntimeException("Unable to get MIN of '{}' and '{}'.", input_value.type(), value_it->type());
          }
          break;
        }
        case Aggregation::Op::MAX: {
          //  all comments as for Op::Min
          EnsureOkForMinMax(input_value);
          try {
            TypedValue comparison_result = input_value > *value_it;
            if (comparison_result.ValueBool()) *value_it = input_value;
          } catch (const TypedValueException &) {
            throw QueryRuntimeException("Unable to get MAX of '{}' and '{}'.", input_value.type(), value_it->type());
          }
          break;
        }
        case Aggregation::Op::AVG:
        // for averaging we sum first and divide by count once all
        // the input has been processed
        case Aggregation::Op::SUM:
          EnsureOkForAvgSum(input_value);
          *value_it = *value_it + input_value;
          break;
        case Aggregation::Op::COLLECT_LIST:
          value_it->ValueList().push_back(input_value);
          break;
        case Aggregation::Op::COLLECT_MAP:
          auto key = agg_elem_it->key->Accept(*evaluator);
          if (key.type() != TypedValue::Type::String) throw QueryRuntimeException("Map key must be a string.");
          value_it->ValueMap().emplace(key.ValueString(), input_value);
          break;
      }  // end switch over Aggregation::Op enum
    }    // end loop over all aggregations
  }

  /** Checks if the given TypedValue is legal in MIN and MAX. If not
   * an appropriate exception is thrown. */
  void EnsureOkForMinMax(const TypedValue &value) const {
    switch (value.type()) {
      case TypedValue::Type::Bool:
      case TypedValue::Type::Int:
      case TypedValue::Type::Double:
      case TypedValue::Type::String:
        return;
      default:
        throw QueryRuntimeException(
            "Only boolean, numeric and string values are allowed in "
            "MIN and MAX aggregations.");
    }
  }

  /** Checks if the given TypedValue is legal in AVG and SUM. If not
   * an appropriate exception is thrown. */
  void EnsureOkForAvgSum(const TypedValue &value) const {
    switch (value.type()) {
      case TypedValue::Type::Int:
      case TypedValue::Type::Double:
        return;
      default:
        throw QueryRuntimeException("Only numeric values allowed in SUM and AVG aggregations.");
    }
  }
};

UniqueCursorPtr Aggregate::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::AggregateOperator);

  return MakeUniqueCursorPtr<AggregateCursor>(mem, *this, mem);
}

Skip::Skip(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Skip)

UniqueCursorPtr Skip::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::SkipOperator);

  return MakeUniqueCursorPtr<SkipCursor>(mem, *this, mem);
}

std::vector<Symbol> Skip::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Skip::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Skip::SkipCursor::SkipCursor(const Skip &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Skip::SkipCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Skip");

  while (input_cursor_->Pull(frame, context)) {
    if (to_skip_ == -1) {
      // First successful pull from the input, evaluate the skip expression.
      // The skip expression doesn't contain identifiers so graph view
      // parameter is not important.
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                    storage::View::OLD);
      TypedValue to_skip = self_.expression_->Accept(evaluator);
      if (to_skip.type() != TypedValue::Type::Int)
        throw QueryRuntimeException("Number of elements to skip must be an integer.");

      to_skip_ = to_skip.ValueInt();
      if (to_skip_ < 0) throw QueryRuntimeException("Number of elements to skip must be non-negative.");
    }

    if (skipped_++ < to_skip_) continue;
    return true;
  }
  return false;
}

void Skip::SkipCursor::Shutdown() { input_cursor_->Shutdown(); }

void Skip::SkipCursor::Reset() {
  input_cursor_->Reset();
  to_skip_ = -1;
  skipped_ = 0;
}

Limit::Limit(const std::shared_ptr<LogicalOperator> &input, Expression *expression)
    : input_(input), expression_(expression) {}

ACCEPT_WITH_INPUT(Limit)

UniqueCursorPtr Limit::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::LimitOperator);

  return MakeUniqueCursorPtr<LimitCursor>(mem, *this, mem);
}

std::vector<Symbol> Limit::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Limit::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Limit::LimitCursor::LimitCursor(const Limit &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self_.input_->MakeCursor(mem)) {}

bool Limit::LimitCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Limit");

  // We need to evaluate the limit expression before the first input Pull
  // because it might be 0 and thereby we shouldn't Pull from input at all.
  // We can do this before Pulling from the input because the limit expression
  // is not allowed to contain any identifiers.
  if (limit_ == -1) {
    // Limit expression doesn't contain identifiers so graph view is not
    // important.
    ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                  storage::View::OLD);
    TypedValue limit = self_.expression_->Accept(evaluator);
    if (limit.type() != TypedValue::Type::Int)
      throw QueryRuntimeException("Limit on number of returned elements must be an integer.");

    limit_ = limit.ValueInt();
    if (limit_ < 0) throw QueryRuntimeException("Limit on number of returned elements must be non-negative.");
  }

  // check we have not exceeded the limit before pulling
  if (pulled_++ >= limit_) return false;

  return input_cursor_->Pull(frame, context);
}

void Limit::LimitCursor::Shutdown() { input_cursor_->Shutdown(); }

void Limit::LimitCursor::Reset() {
  input_cursor_->Reset();
  limit_ = -1;
  pulled_ = 0;
}

OrderBy::OrderBy(const std::shared_ptr<LogicalOperator> &input, const std::vector<SortItem> &order_by,
                 const std::vector<Symbol> &output_symbols)
    : input_(input), output_symbols_(output_symbols) {
  // split the order_by vector into two vectors of orderings and expressions
  std::vector<Ordering> ordering;
  ordering.reserve(order_by.size());
  order_by_.reserve(order_by.size());
  for (const auto &ordering_expression_pair : order_by) {
    ordering.emplace_back(ordering_expression_pair.ordering);
    order_by_.emplace_back(ordering_expression_pair.expression);
  }
  compare_ = TypedValueVectorCompare(ordering);
}

ACCEPT_WITH_INPUT(OrderBy)

std::vector<Symbol> OrderBy::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> OrderBy::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

class OrderByCursor : public Cursor {
 public:
  OrderByCursor(const OrderBy &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_.input_->MakeCursor(mem)), cache_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("OrderBy");

    if (!did_pull_all_) {
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                    storage::View::OLD);
      auto *mem = cache_.get_allocator().GetMemoryResource();
      while (input_cursor_->Pull(frame, context)) {
        // collect the order_by elements
        utils::pmr::vector<TypedValue> order_by(mem);
        order_by.reserve(self_.order_by_.size());
        for (auto expression_ptr : self_.order_by_) {
          order_by.emplace_back(expression_ptr->Accept(evaluator));
        }

        // collect the output elements
        utils::pmr::vector<TypedValue> output(mem);
        output.reserve(self_.output_symbols_.size());
        for (const Symbol &output_sym : self_.output_symbols_) output.emplace_back(frame[output_sym]);

        cache_.push_back(Element{std::move(order_by), std::move(output)});
      }

      std::sort(cache_.begin(), cache_.end(), [this](const auto &pair1, const auto &pair2) {
        return self_.compare_(pair1.order_by, pair2.order_by);
      });

      did_pull_all_ = true;
      cache_it_ = cache_.begin();
    }

    if (cache_it_ == cache_.end()) return false;

    if (MustAbort(context)) throw HintedAbortError();

    // place the output values on the frame
    DMG_ASSERT(self_.output_symbols_.size() == cache_it_->remember.size(),
               "Number of values does not match the number of output symbols "
               "in OrderBy");
    auto output_sym_it = self_.output_symbols_.begin();
    for (const TypedValue &output : cache_it_->remember) frame[*output_sym_it++] = output;

    cache_it_++;
    return true;
  }
  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    did_pull_all_ = false;
    cache_.clear();
    cache_it_ = cache_.begin();
  }

 private:
  struct Element {
    utils::pmr::vector<TypedValue> order_by;
    utils::pmr::vector<TypedValue> remember;
  };

  const OrderBy &self_;
  const UniqueCursorPtr input_cursor_;
  bool did_pull_all_{false};
  // a cache of elements pulled from the input
  // the cache is filled and sorted (only on first elem) on first Pull
  utils::pmr::vector<Element> cache_;
  // iterator over the cache_, maintains state between Pulls
  decltype(cache_.begin()) cache_it_ = cache_.begin();
};

UniqueCursorPtr OrderBy::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OrderByOperator);

  return MakeUniqueCursorPtr<OrderByCursor>(mem, *this, mem);
}

Merge::Merge(const std::shared_ptr<LogicalOperator> &input, const std::shared_ptr<LogicalOperator> &merge_match,
             const std::shared_ptr<LogicalOperator> &merge_create)
    : input_(input ? input : std::make_shared<Once>()), merge_match_(merge_match), merge_create_(merge_create) {}

bool Merge::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && merge_match_->Accept(visitor) && merge_create_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

UniqueCursorPtr Merge::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::MergeOperator);

  return MakeUniqueCursorPtr<MergeCursor>(mem, *this, mem);
}

std::vector<Symbol> Merge::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  // Match and create branches should have the same symbols, so just take one
  // of them.
  auto my_symbols = merge_match_->OutputSymbols(table);
  symbols.insert(symbols.end(), my_symbols.begin(), my_symbols.end());
  return symbols;
}

Merge::MergeCursor::MergeCursor(const Merge &self, utils::MemoryResource *mem)
    : input_cursor_(self.input_->MakeCursor(mem)),
      merge_match_cursor_(self.merge_match_->MakeCursor(mem)),
      merge_create_cursor_(self.merge_create_->MakeCursor(mem)) {}

bool Merge::MergeCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Merge");

  while (true) {
    if (pull_input_) {
      if (input_cursor_->Pull(frame, context)) {
        // after a successful input from the input
        // reset merge_match (it's expand iterators maintain state)
        // and merge_create (could have a Once at the beginning)
        merge_match_cursor_->Reset();
        merge_create_cursor_->Reset();
      } else
        // input is exhausted, we're done
        return false;
    }

    // pull from the merge_match cursor
    if (merge_match_cursor_->Pull(frame, context)) {
      // if successful, next Pull from this should not pull_input_
      pull_input_ = false;
      return true;
    } else {
      // failed to Pull from the merge_match cursor
      if (pull_input_) {
        // if we have just now pulled from the input
        // and failed to pull from merge_match, we should create
        __attribute__((unused)) bool merge_create_pull_result = merge_create_cursor_->Pull(frame, context);
        DMG_ASSERT(merge_create_pull_result, "MergeCreate must never fail");
        return true;
      }
      // We have exhausted merge_match_cursor_ after 1 or more successful
      // Pulls. Attempt next input_cursor_ pull
      pull_input_ = true;
      continue;
    }
  }
}

void Merge::MergeCursor::Shutdown() {
  input_cursor_->Shutdown();
  merge_match_cursor_->Shutdown();
  merge_create_cursor_->Shutdown();
}

void Merge::MergeCursor::Reset() {
  input_cursor_->Reset();
  merge_match_cursor_->Reset();
  merge_create_cursor_->Reset();
  pull_input_ = true;
}

Optional::Optional(const std::shared_ptr<LogicalOperator> &input, const std::shared_ptr<LogicalOperator> &optional,
                   const std::vector<Symbol> &optional_symbols)
    : input_(input ? input : std::make_shared<Once>()), optional_(optional), optional_symbols_(optional_symbols) {}

bool Optional::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    input_->Accept(visitor) && optional_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

UniqueCursorPtr Optional::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::OptionalOperator);

  return MakeUniqueCursorPtr<OptionalCursor>(mem, *this, mem);
}

std::vector<Symbol> Optional::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  auto my_symbols = optional_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), my_symbols.begin(), my_symbols.end());
  return symbols;
}

Optional::OptionalCursor::OptionalCursor(const Optional &self, utils::MemoryResource *mem)
    : self_(self), input_cursor_(self.input_->MakeCursor(mem)), optional_cursor_(self.optional_->MakeCursor(mem)) {}

bool Optional::OptionalCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Optional");

  while (true) {
    if (pull_input_) {
      if (input_cursor_->Pull(frame, context)) {
        // after a successful input from the input
        // reset optional_ (it's expand iterators maintain state)
        optional_cursor_->Reset();
      } else
        // input is exhausted, we're done
        return false;
    }

    // pull from the optional_ cursor
    if (optional_cursor_->Pull(frame, context)) {
      // if successful, next Pull from this should not pull_input_
      pull_input_ = false;
      return true;
    } else {
      // failed to Pull from the merge_match cursor
      if (pull_input_) {
        // if we have just now pulled from the input
        // and failed to pull from optional_ so set the
        // optional symbols to Null, ensure next time the
        // input gets pulled and return true
        for (const Symbol &sym : self_.optional_symbols_) frame[sym] = TypedValue(context.evaluation_context.memory);
        pull_input_ = true;
        return true;
      }
      // we have exhausted optional_cursor_ after 1 or more successful Pulls
      // attempt next input_cursor_ pull
      pull_input_ = true;
      continue;
    }
  }
}

void Optional::OptionalCursor::Shutdown() {
  input_cursor_->Shutdown();
  optional_cursor_->Shutdown();
}

void Optional::OptionalCursor::Reset() {
  input_cursor_->Reset();
  optional_cursor_->Reset();
  pull_input_ = true;
}

Unwind::Unwind(const std::shared_ptr<LogicalOperator> &input, Expression *input_expression, Symbol output_symbol)
    : input_(input ? input : std::make_shared<Once>()),
      input_expression_(input_expression),
      output_symbol_(output_symbol) {}

ACCEPT_WITH_INPUT(Unwind)

std::vector<Symbol> Unwind::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.emplace_back(output_symbol_);
  return symbols;
}

class UnwindCursor : public Cursor {
 public:
  UnwindCursor(const Unwind &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), input_value_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Unwind");
    while (true) {
      if (MustAbort(context)) throw HintedAbortError();
      // if we reached the end of our list of values
      // pull from the input
      if (input_value_it_ == input_value_.end()) {
        if (!input_cursor_->Pull(frame, context)) return false;

        // successful pull from input, initialize value and iterator
        ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                      storage::View::OLD);
        TypedValue input_value = self_.input_expression_->Accept(evaluator);
        if (input_value.type() != TypedValue::Type::List)
          throw QueryRuntimeException("Argument of UNWIND must be a list, but '{}' was provided.", input_value.type());
        // Copy the evaluted input_value_list to our vector.
        input_value_ = input_value.ValueList();
        input_value_it_ = input_value_.begin();
      }

      // if we reached the end of our list of values goto back to top
      if (input_value_it_ == input_value_.end()) continue;

      frame[self_.output_symbol_] = *input_value_it_++;
      return true;
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    input_value_.clear();
    input_value_it_ = input_value_.end();
  }

 private:
  const Unwind &self_;
  const UniqueCursorPtr input_cursor_;
  // typed values we are unwinding and yielding
  utils::pmr::vector<TypedValue> input_value_;
  // current position in input_value_
  decltype(input_value_)::iterator input_value_it_ = input_value_.end();
};

UniqueCursorPtr Unwind::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::UnwindOperator);

  return MakeUniqueCursorPtr<UnwindCursor>(mem, *this, mem);
}

class DistinctCursor : public Cursor {
 public:
  DistinctCursor(const Distinct &self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self.input_->MakeCursor(mem)), seen_rows_(mem) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Distinct");

    while (true) {
      if (!input_cursor_->Pull(frame, context)) return false;

      utils::pmr::vector<TypedValue> row(seen_rows_.get_allocator().GetMemoryResource());
      row.reserve(self_.value_symbols_.size());
      for (const auto &symbol : self_.value_symbols_) row.emplace_back(frame[symbol]);
      if (seen_rows_.insert(std::move(row)).second) return true;
    }
  }

  void Shutdown() override { input_cursor_->Shutdown(); }

  void Reset() override {
    input_cursor_->Reset();
    seen_rows_.clear();
  }

 private:
  const Distinct &self_;
  const UniqueCursorPtr input_cursor_;
  // a set of already seen rows
  utils::pmr::unordered_set<utils::pmr::vector<TypedValue>,
                            // use FNV collection hashing specialized for a
                            // vector of TypedValue
                            utils::FnvCollection<utils::pmr::vector<TypedValue>, TypedValue, TypedValue::Hash>,
                            TypedValueVectorEqual>
      seen_rows_;
};

Distinct::Distinct(const std::shared_ptr<LogicalOperator> &input, const std::vector<Symbol> &value_symbols)
    : input_(input ? input : std::make_shared<Once>()), value_symbols_(value_symbols) {}

ACCEPT_WITH_INPUT(Distinct)

UniqueCursorPtr Distinct::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::DistinctOperator);

  return MakeUniqueCursorPtr<DistinctCursor>(mem, *this, mem);
}

std::vector<Symbol> Distinct::OutputSymbols(const SymbolTable &symbol_table) const {
  // Propagate this to potential Produce.
  return input_->OutputSymbols(symbol_table);
}

std::vector<Symbol> Distinct::ModifiedSymbols(const SymbolTable &table) const { return input_->ModifiedSymbols(table); }

Union::Union(const std::shared_ptr<LogicalOperator> &left_op, const std::shared_ptr<LogicalOperator> &right_op,
             const std::vector<Symbol> &union_symbols, const std::vector<Symbol> &left_symbols,
             const std::vector<Symbol> &right_symbols)
    : left_op_(left_op),
      right_op_(right_op),
      union_symbols_(union_symbols),
      left_symbols_(left_symbols),
      right_symbols_(right_symbols) {}

UniqueCursorPtr Union::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::UnionOperator);

  return MakeUniqueCursorPtr<Union::UnionCursor>(mem, *this, mem);
}

bool Union::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    if (left_op_->Accept(visitor)) {
      right_op_->Accept(visitor);
    }
  }
  return visitor.PostVisit(*this);
}

std::vector<Symbol> Union::OutputSymbols(const SymbolTable &) const { return union_symbols_; }

std::vector<Symbol> Union::ModifiedSymbols(const SymbolTable &) const { return union_symbols_; }

WITHOUT_SINGLE_INPUT(Union);

Union::UnionCursor::UnionCursor(const Union &self, utils::MemoryResource *mem)
    : self_(self), left_cursor_(self.left_op_->MakeCursor(mem)), right_cursor_(self.right_op_->MakeCursor(mem)) {}

bool Union::UnionCursor::Pull(Frame &frame, ExecutionContext &context) {
  SCOPED_PROFILE_OP("Union");

  utils::pmr::unordered_map<std::string, TypedValue> results(context.evaluation_context.memory);
  if (left_cursor_->Pull(frame, context)) {
    // collect values from the left child
    for (const auto &output_symbol : self_.left_symbols_) {
      results[output_symbol.name()] = frame[output_symbol];
    }
  } else if (right_cursor_->Pull(frame, context)) {
    // collect values from the right child
    for (const auto &output_symbol : self_.right_symbols_) {
      results[output_symbol.name()] = frame[output_symbol];
    }
  } else {
    return false;
  }

  // put collected values on frame under union symbols
  for (const auto &symbol : self_.union_symbols_) {
    frame[symbol] = results[symbol.name()];
  }
  return true;
}

void Union::UnionCursor::Shutdown() {
  left_cursor_->Shutdown();
  right_cursor_->Shutdown();
}

void Union::UnionCursor::Reset() {
  left_cursor_->Reset();
  right_cursor_->Reset();
}

std::vector<Symbol> Cartesian::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = left_op_->ModifiedSymbols(table);
  auto right = right_op_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), right.begin(), right.end());
  return symbols;
}

bool Cartesian::Accept(HierarchicalLogicalOperatorVisitor &visitor) {
  if (visitor.PreVisit(*this)) {
    left_op_->Accept(visitor) && right_op_->Accept(visitor);
  }
  return visitor.PostVisit(*this);
}

WITHOUT_SINGLE_INPUT(Cartesian);

namespace {

class CartesianCursor : public Cursor {
 public:
  CartesianCursor(const Cartesian &self, utils::MemoryResource *mem)
      : self_(self),
        left_op_frames_(mem),
        right_op_frame_(mem),
        left_op_cursor_(self.left_op_->MakeCursor(mem)),
        right_op_cursor_(self_.right_op_->MakeCursor(mem)) {
    MG_ASSERT(left_op_cursor_ != nullptr, "CartesianCursor: Missing left operator cursor.");
    MG_ASSERT(right_op_cursor_ != nullptr, "CartesianCursor: Missing right operator cursor.");
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("Cartesian");

    if (!cartesian_pull_initialized_) {
      // Pull all left_op frames.
      while (left_op_cursor_->Pull(frame, context)) {
        left_op_frames_.emplace_back(frame.elems().begin(), frame.elems().end());
      }

      // We're setting the iterator to 'end' here so it pulls the right
      // cursor.
      left_op_frames_it_ = left_op_frames_.end();
      cartesian_pull_initialized_ = true;
    }

    // If left operator yielded zero results there is no cartesian product.
    if (left_op_frames_.empty()) {
      return false;
    }

    auto restore_frame = [&frame](const auto &symbols, const auto &restore_from) {
      for (const auto &symbol : symbols) {
        frame[symbol] = restore_from[symbol.position()];
      }
    };

    if (left_op_frames_it_ == left_op_frames_.end()) {
      // Advance right_op_cursor_.
      if (!right_op_cursor_->Pull(frame, context)) return false;

      right_op_frame_.assign(frame.elems().begin(), frame.elems().end());
      left_op_frames_it_ = left_op_frames_.begin();
    } else {
      // Make sure right_op_cursor last pulled results are on frame.
      restore_frame(self_.right_symbols_, right_op_frame_);
    }

    if (MustAbort(context)) throw HintedAbortError();

    restore_frame(self_.left_symbols_, *left_op_frames_it_);
    left_op_frames_it_++;
    return true;
  }

  void Shutdown() override {
    left_op_cursor_->Shutdown();
    right_op_cursor_->Shutdown();
  }

  void Reset() override {
    left_op_cursor_->Reset();
    right_op_cursor_->Reset();
    right_op_frame_.clear();
    left_op_frames_.clear();
    left_op_frames_it_ = left_op_frames_.end();
    cartesian_pull_initialized_ = false;
  }

 private:
  const Cartesian &self_;
  utils::pmr::vector<utils::pmr::vector<TypedValue>> left_op_frames_;
  utils::pmr::vector<TypedValue> right_op_frame_;
  const UniqueCursorPtr left_op_cursor_;
  const UniqueCursorPtr right_op_cursor_;
  utils::pmr::vector<utils::pmr::vector<TypedValue>>::iterator left_op_frames_it_;
  bool cartesian_pull_initialized_{false};
};

}  // namespace

UniqueCursorPtr Cartesian::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CartesianOperator);

  return MakeUniqueCursorPtr<CartesianCursor>(mem, *this, mem);
}

OutputTable::OutputTable(std::vector<Symbol> output_symbols, std::vector<std::vector<TypedValue>> rows)
    : output_symbols_(std::move(output_symbols)), callback_([rows](Frame *, ExecutionContext *) { return rows; }) {}

OutputTable::OutputTable(std::vector<Symbol> output_symbols,
                         std::function<std::vector<std::vector<TypedValue>>(Frame *, ExecutionContext *)> callback)
    : output_symbols_(std::move(output_symbols)), callback_(std::move(callback)) {}

WITHOUT_SINGLE_INPUT(OutputTable);

class OutputTableCursor : public Cursor {
 public:
  OutputTableCursor(const OutputTable &self) : self_(self) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    if (!pulled_) {
      rows_ = self_.callback_(&frame, &context);
      for (const auto &row : rows_) {
        MG_ASSERT(row.size() == self_.output_symbols_.size(), "Wrong number of columns in row!");
      }
      pulled_ = true;
    }
    if (current_row_ < rows_.size()) {
      for (size_t i = 0; i < self_.output_symbols_.size(); ++i) {
        frame[self_.output_symbols_[i]] = rows_[current_row_][i];
      }
      current_row_++;
      return true;
    }
    return false;
  }

  void Reset() override {
    pulled_ = false;
    current_row_ = 0;
    rows_.clear();
  }

  void Shutdown() override {}

 private:
  const OutputTable &self_;
  size_t current_row_{0};
  std::vector<std::vector<TypedValue>> rows_;
  bool pulled_{false};
};

UniqueCursorPtr OutputTable::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<OutputTableCursor>(mem, *this);
}

OutputTableStream::OutputTableStream(
    std::vector<Symbol> output_symbols,
    std::function<std::optional<std::vector<TypedValue>>(Frame *, ExecutionContext *)> callback)
    : output_symbols_(std::move(output_symbols)), callback_(std::move(callback)) {}

WITHOUT_SINGLE_INPUT(OutputTableStream);

class OutputTableStreamCursor : public Cursor {
 public:
  explicit OutputTableStreamCursor(const OutputTableStream *self) : self_(self) {}

  bool Pull(Frame &frame, ExecutionContext &context) override {
    const auto row = self_->callback_(&frame, &context);
    if (row) {
      MG_ASSERT(row->size() == self_->output_symbols_.size(), "Wrong number of columns in row!");
      for (size_t i = 0; i < self_->output_symbols_.size(); ++i) {
        frame[self_->output_symbols_[i]] = row->at(i);
      }
      return true;
    }
    return false;
  }

  // TODO(tsabolcec): Come up with better approach for handling `Reset()`.
  // One possibility is to implement a custom closure utility class with
  // `Reset()` method.
  void Reset() override { throw utils::NotYetImplemented("OutputTableStreamCursor::Reset"); }

  void Shutdown() override {}

 private:
  const OutputTableStream *self_;
};

UniqueCursorPtr OutputTableStream::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<OutputTableStreamCursor>(mem, this);
}

CallProcedure::CallProcedure(std::shared_ptr<LogicalOperator> input, std::string name, std::vector<Expression *> args,
                             std::vector<std::string> fields, std::vector<Symbol> symbols, Expression *memory_limit,
                             size_t memory_scale, bool is_write)
    : input_(input ? input : std::make_shared<Once>()),
      procedure_name_(name),
      arguments_(args),
      result_fields_(fields),
      result_symbols_(symbols),
      memory_limit_(memory_limit),
      memory_scale_(memory_scale),
      is_write_(is_write) {}

ACCEPT_WITH_INPUT(CallProcedure);

std::vector<Symbol> CallProcedure::OutputSymbols(const SymbolTable &) const { return result_symbols_; }

std::vector<Symbol> CallProcedure::ModifiedSymbols(const SymbolTable &table) const {
  auto symbols = input_->ModifiedSymbols(table);
  symbols.insert(symbols.end(), result_symbols_.begin(), result_symbols_.end());
  return symbols;
}

void CallProcedure::IncrementCounter(const std::string &procedure_name) {
  procedure_counters_.WithLock([&](auto &counters) { ++counters[procedure_name]; });
}

std::unordered_map<std::string, int64_t> CallProcedure::GetAndResetCounters() {
  auto counters = procedure_counters_.Lock();
  auto ret = std::move(*counters);
  counters->clear();
  return ret;
}

namespace {

void CallCustomProcedure(const std::string_view &fully_qualified_procedure_name, const mgp_proc &proc,
                         const std::vector<Expression *> &args, mgp_graph &graph, ExpressionEvaluator *evaluator,
                         utils::MemoryResource *memory, std::optional<size_t> memory_limit, mgp_result *result) {
  static_assert(std::uses_allocator_v<mgp_value, utils::Allocator<mgp_value>>,
                "Expected mgp_value to use custom allocator and makes STL "
                "containers aware of that");
  // Build and type check procedure arguments.
  mgp_list proc_args(memory);
  proc_args.elems.reserve(args.size());
  if (args.size() < proc.args.size() ||
      // Rely on `||` short circuit so we can avoid potential overflow of
      // proc.args.size() + proc.opt_args.size() by subtracting.
      (args.size() - proc.args.size() > proc.opt_args.size())) {
    if (proc.args.empty() && proc.opt_args.empty()) {
      throw QueryRuntimeException("'{}' requires no arguments.", fully_qualified_procedure_name);
    } else if (proc.opt_args.empty()) {
      throw QueryRuntimeException("'{}' requires exactly {} {}.", fully_qualified_procedure_name, proc.args.size(),
                                  proc.args.size() == 1U ? "argument" : "arguments");
    } else {
      throw QueryRuntimeException("'{}' requires between {} and {} arguments.", fully_qualified_procedure_name,
                                  proc.args.size(), proc.args.size() + proc.opt_args.size());
    }
  }
  for (size_t i = 0; i < args.size(); ++i) {
    auto arg = args[i]->Accept(*evaluator);
    std::string_view name;
    const query::procedure::CypherType *type;
    if (proc.args.size() > i) {
      name = proc.args[i].first;
      type = proc.args[i].second;
    } else {
      MG_ASSERT(proc.opt_args.size() > i - proc.args.size());
      name = std::get<0>(proc.opt_args[i - proc.args.size()]);
      type = std::get<1>(proc.opt_args[i - proc.args.size()]);
    }
    if (!type->SatisfiesType(arg)) {
      throw QueryRuntimeException("'{}' argument named '{}' at position {} must be of type {}.",
                                  fully_qualified_procedure_name, name, i, type->GetPresentableName());
    }
    proc_args.elems.emplace_back(std::move(arg), &graph);
  }
  // Fill missing optional arguments with their default values.
  MG_ASSERT(args.size() >= proc.args.size());
  size_t passed_in_opt_args = args.size() - proc.args.size();
  MG_ASSERT(passed_in_opt_args <= proc.opt_args.size());
  for (size_t i = passed_in_opt_args; i < proc.opt_args.size(); ++i) {
    proc_args.elems.emplace_back(std::get<2>(proc.opt_args[i]), &graph);
  }
  if (memory_limit) {
    SPDLOG_INFO("Running '{}' with memory limit of {}", fully_qualified_procedure_name,
                utils::GetReadableSize(*memory_limit));
    utils::LimitedMemoryResource limited_mem(memory, *memory_limit);
    mgp_memory proc_memory{&limited_mem};
    MG_ASSERT(result->signature == &proc.results);
    // TODO: What about cross library boundary exceptions? OMG C++?!
    proc.cb(&proc_args, &graph, result, &proc_memory);
    size_t leaked_bytes = limited_mem.GetAllocatedBytes();
    if (leaked_bytes > 0U) {
      spdlog::warn("Query procedure '{}' leaked {} *tracked* bytes", fully_qualified_procedure_name, leaked_bytes);
    }
  } else {
    // TODO: Add a tracking MemoryResource without limits, so that we report
    // memory leaks in procedure.
    mgp_memory proc_memory{memory};
    MG_ASSERT(result->signature == &proc.results);
    // TODO: What about cross library boundary exceptions? OMG C++?!
    proc.cb(&proc_args, &graph, result, &proc_memory);
  }
}

}  // namespace

class CallProcedureCursor : public Cursor {
  const CallProcedure *self_;
  UniqueCursorPtr input_cursor_;
  mgp_result result_;
  decltype(result_.rows.end()) result_row_it_{result_.rows.end()};
  size_t result_signature_size_{0};

 public:
  CallProcedureCursor(const CallProcedure *self, utils::MemoryResource *mem)
      : self_(self),
        input_cursor_(self_->input_->MakeCursor(mem)),
        // result_ needs to live throughout multiple Pull evaluations, until all
        // rows are produced. Therefore, we use the memory dedicated for the
        // whole execution.
        result_(nullptr, mem) {
    MG_ASSERT(self_->result_fields_.size() == self_->result_symbols_.size(), "Incorrectly constructed CallProcedure");
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("CallProcedure");

    if (MustAbort(context)) throw HintedAbortError();

    // We need to fetch new procedure results after pulling from input.
    // TODO: Look into openCypher's distinction between procedures returning an
    // empty result set vs procedures which return `void`. We currently don't
    // have procedures registering what they return.
    // This `while` loop will skip over empty results.
    while (result_row_it_ == result_.rows.end()) {
      if (!input_cursor_->Pull(frame, context)) return false;
      result_.signature = nullptr;
      result_.rows.clear();
      result_.error_msg.reset();
      // It might be a good idea to resolve the procedure name once, at the
      // start. Unfortunately, this could deadlock if we tried to invoke a
      // procedure from a module (read lock) and reload a module (write lock)
      // inside the same execution thread. Also, our RWLock is setup so that
      // it's not possible for a single thread to request multiple read locks.
      // Builtin module registration in query/procedure/module.cpp depends on
      // this locking scheme.
      const auto &maybe_found = procedure::FindProcedure(procedure::gModuleRegistry, self_->procedure_name_,
                                                         context.evaluation_context.memory);
      if (!maybe_found) {
        throw QueryRuntimeException("There is no procedure named '{}'.", self_->procedure_name_);
      }
      const auto &[module, proc] = *maybe_found;
      if (proc->is_write_procedure != self_->is_write_) {
        auto get_proc_type_str = [](bool is_write) { return is_write ? "write" : "read"; };
        throw QueryRuntimeException("The procedure named '{}' was a {} procedure, but changed to be a {} procedure.",
                                    self_->procedure_name_, get_proc_type_str(self_->is_write_),
                                    get_proc_type_str(proc->is_write_procedure));
      }
      const auto graph_view = proc->is_write_procedure ? storage::View::NEW : storage::View::OLD;
      ExpressionEvaluator evaluator(&frame, context.symbol_table, context.evaluation_context, context.db_accessor,
                                    graph_view);

      result_.signature = &proc->results;
      // Use evaluation memory, as invoking a procedure is akin to a simple
      // evaluation of an expression.
      // TODO: This will probably need to be changed when we add support for
      // generator like procedures which yield a new result on each invocation.
      auto *memory = context.evaluation_context.memory;
      auto memory_limit = EvaluateMemoryLimit(&evaluator, self_->memory_limit_, self_->memory_scale_);
      mgp_graph graph{context.db_accessor, graph_view, &context};
      CallCustomProcedure(self_->procedure_name_, *proc, self_->arguments_, graph, &evaluator, memory, memory_limit,
                          &result_);

      // Reset result_.signature to nullptr, because outside of this scope we
      // will no longer hold a lock on the `module`. If someone were to reload
      // it, the pointer would be invalid.
      result_signature_size_ = result_.signature->size();
      result_.signature = nullptr;
      if (result_.error_msg) {
        throw QueryRuntimeException("{}: {}", self_->procedure_name_, *result_.error_msg);
      }
      result_row_it_ = result_.rows.begin();
    }

    const auto &values = result_row_it_->values;
    // Check that the row has all fields as required by the result signature.
    // C API guarantees that it's impossible to set fields which are not part of
    // the result record, but it does not gurantee that some may be missing. See
    // `mgp_result_record_insert`.
    if (values.size() != result_signature_size_) {
      throw QueryRuntimeException(
          "Procedure '{}' did not yield all fields as required by its "
          "signature.",
          self_->procedure_name_);
    }
    for (size_t i = 0; i < self_->result_fields_.size(); ++i) {
      std::string_view field_name(self_->result_fields_[i]);
      auto result_it = values.find(field_name);
      if (result_it == values.end()) {
        throw QueryRuntimeException("Procedure '{}' did not yield a record with '{}' field.", self_->procedure_name_,
                                    field_name);
      }
      frame[self_->result_symbols_[i]] = result_it->second;
    }
    ++result_row_it_;

    return true;
  }

  void Reset() override {
    result_.rows.clear();
    result_.error_msg.reset();
    input_cursor_->Reset();
  }

  void Shutdown() override {}
};

UniqueCursorPtr CallProcedure::MakeCursor(utils::MemoryResource *mem) const {
  EventCounter::IncrementCounter(EventCounter::CallProcedureOperator);
  CallProcedure::IncrementCounter(procedure_name_);

  return MakeUniqueCursorPtr<CallProcedureCursor>(mem, this, mem);
}

LoadCsv::LoadCsv(std::shared_ptr<LogicalOperator> input, Expression *file, bool with_header, bool ignore_bad,
                 Expression *delimiter, Expression *quote, Symbol row_var)
    : input_(input ? input : (std::make_shared<Once>())),
      file_(file),
      with_header_(with_header),
      ignore_bad_(ignore_bad),
      delimiter_(delimiter),
      quote_(quote),
      row_var_(row_var) {
  MG_ASSERT(file_, "Something went wrong - '{}' member file_ shouldn't be a nullptr", __func__);
}

bool LoadCsv::Accept(HierarchicalLogicalOperatorVisitor &visitor) { return false; };

class LoadCsvCursor;

std::vector<Symbol> LoadCsv::OutputSymbols(const SymbolTable &sym_table) const { return {row_var_}; };

std::vector<Symbol> LoadCsv::ModifiedSymbols(const SymbolTable &sym_table) const {
  auto symbols = input_->ModifiedSymbols(sym_table);
  symbols.push_back(row_var_);
  return symbols;
};

namespace {
// copy-pasted from interpreter.cpp
TypedValue EvaluateOptionalExpression(Expression *expression, ExpressionEvaluator *eval) {
  return expression ? expression->Accept(*eval) : TypedValue();
}

auto ToOptionalString(ExpressionEvaluator *evaluator, Expression *expression) -> std::optional<utils::pmr::string> {
  const auto evaluated_expr = EvaluateOptionalExpression(expression, evaluator);
  if (evaluated_expr.IsString()) {
    return utils::pmr::string(evaluated_expr.ValueString(), utils::NewDeleteResource());
  }
  return std::nullopt;
};

TypedValue CsvRowToTypedList(csv::Reader::Row row) {
  auto *mem = row.get_allocator().GetMemoryResource();
  auto typed_columns = utils::pmr::vector<TypedValue>(mem);
  typed_columns.reserve(row.size());
  for (auto &column : row) {
    typed_columns.emplace_back(std::move(column));
  }
  return TypedValue(typed_columns, mem);
}

TypedValue CsvRowToTypedMap(csv::Reader::Row row, csv::Reader::Header header) {
  // a valid row has the same number of elements as the header
  auto *mem = row.get_allocator().GetMemoryResource();
  utils::pmr::map<utils::pmr::string, TypedValue> m(mem);
  for (auto i = 0; i < row.size(); ++i) {
    m.emplace(std::move(header[i]), std::move(row[i]));
  }
  return TypedValue(m, mem);
}

}  // namespace

class LoadCsvCursor : public Cursor {
  const LoadCsv *self_;
  const UniqueCursorPtr input_cursor_;
  bool input_is_once_;
  std::optional<csv::Reader> reader_{};

 public:
  LoadCsvCursor(const LoadCsv *self, utils::MemoryResource *mem)
      : self_(self), input_cursor_(self_->input_->MakeCursor(mem)) {
    input_is_once_ = dynamic_cast<Once *>(self_->input_.get());
  }

  bool Pull(Frame &frame, ExecutionContext &context) override {
    SCOPED_PROFILE_OP("LoadCsv");

    if (MustAbort(context)) throw HintedAbortError();

    // ToDo(the-joksim):
    //  - this is an ungodly hack because the pipeline of creating a plan
    //  doesn't allow evaluating the expressions contained in self_->file_,
    //  self_->delimiter_, and self_->quote_ earlier (say, in the interpreter.cpp)
    //  without massacring the code even worse than I did here
    if (UNLIKELY(!reader_)) {
      reader_ = MakeReader(&context.evaluation_context);
    }

    bool input_pulled = input_cursor_->Pull(frame, context);

    // If the input is Once, we have to keep going until we read all the rows,
    // regardless of whether the pull on Once returned false.
    // If we have e.g. MATCH(n) LOAD CSV ... AS x SET n.name = x.name, then we
    // have to read at most cardinality(n) rows (but we can read less and stop
    // pulling MATCH).
    if (!input_is_once_ && !input_pulled) return false;

    if (auto row = reader_->GetNextRow(context.evaluation_context.memory)) {
      if (!reader_->HasHeader()) {
        frame[self_->row_var_] = CsvRowToTypedList(std::move(*row));
      } else {
        frame[self_->row_var_] = CsvRowToTypedMap(
            std::move(*row), csv::Reader::Header(reader_->GetHeader(), context.evaluation_context.memory));
      }
      return true;
    }

    return false;
  }

  void Reset() override { input_cursor_->Reset(); }
  void Shutdown() override { input_cursor_->Shutdown(); }

 private:
  csv::Reader MakeReader(EvaluationContext *eval_context) {
    Frame frame(0);
    SymbolTable symbol_table;
    DbAccessor *dba = nullptr;
    auto evaluator = ExpressionEvaluator(&frame, symbol_table, *eval_context, dba, storage::View::OLD);

    auto maybe_file = ToOptionalString(&evaluator, self_->file_);
    auto maybe_delim = ToOptionalString(&evaluator, self_->delimiter_);
    auto maybe_quote = ToOptionalString(&evaluator, self_->quote_);

    // No need to check if maybe_file is std::nullopt, as the parser makes sure
    // we can't get a nullptr for the 'file_' member in the LoadCsv clause.
    // Note that the reader has to be given its own memory resource, as it
    // persists between pulls, so it can't use the evalutation context memory
    // resource.
    return csv::Reader(
        *maybe_file,
        csv::Reader::Config(self_->with_header_, self_->ignore_bad_, std::move(maybe_delim), std::move(maybe_quote)),
        utils::NewDeleteResource());
  }
};

UniqueCursorPtr LoadCsv::MakeCursor(utils::MemoryResource *mem) const {
  return MakeUniqueCursorPtr<LoadCsvCursor>(mem, this, mem);
};

}  // namespace query::plan
