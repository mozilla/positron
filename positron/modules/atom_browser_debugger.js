/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const positronUtil = process.positronBinding('positron_util');

exports._setWrapDebugger = positronUtil.makeStub('atom_browser_debugger._setWrapDebugger');
