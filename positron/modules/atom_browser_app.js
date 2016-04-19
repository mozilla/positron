/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

Cu.import('resource://gre/modules/Services.jsm');

exports.app = {
  quit() {
    // XXX Emit the before-quit and will-quit events.
    Services.startup.quit(Services.startup.eAttemptQuit);
  },
};

exports.appendSwitch = function() { /* stub */ };
exports.appendArgument = function() { /* stub */ };
exports.dockBounce = function() { /* stub */ };
exports.cancelBounce = function() { /* stub */ };
exports.setBadge = function() { /* stub */ };
exports.getBadge = function() { /* stub */ };
exports.hide = function() { /* stub */ };
exports.show = function() { /* stub */ };
exports.setMenu = function() { /* stub */ };
exports.setIcon = function() { /* stub */ };

// There isn't currently anything we need to do before emitting app.ready,
// but apps will expect it to happen after a tick, so emit it in a timeout.
let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
timer.initWithCallback({ notify: function() {
  exports.app.emit('ready');
} }, 0, Ci.nsITimer.TYPE_ONE_SHOT);
