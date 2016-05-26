/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

exports.makeStub = function(signature, returnValue) {
  let doNotWarn;

  if (typeof returnValue === "function") {
    doNotWarn = returnValue;
  } else {
    doNotWarn = function() { return returnValue };
  }

  let warnOnce = function() {
    console.warn(signature + ' not implemented');
    warnOnce = doNotWarn;
    return doNotWarn.apply(this, arguments);
  }

  return function() {
    return warnOnce.apply(this, arguments);
  }
};
