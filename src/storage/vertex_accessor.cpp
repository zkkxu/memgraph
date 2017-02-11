#include "storage/edge_accessor.hpp"
#include "storage/vertex_accessor.hpp"

size_t VertexAccessor::out_degree() const {
  return this->view()->out_.size();
}

size_t VertexAccessor::in_degree() const {
  return this->view()->in_.size();
}

bool VertexAccessor::add_label(GraphDb::Label label) {
  return this->update()->labels_.emplace(label).second;
}

size_t VertexAccessor::remove_label(GraphDb::Label label) {
  return this->update()->labels_.erase(label);
}

bool VertexAccessor::has_label(GraphDb::Label label) const {
  auto &label_set = this->view()->labels_;
  return label_set.find(label) != label_set.end();
}

const std::set<GraphDb::Label>& VertexAccessor::labels() const {
  return this->view()->labels_;
}

bool VertexAccessor::remove() {
  // TODO consider if this works well with MVCC
  if (out_degree() > 0 || in_degree() > 0)
    return false;

  vlist_->remove(view(), db_accessor_->transaction_);
  return true;
}

void VertexAccessor::detach_remove() {
  // removing edges via accessors is both safe
  // and it should remove all the pointers in the relevant
  // vertices (including this one)
  for (auto edge_vlist : view()->out_)
    EdgeAccessor(edge_vlist, db_accessor_->transaction_).remove();

  for (auto edge_vlist : view()->in_)
    EdgeAccessor(edge_vlist, db_accessor_->transaction_).remove();

  vlist_->remove(view(), db_accessor_->transaction_);
}

void VertexAccessor::attach_in(mvcc::VersionList<Edge>* edge_vlist, PassKey<GraphDb>) {
  this->update()->in_.emplace_back(edge_vlist);
}

void VertexAccessor::attach_out(mvcc::VersionList<Edge>* edge_vlist, PassKey<GraphDb>) {
  this->update()->out_.emplace_back(edge_vlist);
}
