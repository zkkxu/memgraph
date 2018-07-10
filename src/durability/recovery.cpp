#include "durability/recovery.hpp"

#include <experimental/filesystem>
#include <limits>
#include <unordered_map>

#include "database/graph_db_accessor.hpp"
#include "database/indexes/label_property_index.hpp"
#include "durability/hashed_file_reader.hpp"
#include "durability/paths.hpp"
#include "durability/snapshot_decoded_value.hpp"
#include "durability/snapshot_decoder.hpp"
#include "durability/version.hpp"
#include "durability/wal.hpp"
#include "glue/conversion.hpp"
#include "query/typed_value.hpp"
#include "storage/address_types.hpp"
#include "transactions/type.hpp"
#include "utils/algorithm.hpp"

namespace fs = std::experimental::filesystem;

namespace durability {

bool ReadSnapshotSummary(HashedFileReader &buffer, int64_t &vertex_count,
                         int64_t &edge_count, uint64_t &hash) {
  auto pos = buffer.Tellg();
  auto offset = sizeof(vertex_count) + sizeof(edge_count) + sizeof(hash);
  buffer.Seek(-offset, std::ios_base::end);
  bool r_val = buffer.ReadType(vertex_count, false) &&
               buffer.ReadType(edge_count, false) &&
               buffer.ReadType(hash, false);
  buffer.Seek(pos);
  return r_val;
}

namespace {
using communication::bolt::DecodedValue;

// A data structure for exchanging info between main recovery function and
// snapshot and WAL recovery functions.
struct RecoveryData {
  tx::TransactionId snapshooter_tx_id{0};
  tx::TransactionId wal_max_recovered_tx_id{0};
  std::vector<tx::TransactionId> snapshooter_tx_snapshot;
  // A collection into which the indexes should be added so they
  // can be rebuilt at the end of the recovery transaction.
  std::vector<std::pair<std::string, std::string>> indexes;

  void Clear() {
    snapshooter_tx_id = 0;
    snapshooter_tx_snapshot.clear();
    indexes.clear();
  }
};

#define RETURN_IF_NOT(condition) \
  if (!(condition)) {            \
    reader.Close();              \
    return false;                \
  }

bool RecoverSnapshot(const fs::path &snapshot_file, database::GraphDb &db,
                     RecoveryData &recovery_data) {
  HashedFileReader reader;
  SnapshotDecoder<HashedFileReader> decoder(reader);

  RETURN_IF_NOT(reader.Open(snapshot_file));

  auto magic_number = durability::kMagicNumber;
  reader.Read(magic_number.data(), magic_number.size());
  RETURN_IF_NOT(magic_number == durability::kMagicNumber);

  // Read the vertex and edge count, and the hash, from the end of the snapshot.
  int64_t vertex_count;
  int64_t edge_count;
  uint64_t hash;
  RETURN_IF_NOT(
      durability::ReadSnapshotSummary(reader, vertex_count, edge_count, hash));

  DecodedValue dv;

  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::Int) &&
                dv.ValueInt() == durability::kVersion);

