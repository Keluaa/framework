// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2026 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* DynamicMeshKindInfos.cc                                     (C) 2000-2025 */
/*                                                                           */
/* Mesh info for a given entity kind.                                        */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include "arcane/core/ArcaneTypes.h"
#include "arcane/utils/FatalErrorException.h"
#include "arcane/utils/PlatformUtils.h"
#include "arcane/utils/NotImplementedException.h"
#include "arcane/utils/StringBuilder.h"

#include "arcane/core/IMesh.h"
#include "arcane/core/IItemFamily.h"
#include "arcane/core/ItemEnumerator.h"
#include "arcane/core/IParallelMng.h"
#include "arcane/core/ItemPrinter.h"
#include "arcane/core/VariableTypes.h"
#include "arcane/core/Timer.h"
#include "arcane/core/IItemInternalSortFunction.h"
#include "arcane/core/ItemFamilyCompactInfos.h"
#include "arcane/core/IMeshCompacter.h"
#include "arcane/core/MeshPartInfo.h"
#include "arcane/core/ItemFamilyItemListChangedEventArgs.h"

#include "arcane/mesh/DynamicMeshKindInfos.h"
#include "arcane/mesh/ItemFamily.h"
#include "arcane/mesh/ItemSharedInfoList.h"

#include "arcane/accelerator/core/IAcceleratorMng.h"
#include "arcane/accelerator/core/RunQueue.h"
#include "arcane/accelerator/RunCommandLoop.h"
#include "arcane/accelerator/core/Memory.h"
#include "arccore/common/MemoryUtils.h"

#include "arcane/accelerator/core/ProfileRegion.h"
#include "arccore/common/accelerator/Runner.h"

#include <algorithm>
#include <set>

