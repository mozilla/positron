/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- /
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");
Components.utils.import("resource:///modules/PositronAppRunner.jsm");

const nsISupports              = Components.interfaces.nsISupports;

const nsIAppStartup            = Components.interfaces.nsIAppStartup;
const nsICommandLine           = Components.interfaces.nsICommandLine;
const nsICommandLineHandler    = Components.interfaces.nsICommandLineHandler;
const nsISupportsString        = Components.interfaces.nsISupportsString;
const nsIProperties            = Components.interfaces.nsIProperties;
const nsIFile                  = Components.interfaces.nsIFile;
const nsISimpleEnumerator      = Components.interfaces.nsISimpleEnumerator;

const NS_ERROR_INVALID_ARG     = Components.results.NS_ERROR_INVALID_ARG;

function getDirectoryService()
{
  return Components.classes["@mozilla.org/file/directory_service;1"]
                   .getService(nsIProperties);
}

function quit()
{
  let appStartup = Components.classes["@mozilla.org/toolkit/app-startup;1"]
                             .getService(nsIAppStartup);
  appStartup.quit(nsIAppStartup.eForceQuit);
}

function PositronCLH() { }
PositronCLH.prototype = {
  classID: Components.ID("{dd20d954-f2a9-11e5-aeae-782bcb9e2c3f}"),

  /* nsISupports */

  QueryInterface : XPCOMUtils.generateQI([nsICommandLineHandler]),

  /* nsICommandLineHandler */

  handle : function clh_handle(cmdLine) {
    var printDir;
    while ((printDir = cmdLine.handleFlagWithParam("print-xpcom-dir", false))) {
      var out = "print-xpcom-dir(\"" + printDir + "\"): ";
      try {
        out += getDirectoryService().get(printDir, nsIFile).path;
      }
      catch (e) {
        out += "<Not Provided>";
      }

      dump(out + "\n");
      Components.utils.reportError(out);
    }

    var printDirList;
    while ((printDirList = cmdLine.handleFlagWithParam("print-xpcom-dirlist",
                                                       false))) {
      out = "print-xpcom-dirlist(\"" + printDirList + "\"): ";
      try {
        var list = getDirectoryService().get(printDirList,
                                             nsISimpleEnumerator);
        while (list.hasMoreElements())
          out += list.getNext().QueryInterface(nsIFile).path + ";";
      }
      catch (e) {
        out += "<Not Provided>";
      }

      dump(out + "\n");
      Components.utils.reportError(out);
    }

    let appPath;
    try {
      appPath = cmdLine.getArgument(0);
    } catch (e) {
      if (e.result == NS_ERROR_INVALID_ARG) {
        dump("no app provided\n");
      } else {
        dump("found exception " + e + "\n");
      }
      quit();
      return;
    }

    dump("Loading app at " + appPath + "\n");

    let appBaseDir = cmdLine.resolveFile(appPath);
    if (!appBaseDir || !appBaseDir.exists()) {
      dump("App at '" + appPath + "' does not exist!\n");
      quit();
      return;
    }

    let appPackageJSON = appBaseDir.clone();
    appPackageJSON.append("package.json");
    if (!appPackageJSON || !appPackageJSON.exists()) {
      dump("App at '" + appPath + "' has no package.json!\n");
      quit();
      return;
    }

    let appRunner = new PositronAppRunner();
    appRunner.run(appBaseDir, appPackageJSON);
  },

  helpInfo : "",
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([PositronCLH]);