  // Checks worker id was set correctly
  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::Int) &&
                dv.ValueInt() == db.WorkerId());

  // Vertex and edge generator ids
  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::Int));
  uint64_t vertex_generator_cnt = dv.ValueInt();
  db.storage().VertexGenerator().SetId(std::max(
      db.storage().VertexGenerator().LocalCount(), vertex_generator_cnt));
  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::Int));
  uint64_t edge_generator_cnt = dv.ValueInt();
  db.storage().EdgeGenerator().SetId(
      std::max(db.storage().EdgeGenerator().LocalCount(), edge_generator_cnt));

  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::Int));
  recovery_data.snapshooter_tx_id = dv.ValueInt();
  // Transaction snapshot of the transaction that created the snapshot.
  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::List));
  for (const auto &value : dv.ValueList()) {
    RETURN_IF_NOT(value.IsInt());
    recovery_data.snapshooter_tx_snapshot.emplace_back(value.ValueInt());
  }

  // A list of label+property indexes.
  RETURN_IF_NOT(decoder.ReadValue(&dv, DecodedValue::Type::List));
  auto index_value = dv.ValueList();
  for (auto it = index_value.begin(); it != index_value.end();) {
    auto label = *it++;
    RETURN_IF_NOT(it != index_value.end());
    auto property = *it++;
    RETURN_IF_NOT(label.IsString() && property.IsString());
    recovery_data.indexes.emplace_back(label.ValueString(),
                                       property.ValueString());
  }

  database::GraphDbAccessor dba(db);
  std::unordered_map<gid::Gid,
                     std::pair<storage::VertexAddress, storage::VertexAddress>>
      edge_gid_endpoints_mapping;

  for (int64_t i = 0; i < vertex_count; ++i) {
    auto vertex = decoder.ReadSnapshotVertex();
    RETURN_IF_NOT(vertex);

    auto vertex_accessor = dba.InsertVertex(vertex->gid, vertex->cypher_id);
    for (const auto &label : vertex->labels) {
      vertex_accessor.add_label(dba.Label(label));
    }
    for (const auto &property_pair : vertex->properties) {
      vertex_accessor.PropsSet(dba.Property(property_pair.first),
                               glue::ToTypedValue(property_pair.second));
    }
    auto vertex_record = vertex_accessor.GetNew();
    for (const auto &edge : vertex->in) {
      vertex_record->in_.emplace(edge.vertex, edge.address,
                                 dba.EdgeType(edge.type));
      edge_gid_endpoints_mapping[edge.address.gid()] = {
          edge.vertex, vertex_accessor.GlobalAddress()};
    }
    for (const auto &edge : vertex->out) {
      vertex_record->out_.emplace(edge.vertex, edge.address,
                                  dba.EdgeType(edge.type));
      edge_gid_endpoints_mapping[edge.address.gid()] = {
          vertex_accessor.GlobalAddress(), edge.vertex};
    }
  }

  auto vertex_transform_to_local_if_possible =
      [&db, &dba](storage::VertexAddress &address) {
        if (address.is_local()) return;
        // If the worker id matches it should be a local apperance
        if (address.worker_id() == db.WorkerId()) {
          address = storage::VertexAddress(
              dba.db().storage().LocalAddress<Vertex>(address.gid()));
          CHECK(address.is_local()) << "Address should be local but isn't";
        }
      };

  auto edge_transform_to_local_if_possible =
      [&db, &dba](storage::EdgeAddress &address) {
        if (address.is_local()) return;
        // If the worker id matches it should be a local apperance
        if (address.worker_id() == db.WorkerId()) {
          address = storage::EdgeAddress(
              dba.db().storage().LocalAddress<Edge>(address.gid()));
          CHECK(address.is_local()) << "Address should be local but isn't";
        }
      };

  DecodedValue dv_cypher_id;

  for (int64_t i = 0; i < edge_count; ++i) {
    RETURN_IF_NOT(
        decoder.ReadValue(&dv, communication::bolt::DecodedValue::Type::Edge));
    auto &edge = dv.ValueEdge();

    // Read cypher_id
    RETURN_IF_NOT(decoder.ReadValue(
        &dv_cypher_id, communication::bolt::DecodedValue::Type::Int));
    auto cypher_id = dv_cypher_id.ValueInt();

    // We have to take full edge endpoints from vertices since the endpoints
    // found here don't containt worker_id, and this can't be changed since this
    // edges must be bolt-compliant
    auto &edge_endpoints = edge_gid_endpoints_mapping[edge.id.AsUint()];

    storage::VertexAddress from;
    storage::VertexAddress to;
    std::tie(from, to) = edge_endpoints;

    // From and to are written in the global_address format and we should
    // convert them back to local format for speedup - if possible
    vertex_transform_to_local_if_possible(from);
    vertex_transform_to_local_if_possible(to);

    auto edge_accessor = dba.InsertOnlyEdge(from, to, dba.EdgeType(edge.type),
                                            edge.id.AsUint(), cypher_id);

    for (const auto &property_pair : edge.properties)
      edge_accessor.PropsSet(dba.Property(property_pair.first),
                             glue::ToTypedValue(property_pair.second));
  }

  // Vertex and edge counts are included in the hash. Re-read them to update the
  // hash.
  reader.ReadType(vertex_count);
  reader.ReadType(edge_count);
  if (!reader.Close() || reader.hash() != hash) {
    dba.Abort();
    return false;
  }

  // We have to replace global_ids with local ids where possible for all edges
  // in every vertex and this can only be done after we inserted the edges; this
  // is to speedup execution
  for (auto &vertex_accessor : dba.Vertices(true)) {
    auto vertex = vertex_accessor.GetNew();
    auto iterate_and_transform =
        [vertex_transform_to_local_if_possible,
         edge_transform_to_local_if_possible](Edges &edges) {
          Edges transformed;
          for (auto &element : edges) {
            auto vertex = element.vertex;
            vertex_transform_to_local_if_possible(vertex);

            auto edge = element.edge;
            edge_transform_to_local_if_possible(edge);

            transformed.emplace(vertex, edge, element.edge_type);
          }

          return transformed;
        };

    vertex->in_ = iterate_and_transform(vertex->in_);
    vertex->out_ = iterate_and_transform(vertex->out_);
  }

  // Ensure that the next transaction ID in the recovered DB will be greater
  // than the latest one we have recovered. Do this to make sure that
  // subsequently created snapshots and WAL files will have transactional info
  // that does not interfere with that found in previous snapshots and WAL.
  tx::TransactionId max_id = recovery_data.snapshooter_tx_id;
  auto &snap = recovery_data.snapshooter_tx_snapshot;
  if (!snap.empty()) max_id = *std::max_element(snap.begin(), snap.end());
  dba.db().tx_engine().EnsureNextIdGreater(max_id);
  dba.Commit();
  return true;
}

