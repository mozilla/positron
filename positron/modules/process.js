/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

Cu.import('resource://gre/modules/Services.jsm');

// In Node, process.js is basically a noop, as it just re-exports the existing
// `process` global.  But in Positron, we implement the `process` global via
// this module, so we construct the `process` global by importing this module
// rather than the other way around.

// Because we construct the `process` global in this module, and we also inject
// the global into this module, we don't actually have to export its symbols
// here.  We just have to attach them to this module's own `process` global.
// XXX Figure out if that's really the best way to implement this functionality.

// This comes from Electron, where it's defined by common/init.js, which we
// currently don't load.  Once we enable loading of init.js modules, we should
// remove it from this module.
process.atomBinding = function(name) {
  try {
    return process.binding("atom_" + process.type + "_" + name);
  } catch (error) {
    if (/No such module/.test(error.message)) {
      return process.binding("atom_common_" + name);
    }
    throw error;
  }
};

// This loads a native binding in Node, but we aren't using real Node yet,
// so we simply require the module with the same name, so we can implement
// bindings in JavaScript.
process.binding = function(name) {
  return require(name);
}

// Per <https://nodejs.org/api/process.html#process_process_platform>,
// valid values for this property are darwin, freebsd, linux, sunos and win32;
// so we convert winnt to win32.  We also lowercase the value, but otherwise
// we don't modify it, so it *might* contain a value outside the Node set, per
// <https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/OS_TARGET>.
process.platform = Services.appinfo.OS.toLowerCase().replace(/^winnt$/, 'win32');

process.versions = {
  node: '0',
  chrome: Services.appinfo.platformVersion,
  electron: Services.appinfo.version,
};
