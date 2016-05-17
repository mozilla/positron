/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- /
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

Components.utils.import("resource://gre/modules/Services.jsm");
Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");
try {
  Components.utils.import("resource:///modules/PositronAppRunner.jsm");
}catch (e) {
  dump("Exception: " + e + "\n");
}

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

function loadApp(appBaseDir, appPackageJSON) {
  let appRunner = new PositronAppRunner();
  try {
    appRunner.run(appBaseDir, appPackageJSON);
  } catch (e) {
    dump("Exception: " + e + "\n");
  }
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

    // Firefox, in nsBrowserContentHandler, has a more robust handler
    // for the --chrome flag, which tries to correct typos in the URL
    // being loaded.  But we only need to handle loading devtools in a separate
    // process to debug Positron itself, so our implementation is simpler.
    var chromeParam = cmdLine.handleFlagWithParam("chrome", false);
    if (chromeParam) {
      try {
        let resolvedURI = cmdLine.resolveURI(chromeParam);

        let isLocal = uri => {
          let localSchemes = new Set(["chrome", "file", "resource"]);
          if (uri instanceof Components.interfaces.nsINestedURI) {
            uri = uri.QueryInterface(Components.interfaces.nsINestedURI).innerMostURI;
          }
          return localSchemes.has(uri.scheme);
        };
        if (isLocal(resolvedURI)) {
          // If the URI is local, we are sure it won't wrongly inherit chrome privs
          var features = "chrome,dialog=no,all";
          Services.ww.openWindow(null, resolvedURI.spec, "_blank", features, null);
          cmdLine.preventDefault = true;
          return;
        } else {
          dump("*** Preventing load of web URI as chrome\n");
          dump("    If you're trying to load a webpage, do not pass --chrome.\n");
        }
      }
      catch (e) {
        dump(e + '\n');
      }
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

    if (cmdLine.findFlag("jsdebugger", false) === -1) {
      loadApp(appBaseDir, appPackageJSON);
    } else {
      // The command line includes --jsdebugger, which means devtools-startup
      // will start a toolbox process.  So we wait for that to finish starting
      // up before continuing, to enable Positron developers to debug startup.
      let observe = function(subject, topic, data) {
        Services.obs.removeObserver(observe, "devtools-thread-resumed");
        loadApp(appBaseDir, appPackageJSON);
      };
      Services.obs.addObserver(observe, "devtools-thread-resumed", false);
    }
  },

  helpInfo : "",
};

this.NSGetFactory = XPCOMUtils.generateNSGetFactory([PositronCLH]);