#undef RETURN_IF_NOT

// TODO - finer-grained recovery feedback could be useful here.
bool RecoverWal(const fs::path &wal_dir, database::GraphDb &db,
                RecoveryData &recovery_data) {
  // Get paths to all the WAL files and sort them (on date).
  std::vector<fs::path> wal_files;
  if (!fs::exists(wal_dir)) return true;
  for (auto &wal_file : fs::directory_iterator(wal_dir))
    wal_files.emplace_back(wal_file);
  std::sort(wal_files.begin(), wal_files.end());

  // Track which transaction should be recovered first, and define logic for
  // which transactions should be skipped in recovery.
  auto &tx_sn = recovery_data.snapshooter_tx_snapshot;
  auto first_to_recover = tx_sn.empty() ? recovery_data.snapshooter_tx_id + 1
                                        : *std::min(tx_sn.begin(), tx_sn.end());
  auto should_skip = [&tx_sn, &recovery_data,
                      first_to_recover](tx::TransactionId tx_id) {
    return tx_id < first_to_recover ||
           (tx_id < recovery_data.snapshooter_tx_id &&
            !utils::Contains(tx_sn, tx_id));
  };

  std::unordered_map<tx::TransactionId, database::GraphDbAccessor> accessors;
  auto get_accessor =
      [&accessors](tx::TransactionId tx_id) -> database::GraphDbAccessor & {
    auto found = accessors.find(tx_id);
    CHECK(found != accessors.end())
        << "Accessor does not exist for transaction: " << tx_id;
    return found->second;
  };

  // Ensure that the next transaction ID in the recovered DB will be greater
  // then the latest one we have recovered. Do this to make sure that
  // subsequently created snapshots and WAL files will have transactional info
  // that does not interfere with that found in previous snapshots and WAL.
  tx::TransactionId max_observed_tx_id{0};

  // Read all the WAL files whose max_tx_id is not smaller than
  // min_tx_to_recover.
  for (auto &wal_file : wal_files) {
    auto wal_file_max_tx_id = TransactionIdFromWalFilename(wal_file.filename());
    if (!wal_file_max_tx_id || *wal_file_max_tx_id < first_to_recover) continue;

    HashedFileReader wal_reader;
    if (!wal_reader.Open(wal_file)) return false;
    communication::bolt::Decoder<HashedFileReader> decoder(wal_reader);
    while (true) {
      auto delta = database::StateDelta::Decode(wal_reader, decoder);
      if (!delta) break;
      max_observed_tx_id = std::max(max_observed_tx_id, delta->transaction_id);
      if (should_skip(delta->transaction_id)) continue;
      switch (delta->type) {
        case database::StateDelta::Type::TRANSACTION_BEGIN:
          DCHECK(accessors.find(delta->transaction_id) == accessors.end())
              << "Double transaction start";
          accessors.emplace(delta->transaction_id, db);
          break;
        case database::StateDelta::Type::TRANSACTION_ABORT:
          get_accessor(delta->transaction_id).Abort();
          accessors.erase(accessors.find(delta->transaction_id));
          break;
        case database::StateDelta::Type::TRANSACTION_COMMIT:
          get_accessor(delta->transaction_id).Commit();
          recovery_data.wal_max_recovered_tx_id = delta->transaction_id;
          accessors.erase(accessors.find(delta->transaction_id));
          break;
        case database::StateDelta::Type::BUILD_INDEX:
          // TODO index building might still be problematic in HA
          recovery_data.indexes.emplace_back(delta->label_name,
                                             delta->property_name);
          break;
        default:
          delta->Apply(get_accessor(delta->transaction_id));
      }
    }  // reading all deltas in a single wal file
  }    // reading all wal files

  // TODO when implementing proper error handling return one of the following:
  // - WAL fully recovered
  // - WAL partially recovered
  // - WAL recovery error

  db.tx_engine().EnsureNextIdGreater(max_observed_tx_id);
  return true;
}
}  // anonymous namespace

