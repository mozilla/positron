// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "NodeBindingsMac.h"

#include <errno.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>

#include "uv.h"

namespace mozilla {

NodeBindingsMac::NodeBindingsMac(bool is_browser)
    : NodeBindings(is_browser) {
}

NodeBindingsMac::~NodeBindingsMac() {
}

void NodeBindingsMac::RunMessageLoop() {
  // Get notified when libuv's watcher queue changes.
  uv_loop_->data = this;
  uv_loop_->on_watcher_queue_updated = OnWatcherQueueChanged;

  NodeBindings::RunMessageLoop();
}

// static
void NodeBindingsMac::OnWatcherQueueChanged(uv_loop_t* loop) {
  NodeBindingsMac* self = static_cast<NodeBindingsMac*>(loop->data);

  // We need to break the io polling in the kqueue thread when loop's watcher
  // queue changes, otherwise new events cannot be notified.
  self->WakeupEmbedThread();
}

void NodeBindingsMac::PollEvents() {
  struct timeval tv;
  int timeout = uv_backend_timeout(uv_loop_);
  if (timeout != -1) {
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
  }

  fd_set readset;
  int fd = uv_backend_fd(uv_loop_);
  FD_ZERO(&readset);
  FD_SET(fd, &readset);

  // Wait for new libuv events.
  int r;
  do {
    r = select(fd + 1, &readset, NULL, NULL, timeout == -1 ? NULL : &tv);
  } while (r == -1 && errno == EINTR);
}

// static
NodeBindings* NodeBindings::Create(bool is_browser) {
  return new NodeBindingsMac(is_browser);
}

}  // namespace mozilla
