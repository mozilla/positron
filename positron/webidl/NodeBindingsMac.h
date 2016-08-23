// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef MOZILLA_COMMON_NODE_BINDINGS_MAC_H_
#define MOZILLA_COMMON_NODE_BINDINGS_MAC_H_

#include "NodeBindings.h"

namespace mozilla {

class NodeBindingsMac : public NodeBindings {
 public:
  explicit NodeBindingsMac(bool is_browser);

  void RunMessageLoop() override;

 private:
  virtual ~NodeBindingsMac();
  // Called when uv's watcher queue changes.
  static void OnWatcherQueueChanged(uv_loop_t* loop);

  void PollEvents() override;
};

}  // namespace mozilla

#endif  // MOZILLA_COMMON_NODE_BINDINGS_MAC_H_
