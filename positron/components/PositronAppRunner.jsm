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
Cu.import("resource://gre/modules/Services.jsm");
Cu.import("resource:///modules/Require.jsm");

this.EXPORTED_SYMBOLS = ["PositronAppRunner"];

function quit() {
  let appStartup = Cc["@mozilla.org/toolkit/app-startup;1"]
                     .getService(nsIAppStartup);
  appStartup.quit(nsIAppStartup.eForceQuit);
}

this.PositronAppRunner = function PositronAppRunner() {
  this._packageJSON = null;
  this._sandbox = null;
  this._mainScript = null;
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

  _executeMainScript: function par_ExecuteMainScript(aData) {
    dump("Main script is " + aData + "\n");
    try {
      Cu.evalInSandbox(aData, this._sandbox, "1.8", this._mainScript.path, 1);
    } catch(e) {
      dump("Error evaluating main script: " + e + "\n" + e.stack + "\n");
      quit();
    }
  },

  _loadMainScript: function par_LoadMainScript(aScriptName) {
    this._mainScript = this._baseDir.clone();
    this._mainScript.append(aScriptName);

    let systemPrincipal = Cc["@mozilla.org/systemprincipal;1"].
                          createInstance(Ci.nsIPrincipal);

    this._sandbox = new Cu.Sandbox(systemPrincipal, {
      sandboxName: aScriptName,
      wantComponents: false
    });

    this._sandbox.require = new Require({ id: Services.io.newFileURI(this._mainScript).spec });

    NetUtil.asyncFetch(this._mainScript,
	               this._loadAndContinue(this._executeMainScript));
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
