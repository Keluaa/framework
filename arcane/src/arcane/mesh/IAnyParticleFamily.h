// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2026 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* IAnyParticleFamily.h                                        (C) 2000-2026 */
/*                                                                           */
/* Common features for all particle families.                                */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
#ifndef ARCANE_PARTICLEFAMILIES_H
#define ARCANE_PARTICLEFAMILIES_H

#include "arcane/core/IParticleFamily.h"
#include "arcane/mesh/ItemFamily.h"

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

namespace Arcane::mesh
{

//! The kind of IParticleFamily
enum class ParticleFamilyKind
{
  Sequential = 0,  //!< particle family with sequential algorithms
  Parallel = 1     //!< particle family with accelerator-compatible algorithms
};


//! Base class for implementors of IParticleFamily
class IAnyParticleFamily
: public ItemFamily
, public IParticleFamily
{
 public:
  // Inherit the ItemFamily constructors
  using ItemFamily::ItemFamily;

  //! The kind of IParticleFamily
  virtual ParticleFamilyKind particleFamilyKind() const = 0;

  // Some methods are defined in both ItemFamily and IParticleFamily, therefore
  // we choose which specific method to call here.

  virtual String name() const override
  {
    return ItemFamily::name();
  }

  virtual void endUpdate() override
  {
    return ItemFamily::endUpdate();
  }
};

}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#endif
