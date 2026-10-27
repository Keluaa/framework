// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2026 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* ParallelParticleFamily.cc                                   (C) 2000-2026 */
/*                                                                           */
/* Particle family.                                                          */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include "arcane/mesh/ParallelParticleFamily.h"

#include "arcane/utils/FatalErrorException.h"
#include "arcane/utils/NotSupportedException.h"
#include "arcane/utils/PlatformUtils.h"

#include "arcane/core/ISubDomain.h"
#include "arcane/core/IMesh.h"
#include "arcane/core/IVariableMng.h"
#include "arcane/core/Properties.h"
#include "arcane/core/ItemPrinter.h"

#include "arcane/mesh/ItemsExchangeInfo2.h"
#include "arcane/mesh/DynamicMesh.h"
#include "arcane/mesh/ItemConnectivitySelector.h"
#include "arcane/mesh/DynamicMeshKindInfos.h"

#include "arcane/accelerator/core/IAcceleratorMng.h"
#include "arcane/accelerator/core/RunQueue.h"
#include "arcane/accelerator/RunCommandLoop.h"

#include "arcane/accelerator/core/ProfileRegion.h"  // TODO: tmp

namespace ax = Arcane::Accelerator;

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace Arcane::mesh
{

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ParallelParticleFamily::
ParallelParticleFamily(IMesh* mesh, const String& name)
: IAnyParticleFamily(mesh, IK_Particle, name)
, m_particle_type_info(nullptr)
, m_particle_shared_info(nullptr)
, m_sub_domain_id(NULL_SUB_DOMAIN_ID)
, m_enable_ghost_items(false)
, m_cell_connectivity(nullptr)
{
  auto queue = subDomain()->acceleratorMng()->queue();
  setQueue(queue);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ParallelParticleFamily::
~ParallelParticleFamily()
{
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
build()
{
  ItemFamily::build();
  ItemTypeMng* itm = m_mesh->itemTypeMng();
  m_particle_type_info = itm->typeFromId(IT_NullType);
  m_sub_domain_id = subDomain()->subDomainId();

  // Temporary: to disable the hash table for uniqueId
  if (!platform::getEnvironmentVariable("ARCANE_PARTICLE_NO_UNIQUE_ID_MAP").null()) {
    pwarning() << "TEMPORARY: suppress particule uniqueId map";
    setHasUniqueIdMap(false);
  }
  else {
    bool has_unique_id_map = !m_properties->getBool("no-unique-id-map");
    _setHasUniqueIdMap(has_unique_id_map);
  }

  m_cell_connectivity = new CellConnectivity(this, mesh()->cellFamily(), "ParticleCell");

  _addConnectivitySelector(m_cell_connectivity);

  _buildConnectivitySelectors();

  // Pre-allocates a cell per particle.
  m_cell_connectivity->setPreAllocatedSize(1);

  _setSharedInfo();

  auto network = m_mesh->itemFamilyNetwork();
  if (network) {
    network->addDependency(this, mesh()->cellFamily(), m_cell_connectivity->customConnectivity(), false);
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*!
 * \brief Positions the connectivity info for a particle that has just been allocated in memory.
 */
inline void ParallelParticleFamily::
_initializeNewlyAllocatedParticle(ItemInternal* particle, Int64 uid)
{
  _allocateInfos(particle, uid, m_particle_shared_info);
  m_cell_connectivity->addConnectedItem(ItemLocalId(particle), ItemLocalId(NULL_ITEM_LOCAL_ID));
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

inline ItemInternal* ParallelParticleFamily::
_allocParticle(Int64 uid, bool& need_alloc)
{
  ItemInternal* ii = _allocOne(uid, need_alloc);

  if (!need_alloc)
    ii->setUniqueId(uid);
  else
    _initializeNewlyAllocatedParticle(ii, uid);  // TODO: on GPU

  // A particle always belongs to the sub-domain that created it
  ii->setOwner(m_sub_domain_id, m_sub_domain_id);  // TODO: on GPU
  return ii;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

inline ItemInternal* ParallelParticleFamily::
_findOrAllocParticle(Int64 uid, bool& is_alloc)
{
  ItemInternal* ii = ItemFamily::_findOrAllocOne(uid, is_alloc);
  if (is_alloc)
    _initializeNewlyAllocatedParticle(ii, uid);
  return ii;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ParticleVectorView ParallelParticleFamily::
addParticles(Int64ConstArrayView unique_ids, Int32ArrayView items)
{
  _addItems(unique_ids, items);
  return view(items);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ParticleVectorView ParallelParticleFamily::
addParticles2(Int64ConstArrayView unique_ids,
              Int32ConstArrayView owners,
              Int32ArrayView items)
{
  addItems(unique_ids, owners, items);
  return view(items);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
_setCell(ItemLocalId particle, ItemLocalId cell)
{
  m_cell_connectivity->replaceItem(particle, 0, cell);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ParticleVectorView ParallelParticleFamily::
addParticles(Int64ConstArrayView unique_ids,
             Int32ConstArrayView cells_local_id,
             Int32ArrayView items)
{
  info() << "WE ARE USING " << __PRETTY_FUNCTION__;
  _addItems(unique_ids, items);
  Integer n = items.size();
  for (Integer i = 0; i < n; ++i) {
    _setCell(ItemLocalId(items[i]), ItemLocalId(cells_local_id[i]));
  }

  return view(items);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
setParticleCell(Particle particle, Cell new_cell)
{
  _setCell(particle, new_cell);
  m_need_prepare_dump = true;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
setParticlesCell(ParticleVectorView particles, CellVectorView new_cells)
{
  Int32ConstArrayView cell_ids = new_cells.localIds();
  Int32ConstArrayView particle_ids = particles.localIds();
  for (Integer i = 0, n = particle_ids.size(); i < n; ++i)
    _setCell(ItemLocalId(particle_ids[i]), ItemLocalId(cell_ids[i]));
  m_need_prepare_dump = true;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
_addItems(Int64ConstArrayView unique_ids, Int32ArrayView items)
{
  Integer nb_item = unique_ids.size();
  if (nb_item == 0)
    return;
  preAllocate(nb_item);  // TODO: should this be on GPU?

  _allocMany(unique_ids, items, m_sub_domain_id, m_particle_shared_info);
  _allocateManyInfos(items);

  // TODO: check if still useful because ItemInternal::reinitialize() must do it
  //(*m_items_unique_id)[local_id] = uid;

  // Notify the incremental connectivities that we added an item to the source

  // TODO: have an optional list of CID as argument and set the cells here
  _resetConnectivity(items);


  // TODO: on GPU
  // m_cell_connectivity->addConnectedItem(ItemLocalId(particle), ItemLocalId(NULL_ITEM_LOCAL_ID));
  // ii->setOwner(m_sub_domain_id, m_sub_domain_id);

  // TODO: put this on GPU
  // bool need_alloc = false;
  // for (Integer i = 0; i < nb_item; ++i) {
  //   Int64 uid = unique_ids[i];
  //   ItemInternal* ii = _allocParticle(uid, need_alloc);
  //   items[i] = ii->localId();
  // }

  m_need_prepare_dump = true;
  _printInfos(nb_item);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
_resetConnectivity(Int32ConstArrayView items)
{
  auto queue = m_queue;
  if (ax::getPointerAccessibility(queue, items.data()) == ax::ePointerAccessibility::No) {
    queue = getHostQueue();
    // TODO: changer en "debug"?
    warning() << "Cannot run ParallelParticleFamily::_resetConnectivity using the accelerator as 'items' is inaccessible on the device";
  }
  else {
    info() << "ParallelParticleFamily::_resetConnectivity is running on the device";  // TODO : tmp
  }

  ax::ProfileRegion reg(queue, "_resetConnectivity");

  auto cmd = ax::makeCommand(queue);

  MutableIndexedParticleCellConnectivityView connectivity(itemFamily());
  auto in_lid = SmallSpan<const Int32>(items);

  // TODO: we are missing a call to `addConnectedItem(lid, NULL_ITEM_LOCAL_ID)` which ALSO
  //   adds 1 to the number of connected items. Here it stays 0 for newly allocated particles.
  cmd << RUNCOMMAND_LOOP1(i, in_lid.size()) {
    ParticleLocalId lid(in_lid[i]);
    CellLocalId cid(NULL_ITEM_LOCAL_ID);
    connectivity.setCellId(lid, cid);
  };

  // {
  //   ax::ProfileRegion reg(queue, "Force update connectivity");
  //   for (Int32 raw_lid : items) {
  //     ItemLocalId lid(raw_lid);
  //     Int32 nb_connections = m_cell_connectivity->trueCustomConnectivity()->nbConnectedItem(lid);
  //     if (nb_connections == 0) {
  //       m_cell_connectivity->addConnectedItem(lid, ItemLocalId(NULL_ITEM_LOCAL_ID));
  //     } else {
  //       m_cell_connectivity->replaceItem(lid, 0, ItemLocalId(NULL_ITEM_LOCAL_ID));
  //     }
  //   }
  // }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
addItems(Int64ConstArrayView unique_ids, Int32ConstArrayView owners, Int32ArrayView items)
{
  Integer nb_item = unique_ids.size();
  if (nb_item == 0)
    return;
  preAllocate(nb_item);
  // The _findOrAlloc method requires the uniqueId hash map.
  if (!hasUniqueIdMap())
    ARCANE_FATAL("Can not add particles with owners when hasUniqueIdMap()==false family={0}",
                 name());

  info() << "WE ARE USING " << __PRETTY_FUNCTION__;
  bool need_alloc = false;
  for (Integer i = 0; i < nb_item; ++i) {
    Int64 uid = unique_ids[i];
    ItemInternal* ii = _findOrAllocParticle(uid, need_alloc);
    ii->setOwner(owners[i], m_sub_domain_id);

    items[i] = ii->localId();
  }

  m_need_prepare_dump = true;
  _printInfos(nb_item);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
exchangeParticles()
{
  ItemsExchangeInfo2 ex(this);
  ex.computeExchangeItems();
  ex.computeExchangeInfos();
  ex.prepareToSend();
  ex.processExchange();
  ex.removeSentItems();
  ex.readAndAllocItems();
  notifyItemsOwnerChanged();
  endUpdate();
  ex.readGroups();
  ex.readVariables();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
_printInfos(Integer nb_added)
{
  ARCANE_UNUSED(nb_added);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
internalRemoveItems(Int32ConstArrayView local_ids, bool keep_ghost, ax::RunQueue* queue)
{
  ARCANE_UNUSED(keep_ghost);

  m_queue.setAsync(false);  // TODO: tmp
  ax::ProfileRegion reg(m_queue, "internalRemoveItems");

  info() << "removing " << local_ids.size();

  InternalConnectivityPolicy policy = mesh()->_connectivityPolicy();

  // Note that this loop is not useful for the moment if we
  // are not using the new connectivities.
  // It will be if we want to have inverse connectivities
  // cell->particle.
  bool want_nullify_cell = (policy != InternalConnectivityPolicy::Legacy);

  auto network = m_mesh->itemFamilyNetwork();
  if (network) {
    ARCANE_THROW(NotSupportedException, "ItemFamily networks not supported by ParallelParticleFamily");
  }

  if (want_nullify_cell) {
    ax::ProfileRegion reg(m_queue, "nullify cell");
    MutableIndexedParticleCellConnectivityView mut_cell_connectivity(itemFamily());

    auto cmd = ax::makeCommand(m_queue);
    auto in_local_ids = SmallSpan<const Int32>(local_ids);

    ARCCORE_CHECK_ACCESSIBLE_POINTER_ALWAYS(m_queue, local_ids.data());

    cmd << RUNCOMMAND_LOOP1(i, in_local_ids.size()) {
      ParticleLocalId pid(in_local_ids[i]);
      CellLocalId null_cid(NULL_ITEM_LOCAL_ID);
      mut_cell_connectivity.setCellId(pid, null_cid);
    };
  }

  _removeMany(local_ids);
  m_queue.barrier();

  m_need_prepare_dump = true;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
removeParticles(Int32ConstArrayView items_local_id)
{
  internalRemoveItems(items_local_id, false);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
prepareForDump()
{
  Integer nb_item = nbItem();
  info(4) << "ParallelParticleFamily::prepareForDump: " << name()
          << " n=" << nb_item;
  ItemFamily::prepareForDump();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
readFromDump()
{
  ItemFamily::readFromDump();
  // Updates the shared_info because it may change after a reload
  _setSharedInfo();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
preAllocate(Integer nb_item)
{
  Integer nb_hash = itemsMap().nbBucket();
  Integer wanted_size = 2 * (nb_item + nbItem());
  if (nb_hash < wanted_size)
    itemsMap().resize(wanted_size, true);
  _preAllocate(nb_item + nbItem(), true);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
setHasUniqueIdMap(bool v)
{
  _setHasUniqueIdMap(v);
  m_properties->setBool("no-unique-id-map", !v);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

bool ParallelParticleFamily::
hasUniqueIdMap() const
{
  return _infos().hasUniqueIdMap();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
_setSharedInfo()
{
  m_particle_shared_info = _findSharedInfo(m_particle_type_info);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
checkValidConnectivity()
{
  ItemFamily::checkValidConnectivity();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void ParallelParticleFamily::
removeNeedRemoveMarkedItems()
{
  if (getEnableGhostItems()) {
    UniqueArray<Integer> lids_to_remove;
    lids_to_remove.reserve(1000);

    ItemInternalMap& particle_map = itemsMap();
    particle_map.eachItem([&](Item item) {
      Integer f = item.itemBase().flags();
      if (f & ItemFlags::II_NeedRemove) {
        f &= ~ItemFlags::II_NeedRemove;
        item.mutableItemBase().setFlags(f);
        lids_to_remove.add(item.localId());
      }
    });

    info() << "Number of particles of family " << name() << " to remove: " << lids_to_remove.size();
    if (lids_to_remove.size() > 0)
      removeParticles(lids_to_remove);
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

} // End namespace Arcane::mesh

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
