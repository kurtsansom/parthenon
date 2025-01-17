//========================================================================================
// (C) (or copyright) 2020-2021. Triad National Security, LLC. All rights reserved.
//
// This program was produced under U.S. Government contract 89233218CNA000001 for Los
// Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
// for the U.S. Department of Energy/National Nuclear Security Administration. All rights
// in the program are reserved by Triad National Security, LLC, and the U.S. Department
// of Energy/National Nuclear Security Administration. The Government is granted for
// itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
// license in this material to reproduce, prepare derivative works, distribute copies to
// the public, perform publicly and display publicly, and to permit others to do so.
//========================================================================================

#include "interface/meshblock_data.hpp"

#include <cstdlib>
#include <memory>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bvals/cc/bvals_cc.hpp"
#include "globals.hpp"
#include "interface/metadata.hpp"
#include "interface/state_descriptor.hpp"
#include "interface/variable.hpp"
#include "interface/variable_pack.hpp"
#include "mesh/mesh.hpp"
#include "mesh/meshblock.hpp"
#include "utils/error_checking.hpp"
#include "utils/utils.hpp"

namespace parthenon {

template <typename T>
void MeshBlockData<T>::Initialize(
    const std::shared_ptr<StateDescriptor> resolved_packages,
    const std::shared_ptr<MeshBlock> pmb) {
  SetBlockPointer(pmb);
  resolved_packages_ = resolved_packages;

  // clear all variables, maps, and pack caches
  varVector_.clear();
  faceVector_.clear();
  varMap_.clear();
  faceMap_.clear();
  varPackMap_.clear();
  coarseVarPackMap_.clear();
  varFluxPackMap_.clear();

  for (auto const &q : resolved_packages->AllFields()) {
    AddField(q.first.base_name, q.second, q.first.sparse_id);
  }
}

///
/// The internal routine for adding a new field.  This subroutine
/// is topology aware and will allocate accordingly.
///
/// @param label the name of the variable
/// @param metadata the metadata associated with the variable
/// @param sparse_id the sparse id of the variable
template <typename T>
void MeshBlockData<T>::AddField(const std::string &base_name, const Metadata &metadata,
                                int sparse_id) {
  // branch on kind of variable
  if (metadata.Where() == Metadata::Node) {
    PARTHENON_THROW("Node variables are not implemented yet");
  } else if (metadata.Where() == Metadata::Edge) {
    // add an edge variable
    std::cerr << "Accessing unliving edge array in stage" << std::endl;
    std::exit(1);
    // s->_edgeVector.push_back(
    //     new EdgeVariable(label, metadata,
    //                      pmy_block->ncells3, pmy_block->ncells2, pmy_block->ncells1));
  } else if (metadata.Where() == Metadata::Face) {
    if (!(metadata.IsSet(Metadata::OneCopy))) {
      std::cerr << "Currently one one-copy face fields are supported" << std::endl;
      std::exit(1);
    }
    if (metadata.IsSet(Metadata::FillGhost)) {
      std::cerr << "Ghost zones not yet supported for face fields" << std::endl;
      std::exit(1);
    }
    // add a face variable
    auto pfv = std::make_shared<FaceVariable<T>>(
        base_name, metadata.GetArrayDims(pmy_block, false), metadata);
    Add(pfv);
  } else {
    auto pvar =
        std::make_shared<CellVariable<T>>(base_name, metadata, sparse_id, pmy_block);
    Add(pvar);

    if (!Globals::sparse_config.enabled || !pvar->IsSparse()) {
      pvar->Allocate(pmy_block);
    }
  }
}

template <typename T>
void MeshBlockData<T>::CopyFrom(const MeshBlockData<T> &src, bool shallow_copy,
                                const std::vector<std::string> &names,
                                const std::vector<MetadataFlag> &flags,
                                const std::vector<int> &sparse_ids) {
  SetBlockPointer(src);
  resolved_packages_ = src.resolved_packages_;
  std::unordered_set<int> sparse_ids_set(sparse_ids.begin(), sparse_ids.end());

  auto add_var = [=, &flags, &sparse_ids](auto var) {
    if (!flags.empty() && !var->metadata().AnyFlagsSet(flags)) {
      return;
    }

    if (!sparse_ids.empty() && var->IsSparse() &&
        (sparse_ids_set.count(var->GetSparseID()) == 0)) {
      return;
    }

    if (shallow_copy || var->IsSet(Metadata::OneCopy)) {
      Add(var);
    } else {
      Add(var->AllocateCopy(pmy_block));
    }
  };

  if (names.empty()) {
    for (auto v : src.GetCellVariableVector()) {
      add_var(v);
    }
    for (auto fv : src.GetFaceVector()) {
      add_var(fv);
    }
  } else {
    auto var_map = src.GetCellVariableMap();
    auto face_map = src.GetFaceMap();

    for (const auto &name : names) {
      bool found = false;
      auto v = var_map.find(name);
      if (v != var_map.end()) {
        found = true;
        add_var(v->second);
      }

      auto fv = face_map.find(name);
      if (fv != face_map.end()) {
        PARTHENON_REQUIRE_THROWS(!found, "MeshBlockData::CopyFrom: Variable '" + name +
                                             "' found more than once");
        found = true;
        add_var(fv->second);
      }

      if (!found && (resolved_packages_ != nullptr)) {
        // check if this is a sparse base name, if so we get its pool of sparse_ids,
        // otherwise we get an empty pool
        const auto &sparse_pool = resolved_packages_->GetSparsePool(name);

        // add all sparse ids of the pool
        for (const auto iter : sparse_pool.pool()) {
          // this variable must exist, if it doesn't something is very wrong
          const auto &v = varMap_.at(MakeVarLabel(name, iter.first));
          add_var(v);
          found = true;
        }
      }

      PARTHENON_REQUIRE_THROWS(found, "MeshBlockData::CopyFrom: Variable '" + name +
                                          "' not found");
    }
  }
}

// Constructor for getting sub-containers
// the variables returned are all shallow copies of the src container.
// Optionally extract only some of the sparse ids of src variable.
template <typename T>
MeshBlockData<T>::MeshBlockData(const MeshBlockData<T> &src,
                                const std::vector<std::string> &names,
                                const std::vector<int> &sparse_ids) {
  CopyFrom(src, true, names, {}, sparse_ids);
}

template <typename T>
MeshBlockData<T>::MeshBlockData(const MeshBlockData<T> &src,
                                const std::vector<MetadataFlag> &flags,
                                const std::vector<int> &sparse_ids) {
  CopyFrom(src, true, {}, flags, sparse_ids);
}

// provides a container that has a single sparse slice
template <typename T>
std::shared_ptr<MeshBlockData<T>>
MeshBlockData<T>::SparseSlice(const std::vector<int> &sparse_ids) const {
  auto c = std::make_shared<MeshBlockData<T>>();
  c->CopyFrom(*this, true, {}, {}, sparse_ids);
  return c;
}

/// Queries related to variable packs
/// TODO(JMM): Make sure this is thread-safe
/// TODO(JMM): Should the vector of names be sorted to enforce uniqueness?
/// This is a helper function that queries the cache for the given pack.
/// The strings are the keys and the lists are the values.
/// Inputs:
/// variables = forward list of shared pointers of vars to pack
/// fluxes = forward list of shared pointers of fluxes to pack
/// Returns:
/// A FluxMetaPack<T> that contains the actual VariableFluxPack, the PackIndexMap, and the
/// keys
template <typename T>
const VariableFluxPack<T> &MeshBlockData<T>::PackListedVariablesAndFluxes(
    const VarLabelList &var_list, const VarLabelList &flux_list, PackIndexMap *map,
    vpack_types::StringPair *key) {
  vpack_types::StringPair keys =
      std::make_pair(std::move(var_list.labels()), std::move(flux_list.labels()));

  auto itr = varFluxPackMap_.find(keys);
  bool make_new_pack = false;
  if (itr == varFluxPackMap_.end()) {
    // we don't have a cached pack, need to make a new one
    make_new_pack = true;
  } else {
    // we have a cached pack, check allocation status
    if ((var_list.alloc_status() != itr->second.alloc_status) ||
        (flux_list.alloc_status() != itr->second.flux_alloc_status)) {
      // allocation statuses differ, need to make a new pack and remove outdated one
      make_new_pack = true;
      varFluxPackMap_.erase(itr);
    }
  }

  if (make_new_pack) {
    FluxPackIndxPair<T> new_item;
    new_item.alloc_status = var_list.alloc_status();
    new_item.flux_alloc_status = flux_list.alloc_status();
    new_item.pack = MakeFluxPack(var_list, flux_list, &new_item.map);
    new_item.pack.coords = GetParentPointer()->coords_device;
    itr = varFluxPackMap_.insert({keys, new_item}).first;

    // need to grab pointers here
    itr->second.pack.alloc_status_ = &itr->second.alloc_status;
    itr->second.pack.flux_alloc_status_ = &itr->second.flux_alloc_status;
  }

  if (map != nullptr) {
    *map = itr->second.map;
  }
  if (key != nullptr) {
    *key = itr->first;
  }

  return itr->second.pack;
}

/// This is a helper function that queries the cache for the given pack.
/// The strings are the key and the lists are the values.
/// Inputs:
/// vars = forward list of shared pointers of vars to pack
/// coarse = whether to use coarse pack map or not
/// Returns:
/// A VarMetaPack<T> that contains the actual VariablePack, the PackIndexMap, and the key
template <typename T>
const VariablePack<T> &
MeshBlockData<T>::PackListedVariables(const VarLabelList &var_list, bool coarse,
                                      PackIndexMap *map,
                                      std::vector<std::string> *key_out) {
  const auto &key = var_list.labels();
  auto &packmap = coarse ? coarseVarPackMap_ : varPackMap_;

  auto itr = packmap.find(key);
  bool make_new_pack = false;
  if (itr == packmap.end()) {
    // we don't have a cached pack, need to make a new one
    make_new_pack = true;
  } else {
    // we have a cached pack, check allocation status
    if (var_list.alloc_status() != itr->second.alloc_status) {
      // allocation statuses differ, need to make a new pack and remove outdated one
      make_new_pack = true;
      packmap.erase(itr);
    }
  }

  if (make_new_pack) {
    PackIndxPair<T> new_item;
    new_item.alloc_status = var_list.alloc_status();
    new_item.pack = MakePack<T>(var_list, coarse, &new_item.map);
    new_item.pack.coords = GetParentPointer()->coords_device;

    itr = packmap.insert({key, new_item}).first;

    // need to grab pointers after map insertion
    itr->second.pack.alloc_status_ = &itr->second.alloc_status;
  }

  if (map != nullptr) {
    *map = itr->second.map;
  }
  if (key_out != nullptr) {
    *key_out = itr->first;
  }

  return itr->second.pack;
}

/***********************************/
/* PACK VARIABLES INTERFACE        */
/***********************************/

/// Variables and fluxes by Name
template <typename T>
const VariableFluxPack<T> &MeshBlockData<T>::PackVariablesAndFluxesImpl(
    const std::vector<std::string> &var_names, const std::vector<std::string> &flx_names,
    const std::vector<int> &sparse_ids, PackIndexMap *map, vpack_types::StringPair *key) {
  return PackListedVariablesAndFluxes(GetVariablesByName(var_names, sparse_ids),
                                      GetVariablesByName(flx_names, sparse_ids), map,
                                      key);
}

/// Variables and fluxes by Metadata Flags
template <typename T>
const VariableFluxPack<T> &MeshBlockData<T>::PackVariablesAndFluxesImpl(
    const std::vector<MetadataFlag> &flags, const std::vector<int> &sparse_ids,
    PackIndexMap *map, vpack_types::StringPair *key) {
  return PackListedVariablesAndFluxes(GetVariablesByFlag(flags, true, sparse_ids),
                                      GetVariablesByFlag(flags, true, sparse_ids), map,
                                      key);
}

/// All variables and fluxes by Metadata Flags
template <typename T>
const VariableFluxPack<T> &MeshBlockData<T>::PackVariablesAndFluxesImpl(
    const std::vector<int> &sparse_ids, PackIndexMap *map, vpack_types::StringPair *key) {
  return PackListedVariablesAndFluxes(GetAllVariables(sparse_ids),
                                      GetAllVariables(sparse_ids), map, key);
}

/// Variables by Name
template <typename T>
const VariablePack<T> &
MeshBlockData<T>::PackVariablesImpl(const std::vector<std::string> &names,
                                    const std::vector<int> &sparse_ids, bool coarse,
                                    PackIndexMap *map, std::vector<std::string> *key) {
  return PackListedVariables(GetVariablesByName(names, sparse_ids), coarse, map, key);
}

/// Variables by Metadata Flags
template <typename T>
const VariablePack<T> &
MeshBlockData<T>::PackVariablesImpl(const std::vector<MetadataFlag> &flags,
                                    const std::vector<int> &sparse_ids, bool coarse,
                                    PackIndexMap *map, std::vector<std::string> *key) {
  return PackListedVariables(GetVariablesByFlag(flags, true, sparse_ids), coarse, map,
                             key);
}

/// All variables
template <typename T>
const VariablePack<T> &
MeshBlockData<T>::PackVariablesImpl(const std::vector<int> &sparse_ids, bool coarse,
                                    PackIndexMap *map, std::vector<std::string> *key) {
  return PackListedVariables(GetAllVariables(sparse_ids), coarse, map, key);
}

// Get variables with the given names. The given name could either be a full variable
// label or a sparse base name. Optionally only extract sparse fields with a sparse id in
// the given set of sparse ids
template <typename T>
typename MeshBlockData<T>::VarLabelList
MeshBlockData<T>::GetVariablesByName(const std::vector<std::string> &names,
                                     const std::vector<int> &sparse_ids) {
  typename MeshBlockData<T>::VarLabelList var_list;
  std::unordered_set<int> sparse_ids_set(sparse_ids.begin(), sparse_ids.end());

  for (const auto &name : names) {
    const auto itr = varMap_.find(name);
    if (itr != varMap_.end()) {
      const auto &v = itr->second;
      // this name exists, add it
      var_list.Add(v, sparse_ids_set);
    } else if ((resolved_packages_ != nullptr) &&
               (resolved_packages_->SparseBaseNamePresent(name))) {
      const auto &sparse_pool = resolved_packages_->GetSparsePool(name);

      // add all sparse ids of the pool
      for (const auto iter : sparse_pool.pool()) {
        // this variable must exist, if it doesn't something is very wrong
        const auto &v = varMap_.at(MakeVarLabel(name, iter.first));
        var_list.Add(v, sparse_ids_set);
      }
    }
  }

  return var_list;
}

// From a given container, extract all variables whose Metadata matchs the all of the
// given flags (if the list of flags is empty, extract all variables), optionally only
// extracting sparse fields with an index from the given list of sparse indices
template <typename T>
typename MeshBlockData<T>::VarLabelList
MeshBlockData<T>::GetVariablesByFlag(const std::vector<MetadataFlag> &flags,
                                     bool match_all, const std::vector<int> &sparse_ids) {
  typename MeshBlockData<T>::VarLabelList var_list;
  std::unordered_set<int> sparse_ids_set(sparse_ids.begin(), sparse_ids.end());

  // let's use varMap_ here instead of varVector_ because iterating over either has O(N)
  // complexity but with varMap_ we get a sorted list
  for (const auto &pair : varMap_) {
    const auto &v = pair.second;
    // add this variable to the list if the Metadata flags match or no flags are specified
    if (flags.empty() || (match_all && v->metadata().AllFlagsSet(flags)) ||
        (!match_all && v->metadata().AnyFlagsSet(flags))) {
      var_list.Add(v, sparse_ids_set);
    }
  }

  return var_list;
}

template <typename T>
void MeshBlockData<T>::Remove(const std::string &label) {
  throw std::runtime_error("MeshBlockData<T>::Remove not yet implemented");
}

template <typename T>
void MeshBlockData<T>::SetLocalNeighborAllocated() {
#ifdef ENABLE_SPARSE
  Kokkos::Profiling::pushRegion("SetLocalNeighborAllocated");

  const auto &bval = pmy_block.lock()->pbval;
  // set local_neighbor_allocated for each variable
  for (int n = 0; n < bval->nneighbor; n++) {
    // find neighbor block
    if (bval->neighbor[n].snb.rank != Globals::my_rank) {
      continue;
    }

    auto neighbor_data = pmy_block.lock()
                             ->pmy_mesh->FindMeshBlock(bval->neighbor[n].snb.gid)
                             ->meshblock_data.Get();

    assert(varVector_.size() == neighbor_data->varVector_.size());
    for (size_t i = 0; i < varVector_.size(); ++i) {
      assert(varVector_[i]->label() == neighbor_data->varVector_[i]->label());
      if (!varVector_[i]->IsSet(Metadata::FillGhost)) {
        continue;
      }

      varVector_[i]->vbvar->local_neighbor_allocated[n] =
          neighbor_data->varVector_[i]->IsAllocated();
    }
  }

  Kokkos::Profiling::popRegion(); // SetLocalNeighborAllocated

#endif // ENABLE_SPARSE
}

template <typename T>
TaskStatus MeshBlockData<T>::SendFluxCorrection() {
  Kokkos::Profiling::pushRegion("Task_SendFluxCorrection");
  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::WithFluxes) && v->IsSet(Metadata::FillGhost)) {
      v->vbvar->SendFluxCorrection(v->IsAllocated());
    }
  }

  Kokkos::Profiling::popRegion(); // Task_SendFluxCorrection
  return TaskStatus::complete;
}

