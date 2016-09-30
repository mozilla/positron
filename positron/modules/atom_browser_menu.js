/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const positronUtil = process.positronBinding('positron_util');

positronUtil.makeStub('atom_browser_menu.Menu')();
exports.Menu = function() {
  this._init();
};
exports.Menu.prototype = {
  constructor: exports.Menu,
  getItemCount: positronUtil.makeStub('atom_browser_menu.Menu.getItemCount', () => 0),
  insertItem: positronUtil.makeStub('atom_browser_menu.Menu.insertItem'),
  insertSeparator: positronUtil.makeStub('atom_browser_menu.Menu.insertSeparator'),
  insertSubMenu: positronUtil.makeStub('atom_browser_menu.Menu.insertSubMenu'),
  setSublabel: positronUtil.makeStub('atom_browser_menu.Menu.setSublabel'),
  setRole: positronUtil.makeStub('atom_browser_menu.Menu.setRole'),
};

exports.setApplicationMenu = positronUtil.makeStub('atom_browser_menu.setApplicationMenu');
exports.sendActionToFirstResponder = positronUtil.makeStub('atom_browser_menu.sendActionToFirstResponder');
