'use strict';

const EventEmitter = require('events').EventEmitter;
const deprecate = require('electron').deprecate;
const ipcMain = require('electron').ipcMain;
const NavigationController = require('electron').NavigationController;
const Menu = require('electron').Menu;

const binding = process.atomBinding('web_contents');
const debuggerBinding = process.atomBinding('debugger');

let nextId = 0;

let getNextId = function() {
  return ++nextId;
};

let PDFPageSize = {
  A5: {
    custom_display_name: "A5",
    height_microns: 210000,
    name: "ISO_A5",
    width_microns: 148000
  },
  A4: {
    custom_display_name: "A4",
    height_microns: 297000,
    name: "ISO_A4",
    is_default: "true",
    width_microns: 210000
  },
  A3: {
    custom_display_name: "A3",
    height_microns: 420000,
    name: "ISO_A3",
    width_microns: 297000
  },
  Legal: {
    custom_display_name: "Legal",
    height_microns: 355600,
    name: "NA_LEGAL",
    width_microns: 215900
  },
  Letter: {
    custom_display_name: "Letter",
    height_microns: 279400,
    name: "NA_LETTER",
    width_microns: 215900
  },
  Tabloid: {
    height_microns: 431800,
    name: "NA_LEDGER",
    width_microns: 279400,
    custom_display_name: "Tabloid"
  }
};

// Following methods are mapped to webFrame.
const webFrameMethods = [
  'insertText',
  'setZoomFactor',
  'setZoomLevel',
  'setZoomLevelLimits'
];

let wrapWebContents = function(webContents) {
  // webContents is an EventEmitter.
  var controller, method, name, ref1;
  webContents.__proto__ = EventEmitter.prototype;

  // Every remote callback from renderer process would add a listenter to the
  // render-view-deleted event, so ignore the listenters warning.
  webContents.setMaxListeners(0);

  // WebContents::send(channel, args..)
  webContents.send = function(channel, ...args) {
    if (channel == null) {
      throw new Error('Missing required channel argument');
    }
    return this._send(channel, args);
  };

  // The navigation controller.
  controller = new NavigationController(webContents);
  ref1 = NavigationController.prototype;
  for (name in ref1) {
    method = ref1[name];
    if (method instanceof Function) {
      (function(name, method) {
        return webContents[name] = function() {
          return method.apply(controller, arguments);
        };
      })(name, method);
    }
  }

  // Mapping webFrame methods.
  for (let method2 of webFrameMethods) {
    // Redeclare variable within block to work around Mozilla bug 449811.
    // TODO: remove workaround once Mozilla bug is fixed.
    // https://github.com/mozilla/positron/issues/68
    let method = method2;

    webContents[method] = function(...args) {
      this.send('ELECTRON_INTERNAL_RENDERER_WEB_FRAME_METHOD', method, args);
    };
  }

  const asyncWebFrameMethods = function(requestId, method, callback, ...args) {
    this.send('ELECTRON_INTERNAL_RENDERER_ASYNC_WEB_FRAME_METHOD', requestId, method, args);
    ipcMain.once(`ELECTRON_INTERNAL_BROWSER_ASYNC_WEB_FRAME_RESPONSE_${requestId}`, function(event, result) {
      if (callback)
        callback(result);
    });
  };

  // Make sure webContents.executeJavaScript would run the code only when the
  // webContents has been loaded.
  webContents.executeJavaScript = function(code, hasUserGesture, callback) {
    let requestId = getNextId();
    if (typeof hasUserGesture === "function") {
      callback = hasUserGesture;
      hasUserGesture = false;
    }
    if (this.getURL() && !this.isLoading())
      return asyncWebFrameMethods.call(this, requestId, "executeJavaScript", callback, code, hasUserGesture);
    else
      return this.once('did-finish-load', asyncWebFrameMethods.bind(this, requestId, "executeJavaScript", callback, code, hasUserGesture));
  };

  // Dispatch IPC messages to the ipc module.
  webContents.on('ipc-message', function(event, [channel, ...args]) {
    return ipcMain.emit.apply(ipcMain, [channel, event].concat(args));
  });
  webContents.on('ipc-message-sync', function(event, [channel, ...args]) {
    Object.defineProperty(event, 'returnValue', {
      set: function(value) {
        return event.sendReply(JSON.stringify(value));
      }
    });
    return ipcMain.emit.apply(ipcMain, [channel, event].concat(args));
  });

  // Handle context menu action request from pepper plugin.
  webContents.on('pepper-context-menu', function(event, params) {
    var menu;
    menu = Menu.buildFromTemplate(params.menu);
    return menu.popup(params.x, params.y);
  });

  // This error occurs when host could not be found.
  webContents.on('did-fail-provisional-load', function(...args) {
    // Calling loadURL during this event might cause crash, so delay the event
    // until next tick.
    setImmediate(() => {
      this.emit.apply(this, ['did-fail-load'].concat(args));
    });
  });

  // Delays the page-title-updated event to next tick.
  webContents.on('-page-title-updated', function(...args) {
    setImmediate(() => {
      this.emit.apply(this, ['page-title-updated'].concat(args));
    });
  });

  // Deprecated.
  deprecate.rename(webContents, 'loadUrl', 'loadURL');
  deprecate.rename(webContents, 'getUrl', 'getURL');
  deprecate.event(webContents, 'page-title-set', 'page-title-updated', function(...args) {
    return this.emit.apply(this, ['page-title-set'].concat(args));
  });
  return webContents.printToPDF = function(options, callback) {
    var printingSetting;
    printingSetting = {
      pageRage: [],
      mediaSize: {},
      landscape: false,
      color: 2,
      headerFooterEnabled: false,
      marginsType: 0,
      isFirstRequest: false,
      requestID: getNextId(),
      previewModifiable: true,
      printToPDF: true,
      printWithCloudPrint: false,
      printWithPrivet: false,
      printWithExtension: false,
      deviceName: "Save as PDF",
      generateDraftData: true,
      fitToPageEnabled: false,
      duplex: 0,
      copies: 1,
      collate: true,
      shouldPrintBackgrounds: false,
      shouldPrintSelectionOnly: false
    };
    if (options.landscape) {
      printingSetting.landscape = options.landscape;
    }
    if (options.marginsType) {
      printingSetting.marginsType = options.marginsType;
    }
    if (options.printSelectionOnly) {
      printingSetting.shouldPrintSelectionOnly = options.printSelectionOnly;
    }
    if (options.printBackground) {
      printingSetting.shouldPrintBackgrounds = options.printBackground;
    }
    if (options.pageSize && PDFPageSize[options.pageSize]) {
      printingSetting.mediaSize = PDFPageSize[options.pageSize];
    } else {
      printingSetting.mediaSize = PDFPageSize['A4'];
    }
    return this._printToPDF(printingSetting, callback);
  };
};

// Wrapper for native class.
let wrapDebugger = function(webContentsDebugger) {
  // debugger is an EventEmitter.
  webContentsDebugger.__proto__ = EventEmitter.prototype;
};

binding._setWrapWebContents(wrapWebContents);
debuggerBinding._setWrapDebugger(wrapDebugger);

module.exports.create = function(options) {
  if (options == null) {
    options = {};
  }
  return binding.create(options);
};
