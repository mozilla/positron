/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;
const { Services } = Cu.import("resource://gre/modules/Services.jsm", {});

let wrapWebContents = null;

exports._setWrapWebContents = function(aWrapWebContents) {
  wrapWebContents = aWrapWebContents;
};

// We can't attach properties to a WebContents instance by setting
// WebContents.prototype, because wrapWebContents sets the instance's __proto__
// property to EventEmitter.prototype.
//
// So instead we attach "prototype" properties directly to the instance itself
// inside the WebContents constructor, using this WebContents_prototype object
// to collect the set of properties that should be assigned to the instance.
//
let WebContents_prototype = {
  // In Electron, this is implemented via GetRenderProcessHost()->GetID(),
  // which appears to return the process ID of the renderer process.  We don't
  // actually create a unique process for each renderer, so we simply give
  // each WebContents instance an arbitrary unique ID.
  getId() {
    return this._id;
  },

  getOwnerBrowserWindow() {
    return this._browserWindow;
  },

  getURL: function() {
    if (this._browserWindow._domWindow) {
      return this._browserWindow._domWindow.location;
    }
    return null;
  },

  loadURL: function(url) {
    this._browserWindow._domWindow.location = url;
  },

  getTitle: function() {
    /* stub */
    return "";
  },

  openDevTools() {
    // TODO: When tools can be opened inside the content window, support
    // `detach` option to force into a new window instead.

    // Ensure DevTools core modules are loaded, including support for the about
    // URL below which is registered dynamically.
    const { loader } = Cu.import("resource://devtools/shared/Loader.jsm", {});
    loader.require("devtools/client/framework/devtools-browser");

    // The current approach below avoids the need for a container window
    // wrapping a tools frame, but it does replicate close handling, etc.
    // Historically we would have used toolbox-hosts.js to handle this, but
    // DevTools will be moving away from that, and so it seems fine to
    // experiment with toolbox management here.
    let window = this._browserWindow._domWindow;
    let id = window.QueryInterface(Ci.nsIInterfaceRequestor)
                   .getInterface(Ci.nsIDOMWindowUtils)
                   .outerWindowID;
    let url = `about:devtools-toolbox?type=window&id=${id}`;
    let features = "chrome,resizable,centerscreen," +
                   "width=1024,height=768";
    let toolsWindow = Services.ww.openWindow(null, url, null, features, null);

    // Emit DevTools events that are in the webContents API
    let onLoad = () => {
      toolsWindow.removeEventListener("load", onLoad);
      toolsWindow.addEventListener("unload", onUnload);
      this.emit("devtools-opened");
    }
    let onUnload = () => {
      toolsWindow.removeEventListener("unload", onUnload);
      toolsWindow.removeEventListener("message", onMessage);
      this.emit("devtools-closed");
    }

    // Close the DevTools window if the browser window closes
    let onBrowserClosed = () => {
      toolsWindow.close();
    };

    // Listen for the toolbox's built-in close button, which sends a message
    // asking the toolbox's opener how to handle things.  In this case, just
    // close the toolbox.
    let onMessage = ({ data }) => {
      data = JSON.parse(data);
      if (data.name !== "toolbox-close") {
        return;
      }
      toolsWindow.close();
    };

    toolsWindow.addEventListener("message", onMessage);
    toolsWindow.addEventListener("load", onLoad);
    this._browserWindow.on("closed", onBrowserClosed);
  },
};

let lastWebContentsID = 0;

function WebContents(options) {
  // XXX Consider using WeakMap to hide private properties from consumers.
  this._browserWindow = options.browserWindow;
  this._id = ++lastWebContentsID;

  Object.assign(this, WebContents_prototype);
  this._getURL = this.getURL;
}

exports.create = function(options) {
  // Sadly, wrapWebContents returns one of the functions it attaches
  // to the WebContents instance rather than the instance itself, so we can't
  // simply return the result of wrapping a new instance.
  // XXX Request a pull to fix this upstream, as this seems unintentional.
  // return wrapWebContents(new WebContents(options));
  let webContents = new WebContents(options);
  wrapWebContents(webContents);
  return webContents;
};
