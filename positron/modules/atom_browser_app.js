/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

Cu.import('resource://gre/modules/Services.jsm');
const positronUtil = process.binding('positron_util');

exports.app = {
  quit() {
    // XXX Emit the before-quit and will-quit events.
    // Wait one turn of the event loop for ancillary windows like DevTools to
    // close first.
    Services.tm.mainThread.dispatch({
      run() {
        Services.startup.quit(Services.startup.eAttemptQuit);
      }
    }, Ci.nsIThread.DISPATCH_NORMAL);
  },
};

exports.appendSwitch = positronUtil.makeStub('atom_browser_app.appendSwitch');
exports.appendArgument = positronUtil.makeStub('atom_browser_app.');
exports.dockBounce = positronUtil.makeStub('atom_browser_app.dockBounce');
exports.cancelBounce = positronUtil.makeStub('atom_browser_app.cancelBounce');
exports.setBadge = positronUtil.makeStub('atom_browser_app.setBadge');
exports.getBadge = positronUtil.makeStub('atom_browser_app.getBadge');
exports.hide = positronUtil.makeStub('atom_browser_app.hide');
exports.show = positronUtil.makeStub('atom_browser_app.show');
exports.setMenu = positronUtil.makeStub('atom_browser_app.setMenu');
exports.setIcon = positronUtil.makeStub('atom_browser_app.setIcon');

// There isn't currently anything we need to do before emitting app.ready,
// but apps will expect it to happen after a tick, so emit it in a timeout.
let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
timer.initWithCallback({ notify: function() {
  exports.app.emit('ready');
} }, 0, Ci.nsITimer.TYPE_ONE_SHOT);
