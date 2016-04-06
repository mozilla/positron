/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * A minimally-viable Node-like module loader.
 */

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

Cu.import('resource://gre/modules/Services.jsm');

const subScriptLoader = Cc['@mozilla.org/moz/jssubscript-loader;1'].
                        getService(Ci.mozIJSSubScriptLoader);

this.EXPORTED_SYMBOLS = ["Require"];

const systemPrincipal = Cc["@mozilla.org/systemprincipal;1"].
                        createInstance(Ci.nsIPrincipal);

// An (incomplete) list of "global paths" (i.e. search paths for modules
// specified by name rather than path).  Currently this is a single list,
// but it needs to vary by process type, as the browser and renderer processes
// have their own sets of standard modules (in addition to the shared set).
const globalPaths = [
  // Browser standard modules.
  'browser/api/exports/',

  // Common standard modules.
  'common/api/',

  // Modules that are imported via process.atomBinding().  In Electron,
  // these are all natives; in Positron, they're currently all JavaScript.
  'atom/',

  // Node standard modules, which are copied directly from Node where possible.
  'node/',
];

/**
 * Mapping from module IDs (resource: URLs) to module objects.
 *
 * @keys {string} The ID (resource: URL) for the module.
 * @values {object} An object representing the module.
 */
let modules = new Map();

/**
 * Construct a module importer (`require()` global function).
 *
 * @param requirer {Module} the module that will use the importer.
 * @return {Function} a module importer.
 */
function Require(requirer) {
  /**
   * Import a module.
   *
   * @param path {string} the path to the module.
   * @return {*} an `exports` object.
   */
  return function require(path) {
    let uri, file;

    // dump('require: ' + requirer.id + ' requires ' + path + '\n');

    if (path.indexOf("://") !== -1) {
      // A URL.
      uri = Services.io.newURI(path, null, null);
      file = uri.QueryInterface(Ci.nsIFileURL).file;
    } else if (path.indexOf('/') === 0) {
      // An absolute path.
      uri = Services.io.newURI('resource:///modules' + path + '.js', null, null);
      file = uri.QueryInterface(Ci.nsIFileURL).file;
    } else {
      // A relative path or built-in module name.
      let baseURI = Services.io.newURI(requirer.id, null, null);
      uri = Services.io.newURI(path + '.js', null, baseURI);
      file = uri.QueryInterface(Ci.nsIFileURL).file;

      if (!file.exists() && path.indexOf('/') === -1) {
        // It isn't a relative path; perhaps it's a built-in module name.
        for (let globalPath of globalPaths) {
          uri = Services.io.newURI('resource:///modules/' + globalPath + path + '.js', null, null);
          file = uri.QueryInterface(Ci.nsIFileURL).file;
          if (file.exists()) {
            break;
          }
        }
      }
    }

    if (!file.exists()) {
      throw new Error('No such module: ' + path);
    }

    // dump('require: module found at ' + uri.spec + '\n');

    // Exports provided by the module.
    let exports = Object.create(null);

    // The module object.  This gets exposed to the module itself,
    // and it also gets cached for reuse, so multiple `require(module)` calls
    // return a single instance of the module.
    let module = {
      id: uri.spec,
      exports: exports,
    };

    // Return module immediately if it's already started loading.  Note that
    // this can occur before the module is fully loaded, if there are circular
    // dependencies.
    if (modules.has(module.id)) {
      return modules.get(module.id).exports;
    }
    modules.set(module.id, module);

    var sandbox = new Cu.Sandbox(systemPrincipal, {
      sandboxName: uri.spec,
      wantComponents: false
    });

    sandbox.exports = exports;
    sandbox.require = new Require(module);
    sandbox.module = module;

    // Require `process` by absolute URL so the resolution algorithm doesn't try
    // to resolve it relative to the requirer's URL.
    sandbox.process = require('resource:///modules/node/process.js');

    try {
      // XXX evalInSandbox?
      subScriptLoader.loadSubScript(uri.spec, sandbox, 'UTF-8');
      return module.exports;
    } catch(ex) {
      modules.delete(module.id);
      // dump('require: error loading module ' + path + ' from ' + uri.spec + ': ' + ex + '\n' + ex.stack + '\n');
      throw ex;
    }
  };
};
