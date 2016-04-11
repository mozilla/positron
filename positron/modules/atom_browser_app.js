/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

exports.app = { /* stub */ };
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

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

// There isn't currently anything we need to do before emitting app.ready,
// but apps will expect it to happen after a tick, so emit it in a timeout.
let timer = Cc["@mozilla.org/timer;1"].createInstance(Ci.nsITimer);
timer.initWithCallback({ notify: function() {
  exports.app.emit('ready');
} }, 0, Ci.nsITimer.TYPE_ONE_SHOT);
