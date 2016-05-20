/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

// Described In Electron's id_weak_map.h as:
// "Like ES6's WeakMap, but the key is Integer and the value is Weak Pointer."

function IDWeakMap() {
  const map = new Map();
  this.set = function(key, value) {
    map.set(key, Cu.getWeakReference(value));
  };
  this.get = function(key) {
    return map.get(key).get();
  }
  this.has = map.has.bind(map);
  this.clear = map.clear.bind(map);
}

exports.IDWeakMap = IDWeakMap;
