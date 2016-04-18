/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// getHiddenValue/setHiddenValue attach properties to JavaScript objects
// that are inaccessible except via the V8 API.  We emulate that API by storing
// these values in a weak map of JavaScript objects to maps of property names
// to property values.

const hiddenValueMaps = new WeakMap();

exports.getHiddenValue = function(obj, name) {
  const hiddenValueMap = hiddenValueMaps.get(obj);
  if (hiddenValueMap) {
    return hiddenValueMap.get(name);
  }
  return undefined;
};

exports.setHiddenValue = function(obj, name, val) {
  let hiddenValueMap = hiddenValueMaps.get(obj);
  if (!hiddenValueMap) {
    hiddenValueMap = new Map();
    hiddenValueMaps.set(obj, hiddenValueMap);
  }
  hiddenValueMap.set(name, val);
};
