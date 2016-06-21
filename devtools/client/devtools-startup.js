/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * This XPCOM component is loaded very early.
 * It handles command line arguments like -jsconsole, but also ensures starting
 * core modules like 'devtools-browser.js' that hooks the browser windows
 * and ensure setting up tools.
 *
 * Be careful to lazy load dependencies as much as possible.
 **/

"use strict";

const { classes: Cc, interfaces: Ci, utils: Cu } = Components;
const kDebuggerPrefs = [
  "devtools.debugger.remote-enabled",
  "devtools.chrome.enabled"
];
const { XPCOMUtils } = Cu.import("resource://gre/modules/XPCOMUtils.jsm", {});
XPCOMUtils.defineLazyModuleGetter(this, "Services", "resource://gre/modules/Services.jsm");

function DevToolsStartup() {}

DevToolsStartup.prototype = {
  handle: function (cmdLine) {
    let consoleFlag = cmdLine.handleFlag("jsconsole", false);
    let debuggerFlag = cmdLine.handleFlag("jsdebugger", false);
    let devtoolsFlag = cmdLine.handleFlag("devtools", false);

    if (consoleFlag) {
      this.handleConsoleFlag(cmdLine);
    }
    if (debuggerFlag) {
      this.handleDebuggerFlag(cmdLine);
    }
    let debuggerServerFlag;
    try {
      debuggerServerFlag =
        cmdLine.handleFlagWithParam("start-debugger-server", false);
    } catch (e) {
      // We get an error if the option is given but not followed by a value.
      // By catching and trying again, the value is effectively optional.
      debuggerServerFlag = cmdLine.handleFlag("start-debugger-server", false);
    }
    if (debuggerServerFlag) {
      this.handleDebuggerServerFlag(cmdLine, debuggerServerFlag);
    }

    let onStartup = function (window) {
      Services.obs.removeObserver(onStartup,
                                  "browser-delayed-startup-finished");
      // Ensure loading core module once firefox is ready
      this.initDevTools();

      if (devtoolsFlag) {
        this.handleDevToolsFlag(window);
      }
    }.bind(this);
    Services.obs.addObserver(onStartup, "browser-delayed-startup-finished",
                             false);
  },

  initDevTools: function () {
    let { loader } = Cu.import("resource://devtools/shared/Loader.jsm", {});
    // Ensure loading main devtools module that hooks up into browser UI
    // and initialize all devtools machinery.
    loader.require("devtools/client/framework/devtools-browser");
  },

  handleConsoleFlag: function (cmdLine) {
    let window = Services.wm.getMostRecentWindow("devtools:webconsole");
    if (!window) {
      this.initDevTools();

      let { require } = Cu.import("resource://devtools/shared/Loader.jsm", {});
      let hudservice = require("devtools/client/webconsole/hudservice");
      let { console } = Cu.import("resource://gre/modules/Console.jsm", {});
      hudservice.toggleBrowserConsole().then(null, console.error);
    } else {
      // the Browser Console was already open
      window.focus();
    }

    if (cmdLine.state == Ci.nsICommandLine.STATE_REMOTE_AUTO) {
      cmdLine.preventDefault = true;
    }
  },

  // Open the toolbox on the selected tab once the browser starts up.
  handleDevToolsFlag: function (window) {
    const {require} = Cu.import("resource://devtools/shared/Loader.jsm", {});
    const {gDevTools} = require("devtools/client/framework/devtools");
    const {TargetFactory} = require("devtools/client/framework/target");
    let target = TargetFactory.forTab(window.gBrowser.selectedTab);
    gDevTools.showToolbox(target);
  },

  _isRemoteDebuggingEnabled() {
    let remoteDebuggingEnabled = false;
    try {
      remoteDebuggingEnabled = kDebuggerPrefs.every(pref => {
        return Services.prefs.getBoolPref(pref);
      });
    } catch (ex) {
      console.error(ex);
      return false;
    }
    if (!remoteDebuggingEnabled) {
      let errorMsg = "Could not run chrome debugger! You need the following " +
                     "prefs to be set to true: " + kDebuggerPrefs.join(", ");
      console.error(new Error(errorMsg));
      // Dump as well, as we're doing this from a commandline, make sure people
      // don't miss it:
      dump(errorMsg + "\n");
    }
    return remoteDebuggingEnabled;
  },

  handleDebuggerFlag: function (cmdLine) {
    if (!this._isRemoteDebuggingEnabled()) {
      return;
    }

    // TODO: upstream this to mozilla-central so it isn't Positron-specific.
    let devtoolsThreadResumed = false;
    let observe = function(subject, topic, data) {
      devtoolsThreadResumed = true;
      Services.obs.removeObserver(observe, "devtools-thread-resumed");
    };
    Services.obs.addObserver(observe, "devtools-thread-resumed", false);

    const { BrowserToolboxProcess } = Cu.import("resource://devtools/client/framework/ToolboxProcess.jsm", {});
    BrowserToolboxProcess.init();

    // Spin the event loop until the debugger connects.
    let thread = Cc["@mozilla.org/thread-manager;1"].getService().currentThread;
    while (!devtoolsThreadResumed) {
      thread.processNextEvent(true);
    }

    if (cmdLine.state == Ci.nsICommandLine.STATE_REMOTE_AUTO) {
      cmdLine.preventDefault = true;
    }
  },

  handleDebuggerServerFlag: function (cmdLine, portOrPath) {
    if (!this._isRemoteDebuggingEnabled()) {
      return;
    }
    if (portOrPath === true) {
      // Default to TCP port 6000 if no value given
      portOrPath = 6000;
    }
    let { DevToolsLoader } =
      Cu.import("resource://devtools/shared/Loader.jsm", {});

    try {
      // Create a separate loader instance, so that we can be sure to receive
      // a separate instance of the DebuggingServer from the rest of the
      // devtools.  This allows us to safely use the tools against even the
      // actors and DebuggingServer itself, especially since we can mark
      // serverLoader as invisible to the debugger (unlike the usual loader
      // settings).
      let serverLoader = new DevToolsLoader();
      serverLoader.invisibleToDebugger = true;
      let { DebuggerServer: debuggerServer } =
        serverLoader.require("devtools/server/main");
      debuggerServer.init();
      debuggerServer.addBrowserActors();
      debuggerServer.allowChromeProcess = true;

      let listener = debuggerServer.createListener();
      listener.portOrPath = portOrPath;
      listener.open();
      dump("Started debugger server on " + portOrPath + "\n");
    } catch (e) {
      dump("Unable to start debugger server on " + portOrPath + ": " + e);
    }

    if (cmdLine.state == Ci.nsICommandLine.STATE_REMOTE_AUTO) {
      cmdLine.preventDefault = true;
    }
  },

  helpInfo: "  --jsconsole        Open the Browser Console.\n" +
            "  --jsdebugger       Open the Browser Toolbox.\n" +
            "  --devtools         Open DevTools on initial load.\n" +
            "  --start-debugger-server [port|path] " +
            "Start the debugger server on a TCP port or " +
            "Unix domain socket path.  Defaults to TCP port 6000.\n",

  classID: Components.ID("{9e9a9283-0ce9-4e4a-8f1c-ba129a032c32}"),
  QueryInterface: XPCOMUtils.generateQI([Ci.nsICommandLineHandler]),
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory(
  [DevToolsStartup]);
