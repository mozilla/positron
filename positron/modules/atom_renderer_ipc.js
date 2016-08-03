/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

const cpmm = Cc["@mozilla.org/childprocessmessagemanager;1"].
             getService(Ci.nsISyncMessageSender);

const v8Util = process.atomBinding('v8_util');
const ipcRenderer = v8Util.getHiddenValue(global, 'ipc');

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
  let result = cpmm.sendSyncMessage(name, args, { window })[0];
  // If no one was listening for the message, wrap up the falsy result in the
  // meta protocol used by Electron's RPC server.  This increases the chances we
  // log an error specific to problem site, instead of here at this IPC code.
  if (!result) {
    return JSON.stringify({
      type: 'value',
      value: result,
    });
  }
  return result;
};

cpmm.addMessageListener("ipc-message", {
  receiveMessage(message) {
    if (message.objects.window !== window) {
      return;
    }

    // Insert a stub 'event' object into the array of message values.
    // This is clearly wrong, but it works for the single message we receive
    // so far, which ignores this value.
    // TODO: figure out what the 'event' object should really be.
    // https://github.com/mozilla/positron/issues/72
    const event = null;
    message.data.splice(1, 0, event);

    ipcRenderer.emit.apply(ipcRenderer, message.data);
  },
});
