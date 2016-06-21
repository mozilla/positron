/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=99: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_gfx_ipc_GPUParent_h__
#define _include_gfx_ipc_GPUParent_h__

#include "mozilla/gfx/PGPUParent.h"

namespace mozilla {
namespace gfx {

class GPUParent final : public PGPUParent
{
public:
  GPUParent();
  ~GPUParent();

  bool Init(base::ProcessId aParentPid,
            MessageLoop* aIOLoop,
            IPC::Channel* aChannel);

  bool RecvNothing() override;

  void ActorDestroy(ActorDestroyReason aWhy) override;
};

} // namespace gfx
} // namespace mozilla

#endif // _include_gfx_ipc_GPUParent_h__
