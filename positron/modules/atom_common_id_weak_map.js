/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

// Described In Electron's id_weak_map.h as:
// "Like ES6's WeakMap, but the key is Integer and the value is Weak Pointer."

const instances = new WeakMap();

function IDWeakMap() {
  instances.set(this, new Map());
}

IDWeakMap.prototype = {
  set(key, value) {
    instances.get(this).set(key, Cu.getWeakReference(value));
  },
  get(key) {
    return instances.get(this).get(key).get();
  },
  has(value) {
    return instances.get(this).has(value);
  },
  clear() {
    instances.get(this).clear();
  },
};

exports.IDWeakMap = IDWeakMap;
