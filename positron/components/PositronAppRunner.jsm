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

this.EXPORTED_SYMBOLS = ["PositronAppRunner"];

function quit() {
  let appStartup = Cc["@mozilla.org/toolkit/app-startup;1"]
                     .getService(nsIAppStartup);
  appStartup.quit(nsIAppStartup.eForceQuit);
}

this.PositronAppRunner = function PositronAppRunner() {
}

PositronAppRunner.prototype = {
  _continueWithPackageJSON: function par_ContinueWithPackageJSON(aInputStream,
  		                                                 aStatusCode,
								 aRequest) {
    if (aStatusCode != NS_OK) {
      // Load failed somehow.
      quit();
      return;
    }

    let json = NetUtil.readInputStreamToString(aInputStream,
    	                                       aInputStream.available());
    dump("Package JSON: " + json + "\n");

    let parsedJSON;
    try {
      parsedJSON = JSON.parse(json);
    } catch (e) {
      dump("package.json parse failed!\n");
      quit();
      return;
    }

    let mainScript = parsedJSON["main"];
    dump("main script is " + mainScript + "\n");
    dump("One day I'll execute it!\n\n\n");
    quit();
    return;
  },

  run: function par_Run(appBaseDir, appPackageJSON) {
    this.baseDir = appBaseDir;
    this.packageJSON = appPackageJSON;

    NetUtil.asyncFetch(appPackageJSON, this._continueWithPackageJSON.bind(this));
  }
}