// #define ARCANE_DEBUG_MESH

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace Arcane::mesh
{

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

DynamicMeshKindInfos::
DynamicMeshKindInfos(IMesh* mesh, eItemKind kind, const String& kind_name)
: TraceAccessor(mesh->traceMng())
, m_mesh(mesh)
, m_item_family(nullptr)
, m_kind(kind)
, m_kind_name(kind_name)
, m_nb_item(0)
, m_is_verbose(false)
, m_use_new_finalize(true)
, m_is_first_finalize(true)
, m_has_unique_id_map(true)
, m_compact_infos(nullptr)
, m_item_internals_buffer(new MultiBufferT<ItemInternal>(5000))
{
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

DynamicMeshKindInfos::
~DynamicMeshKindInfos()
{
  delete m_item_internals_buffer;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
build()
{
  // Create the group containing all entities of this kind.
  {
    // If the name of this group changes, it must be updated
    // the equivalent name in VariableInfo.cc
    StringBuilder str("All");
    str += m_kind_name;
    str += "s";
    m_all_group_name = str.toString();
    m_all_group = m_item_family->createGroup(m_all_group_name);
    m_all_group.internal()->setIsAllItems();
    m_all_group.setLocalToSubDomain(true);
  }
  if (!platform::getEnvironmentVariable("ARCANE_OLD_FINALIZE").null()) {
    m_use_new_finalize = false;
    pwarning() << "USE OLD FINALIZE";
  }
  m_is_first_finalize = true;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*!
 */
void DynamicMeshKindInfos::
finalizeMeshChanged()
{
  Trace::Setter mci(traceMng(), "Mesh");

  ItemGroupImpl& impl = *m_all_group.internal();
  Integer internal_size = m_internals.size();

  {
    info(5) << A_FUNCNAME << " " << m_kind_name
            << " nb_item=" << m_nb_item
            << " internal size= " << internal_size
            << " max_local_id=" << maxUsedLocalId();
#if 0
    for( Integer i=0; i<internal_size; ++i ){
      ItemInternal* item = m_internals[i];
      info() << "Item " << i << " lib=" << item->localId()
             << " uid=" << item->uniqueId() << " owner=" << item->owner()
             << " item=" << item
             << " removed?=" << item->isSuppressed();
    }
#endif
  }

  Integer nb_added_items = m_added_items.size();
  Integer nb_removed_items = m_removed_items.size();
  bool test_mode = m_use_new_finalize;

  info(5) << A_FUNCNAME << " " << m_kind_name
          << " nb_item " << m_nb_item
          << " internal size " << internal_size
          << " nb_add=" << nb_added_items
          << " nb_remove=" << nb_removed_items;

  // GG: switched to OLD in 11/2018. If everything is OK on the IFPEN side
  // we will be able to permanently remove this.
#if OLD
  IParallelMng* pm = m_mesh->parallelMng();
  // In sequential mode, position the m_owner field of each entity.
  // TODO: should not be done here, but in the mesh reader...
  if (!pm->isParallel() && nb_added_items != 0) {
    const Integer sid = pm->commRank();
    for (Integer i = 0; i < nb_added_items; ++i)
      m_internals[m_added_items[i]]->setOwner(sid, sid);
  }
#endif
  // (HP) TODO: We can probably optimize the reorganization via
  // addItems, removeItems, changeIds...
  // The problem is that here we must not impact the calculated sub-groups
  impl.beginTransaction();
  if (nb_added_items == 0 && nb_removed_items != 0 && test_mode) {
    impl.removeItems(m_removed_items, true);
  }
  else if (nb_added_items != 0 && nb_removed_items == 0 && test_mode) {
    impl.addItems(m_added_items, true);
  }
  else {
    impl.removeAddItems(m_removed_items, m_added_items, true);
  }
  impl.endTransaction();

  // restore integrity of allItems group after this aggressive modification
  impl.checkNeedUpdate();

#if defined(ARCANE_DEBUG_MESH)
  if (arcaneIsCheck())
    m_all_group.checkValid();
#endif
  // We must also change the groups of the owned entities
  // (To be done only in parallel?)
  m_item_family->notifyItemsOwnerChanged();

#if defined(ARCANE_DEBUG_MESH)
  if (arcaneIsCheck())
    checkValid();
#endif

  m_removed_items.clear();
  m_added_items.clear();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
itemsUniqueIdToLocalId(ArrayView<Int64> ids, bool do_fatal) const
{
  if (!m_has_unique_id_map)
    _badUniqueIdMap();
  if (!arcaneIsCheck()) {
    if (do_fatal) {
      for (Integer i = 0, s = ids.size(); i < s; ++i) {
        Int64 unique_id = ids[i];
        ids[i] = m_items_map.findLocalId(unique_id);
      }
    }
    else {
      for (Integer i = 0, s = ids.size(); i < s; ++i) {
        Int64 unique_id = ids[i];
        ids[i] = m_items_map.tryFindLocalId(unique_id);
      }
    }
  }
  else {
    Integer nb_error = 0;
    for (Integer i = 0, s = ids.size(); i < s; ++i) {
      Int64 unique_id = ids[i];
      ids[i] = m_items_map.tryFindLocalId(unique_id);
      if ((ids[i] == NULL_ITEM_LOCAL_ID) && do_fatal) {
        if (nb_error < 10) {
          error() << "DynamicMeshKindInfos::itemsUniqueIdToLocalId() can't find "
                  << "entity " << m_kind_name << " with global id "
                  << unique_id << " in the subdomain.";
        }
        ++nb_error;
      }
    }
    if (nb_error != 0) {
      if (do_fatal)
        ARCANE_FATAL("{0} entities not found", nb_error);
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
itemsUniqueIdToLocalId(Int32ArrayView local_ids,
                       Int64ConstArrayView unique_ids, bool do_fatal) const
{
  if (!m_has_unique_id_map)
    _badUniqueIdMap();
  if (!arcaneIsCheck()) {
    if (do_fatal) {
      for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
        Int64 unique_id = unique_ids[i];
        local_ids[i] = (unique_id == NULL_ITEM_UNIQUE_ID) ? NULL_ITEM_LOCAL_ID : m_items_map.findLocalId(unique_id);
      }
    }
    else {
      for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
        Int64 unique_id = unique_ids[i];
        local_ids[i] = m_items_map.tryFindLocalId(unique_id);
      }
    }
  }
  else {
    Integer nb_error = 0;
    for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
      Int64 unique_id = unique_ids[i];
      local_ids[i] = m_items_map.tryFindLocalId(unique_id);
      if ((local_ids[i] == NULL_ITEM_LOCAL_ID) && do_fatal && unique_id != NULL_ITEM_UNIQUE_ID) {
        if (nb_error < 10) {
          error() << "DynamicMeshKindInfos::itemsUniqueIdToLocalId() can't find "
                  << "entity " << m_kind_name << " with global id "
                  << unique_id << " in the subdomain.";
        }
        ++nb_error;
      }
    }
    if (nb_error != 0) {
      if (do_fatal)
        ARCANE_FATAL("{0} entities not found", nb_error);
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
itemsUniqueIdToLocalId(Int32ArrayView local_ids,
                       ConstArrayView<ItemUniqueId> unique_ids, bool do_fatal) const
{
  if (!m_has_unique_id_map)
    _badUniqueIdMap();
  if (!arcaneIsCheck()) {
    if (do_fatal) {
      for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
        Int64 unique_id = unique_ids[i];
        local_ids[i] = m_items_map.findLocalId(unique_id);
      }
    }
    else {
      for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
        Int64 unique_id = unique_ids[i];
        local_ids[i] = m_items_map.tryFindLocalId(unique_id);
      }
    }
  }
  else {
    Integer nb_error = 0;
    for (Integer i = 0, s = unique_ids.size(); i < s; ++i) {
      Int64 unique_id = unique_ids[i];
      local_ids[i] = m_items_map.tryFindLocalId(unique_id);
      if ((local_ids[i] == NULL_ITEM_LOCAL_ID) && do_fatal) {
        if (nb_error < 10) {
          error() << "DynamicMeshKindInfos::itemsUniqueIdToLocalId() can't find "
                  << "entity " << m_kind_name << " with global id "
                  << unique_id << " in the subdomain.";
        }
        ++nb_error;
      }
    }
    if (nb_error != 0) {
      if (do_fatal)
        ARCANE_FATAL("{0} entities not found", nb_error);
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
prepareForDump()
{
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
readFromDump()
{
  m_all_group = m_mesh->findGroup(m_all_group_name);
  // Remove all entities
  m_nb_item = 0;
  m_internals.clear();
  _updateItemSharedInfoInternalView();
  m_free_internals.clear();
  m_items_map.clear();
  delete m_item_internals_buffer;
  m_item_internals_buffer = new MultiBufferT<ItemInternal>(5000);
  m_free_internals_in_multi_buffer.clear();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
checkValid()
{
  Int32 sid = m_mesh->meshPartInfo().partRank();

  debug() << "DynamicMeshKindInfos::checkValid(): " << m_kind_name;

  Integer nb_killed_item = 0;
  Integer nb_internal = m_internals.size();
  Integer nb_error = 0;

  // First, item->localId() must correspond to the index
  // in the m_internal array
  for (Integer i = 0; i < nb_internal; ++i) {
    ItemInternal* item = m_internals[i];
    if (item->localId() != i && nb_error < 10) {
      error() << "The local id (" << item->localId() << ") of the entity "
              << m_kind_name << ':' << item->uniqueId() << " is not "
              << "consistent with its internal value (" << i << ")";
      ++nb_error;
    }
    if (item->isSuppressed()) {
      ++nb_killed_item;
      continue;
    }
    if (item->owner() == NULL_SUB_DOMAIN_ID && nb_error < 10) {
      error() << "entity " << m_kind_name << ":lid=" << item->localId()
              << ":uid=" << item->uniqueId() << "  belongs to no subdomain";
      ++nb_error;
    }

    if (item->owner() != sid && item->isOwn() && nb_error < 10) {
      error() << "entity " << m_kind_name << ":lid=" << item->localId()
              << ":uid=" << item->uniqueId() << "  incoherence between isOwn() and owner()"
              << " " << item->owner() << ' ' << sid;
      ++nb_error;
    }
  }
  Integer nb_free = m_free_internals.size();
  if (nb_killed_item != nb_free) {
    error() << "DynamicMeshKindInfos::checkValid(): " << m_kind_name
            << ": incoherence between killed and free entities"
            << " free=" << nb_free
            << " killed=" << nb_killed_item
            << " internal=" << nb_internal
            << " count=" << m_nb_item;
    ++nb_error;
  }

  if (nb_error != 0)
    ARCANE_FATAL("Internal error in the mesh structure mesh={0} part={1}",
                 m_mesh->name(), sid);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#ifdef OLD
class ItemParticleCompareWithSuppression
{
 public:

  bool operator()(const ItemInternal* item1, const ItemInternal* item2) const
  {
    // Deleted entities must be placed at the end of the list
    //cout << "Compare: " << item1->uniqueId() << " " << item2->uniqueId() << '\n';
    bool s1 = item1->isSuppressed();
    bool s2 = item2->isSuppressed();
    if (s1 && !s2)
      return false;
    if (!s1 && s2)
      return true;
    Int64 uid1 = Particle(item1).cell(0)->uniqueId();
    Int64 uid2 = Particle(item2).cell(0)->uniqueId();
    if (uid1 == uid2)
      return item1->uniqueId() < item2->uniqueId();
    return uid1 < uid2;
  }
};
#endif

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

class ItemLocalIdAndUniqueId
{
 public:

  bool operator()(const ItemLocalIdAndUniqueId& item1, const ItemLocalIdAndUniqueId item2) const
  {
    return item1.m_unique_id < item2.m_unique_id;
  }
  Integer m_local_id;
  Integer m_unique_id;
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

Int32ConstArrayView DynamicMeshKindInfos::
oldToNewLocalIds() const
{
  if (!m_compact_infos)
    return Int32ConstArrayView();
  return m_compact_infos->oldToNewLocalIds();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

Int32ConstArrayView DynamicMeshKindInfos::
newToOldLocalIds() const
{
  if (!m_compact_infos)
    return Int32ConstArrayView();
  return m_compact_infos->newToOldLocalIds();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*!
  \brief Calculates the new local IDs of the entities so that they are consecutive.

  If \a do_sort is true, the entities are sorted such that their unique id
  and local id are in ascending order.

  Before calling this function, the mesh must be valid and finalized

  After calling this function, the oldToNewLocalIds() and
  newToOldLocalIds() arrays are populated and
  contain the conversions between new and
  old local numbers.
*/
void DynamicMeshKindInfos::
beginCompactItems(ItemFamilyCompactInfos& compact_infos)
{
  m_compact_infos = &compact_infos;
  bool do_sort = compact_infos.compacter()->isSorted();

  if (arcaneIsCheck())
    checkValid();

  Integer nb_internal = m_internals.size();
  info(4) << m_kind_name << " beginCompactItems() Family Internal Size=" << nb_internal
          << " max_id=" << maxUsedLocalId()
          << " nb_item=" << m_nb_item
          << " do_sort=" << do_sort;

  if (nb_internal == 0)
    return;

  UniqueArray<Int32> old_to_new_local_ids(maxUsedLocalId());
  UniqueArray<Int32> new_to_old_local_ids(m_nb_item);
  // m_new_to_old_local_ids[lid] contient l'ancien numéro local de l'entité dont le nouveau numéro local est \a lid
  // m_old_to_new_local_ids[lid] contient le nouveau numéro local de l'entité dont l'ancien numéro local est \a lid
  old_to_new_local_ids.fill(NULL_ITEM_LOCAL_ID);
  // après compactage, il ne peut y avoir que m_nb_item items actifs (et forcément en tête de numérotation)
  new_to_old_local_ids.fill(NULL_ITEM_LOCAL_ID);

  if (do_sort) {
    // Fait une copie temporaire des ItemInternal pour le tri
    UniqueArray<ItemInternal*> items(m_internals);

    debug() << "BEGIN = " << items.data() << " " << m_internals.data()
            << " S1=" << items.size() << " S2=" << m_internals.size();
    const bool print_infos = false;
    {
      ITimerMng* tm = m_mesh->parallelMng()->timerMng();
      Timer timer(tm, "DynamicMeshKindInfos::beginCompactItems()", Timer::TimerReal);
      IItemInternalSortFunction* sort_func = m_item_family->itemSortFunction();
      if (print_infos) {
        for (Integer i = 0, is = items.size(); i < is; ++i) {
          info() << "Before Sort: " << i << " uid=" << items[i]->uniqueId()
                 << " lid=" << items[i]->localId()
                 << " destroyed?=" << items[i]->isSuppressed();
        }
      }
      {
        Timer::Sentry sentry(&timer);
        sort_func->sortItems(items);
      }
      info(4) << "Temps pour trier les entités <" << m_kind_name << "> "
              << timer.lastActivationTime()
              << " sort_func_name=" << sort_func->name();
      info(4) << "Sort Infos: size=" << items.size() << " nb_item=" << m_nb_item;
      if (print_infos) {
        for (Integer i = 0, n = items.size(); i < n; ++i) {
          info() << "After Sort: " << i << " uid=" << items[i]->uniqueId()
                 << " lid=" << items[i]->localId()
                 << " destroyed?=" << items[i]->isSuppressed();
        }
      }
    }

#ifdef ARCANE_TEST_ADD_MESH
    typedef std::set<ItemInternal*, ItemCompare> ItemSet;
    ItemSet items_set;
    items_set.insert(m_internals.begin(), m_internals.end());
    UniqueArray<ItemInternal*> items(m_internals.size());
    {
      ItemSet::const_iterator b = items_set.begin();
      ItemSet::const_iterator e = items_set.end();
      for (Integer z = 0; b != e; ++b, ++z)
        items[z] = *b;
    }
#endif

    for (Integer i = 0, n = m_nb_item; i < n; ++i) {
      Integer current_local_id = items[i]->localId();
      //info() << "Item: " << " uid=" << items[i]->uniqueId() << " lid="
      //<< current_local_id << " newid=" << i;
      old_to_new_local_ids[current_local_id] = i;
      new_to_old_local_ids[i] = current_local_id;
    }
  }
  else {
    const bool use_old = true;
    if (use_old) {
      info(4) << "USE_OLD_METHOD";
      Int32ConstArrayView existing_items = m_all_group.internal()->itemsLocalId();

      for (Integer i = 0; i < m_nb_item; ++i) {
        Integer current_local_id = existing_items[i];
        old_to_new_local_ids[current_local_id] = i;
        new_to_old_local_ids[i] = current_local_id;
      }

      // Place les éléments détruits en fin de liste et vérifie
      // que le nombre de trous + le nombre d'entités est égal à #nb_internal
      {
        Integer old_index = m_nb_item;
        Integer total_nb = maxUsedLocalId();
        for (Integer i = 0, n = total_nb; i < n; ++i) {
          //bool is_destroyed = false;
          if (old_to_new_local_ids[i] == NULL_ITEM_ID) {
            //is_destroyed = true;
            old_to_new_local_ids[i] = old_index;
            //m_new_to_old_local_ids[old_index] = i;
            ++old_index;
          }
          //debug() << "OLD_TO_NEW i=" << i
          //        << " old=" << m_old_to_new_local_ids[i]
          //        << " destroyed=" << is_destroyed;
        }
        if (old_index != total_nb) {
          ARCANE_FATAL("Family '{0}' bad indices: expected={1} found={2} (nb_internal={3})",
                       itemFamily()->name(), old_index, total_nb, nb_internal);
        }
      }
    }
    else {
      // Compactage sans triage, ce qui revient à déplacer les trous à la fin

      // Place les éléments détruits en fin de liste et vérifie
      // que l'on a bien tout rangé (on conserve l'ordre relatif des items)
      Integer new_index = 0;
      Integer free_index = m_nb_item;
      for (Integer i = 0, n = maxUsedLocalId(); i < n; ++i) {
        if (m_internals[i]->isSuppressed()) {
          old_to_new_local_ids[i] = free_index;
          // m_new_to_old_local_ids[free_index] = i; // non utilisé quand m_new_to_old_local_ids restreint à m_nb_item
          ++free_index;
        }
        else {
          old_to_new_local_ids[i] = new_index;
          new_to_old_local_ids[new_index] = i;
          ++new_index;
        }
        //debug() << "OLD_TO_NEW i=" << i
        //        << " old=" << m_old_to_new_local_ids[i]
        //        << " destroyed=" << m_internals[i]->isSuppressed();
      }
      if (new_index != m_nb_item) {
        ARCANE_FATAL("Family '{0}' bad indices: expected={1} found={2}",
                     itemFamily()->name(), m_nb_item, new_index);
      }
    }
  }
  compact_infos.setOldToNewLocalIds(std::move(old_to_new_local_ids));
  compact_infos.setNewToOldLocalIds(std::move(new_to_old_local_ids));
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
finishCompactItems(ItemFamilyCompactInfos& compact_infos)
{
  if (arcaneIsCheck())
    checkValid();

  Int32ConstArrayView old_to_new_local_ids = compact_infos.oldToNewLocalIds();
  Int32ConstArrayView new_to_old_local_ids = compact_infos.newToOldLocalIds();

  Integer nb_internal = m_internals.size();

  info(4) << m_kind_name << " Family Compression: nb_item=" << m_nb_item
          << " nb_internal=" << nb_internal
          << " buf_size=" << m_item_internals_buffer->bufferSize()
          << " nb_allocated_buf=" << m_item_internals_buffer->nbAllocatedBuffer()
          << " nb_free" << m_free_internals_in_multi_buffer.size();

  if (nb_internal == 0)
    return;

  // Finalise la réorganisation des structures après un tri
  // 1. Il faut mettre à jour la structure m_items_map pour référencer
  // le nouveau local_id
  // 2. Il faut recopier les valeurs de chaque ItemInternal pour qu'il
  // soit bien placé dans la nouvelle numérotation.
  // IMPORTANT: Cette opération doit toujours être la dernière car ensuite
  // on perd la relation entre les anciens local_ids et les nouveaux à
  // travers cette structure
  m_items_map._changeLocalIds(m_internals, old_to_new_local_ids);

  if (m_is_verbose) {
    info() << "DumpItemsBefore:";
    _dumpList();
  }

  // Pour l'instant, utilise un tableau temporaire
  // TODO: tableau a supprimer
  UniqueArray<ItemInternal> new_items(m_nb_item);
  UniqueArray<Int64> new_uids(m_nb_item);
  UniqueArray<Int32> new_owners(m_nb_item);
  UniqueArray<Int32> new_flags(m_nb_item);
  UniqueArray<Int16> new_typeids(m_nb_item);

  for (Integer i = 0, n = m_nb_item; i < n; ++i) {
    ItemInternal* old_item = m_internals[new_to_old_local_ids[i]];
    new_uids[i] = old_item->uniqueId().asInt64();
    new_owners[i] = old_item->owner();
    new_flags[i] = old_item->flags();
    new_typeids[i] = old_item->typeId();
    new_items[i] = *old_item;
  }

  Integer nb_error = 0;
  Int32 sid = m_mesh->meshPartInfo().partRank();

  for (Integer i = 0; i < m_nb_item; ++i) {
    ItemInternal* ii = m_internals[i];
    *ii = new_items[i];
    ii->setLocalId(i);
    ii->setFlags(new_flags[i]);
    ii->setOwner(new_owners[i], sid);
    ii->setUniqueId(new_uids[i]);
    ItemSharedInfo* isi = ItemInternalCompatibility::_getSharedInfo(ii);
    ii->_setSharedInfo(isi, ItemTypeId(new_typeids[i]));
    // L'entité est marqué comme créée
    //_setAdded(ii);
#ifdef ARCANE_CHECK
    if (ii->isSuppressed()) {
      if (nb_error < 10)
        error() << "Entity deleted from the list of created entities "
                << ItemPrinter(ii) << " index=" << i;
      ++nb_error;
    }
#endif /* ARCANE_CHECK */
  }

  // Il faut remplir les m_free_internals_in_multi_buffer avec les entités supprimées
  // pour qu'elles puissent être réutilisées sinon on va réallouer
  // des blocs dans m_item_internals_buffer à chaque recréation des
  // entités
  for (Integer i = m_nb_item, n = m_internals.size(); i < n; ++i) {
    // Empile dans l'ordre inverse pour que allocOne()
    // dépile dans l'ordre des m_internals et profite des
    // effets de cache.
    ItemInternal* removed_item = m_internals[m_nb_item + n - (i + 1)];
    m_free_internals_in_multi_buffer.add(removed_item);
  }

  // Retaillage optimal des tableaux
  m_internals.resize(m_nb_item);
  _updateItemSharedInfoInternalView();
  m_free_internals.clear();

  if (m_is_verbose) {
    info() << "DumpItemsAfter:";
    _dumpList();
  }

  if (nb_error != 0)
    ARCANE_FATAL("Error in compacting nb_error={0}", nb_error);

  debug() << "Compression: old=" << nb_internal << " new=" << m_nb_item
          << " internal=" << m_internals.size()
          << " free=" << m_free_internals.size();

  // Vide les tableaux de 'compact_infos' car ils ne sont plus valides.
  compact_infos.clear();

  m_compact_infos = nullptr;
  finalizeMeshChanged();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace
{
  class _Inverse
  {
   public:

    bool operator()(Int32 i1, Int32 i2) const
    {
      return i1 > i2;
    }
  };
} // namespace

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
clear()
{
  Int32ConstArrayView items_id = m_all_group.internal()->itemsLocalId();

  // Pour réutiliser au moins les m_free_internals afin
  // d'avoir des localId() consécutifs, il faut trier
  // les ids par ordre décroissant.
  Int32UniqueArray ids(items_id);
  std::sort(std::begin(ids), std::end(ids), _Inverse());

  removeMany(ids);
  //printFreeInternals(10000);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
printFreeInternals(Integer max_print)
{
  Integer nb_free = m_free_internals.size();
  max_print = math::min(nb_free, max_print);
  for (Integer i = 0; i < max_print; ++i) {
    Integer pos = (nb_free - 1) - i;
    Int32 index = m_free_internals[pos];
    ItemInternal* iitem = m_internals[index];
    info() << "I=" << i << " pos=" << pos << " index=" << index
           << " local_id=" << iitem->localId();
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
removeMany(Int32ConstArrayView local_ids)
{
  Integer nb_item = local_ids.size();

  Accelerator::RunQueue queue = m_item_family->getQueue();
  if (Accelerator::getPointerAccessibility(queue, local_ids.data()) == Accelerator::ePointerAccessibility::No) {
    queue = Accelerator::RunQueue();
    // TODO: changer en "debug"?
    warning() << "Cannot run DynamicMeshKindInfos::removeMany using the accelerator as 'local_ids' is inaccessible on the device";
  }
  Accelerator::ProfileRegion reg(queue, "removeMany");  // TODO: tmp

  if (m_has_unique_id_map) {
    if (!queue.isNull()) {
      ARCANE_THROW(NotSupportedException, "Unique ID hash maps are unsupported on accelerator");
    }

    Int64ArrayView uids = *(m_item_family->uniqueIds());

    for (Integer i = 0; i < nb_item; ++i) {
      Int32 lid = local_ids[i];
      ItemInternal* item = m_internals[lid];
#ifdef ARCANE_CHECK
      _checkActiveItem(item);
#endif
      _setSuppressed(item);
      if (uids[lid] != NULL_ITEM_UNIQUE_ID)
        m_items_map.remove(uids[lid]);
    }
  }
  else if (!queue.isNull()) {
    Accelerator::ProfileRegion reg(queue, "update flags");  // TODO: tmp
    auto cmd = Accelerator::makeCommand(queue);
    auto in_local_ids = SmallSpan<const Int32>(local_ids);
    auto inout_item_flags = SmallSpan<Int32>(m_common_item_shared_info->m_flags);

    ARCCORE_CHECK_ACCESSIBLE_POINTER_ALWAYS(queue, in_local_ids.data());
    ARCCORE_CHECK_ACCESSIBLE_POINTER_ALWAYS(queue, inout_item_flags.data());

    cmd << RUNCOMMAND_LOOP1(i, nb_item) {
      ItemLocalId lid(in_local_ids[i]);
      Int32 f = inout_item_flags[lid];
      f &= ~ItemFlags::II_Added;
      f |= ItemFlags::II_Suppressed;
      inout_item_flags[lid] = f;
    };
  }
  else {
    for (Integer i = 0; i < nb_item; ++i) {
      Int32 lid = local_ids[i];
      ItemInternal* item = m_internals[lid];
#ifdef ARCANE_CHECK
      _checkActiveItem(item);
#endif
      _setSuppressed(item);
    }
  }

  m_nb_item -= nb_item;

  Integer nb_removed = m_removed_items.size();
  Integer nb_free = m_free_internals.size();
  m_removed_items.resize(nb_removed + nb_item);
  m_free_internals.resize(nb_free + nb_item);

  if (!queue.isNull()) {
    Accelerator::ProfileRegion reg(queue, "copy");  // TODO: tmp
    queue.barrier();  // TODO: if resizes where async and placed on this queue, we could remove this barrier
                      // or maybe it is safe to do so without the barrier
    MemoryUtils::copy(m_removed_items.smallSpan().subSpan(nb_removed, nb_item), SmallSpan<const Int32>(local_ids), &queue);
    MemoryUtils::copy(m_free_internals.smallSpan().subSpan(nb_free, nb_item), SmallSpan<const Int32>(local_ids), &queue);
  }
  else {
    memcpy(m_removed_items.data() + nb_removed, local_ids.data(), sizeof(Int32) * nb_item);
    memcpy(m_free_internals.data() + nb_free, local_ids.data(), sizeof(Int32) * nb_item);
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

// TODO: tmp
template<size_t N>
void whoIsAvailable(TraceMessage&& msg, Accelerator::RunQueue* queue, const std::array<std::tuple<std::string_view, const void*, Int32>, N>& arrays)
{
  msg << "Accessibility of " << N << " arrays on " << queue->executionPolicy() << ":";
  for (auto [name, ptr, size] : arrays) {
    auto accessibility = Accelerator::getPointerAccessibility(queue, ptr);
    msg << "\n - " << name << " (" << ptr << ", " << size << " elements) : " <<
      (accessibility == Accelerator::ePointerAccessibility::No  ? "No"  :
       accessibility == Accelerator::ePointerAccessibility::Yes ? "Yes" : "Unknown");
  }
}

void DynamicMeshKindInfos::
allocManyAndInitialize(Int64ConstArrayView unique_ids,
                       Int32ArrayView local_ids,
                       Int32 owner_id, Int32 current_sub_domain_id,
                       ItemSharedInfoWithType* isi)
{
  // TODO: move the queue choice in the `cmd` region, after removing the ProfileRegions
  Accelerator::RunQueue queue = m_item_family->getQueue();
  if (Accelerator::getPointerAccessibility(queue, unique_ids.data()) == Accelerator::ePointerAccessibility::No) {
    queue = m_item_family->getHostQueue();
    // TODO: changer en "debug"?
    warning() << "Cannot run DynamicMeshKindInfos::allocManyusing the accelerator as 'unique_ids' is inaccessible on the device";
  }
  else {
    info() << "DynamicMeshKindInfos::allocMany is running on the device";  // TODO: tmp
  }

  Accelerator::ProfileRegion reg(queue, "allocMany");  // TODO: tmp

  // TODO: as optional arguments
  ItemSharedInfo* shared_infos = isi->sharedInfo();
  ItemTypeId itid = isi->itemTypeId();
  Int16 type_id   = itid.typeId();
  Int32 parent_id = NULL_ITEM_LOCAL_ID;

  Integer nb_item  = unique_ids.size();
  Integer nb_free  = m_free_internals.size();
  Integer nb_reuse = math::min(nb_item, nb_free);
  Integer nb_alloc = nb_item - nb_reuse;
  Integer nb_internal_before = m_internals.size();

  if (nb_alloc > 0) {
    // The remaining internal items must be allocated and initialized.
    // Here we suppose that a call to `m_item_family._preAllocate(nb_item)` was made, in which
    // case the shared info arrays are already at the right size.

    Accelerator::ProfileRegion reg(queue, "ItemInternal CPU init");

    //
    // CPU
    // TODO: voir pour mettre ça sur GPU, ou au moins s'assurer que ça ne touche pas à la mémoire du device
    //   -> on pourrait aussi paralléliser ces boucles (multi-threading seulement)
    // TODO: idéalement, on place ça APRÈS avoir lancé le kernel GPU et AVANT la barrière, pour recouvrir du travail GPU par du CPU
    //

    Integer nb_free_internals  = m_free_internals_in_multi_buffer.size();
    Integer nb_reuse_internals = math::min(nb_alloc, nb_free_internals);
    Integer nb_alloc_internals = nb_alloc - nb_reuse_internals;
    Integer lid_offset = nb_internal_before;

    if (m_nb_item < 0) {
      ARCANE_FATAL("m_nb_item is negative");
    }

    Integer internals_offset = m_internals.size();
    m_internals.resizeNoInit(m_internals.size() + nb_alloc);

    if (nb_reuse_internals > 0) {
      // Reuse available `ItemInternal` objects, starting from the last ones
      for (Integer i = 0; i < nb_reuse_internals; i++) {
        ItemInternal* new_item = m_free_internals_in_multi_buffer[nb_free_internals - i - 1];
        Int32 lid = lid_offset + i;
        new_item->setLocalId(lid);
        new_item->_setSharedInfo(shared_infos, itid);
        m_internals[internals_offset + i] = new_item;
      }
      // Since we only consumed items from the back, we use `resize` to remove them from the list
      m_free_internals_in_multi_buffer.resize(nb_free_internals - nb_reuse_internals);
      internals_offset += nb_reuse_internals;
      lid_offset += nb_reuse_internals;
    }

    if (nb_alloc_internals > 0) {
      // Allocate new `ItemInternal`s for the remaining objects
      ArrayView<ItemInternal> allocated_items = m_item_internals_buffer->allocMany(nb_alloc_internals);
      for (Int32 i = 0; i < nb_alloc_internals; i++) {
        ItemInternal* new_item = allocated_items.ptrAt(i);
        Int32 lid = lid_offset + i;
        new_item->setLocalId(lid);
        new_item->_setSharedInfo(shared_infos, itid);
        m_internals[internals_offset + i] = new_item;
      }
      internals_offset += nb_alloc_internals;
      lid_offset += nb_alloc_internals;
    }
  }

  _updateItemSharedInfoInternalView();

  Int32 added_items_offset = m_added_items.size();
  m_added_items.resizeNoInit(added_items_offset + nb_item);

  {
    auto cmd = Accelerator::makeCommand(queue);

    auto in_free = m_free_internals.smallSpan();
    auto in_uids = SmallSpan<const Int64>(unique_ids);
    auto out_lids = SmallSpan<Int32>(local_ids);
    auto out_added_items = m_added_items.smallSpan();

    auto out_uids   = SmallSpan<Int64>(m_common_item_shared_info->m_unique_ids);
    auto out_owners = SmallSpan<Int32>(m_common_item_shared_info->m_owners);
    auto out_typeid = SmallSpan<Int16>(m_common_item_shared_info->m_type_ids);
    auto out_parent = SmallSpan<Int32>(m_common_item_shared_info->m_parent_item_ids);
    auto out_flags  = SmallSpan<Int32>(m_common_item_shared_info->m_flags);

    // TODO: tmp
    // whoIsAvailable(info(), &queue, std::array<std::tuple<std::string_view, const void*, Int32>, 9>{
    //   std::make_tuple("in_free", in_free.data(), in_free.size()),
    //   std::make_tuple("in_uids", in_uids.data(), in_uids.size()),
    //   std::make_tuple("out_uids", out_uids.data(), out_uids.size()),
    //   std::make_tuple("out_owners", out_owners.data(), out_owners.size()),
    //   std::make_tuple("out_typeid", out_typeid.data(), out_typeid.size()),
    //   std::make_tuple("out_parent", out_parent.data(), out_parent.size()),
    //   std::make_tuple("out_flags", out_flags.data(), out_flags.size()),
    //   std::make_tuple("out_lids", out_lids.data(), out_lids.size()),
    //   std::make_tuple("out_added_items", out_added_items.data(), out_added_items.size()),
    // });

    Integer nb_allocated_items = out_uids.size();
    if (m_nb_item + nb_reuse + nb_alloc > nb_allocated_items) {
      ARCANE_FATAL("Not enough items were pre allocated prior to this 'allocMany' call. Allocated: {0}, required: {1}",
          nb_allocated_items, m_nb_item + nb_reuse + nb_alloc);
    }

    Int32 allocated_items_lid_offset = nb_internal_before;

    // Mark all items as added
    Int32 flags = ItemFlags::II_Added;
    if (owner_id == current_sub_domain_id) {
      flags |= ItemFlags::II_Own;
    }

    cmd << RUNCOMMAND_LOOP1(i, nb_reuse + nb_alloc) {
      ItemLocalId lid;
      if (i < nb_reuse) {
        // Reused items
        Integer free_idx = nb_free - i - 1;  // consume the last elements of the list first
        lid = ItemLocalId(in_free[free_idx]);
      }
      else {
        // Allocated items
        lid = ItemLocalId(allocated_items_lid_offset - nb_reuse + i);
      }

      out_flags[lid] = flags;
      out_owners[lid] = owner_id;
      out_typeid[lid] = type_id;
      if (out_parent.size() > 0)
        out_parent[lid] = parent_id;
      out_uids[lid] = in_uids[i];
      out_lids[i] = lid;
      out_added_items[added_items_offset + i] = lid;
    };
  }

  queue.barrier();

  m_nb_item += nb_item;

  // Remove the reused items without touching memory from the host
  m_free_internals.resize(nb_free - nb_reuse);

#ifdef ARCANE_CHECK
  for (Int32 i = 0; i < nb_alloc; i++) {
    ItemInternal* item = m_internals[m_internals.size() - nb_alloc + i];
    Int32 internal_lid = item->localId();
    Int32 expected_lid = m_added_items[m_added_items.size() - nb_alloc + i];
    Int32 output_lid = local_ids[local_ids.size() - nb_alloc + i];
    if (expected_lid != internal_lid) {
      ARCANE_FATAL("LocalId discrepancy between m_added_items ({0}) and m_internals ({1})",
          expected_lid, internal_lid);
    }
    if (expected_lid != output_lid) {
      ARCANE_FATAL("LocalId discrepancy between m_added_items ({0}) and local_ids ({1})",
          expected_lid, output_lid);
    }
  }
#endif

  Int32ConstArrayView allocated_item_lids(nb_alloc, m_added_items.data() + added_items_offset + nb_reuse);
  m_item_family->notifyItemsChanged(allocated_item_lids);

  if (m_item_list_change_event.hasObservers()) {
    if (queue.isNull()) {
      for (Int32 i = 0; i < nb_item; i++) {
        ItemFamilyItemListChangedEventArgs args(m_item_family, local_ids[i], unique_ids[i]);
        args.setIsAdd(true);
        m_item_list_change_event.notify(args);
      }
    } else {
      // TODO: notify the observers, but we need to avoid touching the UIDs and LIDs from the host
      ARCANE_THROW(NotImplementedException, "observers on the device are unsupported");
    }
  }

  if (m_has_unique_id_map) {
    if (queue.isNull()) {
      ARCCORE_THROW(NotImplementedException, "unique id map on the host is not yet implemented");
      // TODO: update the map with the new unique IDs
      // if (!m_items_map.add(unique_id, next))
      //   _badSameUniqueId(unique_id);
    } else {
      // TODO: update the map on GPU?
      ARCANE_THROW(NotImplementedException, "unique id map on the device is unsupported");
    }
  }
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
setHasUniqueIdMap(bool v)
{
  if (m_nb_item != 0)
    ARCANE_FATAL("family is not empty");
  m_has_unique_id_map = v;
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_checkActiveItem(ItemInternal* item)
{
  if (item->isSuppressed())
    ARCANE_FATAL("Attempting to remove an entity already deleted item={0}",
                 ItemPrinter(item));
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_dumpList()
{
  Integer nb_internal = m_internals.size();
  Integer computed_nb_item = 0;
  for (Integer i = 0; i < nb_internal; ++i) {
    ItemInternal* item = m_internals[i];
    bool is_suppressed = item->isSuppressed();
    if (is_suppressed)
      ++computed_nb_item;
    info() << "Item: INDEX=" << i
           << " LID=" << item->localId()
           << " UID=" << item->uniqueId()
           << " KILLED=" << is_suppressed;
  }
  info() << "EndOfDump: "
         << " nb_internal=" << nb_internal
         << " nb_item=" << m_nb_item
         << " computed_nb_item= " << (nb_internal - computed_nb_item);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_badSameUniqueId(Int64 unique_id) const
{
  ARCANE_FATAL("duplicate unique id family={0} uid={1}",
               m_item_family->name(), unique_id);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_badUniqueIdMap() const
{
  if (m_has_unique_id_map)
    ARCANE_FATAL("family have unique id map");
  else
    ARCANE_FATAL("family does not have unique id map");
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
setItemFamily(ItemFamily* item_family)
{
  m_item_family = item_family;
  m_common_item_shared_info = item_family->commonItemSharedInfo();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
changeAllocators(Accelerator::RunQueue& old_queue, Accelerator::RunQueue& new_queue)
{
  MemoryAllocationOptions acc_alloc = new_queue.allocationOptions();
  if (acc_alloc == old_queue.allocationOptions()) {
    return;  // no changes to do
  }

  info() << "moving from " << old_queue.executionPolicy() << " to " << new_queue.executionPolicy();  // TODO: tmp
  // This should only happen at initialization, therefore no data movement should occur here,
  // as those arrays are still empty.
  m_removed_items.changeAllocator(acc_alloc, &new_queue);
  m_added_items.changeAllocator(acc_alloc, &new_queue);
  m_free_internals.changeAllocator(acc_alloc, &new_queue);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_updateItemSharedInfoInternalView()
{
  if (m_common_item_shared_info)
    m_common_item_shared_info->m_items_internal = m_internals.constView();
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

EventObservableView<const ItemFamilyItemListChangedEventArgs&> DynamicMeshKindInfos::
itemListChangedEvent()
{
  return EventObservableView<const ItemFamilyItemListChangedEventArgs&>(m_item_list_change_event);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_notifyRemove2(ItemInternal* item)
{
  ItemFamilyItemListChangedEventArgs args(m_item_family, item->localId(), item->uniqueId());
  m_item_list_change_event.notify(args);
  args.setIsAdd(false);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

void DynamicMeshKindInfos::
_notifyAdd2(ItemInternal* item, Int64 uid)
{
  ItemFamilyItemListChangedEventArgs args(m_item_family, item->localId(), uid);
  args.setIsAdd(true);
  m_item_list_change_event.notify(args);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

} // End namespace Arcane::mesh

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