template <typename T>
TaskStatus MeshBlockData<T>::ReceiveFluxCorrection() {
  Kokkos::Profiling::pushRegion("Task_ReceiveFluxCorrection");
  int success = 0, total = 0;
  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::WithFluxes) && v->IsSet(Metadata::FillGhost)) {
      if (v->vbvar->ReceiveFluxCorrection(v->IsAllocated())) {
        success++;
      }
      total++;
    }
  }

  Kokkos::Profiling::popRegion(); // Task_ReceiveFluxCorrection
  if (success == total) return TaskStatus::complete;
  return TaskStatus::incomplete;
}

template <typename T>
void MeshBlockData<T>::SetupPersistentMPI() {
  // setup persistent MPI
  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::FillGhost)) {
      v->resetBoundary();
      v->vbvar->SetupPersistentMPI();
    }
  }
}

template <typename T>
TaskStatus MeshBlockData<T>::ReceiveBoundaryBuffers() {
  Kokkos::Profiling::pushRegion("Task_ReceiveBoundaryBuffers_MeshBlockData");
  bool ret = true;
  // receives the boundary
  for (auto &v : varVector_) {
    if (!v->mpiStatus) {
      if (v->IsSet(Metadata::FillGhost)) {
        // ret = ret & v->vbvar->ReceiveBoundaryBuffers();
        // In case we have trouble with multiple arrays causing
        // problems with task status, we should comment one line
        // above and uncomment the if block below
        v->resetBoundary();
        v->mpiStatus = v->vbvar->ReceiveBoundaryBuffers(v->IsAllocated());
        ret = (ret & v->mpiStatus);
      }
    }
  }

  Kokkos::Profiling::popRegion(); // Task_ReceiveBoundaryBuffers_MeshBlockData
  if (ret) return TaskStatus::complete;
  return TaskStatus::incomplete;
}

