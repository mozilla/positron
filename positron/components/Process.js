/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- /
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

// An XPCOM component that implements the `process` global in BrowserWindow.
// This gets attached the global object through the 'process' WebIDL interface.
// It proxies access to the gecko/process module.

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

Cu.import('resource://gre/modules/XPCOMUtils.jsm');
Cu.import('resource:///modules/ModuleLoader.jsm');

function Process() {}

Process.prototype = {
  _contentWindow: null,
  _processGlobal: null,

  classID: Components.ID('{3c81d709-5fb4-4144-9612-9ecc1be4e7b1}'),
  contractID: '@mozilla.org/positron/process;1',

  /* nsISupports */

  QueryInterface: XPCOMUtils.generateQI([Ci.nsISupports, Ci.nsIDOMGlobalPropertyInitializer]),

  /* nsIDOMGlobalPropertyInitializer */

  /**
   * Initialize the global `process` property.  See the code comment
   * https://dxr.mozilla.org/mozilla-central/rev/21bf1af/toolkit/mozapps/extensions/amInstallTrigger.js#124-133
   * for an explanation of the behavior of this method.
   */
  init: function(window) {
    this._contentWindow = window;

    // The WebIDL binding applies to the hidden window too, but we don't want
    // to initialize the Electron environment for that window, so return early
    // if we've been called to initialize `process` for that window.
    //
    // TODO: restrict the WebIDL binding to windows opened by the Electron app
    // using the Func extended attribute
    // <https://developer.mozilla.org/en-US/docs/Mozilla/WebIDL_bindings#Func>.
    //
    if (window.location.toString() === 'resource://gre-resources/hiddenWindow.html') {
      this._processGlobal = new Proxy({}, {
        get: function(target, name) {
          window.console.error("'process' not defined in hidden window");
        }
      });
      return window.processImpl._create(window, this);
    }

    let loader = ModuleLoader.getLoaderForWindow(window);
    this._processGlobal = loader.process;
    return window.processImpl._create(window, this);
  },

  /* Node `process` interface */

  get argv() {
    return this._processGlobal.argv;
  },

  get env() {
    return this._processGlobal.env;
  },

  get execPath() {
    return this._processGlobal.execPath;
  },

  get pid() {
    return this._processGlobal.pid;
  },

  get platform() {
    return this._processGlobal.platform;
  },

  get release() {
    return this._processGlobal.release;
  },

  get type() {
    return this._processGlobal.type;
  },

  get versions() {
    return this._processGlobal.versions;
  },

  atomBinding(name) {
    return this._processGlobal.atomBinding(name);
  },

  binding(name) {
    try {
      return this._processGlobal.binding(name);
    } catch(ex) {
      // Per
      // https://developer.mozilla.org/en-US/docs/Mozilla/WebIDL_bindings#Throwing_exceptions_from_JS-implemented_APIs
      // we need to recreate Error objects via our own content window,
      // or the WebIDL binding will set its message to "NS_ERROR_UNEXPECTED",
      // which will fail the `/No such module/.test(error.message)` check
      // in common/init.js.
      throw new this._contentWindow.Error(ex.message);
    }
  },

  /* Node `EventEmitter` interface */

  once(name, listener) {
    return this._processGlobal.once(name, listener);
  },

};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([Process]);
