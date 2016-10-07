/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;
// TODO: remove workaround for broken Cu.import.
let scope = {};
Cu.import('resource://gre/modules/Services.jsm', scope);
const Services = scope.Services;

const positronUtil = process.positronBinding('positron_util');

let wrapWebContents = null;

exports._setWrapWebContents = function(aWrapWebContents) {
  wrapWebContents = aWrapWebContents;
};

// We can't attach properties to a WebContents instance by setting
// WebContents.prototype, because wrapWebContents sets the instance's __proto__
// property to EventEmitter.prototype.
//
// So instead we assign properties to a WebContents instance via Object.assign
// in its constructor, assigning both WebContentsPrototype and either
// BrowserWindowWebContentsPrototype or GuestWebContentsPrototype, depending on
// whether the WebContents is for a BrowserWindow or a <webview> guest.

const WebContentsPrototype = {
  // In Electron, this is implemented via GetRenderProcessHost()->GetID(),
  // which appears to return the process ID of the renderer process.  We don't
  // actually create a unique process for each renderer, so we simply give
  // each WebContents instance an arbitrary unique ID.
  getId() {
    return this._id;
  },

  // We need to implement loadURL and getURL because we've disabled the wrapping
  // of WebContents by NavigationController in web-contents.js.

  // The equivalent of NavigationController.prototype.loadURL.
  loadURL(url, options) {
    if (options == null) {
      options = {};
    }
    this._loadURL(url, options);
    return this.emit('load-url', url, options);
  },

  // The equivalent of NavigationController.prototype.getURL.
  getURL() {
    return this._getURL();
  },

};

const BrowserWindowWebContentsPrototype = {
  _send: function(channel, args) {
    this._browserWindow._send(channel, args);
  },

  isGuest() {
    return false;
  },

  getOwnerBrowserWindow() {
    return this._browserWindow;
  },

  _getURL() {
    if (this._browserWindow && this._browserWindow._domWindow) {
      return this._browserWindow._domWindow.location;
    }
    console.warn('cannot get URL for BrowserWindowWebContents');
    return null;
  },

  _loadURL(url) {
    this._browserWindow._loadURL(url);
  },

  getTitle: positronUtil.makeStub('WebContents.getTitle', { returnValue: '' }),

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

// A bunch of the GuestWebContentsPrototype methods check if (this._webView)
// before acting, because they're sometimes called before this._webView
// has been assigned.  We should ensure that it's defined before any of these
// methods get called.
//
// TODO: ensure this._webView is defined before its methods are called.
// https://github.com/mozilla/positron/issues/74

const GuestWebContentsPrototype = {
  _webView: null,
  _url: null,
  _canGoBack: false,
  _canGoForward: false,

  attachWebViewToGuest(webView) {
    this._webView = webView;

    // Determine the current value of the canGoBack and canGoForward properties.
    // The asynchronous calls to getCanGoBack and getCanGoForward can race
    // a mozbrowserlocationchange event, which itself updates those properties.
    // So we listen for that event while awaiting the results and only update
    // the properties if there hasn't already been a location change.
    let locationChanged = false;
    this._webView.browserPluginNode.getCanGoBack()
    .then(canGoBack => {
      if (!locationChanged) {
        this._canGoBack = !!canGoBack;
      }
    });
    this._webView.browserPluginNode.getCanGoForward()
    .then(canGoForward => {
      if (!locationChanged) {
        this._canGoForward = !!canGoForward;
      }
    });

    // Listen for the first location change so we can ignore the results
    // of the asynchronous calls to getCanGoBack and getCanGoForward above.
    // The second mozbrowserlocationchange listener below handles ongoing
    // state changes.
    this._webView.browserPluginNode.addEventListener("mozbrowserlocationchange", function() {
      locationChanged = true;
    }, { once: true });

    let onBrowserLocationChange = (e) => {
      // TODO: Use an event that give us more information so we can fill in
      // inPage and replaceEntry arguments.
      // https://github.com/mozilla/positron/issues/98
      this.emit('navigation-entry-commited', e, e.detail.url, /*inPage*/ false, /*replaceEntry*/ false);
      this._url = e.detail.url;
      this._canGoBack = e.detail.canGoBack;
      this._canGoForward = e.detail.canGoForward;
    };
    this._webView.browserPluginNode.addEventListener("mozbrowserlocationchange", onBrowserLocationChange);
  },

  isGuest() { return true },

  _getURL: function() {
    return this._url;
  },

  _loadURL: function(url) {
    this._webView.browserPluginNode.setAttribute('src', url);
  },

  getTitle: positronUtil.makeStub('WebContents.getTitle', { returnValue: '' }),

  // This appears to be an internal API that gets called by the did-attach
  // handler in guest-view-manager.js and that calls guest_delegate_->SetSize
  // in atom_api_web_contents.cc.
  setSize: positronUtil.makeStub('WebContents.setSize'),

  canGoBack() {
    if (!this._webView) {
      console.warn('WebContents.canGoBack not yet available for guest WebContents');
      return false;
    }

    return this._canGoBack;
  },

  goBack() {
    if (!this._webView) {
      console.warn('WebContents.goBack not yet available for guest WebContents');
      return;
    }
    return this._webView.browserPluginNode.goBack();
  },

  canGoForward() {
    if (!this._webView) {
      console.warn('WebContents.canGoForward not yet available for guest WebContents');
      return false;
    }

    return this._canGoForward;
  },

  goForward() {
    if (!this._webView) {
      console.warn('WebContents.goForward not yet available for guest WebContents');
      return;
    }
    return this._webView.browserPluginNode.goForward();
  },

  stop() {
    return this._webView.browserPluginNode.stop();
  },

  reload() {
    return this._webView.browserPluginNode.reload();
  },

};

let lastWebContentsID = 0;

function WebContents(options) {
  this._id = ++lastWebContentsID;

  Object.assign(this, WebContentsPrototype);

  if (options.isGuest) {
    Object.assign(this, GuestWebContentsPrototype);
    this.partition = options.partition;
    this.embedder = options.embedder;
  } else {
    Object.assign(this, BrowserWindowWebContentsPrototype);
    this._browserWindow = options.browserWindow;
  }
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
