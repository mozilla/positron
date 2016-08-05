/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

const util = process.binding('util');
const positronUtil = process.binding('positron_util');

exports.getHiddenValue = util.getHiddenValue;
exports.setHiddenValue = util.setHiddenValue;

exports.setDestructor = positronUtil.makeStub('atom_common_v8_util.setDestructor');

exports.createDoubleIDWeakMap = function() {
  // TODO: Properly implment this.
  return new WeakMap();
};