RecoveryInfo Recover(
    const fs::path &durability_dir, database::GraphDb &db,
    std::experimental::optional<RecoveryInfo> required_recovery_info) {
  RecoveryData recovery_data;

  // Attempt to recover from snapshot files in reverse order (from newest
  // backwards).
  const auto snapshot_dir = durability_dir / kSnapshotDir;
  std::vector<fs::path> snapshot_files;
  if (fs::exists(snapshot_dir) && fs::is_directory(snapshot_dir))
    for (auto &file : fs::directory_iterator(snapshot_dir))
      snapshot_files.emplace_back(file);
  std::sort(snapshot_files.rbegin(), snapshot_files.rend());
  for (auto &snapshot_file : snapshot_files) {
    if (required_recovery_info) {
      auto snapshot_file_tx_id =
          TransactionIdFromSnapshotFilename(snapshot_file);
      if (!snapshot_file_tx_id || snapshot_file_tx_id.value() !=
                                      required_recovery_info->snapshot_tx_id) {
        LOG(INFO) << "Skipping snapshot file '" << snapshot_file
                  << "' because it does not match the required snapshot tx id: "
                  << required_recovery_info->snapshot_tx_id;
        continue;
      }
    }
    LOG(INFO) << "Starting snapshot recovery from: " << snapshot_file;
    if (!RecoverSnapshot(snapshot_file, db, recovery_data)) {
      db.ReinitializeStorage();
      recovery_data.Clear();
      LOG(WARNING) << "Snapshot recovery failed, trying older snapshot...";
      continue;
    } else {
      LOG(INFO) << "Snapshot recovery successful.";
      break;
    }
  }

  // If snapshot recovery is required, and we failed, don't even deal with the
  // WAL recovery.
  if (required_recovery_info &&
      recovery_data.snapshooter_tx_id != required_recovery_info->snapshot_tx_id)
    return {recovery_data.snapshooter_tx_id, 0};

  // Write-ahead-log recovery.
  // WAL recovery does not have to be complete for the recovery to be
  // considered successful. For the time being ignore the return value,
  // consider a better system.
  RecoverWal(durability_dir / kWalDir, db, recovery_data);

  // Index recovery.
  database::GraphDbAccessor db_accessor_indices{db};
  for (const auto &label_prop : recovery_data.indexes) {
    const database::LabelPropertyIndex::Key key{
        db_accessor_indices.Label(label_prop.first),
        db_accessor_indices.Property(label_prop.second)};
    db_accessor_indices.db().storage().label_property_index_.CreateIndex(key);
    db_accessor_indices.PopulateIndex(key);
    db_accessor_indices.EnableIndex(key);
  }
  db_accessor_indices.Commit();

  return {recovery_data.snapshooter_tx_id,
          recovery_data.wal_max_recovered_tx_id};
}
}  // namespace durability
