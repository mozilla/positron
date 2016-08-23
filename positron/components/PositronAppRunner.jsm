/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const nsIAppStartup            = Ci.nsIAppStartup;

const NS_OK = Cr.NS_OK;

Cu.import("resource://gre/modules/NetUtil.jsm");
Cu.import("resource://gre/modules/osfile.jsm");
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource:///modules/ModuleLoader.jsm");

this.EXPORTED_SYMBOLS = ["PositronAppRunner"];

// Electron doesn't require apps to keep a window open, even on Windows/Linux.
// Apps can run headlessly or only display a Tray.  And they don't open a window
// by default on startup.  So we need to keep Gecko running regardless.
Services.startup.enterLastWindowClosingSurvivalArea();

function quit() {
  let appStartup = Cc["@mozilla.org/toolkit/app-startup;1"]
                     .getService(nsIAppStartup);
  appStartup.quit(nsIAppStartup.eForceQuit);
}

this.PositronAppRunner = function PositronAppRunner() {
  this._packageJSON = null;
}

PositronAppRunner.prototype = {
  _unboundLoadAndContinue: function par_unboundLoadAndContinue(aThenWhat,
                                                               aInputStream,
                                                               aStatusCode,
                                                               aRequest) {
    if (aStatusCode != NS_OK) {
      dump("A load failed!\n");
      // Load failed somehow.
      quit();
      return;
    }

    let data = NetUtil.readInputStreamToString(aInputStream,
    	                                       aInputStream.available());
    return aThenWhat(data);
  },

  _loadAndContinue: function par_LoadAndContinue(aThenWhat) {
    return this._unboundLoadAndContinue.bind(this, aThenWhat.bind(this));
  },

  _loadMainScript: function par_LoadMainScript(aScriptPath) {
    let mainScriptPath = OS.Path.join(this._baseDir.path, aScriptPath);
    let mainScriptURI = OS.Path.toFileURI(mainScriptPath);
    let requirer = {
      id: "resource:///modules/PositronAppRunner.jsm",
      exports: {},
    };
    let nodeLoader = Cc["@mozilla.org/positron/nodeloader;1"]
                     .getService(Ci.nsINodeLoader);
    nodeLoader.init("browser");
  },

  _parsePackageJSON: function par_parsePackageJSON(aData) {
    dump("Package JSON: " + aData + "\n");

    try {
      this._parsedJSON = JSON.parse(aData);
    } catch (e) {
      dump("package.json parse failed!\n");
      quit();
      return;
    }

    let mainScript = this._parsedJSON["main"];
    this._loadMainScript(mainScript);
  },

  run: function par_Run(appBaseDir, appPackageJSON) {
    this._baseDir = appBaseDir;
    this._packageJSON = appPackageJSON;

    NetUtil.asyncFetch(appPackageJSON,
	               this._loadAndContinue(this._parsePackageJSON));
  }
}
