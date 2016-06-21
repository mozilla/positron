/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=8 et :
 */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/TextureForwarder.h"

namespace mozilla {
namespace layers {

TextureForwarder::TextureForwarder(const char* aName)
  : ClientIPCAllocator(aName)
  , mSectionAllocator(nullptr)
{
}

TextureForwarder::~TextureForwarder()
{
  if (mSectionAllocator) {
    delete mSectionAllocator;
  }
}

FixedSizeSmallShmemSectionAllocator*
TextureForwarder::GetTileLockAllocator()
{
  MOZ_ASSERT(IPCOpen());
  if (!IPCOpen()) {
    return nullptr;
  }

  if (!mSectionAllocator) {
    mSectionAllocator = new FixedSizeSmallShmemSectionAllocator(this);
  }
  return mSectionAllocator;
}

} // namespace layers
} // namespace mozilla
