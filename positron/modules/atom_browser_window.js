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

const DEFAULT_URL = 'chrome://positron/content/shell.html';
const DEFAULT_WINDOW_FEATURES = [
  'chrome',
  'close',
  'dialog=no',
  'extrachrome',
  'resizable',
  'scrollbars',
];

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
}

exports.BrowserWindow = BrowserWindow;
exports._setDeprecatedOptionsCheck = function() { /* stub */ };
