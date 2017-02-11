#include <database/creation_exception.hpp>
#include "database/graph_db_accessor.hpp"


GraphDbAccessor::GraphDbAccessor(GraphDb& db) : db_(db), transaction_(std::move(db.tx_engine.begin())) {}

VertexAccessor GraphDbAccessor::insert_vertex() {
  auto vertex_vlist = new mvcc::VersionList<Vertex>();
  vertex_vlist->insert(transaction_);

  // TODO make this configurable
  for (int i = 0; i < 5; ++i) {
    bool success = db_.vertices_.access().insert(vertex_vlist).second;
    if (success)
      return VertexAccessor(vertex_vlist, transaction_);
    // TODO sleep for some amount of time
  }

  throw CreationException("Unable to create a Vertex after 5 attempts");
}

EdgeAccessor GraphDbAccessor::insert_edge(
    VertexAccessor& from,
    VertexAccessor& to,
    GraphDb::EdgeType type) {

  auto edge_vlist = new mvcc::VersionList<Edge>();
  Edge* edge = edge_vlist->insert(transaction_);

  // set the given values of the new edge
  edge->edge_type_ = type;
  // connect the edge to vertices
  edge->from_ = from.vlist(pass_key);
  edge->to_ = to.vlist(pass_key);
  // TODO connect the vertices to edge
  from.add_to_out(edge_vlist, pass_key);
  to.add_to_in(edge_vlist, pass_key);

  // TODO make this configurable
  for (int i = 0; i < 5; ++i) {
    bool success = db_.edges_.access().insert(edge_vlist).second;
    if (success)
      return EdgeAccessor(edge_vlist, transaction_);
    // TODO sleep for some amount of time
  }

  throw CreationException("Unable to create an Edge after 5 attempts");
}

GraphDb::Label GraphDbAccessor::label(const std::string& label_name) {
  return &(*db_.labels_.access().insert(label_name).first);
}

std::string& GraphDbAccessor::label_name(const GraphDb::Label label) const {
  return *label;
}

GraphDb::EdgeType GraphDbAccessor::edge_type(const std::string& edge_type_name){
  return &(*db_.edge_types_.access().insert(edge_type_name).first);
}

std::string& GraphDbAccessor::edge_type_name(const GraphDb::EdgeType edge_type) const {
  return *edge_type;
}

GraphDb::Property GraphDbAccessor::property(const std::string& property_name) {
  return &(*db_.properties_.access().insert(property_name).first);
}

std::string& GraphDbAccessor::property_name(const GraphDb::Property property) const {
  return *property;
}
