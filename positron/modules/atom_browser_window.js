/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

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

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

const ppmm = Cc['@mozilla.org/parentprocessmessagemanager;1'].
             getService(Ci.nsIMessageBroadcaster);

const windowWatcher = Cc['@mozilla.org/embedcomp/window-watcher;1'].
                      getService(Ci.nsIWindowWatcher);

Cu.import('resource://gre/modules/Services.jsm');
Cu.import('resource:///modules/ModuleLoader.jsm');

const WebContents = require('electron').webContents;
const app = process.atomBinding('app').app;
const positronUtil = process.positronBinding('positron_util');
const webViewManager = process.atomBinding('web_view_manager');

const DEFAULT_URL = 'chrome://positron/content/shell.html';
const DEFAULT_WINDOW_FEATURES = [
  'chrome',
  'close',
  'dialog=no',
  'extrachrome',
  'resizable',
  'scrollbars',
];

// Map of DOM window instances to BrowserWindow instances.
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

  ppmm.addMessageListener('ipc-message', this);
  ppmm.addMessageListener('ipc-message-sync', this);

  this._domWindow = windowWatcher.openWindow(null, DEFAULT_URL, '_blank', features.join(','), null);
  browserWindows.set(this._domWindow, this);
}

BrowserWindow.prototype = {
  isVisible: positronUtil.makeStub('BrowserWindow.isVisible', { returnValue: true }),
  isMinimized: positronUtil.makeStub('BrowserWindow.isMinimized', { returnValue: false }),

  _send: function(channel, args) {
    ppmm.broadcastAsyncMessage('ipc-message', [channel].concat(args), { window: this._domWindow });
  },

  _loadURL: function(url) {
    // Observe document-element-inserted and eagerly create the module loader,
    // so <webview> is available by the time the document loads.
    const observer = {
      observe: (subject, topic, data) => {
        // Although we add this observer right before loading the URL
        // (and remove it right afterward), we're still notified asynchronously
        // about the insertion, and we might get notified about others
        // in the meantime, so we need to confirm that the document in question
        // is ours.
        if (subject.defaultView !== this._domWindow) {
          return;
        }

        Services.obs.removeObserver(observer, 'document-element-inserted');

        // Ignore the return value, since we're only calling getLoaderForWindow
        // for its side-effect of creating a new loader for the window.
        ModuleLoader.getLoaderForWindow(subject.defaultView);
      },
    };
    Services.obs.addObserver(observer, 'document-element-inserted', false);

    this._domWindow.location = url;
  },
};

// nsIMessageListener
BrowserWindow.prototype.receiveMessage = function(message) {
  // All our windows are in the same process, so we can't listen to a process-
  // specific message manager to receive messages from only our window.
  // Nor do frame message managers meet our needs (among other reasons, they
  // don't provide sendSyncMessage).
  //
  // So we listen to the global parent-process message manager and identify
  // messages from our window by making the window pass a reference to itself
  // in the message.objects argument.
  //
  if (message.objects.window !== this._domWindow) {
    return;
  }

  // For synchronous messages, the value to return.
  let returnValue;

  let event = {
    sender: this.webContents,

    // The Electron API tells recipients to set the returnValue property
    // of the event object, and the web-contents module defines that property
    // as a setter that calls event.sendReply; so we implement sendReply
    // to capture the value.
    sendReply: function(reply) {
      returnValue = reply;
    }
  };

  this.webContents.emit.apply(this.webContents, [message.name, event, message.data]);

  return returnValue;
};

windowWatcher.registerNotification(function observe(subject, topic, data) {
  switch(topic) {
    case 'domwindowopened':
      break;
    case 'domwindowclosed': {
      let browserWindow = browserWindows.get(subject);
      if (!browserWindow) {
        // The window might be a devtools window, in which case it won't be
        // in our list of browser windows.
        // TODO: figure out if window-all-closed respects devtools windows,
        // in which case the conditional that determines whether or not to emit
        // that event will need to change.
        return;
      }

      browserWindow.emit('closed');
      browserWindows.delete(subject);
      ppmm.removeMessageListener('ipc-message', browserWindow);
      ppmm.removeMessageListener('ipc-message-sync', browserWindow);

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

ppmm.addMessageListener('positron-register-web-view', {
  receiveMessage(message) {
    webViewManager.attachWebViewToGuest(message.objects.webView);
  }
});

exports.BrowserWindow = BrowserWindow;
exports._setDeprecatedOptionsCheck = positronUtil.makeStub('atom_browser_window._setDeprecatedOptionsCheck');