template <typename T>
void MeshBlockData<T>::ResetBoundaryCellVariables() {
  Kokkos::Profiling::pushRegion("ResetBoundaryCellVariables");
  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::FillGhost)) {
      v->vbvar->var_cc = v->data;
    }
  }

  Kokkos::Profiling::popRegion(); // ResetBoundaryCellVariables
}

template <typename T>
TaskStatus MeshBlockData<T>::StartReceiving(BoundaryCommSubset phase) {
  Kokkos::Profiling::pushRegion("Task_StartReceiving");

  SetLocalNeighborAllocated();

  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::FillGhost)) {
      v->resetBoundary();
      v->vbvar->StartReceiving(phase);
      v->mpiStatus = false;
    }
  }

  Kokkos::Profiling::popRegion(); // Task_StartReceiving
  return TaskStatus::complete;
}

template <typename T>
TaskStatus MeshBlockData<T>::ClearBoundary(BoundaryCommSubset phase) {
  Kokkos::Profiling::pushRegion("Task_ClearBoundary");
  for (auto &v : varVector_) {
    if (v->IsSet(Metadata::FillGhost)) {
      v->vbvar->ClearBoundary(phase);
    }
  }

  Kokkos::Profiling::popRegion(); // Task_ClearBoundary
  return TaskStatus::complete;
}

template <typename T>
void MeshBlockData<T>::ProlongateBoundaries() {
  Kokkos::Profiling::pushRegion("ProlongateBoundaries");
  // TODO(JMM): Change this upon refactor of BoundaryValues
  auto pmb = GetBlockPointer();
  pmb->pbval->ProlongateBoundaries();
  Kokkos::Profiling::popRegion();
}

template <typename T>
void MeshBlockData<T>::Print() {
  std::cout << "Variables are:\n";
  for (auto v : varVector_) {
    std::cout << " cell: " << v->info() << std::endl;
  }
  for (auto v : faceVector_) {
    std::cout << " face: " << v->info() << std::endl;
  }
}

template class MeshBlockData<double>;

} // namespace parthenon
