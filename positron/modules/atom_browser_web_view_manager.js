/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

const positronUtil = process.binding('positron_util');
const guestViewManager = require('resource:///modules/browser/guest-view-manager.js');

exports.addGuest = positronUtil.makeStub('atom_browser_web_view_manager.addGuest');
exports.removeGuest = positronUtil.makeStub('atom_browser_web_view_manager.removeGuest');

exports.attachWebViewToGuest = function(webView) {
  const guest = guestViewManager.getGuest(webView.guestInstanceId);
  guest.attachWebViewToGuest(webView);
  guest.emit('did-attach');
};
