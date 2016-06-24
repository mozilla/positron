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

this.EXPORTED_SYMBOLS = ["ModuleLoader"];

const systemPrincipal = Cc["@mozilla.org/systemprincipal;1"].
                        createInstance(Ci.nsIPrincipal);

// The default list of "global paths" (i.e. search paths for modules
// specified by name rather than path).  The list of global paths for a given
// ModuleLoader depends on the process type, so the ModuleLoader constructor
// clones this list and prepends a process-type-specific path to the clone.
const DEFAULT_GLOBAL_PATHS = [
  // Electron standard modules that are common to both process types.
  'common/api/exports/',

  // Node standard modules, which are copied directly from Node where possible.
  'node/',
];

const windowLoaders = new WeakMap();

/**
 * Construct a module loader (`require()` global function).
 *
 * @param processType {String}
 *        the type of Electron process for which to construct the loader,
 *        either "browser" (i.e. the main process) or "renderer"
 *
 * @param window {DOMWindow}
 *        for a renderer process loader, the window associated with the process
 *
 * @return {ModuleLoader} the module loader
 */

function ModuleLoader(processType, window) {
  // Register this loader for the given window early, so modules that we require
  // during loader construction can access window.process without causing
  // the `process` WebIDL binding to create a new module loader (and thus enter
  // an infinite loop).
  if (window) {
    windowLoaders.set(window, this);
  }

  const globalPaths = DEFAULT_GLOBAL_PATHS.slice();
  // Prepend a process-type-specific path to the global paths for this loader.
  globalPaths.unshift(processType + '/api/exports/');

  /**
   * Mapping from module IDs (resource: URLs) to module objects.
   *
   * @keys {string} The ID (resource: URL) for the module.
   * @values {object} An object representing the module.
   */
  const modules = new Map();

  /**
   * The module-wide global object, which is available via a reference
   * to the `global` symbol as well as implicitly via the sandbox prototype.
   * This object is shared across all modules loaded by this loader.
   */
  if (processType === 'browser') {
    this.global = {
      // I don't think this is exactly the same as the Console Web API.
      // XXX Replace this with the real Console Web API (or at least ensure
      // that the current version is compatible with it).
      console: Cu.import('resource://gre/modules/Console.jsm', {}).console,
    };
  } else {
    // For renderer processes, the global object is the window object
    // of the renderer window, so modules have access to all its globals.
    this.global = window;
  }

  const sandbox = new Cu.Sandbox(systemPrincipal, {
    wantComponents: true,
    sandboxPrototype: this.global,
  });

  /**
   * Import a module.
   *
   * @param  requirer {Object} the module importing this module.
   * @param  path     {string} the path to the module being imported.
   * @return          {Object} an `exports` object.
   */
  this.require = function(requirer, path) {
    // dump('require: ' + requirer.id + ' requires ' + path + '\n');

    if (path === 'native_module') {
      return this;
    }

    let uri, file;

    if (path.indexOf("://") !== -1) {
      // A URL.
      uri = Services.io.newURI(path, null, null);
      file = uri.QueryInterface(Ci.nsIFileURL).file;
    } else if (path.indexOf('/') === 0) {
      // An absolute path.
      // XXX Figure out if this is a filesystem path rather than a package path
      // and should be converted to a file: URL instead of a resource: URL.
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
      throw new Error(`No such module: ${path} (required by ${requirer.id})`);
    }

    // dump('require: module found at ' + uri.spec + '\n');

    // The module object.  This gets exposed to the module itself,
    // and it also gets cached for reuse, so multiple `require(module)` calls
    // return a single instance of the module.
    let module = {
      id: uri.spec,
      exports: {},
      paths: globalPaths.slice(),
      parent: requirer,
    };

    // Return module immediately if it's already started loading.  Note that
    // this can occur before the module is fully loaded, if there are circular
    // dependencies.
    if (modules.has(module.id)) {
      return modules.get(module.id).exports;
    }
    modules.set(module.id, module);

    // Provide the Components object to the "native binding modules" (which
    // implement APIs in JS that Node/Electron implement with native bindings).
    const wantComponents = uri.spec.startsWith('resource:///modules/gecko/');

    // Create a target object whose prototype is the sandbox, which we'll use
    // to load the module via the subscript loader, so the module has the global
    // scope of the sandbox and the local scope of the target.
    let target = Object.create(sandbox);

    // In the renderer process, we can't yet load all modules in a single
    // sandbox, because attempts to reference Window properties on the target
    // object then throw errors like:
    //
    //   TypeError: 'get location' called on an object that does not implement interface Window.
    //
    // This is described as "TypeError: invalid Array.prototype.sort argument"
    // in the MDN docs on the error:
    //
    //   https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Errors/Array_sort_argument
    //
    // But that's clearly not the issue here.  Some code is deciding that
    // the target object doesn't implement Window, even though its prototype
    // is a Window instance.
    //
    // Whereas that code does think that a sandbox with a Window prototype
    // implements Window.  So we work around the problem by loading modules
    // in their own sandboxes in renderer processes.
    //
    // TODO: https://github.com/mozilla/positron/issues/93
    //
    if (processType === 'renderer') {
      target = new Cu.Sandbox(systemPrincipal, {
        wantComponents: true,
        sandboxPrototype: window,
      });
    }

    // Inject module-specific locals into the target object.
    target.exports = module.exports;
    target.module = module;
    target.require = this.require.bind(this, module);
    target.global = this.global;
    target.__filename = file.path;
    target.__dirname = file.parent.path;

    try {
      subScriptLoader.loadSubScript(uri.spec, target, 'UTF-8');
      return module.exports;
    } catch(ex) {
      modules.delete(module.id);
      // dump('require: error loading module ' + path + ' from ' + uri.spec + ': ' + ex + '\n' + ex.stack + '\n');
      throw ex;
    }
  };

  // The `process` global is complicated.  It's implemented by a module,
  // process.js, that we require.  But it's also supposed to exist before any
  // modules are required, since it's a global that's supposed to be available
  // in all modules.
  //
  // At the moment, we avoid any issue by ensuring that neither process.js
  // nor any module in its dependency chain accesses the `process` global
  // at evaluation time.  In the future, however, we may want (or need)
  // to implement process.js such that it creates the global before any module
  // can possibly access it.
  //
  this.process = this.require({}, 'resource:///modules/gecko/process.js');

  // In the browser process, assign global.process to this.process directly.
  // In a renderer process, the WebIDL binding sets global.process.
  if (processType === 'browser') {
    this.global.process = this.process;
  }

  this.global.Buffer = this.require({}, 'resource:///modules/node/buffer.js').Buffer;

  // XXX Also define clearImmediate, and other Node globals.
  const timers = this.require({}, 'resource:///modules/node/timers.js');
  this.global.setImmediate = timers.setImmediate;

  // Require the Electron init.js script for the given process type.
  //
  // We only do this for renderer processes for now.  For browser processes,
  // we instead require the browser process modules that renderer/init.js
  // depends on having already been required.
  //
  // Eventually, we should get browser/init.js working and require it here,
  // at which point it'll require those modules for us.
  //
  if (processType === 'renderer') {
    this.require({}, 'resource:///modules/renderer/init.js');
  } else {
    this.require({}, 'resource:///modules/browser/rpc-server.js');
    this.require({}, 'resource:///modules/browser/guest-view-manager.js');
    this.require({}, 'resource:///modules/browser/guest-window-manager.js');
  }
}

ModuleLoader.getLoaderForWindow = function(window) {
  let loader = windowLoaders.get(window);
  if (!loader) {
    loader = new ModuleLoader('renderer', window);
  }
  return loader;
};
