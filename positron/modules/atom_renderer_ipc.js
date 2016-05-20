/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

const cpmm = Cc["@mozilla.org/childprocessmessagemanager;1"].
             getService(Ci.nsISyncMessageSender);

exports.send = function(name, args) {
  // XXX Figure out what the caller in ipc-renderer.js expects us to return.
  return cpmm.sendAsyncMessage(name, args, { window });
};

exports.sendSync = function(name, args) {
  // cpmm.sendSyncMessage returns an array of return values, one for each
  // listener on the other side.  But it looks like ipcRenderer.sendSync
  // <http://electron.atom.io/docs/api/ipc-renderer/#ipcrenderersendsyncchannel-arg1-arg2->
  // expects to receive a single return value.  So this function returns
  // the first item in the array.
  return cpmm.sendSyncMessage(name, args, { window })[0];
};
