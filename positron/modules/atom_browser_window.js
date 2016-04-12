/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// According to Electron's Quick Start tutorial
// <http://electron.atom.io/docs/latest/tutorial/quick-start/>,
// a browser window will be closed when its BrowserWindow instance is GCed.
// But this behavior isn't mentioned in the API reference
// <http://electron.atom.io/docs/v0.37.4/api/browser-window/>.
// And BrowserWindow has a getAllWindows() class method, which implies
// that it retains a reference to a BrowserWindow instance. So it isn't clear
// if the behavior is intentional.
//
// In any case, if we want to support it, then we'll need to re-implement
// BrowserWindow natively, since JS doesn't support destructors.  Unless we
// make BrowserWindow an XPCOM component that nsISupportsWeakReference
// and hold a weak reference to it that we periodically poll to determine
// when it's been GCed.

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const windowWatcher = Cc['@mozilla.org/embedcomp/window-watcher;1'].
                      getService(Ci.nsIWindowWatcher);

const WebContents = require('electron').webContents;
const app = process.atomBinding('app').app;

const DEFAULT_URL = 'chrome://positron/content/shell.html';
const DEFAULT_WINDOW_FEATURES = [
  'chrome',
  'close',
  'dialog=no',
  'extrachrome',
  'resizable',
  'scrollbars',
];

// Map from nsIDOMWindow instances to BrowserWindow instances.
let browserWindows = new Map();

function BrowserWindow(options) {
  var features = DEFAULT_WINDOW_FEATURES.slice();

  if ('width' in options) {
    features.push('width=' + options.width);
  }

  if ('height' in options) {
    features.push('height=' + options.height);
  }

  this.webContents = WebContents.create({
    browserWindow: this,
  });

  this._domWindow = windowWatcher.openWindow(null, DEFAULT_URL, '_blank', features.join(','), null);
  browserWindows.set(this._domWindow, this);
}

windowWatcher.registerNotification(function observe(subject, topic, data) {
  switch(topic) {
    case 'domwindowopened':
      break;
    case 'domwindowclosed': {
      let domWindow = subject.QueryInterface(Ci.nsIDOMWindow);
      let browserWindow = browserWindows.get(domWindow);
      browserWindow.emit('closed');
      browserWindows.delete(domWindow);

      // This assumes that the BrowserWindow module was loaded before any
      // windows were opened.  That's a safe assumption while the module
      // is available only to the browser process, since it needs to load
      // this module to open the first window.  But once this module becomes
      // available to renderer processes, it may no longer be safe
      // (depending on whether this module is reused to implement BrowserWindow
      // in those processes).
      if (browserWindows.size === 0) {
        app.emit('window-all-closed');
      }

      break;
    }
  }
});

exports.BrowserWindow = BrowserWindow;
exports._setDeprecatedOptionsCheck = function() { /* stub */ };
