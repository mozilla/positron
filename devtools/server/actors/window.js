/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { Ci } = require("chrome");
const { TabActor } = require("./webbrowser");

/**
 * Creates a WindowActor for debugging a single window, like a browser window in
 * Firefox, but it can be used to reach any window in the process.  Most of the
 * implementation is inherited from TabActor. WindowActor exposes all tab actors
 * via its form() request, like TabActor.
 *
 * You can request a specific window's actor via RootActor.getWindow().
 *
 * @param connection DebuggerServerConnection
 *        The connection to the client.
 * @param window DOMWindow
 *        The window.
 */
function WindowActor(connection, window) {
  TabActor.call(this, connection);

  let docShell = window.QueryInterface(Ci.nsIInterfaceRequestor)
                       .getInterface(Ci.nsIDocShell);
  Object.defineProperty(this, "docShell", {
    value: docShell,
    configurable: true
  });
}

WindowActor.prototype = Object.create(TabActor.prototype);

// Bug 1266561: This setting is mysteriously named, we should split up the
// functionality that is triggered by it.
WindowActor.prototype.isRootActor = true;

WindowActor.prototype.observe = function(subject, topic, data) {
  TabActor.prototype.observe.call(this, subject, topic, data);
  if (!this.attached) {
    return;
  }
  if (topic == "chrome-webnavigation-create") {
    subject.QueryInterface(Ci.nsIDocShell);
    this._onDocShellCreated(subject);
  } else if (topic == "chrome-webnavigation-destroy") {
    this._onDocShellDestroy(subject);
  }
};

WindowActor.prototype._attach = function() {
  if (this.attached) {
    return false;
  }

  TabActor.prototype._attach.call(this);

  // Listen for chrome docshells in addition to content docshells
  if (this.listenForNewDocShells) {
    Services.obs.addObserver(this, "chrome-webnavigation-create", false);
  }
  Services.obs.addObserver(this, "chrome-webnavigation-destroy", false);

  return true;
};

WindowActor.prototype._detach = function() {
  if (!this.attached) {
    return false;
  }

  if (this.listenForNewDocShells) {
    Services.obs.removeObserver(this, "chrome-webnavigation-create");
  }
  Services.obs.removeObserver(this, "chrome-webnavigation-destroy");

  TabActor.prototype._detach.call(this);

  return true;
};

exports.WindowActor = WindowActor;
