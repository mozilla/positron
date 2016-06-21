/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

var AM_Cc = Components.classes;
var AM_Ci = Components.interfaces;
var AM_Cu = Components.utils;

const CERTDB_CONTRACTID = "@mozilla.org/security/x509certdb;1";
const CERTDB_CID = Components.ID("{fb0bbc5c-452e-4783-b32c-80124693d871}");

const PREF_EM_CHECK_UPDATE_SECURITY   = "extensions.checkUpdateSecurity";
const PREF_EM_STRICT_COMPATIBILITY    = "extensions.strictCompatibility";
const PREF_EM_MIN_COMPAT_APP_VERSION      = "extensions.minCompatibleAppVersion";
const PREF_EM_MIN_COMPAT_PLATFORM_VERSION = "extensions.minCompatiblePlatformVersion";
const PREF_GETADDONS_BYIDS               = "extensions.getAddons.get.url";
const PREF_GETADDONS_BYIDS_PERFORMANCE   = "extensions.getAddons.getWithPerformance.url";
const PREF_XPI_SIGNATURES_REQUIRED    = "xpinstall.signatures.required";

// Forcibly end the test if it runs longer than 15 minutes
const TIMEOUT_MS = 900000;

// Maximum error in file modification times. Some file systems don't store
// modification times exactly. As long as we are closer than this then it
// still passes.
const MAX_TIME_DIFFERENCE = 3000;

// Time to reset file modified time relative to Date.now() so we can test that
// times are modified (10 hours old).
const MAKE_FILE_OLD_DIFFERENCE = 10 * 3600 * 1000;

Components.utils.import("resource://gre/modules/addons/AddonRepository.jsm");
Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");
Components.utils.import("resource://gre/modules/FileUtils.jsm");
Components.utils.import("resource://gre/modules/Services.jsm");
Components.utils.import("resource://gre/modules/NetUtil.jsm");
Components.utils.import("resource://gre/modules/Promise.jsm");
Components.utils.import("resource://gre/modules/Task.jsm");
const { OS } = Components.utils.import("resource://gre/modules/osfile.jsm", {});
Components.utils.import("resource://gre/modules/AsyncShutdown.jsm");

XPCOMUtils.defineLazyModuleGetter(this, "Extension",
                                  "resource://gre/modules/Extension.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "HttpServer",
                                  "resource://testing-common/httpd.js");
XPCOMUtils.defineLazyModuleGetter(this, "MockRegistrar",
                                  "resource://testing-common/MockRegistrar.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "MockRegistry",
                                  "resource://testing-common/MockRegistry.jsm");


// We need some internal bits of AddonManager
var AMscope = Components.utils.import("resource://gre/modules/AddonManager.jsm", {});
var { AddonManager, AddonManagerInternal, AddonManagerPrivate } = AMscope;

// Mock out AddonManager's reference to the AsyncShutdown module so we can shut
// down AddonManager from the test
var MockAsyncShutdown = {
  hook: null,
  status: null,
  profileBeforeChange: {
    addBlocker: function(aName, aBlocker, aOptions) {
      do_print("Mock profileBeforeChange blocker for '" + aName + "'");
      MockAsyncShutdown.hook = aBlocker;
      MockAsyncShutdown.status = aOptions.fetchState;
    }
  },
  // We can use the real Barrier
  Barrier: AsyncShutdown.Barrier
};

AMscope.AsyncShutdown = MockAsyncShutdown;

var gInternalManager = null;
var gAppInfo = null;
var gAddonsList;

var gPort = null;
var gUrlToFileMap = {};

var TEST_UNPACKED = false;

// Map resource://xpcshell-data/ to the data directory
var resHandler = Services.io.getProtocolHandler("resource")
                         .QueryInterface(AM_Ci.nsISubstitutingProtocolHandler);
// Allow non-existent files because of bug 1207735
var dataURI = NetUtil.newURI(do_get_file("data", true));
resHandler.setSubstitution("xpcshell-data", dataURI);

function isManifestRegistered(file) {
  let manifests = Components.manager.getManifestLocations();
  for (let i = 0; i < manifests.length; i++) {
    let manifest = manifests.queryElementAt(i, AM_Ci.nsIURI);

    // manifest is the url to the manifest file either in an XPI or a directory.
    // We want the location of the XPI or directory itself.
    if (manifest instanceof AM_Ci.nsIJARURI) {
      manifest = manifest.JARFile.QueryInterface(AM_Ci.nsIFileURL).file;
    }
    else if (manifest instanceof AM_Ci.nsIFileURL) {
      manifest = manifest.file.parent;
    }
    else {
      continue;
    }

    if (manifest.equals(file))
      return true;
  }
  return false;
}

// Listens to messages from bootstrap.js telling us what add-ons were started
// and stopped etc. and performs some sanity checks that only installed add-ons
// are started etc.
this.BootstrapMonitor = {
  inited: false,

  // Contain the current state of add-ons in the system
  installed: new Map(),
  started: new Map(),

  // Contain the last state of shutdown and uninstall calls for an add-on
  stopped: new Map(),
  uninstalled: new Map(),

  startupPromises: [],
  installPromises: [],

  init() {
    this.inited = true;
    Services.obs.addObserver(this, "bootstrapmonitor-event", false);
  },

  shutdownCheck() {
    if (!this.inited)
      return;

    do_check_eq(this.started.size, 0);
  },

  clear(id) {
    this.installed.delete(id);
    this.started.delete(id);
    this.stopped.delete(id);
    this.uninstalled.delete(id);
  },

  promiseAddonStartup(id) {
    return new Promise(resolve => {
      this.startupPromises.push(resolve);
    });
  },

  promiseAddonInstall(id) {
    return new Promise(resolve => {
      this.installPromises.push(resolve);
    });
  },

  checkMatches(cached, current) {
    do_check_neq(cached, undefined);
    do_check_eq(current.data.version, cached.data.version);
    do_check_eq(current.data.installPath, cached.data.installPath);
    do_check_eq(current.data.resourceURI, cached.data.resourceURI);
  },

  checkAddonStarted(id, version = undefined) {
    let started = this.started.get(id);
    do_check_neq(started, undefined);
    if (version != undefined)
      do_check_eq(started.data.version, version);

    // Chrome should be registered by now
    let installPath = new FileUtils.File(started.data.installPath);
    let isRegistered = isManifestRegistered(installPath);
    do_check_true(isRegistered);
  },

  checkAddonNotStarted(id) {
    do_check_false(this.started.has(id));
  },

  checkAddonInstalled(id, version = undefined) {
    const installed = this.installed.get(id);
    notEqual(installed, undefined);
    if (version !== undefined) {
      equal(installed.data.version, version);
    }
    return installed;
  },

  checkAddonNotInstalled(id) {
    do_check_false(this.installed.has(id));
  },

  observe(subject, topic, data) {
    let info = JSON.parse(data);
    let id = info.data.id;
    let installPath = new FileUtils.File(info.data.installPath);

    // If this is the install event the add-ons shouldn't already be installed
    if (info.event == "install") {
      this.checkAddonNotInstalled(id);

      this.installed.set(id, info);

      for (let resolve of this.installPromises)
        resolve();
      this.installPromises = [];
    }
    else {
      this.checkMatches(this.installed.get(id), info);
    }

    // If this is the shutdown event than the add-on should already be started
    if (info.event == "shutdown") {
      this.checkMatches(this.started.get(id), info);

      this.started.delete(id);
      this.stopped.set(id, info);

      // Chrome should still be registered at this point
      let isRegistered = isManifestRegistered(installPath);
      do_check_true(isRegistered);

      // XPIProvider doesn't bother unregistering chrome on app shutdown but
      // since we simulate restarts we must do so manually to keep the registry
      // consistent.
      if (info.reason == 2 /* APP_SHUTDOWN */)
        Components.manager.removeBootstrappedManifestLocation(installPath);
    }
    else {
      this.checkAddonNotStarted(id);
    }

    if (info.event == "uninstall") {
      // Chrome should be unregistered at this point
      let isRegistered = isManifestRegistered(installPath);
      do_check_false(isRegistered);

      this.installed.delete(id);
      this.uninstalled.set(id, info)
    }
    else if (info.event == "startup") {
      this.started.set(id, info);

      // Chrome should be registered at this point
      let isRegistered = isManifestRegistered(installPath);
      do_check_true(isRegistered);

      for (let resolve of this.startupPromises)
        resolve();
      this.startupPromises = [];
    }
  }
}

function isNightlyChannel() {
  var channel = "default";
  try {
    channel = Services.prefs.getCharPref("app.update.channel");
  }
  catch (e) { }

  return channel != "aurora" && channel != "beta" && channel != "release" && channel != "esr";
}

function createAppInfo(ID, name, version, platformVersion="1.0") {
  let tmp = {};
  AM_Cu.import("resource://testing-common/AppInfo.jsm", tmp);
  tmp.updateAppInfo({
    ID, name, version, platformVersion,
    crashReporter: true,
    extraProps: {
      browserTabsRemoteAutostart: false,
    },
  });
  gAppInfo = tmp.getAppInfo();
}

function getManifestURIForBundle(file) {
  if (file.isDirectory()) {
    file.append("install.rdf");
    if (file.exists()) {
      return NetUtil.newURI(file);
    }

    file.leafName = "manifest.json";
    if (file.exists()) {
      return NetUtil.newURI(file);
    }

    throw new Error("No manifest file present");
  }

  let zip = AM_Cc["@mozilla.org/libjar/zip-reader;1"].
            createInstance(AM_Ci.nsIZipReader);
  zip.open(file);
  try {
    let uri = NetUtil.newURI(file);

    if (zip.hasEntry("install.rdf")) {
      return NetUtil.newURI("jar:" + uri.spec + "!/" + "install.rdf");
    }

    if (zip.hasEntry("manifest.json")) {
      return NetUtil.newURI("jar:" + uri.spec + "!/" + "manifest.json");
    }

    throw new Error("No manifest file present");
  }
  finally {
    zip.close();
  }
}

let getIDForManifest = Task.async(function*(manifestURI) {
  // Load it
  let inputStream = yield new Promise((resolve, reject) => {
    NetUtil.asyncFetch({
      uri: manifestURI,
      loadUsingSystemPrincipal: true,
    }, (inputStream, status) => {
      if (status != Components.results.NS_OK)
        reject(status);
      resolve(inputStream);
    });
  });

  // Get the data as a string
  let data = NetUtil.readInputStreamToString(inputStream, inputStream.available());

  if (manifestURI.spec.endsWith(".rdf")) {
    let rdfParser = AM_Cc["@mozilla.org/rdf/xml-parser;1"].
                    createInstance(AM_Ci.nsIRDFXMLParser)
    let ds = AM_Cc["@mozilla.org/rdf/datasource;1?name=in-memory-datasource"].
             createInstance(AM_Ci.nsIRDFDataSource);
    rdfParser.parseString(ds, manifestURI, data);

    let rdfService = AM_Cc["@mozilla.org/rdf/rdf-service;1"].
                     getService(AM_Ci.nsIRDFService);

    let rdfID = ds.GetTarget(rdfService.GetResource("urn:mozilla:install-manifest"),
                             rdfService.GetResource("http://www.mozilla.org/2004/em-rdf#id"),
                             true);
    return rdfID.QueryInterface(AM_Ci.nsIRDFLiteral).Value;
  }
  else {
    let manifest = JSON.parse(data);
    return manifest.applications.gecko.id;
  }
});

let gUseRealCertChecks = false;
function overrideCertDB(handler) {
  // Unregister the real database. This only works because the add-ons manager
  // hasn't started up and grabbed the certificate database yet.
  let registrar = Components.manager.QueryInterface(AM_Ci.nsIComponentRegistrar);
  let factory = registrar.getClassObject(CERTDB_CID, AM_Ci.nsIFactory);
  registrar.unregisterFactory(CERTDB_CID, factory);

  // Get the real DB
  let realCertDB = factory.createInstance(null, AM_Ci.nsIX509CertDB);

  let verifyCert = Task.async(function*(caller, file, result, cert, callback) {
    // If this isn't a callback we can get directly to through JS then just
    // pass on the results
    if (!callback.wrappedJSObject) {
      caller(callback, result, cert);
      return;
    }

    // Bypassing XPConnect allows us to create a fake x509 certificate from
    // JS
    callback = callback.wrappedJSObject;

    if (gUseRealCertChecks || result != Components.results.NS_ERROR_SIGNED_JAR_NOT_SIGNED) {
      // If the real DB found a useful result of some kind then pass it on.
      caller(callback, result, cert);
      return;
    }

    try {
      let manifestURI = getManifestURIForBundle(file);

      let id = yield getIDForManifest(manifestURI);

      // Make sure to close the open zip file or it will be locked.
      if (file.isFile()) {
        Services.obs.notifyObservers(file, "flush-cache-entry", "cert-override");
      }

      let fakeCert = {
        commonName: id
      }
      caller(callback, Components.results.NS_OK, fakeCert);
    }
    catch (e) {
      // If there is any error then just pass along the original results
      caller(callback, result, cert);
    }
  });

  let fakeCertDB = {
    openSignedAppFileAsync(root, file, callback) {
      // First try calling the real cert DB
      realCertDB.openSignedAppFileAsync(root, file, (result, zipReader, cert) => {
        function call(callback, result, cert) {
          callback.openSignedAppFileFinished(result, zipReader, cert);
        }

        verifyCert(call, file.clone(), result, cert, callback);
      });
    },

    verifySignedDirectoryAsync(root, dir, callback) {
      // First try calling the real cert DB
      realCertDB.verifySignedDirectoryAsync(root, dir, (result, cert) => {
        function call(callback, result, cert) {
          callback.verifySignedDirectoryFinished(result, cert);
        }

        verifyCert(call, dir.clone(), result, cert, callback);
      });
    },

    QueryInterface: XPCOMUtils.generateQI([AM_Ci.nsIX509CertDB])
  };

  for (let property of Object.keys(realCertDB)) {
    if (property in fakeCertDB) {
      continue;
    }

    if (typeof realCertDB[property] == "function") {
      fakeCertDB[property] = realCertDB[property].bind(realCertDB);
    }
  }

  let certDBFactory = {
    createInstance: function(outer, iid) {
      if (outer != null) {
        throw Components.results.NS_ERROR_NO_AGGREGATION;
      }
      return fakeCertDB.QueryInterface(iid);
    }
  };
  registrar.registerFactory(CERTDB_CID, "CertDB",
                            CERTDB_CONTRACTID, certDBFactory);
}

overrideCertDB();

/**
 * Tests that an add-on does appear in the crash report annotations, if
 * crash reporting is enabled. The test will fail if the add-on is not in the
 * annotation.
 * @param  aId
 *         The ID of the add-on
 * @param  aVersion
 *         The version of the add-on
 */
function do_check_in_crash_annotation(aId, aVersion) {
  if (!("nsICrashReporter" in AM_Ci))
    return;

  if (!("Add-ons" in gAppInfo.annotations)) {
    do_check_false(true);
    return;
  }

  let addons = gAppInfo.annotations["Add-ons"].split(",");
  do_check_false(addons.indexOf(encodeURIComponent(aId) + ":" +
                                encodeURIComponent(aVersion)) < 0);
}

/**
 * Tests that an add-on does not appear in the crash report annotations, if
 * crash reporting is enabled. The test will fail if the add-on is in the
 * annotation.
 * @param  aId
 *         The ID of the add-on
 * @param  aVersion
 *         The version of the add-on
 */
function do_check_not_in_crash_annotation(aId, aVersion) {
  if (!("nsICrashReporter" in AM_Ci))
    return;

  if (!("Add-ons" in gAppInfo.annotations)) {
    do_check_true(true);
    return;
  }

  let addons = gAppInfo.annotations["Add-ons"].split(",");
  do_check_true(addons.indexOf(encodeURIComponent(aId) + ":" +
                               encodeURIComponent(aVersion)) < 0);
}

/**
 * Returns a testcase xpi
 *
 * @param  aName
 *         The name of the testcase (without extension)
 * @return an nsIFile pointing to the testcase xpi
 */
function do_get_addon(aName) {
  return do_get_file("addons/" + aName + ".xpi");
}

function do_get_addon_hash(aName, aAlgorithm) {
  let file = do_get_addon(aName);
  return do_get_file_hash(file);
}

function do_get_file_hash(aFile, aAlgorithm) {
  if (!aAlgorithm)
    aAlgorithm = "sha1";

  let crypto = AM_Cc["@mozilla.org/security/hash;1"].
               createInstance(AM_Ci.nsICryptoHash);
  crypto.initWithString(aAlgorithm);
  let fis = AM_Cc["@mozilla.org/network/file-input-stream;1"].
            createInstance(AM_Ci.nsIFileInputStream);
  fis.init(aFile, -1, -1, false);
  crypto.updateFromStream(fis, aFile.fileSize);

  // return the two-digit hexadecimal code for a byte
  let toHexString = charCode => ("0" + charCode.toString(16)).slice(-2);

  let binary = crypto.finish(false);
  let hash = Array.from(binary, c => toHexString(c.charCodeAt(0)));
  return aAlgorithm + ":" + hash.join("");
}

/**
 * Returns an extension uri spec
 *
 * @param  aProfileDir
 *         The extension install directory
 * @return a uri spec pointing to the root of the extension
 */
function do_get_addon_root_uri(aProfileDir, aId) {
  let path = aProfileDir.clone();
  path.append(aId);
  if (!path.exists()) {
    path.leafName += ".xpi";
    return "jar:" + Services.io.newFileURI(path).spec + "!/";
  }
  else {
    return Services.io.newFileURI(path).spec;
  }
}

function do_get_expected_addon_name(aId) {
  if (TEST_UNPACKED)
    return aId;
  return aId + ".xpi";
}

/**
 * Check that an array of actual add-ons is the same as an array of
 * expected add-ons.
 *
 * @param  aActualAddons
 *         The array of actual add-ons to check.
 * @param  aExpectedAddons
 *         The array of expected add-ons to check against.
 * @param  aProperties
 *         An array of properties to check.
 */
function do_check_addons(aActualAddons, aExpectedAddons, aProperties) {
  do_check_neq(aActualAddons, null);
  do_check_eq(aActualAddons.length, aExpectedAddons.length);
  for (let i = 0; i < aActualAddons.length; i++)
    do_check_addon(aActualAddons[i], aExpectedAddons[i], aProperties);
}

/**
 * Check that the actual add-on is the same as the expected add-on.
 *
 * @param  aActualAddon
 *         The actual add-on to check.
 * @param  aExpectedAddon
 *         The expected add-on to check against.
 * @param  aProperties
 *         An array of properties to check.
 */
function do_check_addon(aActualAddon, aExpectedAddon, aProperties) {
  do_check_neq(aActualAddon, null);

  aProperties.forEach(function(aProperty) {
    let actualValue = aActualAddon[aProperty];
    let expectedValue = aExpectedAddon[aProperty];

    // Check that all undefined expected properties are null on actual add-on
    if (!(aProperty in aExpectedAddon)) {
      if (actualValue !== undefined && actualValue !== null) {
        do_throw("Unexpected defined/non-null property for add-on " +
                 aExpectedAddon.id + " (addon[" + aProperty + "] = " +
                 actualValue.toSource() + ")");
      }

      return;
    }
    else if (expectedValue && !actualValue) {
      do_throw("Missing property for add-on " + aExpectedAddon.id +
        ": expected addon[" + aProperty + "] = " + expectedValue);
      return;
    }

    switch (aProperty) {
      case "creator":
        do_check_author(actualValue, expectedValue);
        break;

      case "developers":
      case "translators":
      case "contributors":
        do_check_eq(actualValue.length, expectedValue.length);
        for (let i = 0; i < actualValue.length; i++)
          do_check_author(actualValue[i], expectedValue[i]);
        break;

      case "screenshots":
        do_check_eq(actualValue.length, expectedValue.length);
        for (let i = 0; i < actualValue.length; i++)
          do_check_screenshot(actualValue[i], expectedValue[i]);
        break;

      case "sourceURI":
        do_check_eq(actualValue.spec, expectedValue);
        break;

      case "updateDate":
        do_check_eq(actualValue.getTime(), expectedValue.getTime());
        break;

      case "compatibilityOverrides":
        do_check_eq(actualValue.length, expectedValue.length);
        for (let i = 0; i < actualValue.length; i++)
          do_check_compatibilityoverride(actualValue[i], expectedValue[i]);
        break;

      case "icons":
        do_check_icons(actualValue, expectedValue);
        break;

      default:
        if (remove_port(actualValue) !== remove_port(expectedValue))
          do_throw("Failed for " + aProperty + " for add-on " + aExpectedAddon.id +
                   " (" + actualValue + " === " + expectedValue + ")");
    }
  });
}

/**
 * Check that the actual author is the same as the expected author.
 *
 * @param  aActual
 *         The actual author to check.
 * @param  aExpected
 *         The expected author to check against.
 */
function do_check_author(aActual, aExpected) {
  do_check_eq(aActual.toString(), aExpected.name);
  do_check_eq(aActual.name, aExpected.name);
  do_check_eq(aActual.url, aExpected.url);
}

/**
 * Check that the actual screenshot is the same as the expected screenshot.
 *
 * @param  aActual
 *         The actual screenshot to check.
 * @param  aExpected
 *         The expected screenshot to check against.
 */
function do_check_screenshot(aActual, aExpected) {
  do_check_eq(aActual.toString(), aExpected.url);
  do_check_eq(aActual.url, aExpected.url);
  do_check_eq(aActual.width, aExpected.width);
  do_check_eq(aActual.height, aExpected.height);
  do_check_eq(aActual.thumbnailURL, aExpected.thumbnailURL);
  do_check_eq(aActual.thumbnailWidth, aExpected.thumbnailWidth);
  do_check_eq(aActual.thumbnailHeight, aExpected.thumbnailHeight);
  do_check_eq(aActual.caption, aExpected.caption);
}

/**
 * Check that the actual compatibility override is the same as the expected
 * compatibility override.
 *
 * @param  aAction
 *         The actual compatibility override to check.
 * @param  aExpected
 *         The expected compatibility override to check against.
 */
function do_check_compatibilityoverride(aActual, aExpected) {
  do_check_eq(aActual.type, aExpected.type);
  do_check_eq(aActual.minVersion, aExpected.minVersion);
  do_check_eq(aActual.maxVersion, aExpected.maxVersion);
  do_check_eq(aActual.appID, aExpected.appID);
  do_check_eq(aActual.appMinVersion, aExpected.appMinVersion);
  do_check_eq(aActual.appMaxVersion, aExpected.appMaxVersion);
}

function do_check_icons(aActual, aExpected) {
  for (var size in aExpected) {
    do_check_eq(remove_port(aActual[size]), remove_port(aExpected[size]));
  }
}

// Record the error (if any) from trying to save the XPI
// database at shutdown time
var gXPISaveError = null;

/**
 * Starts up the add-on manager as if it was started by the application.
 *
 * @param  aAppChanged
 *         An optional boolean parameter to simulate the case where the
 *         application has changed version since the last run. If not passed it
 *         defaults to true
 */
function startupManager(aAppChanged) {
  if (gInternalManager)
    do_throw("Test attempt to startup manager that was already started.");

  if (aAppChanged || aAppChanged === undefined) {
    if (gExtensionsINI.exists())
      gExtensionsINI.remove(true);
  }

  gInternalManager = AM_Cc["@mozilla.org/addons/integration;1"].
                     getService(AM_Ci.nsIObserver).
                     QueryInterface(AM_Ci.nsITimerCallback);

  gInternalManager.observe(null, "addons-startup", null);

  // Load the add-ons list as it was after extension registration
  loadAddonsList();
}

/**
 * Helper to spin the event loop until a promise resolves or rejects
 */
function loopUntilPromise(aPromise) {
  let done = false;
  aPromise.then(
    () => done = true,
    err => {
      do_report_unexpected_exception(err);
      done = true;
    });

  let thr = Services.tm.mainThread;

  while (!done) {
    thr.processNextEvent(true);
  }
}

/**
 * Restarts the add-on manager as if the host application was restarted.
 *
 * @param  aNewVersion
 *         An optional new version to use for the application. Passing this
 *         will change nsIXULAppInfo.version and make the startup appear as if
 *         the application version has changed.
 */
function restartManager(aNewVersion) {
  loopUntilPromise(promiseRestartManager(aNewVersion));
}

function promiseRestartManager(aNewVersion) {
  return promiseShutdownManager()
    .then(null, err => do_report_unexpected_exception(err))
    .then(() => {
      if (aNewVersion) {
        gAppInfo.version = aNewVersion;
        startupManager(true);
      }
      else {
        startupManager(false);
      }
    });
}

function shutdownManager() {
  loopUntilPromise(promiseShutdownManager());
}

function promiseShutdownManager() {
  if (!gInternalManager) {
    return Promise.resolve(false);
  }

  let hookErr = null;
  Services.obs.notifyObservers(null, "quit-application-granted", null);
  return MockAsyncShutdown.hook()
    .then(null, err => hookErr = err)
    .then( () => {
      BootstrapMonitor.shutdownCheck();
      gInternalManager = null;

      // Load the add-ons list as it was after application shutdown
      loadAddonsList();

      // Clear any crash report annotations
      gAppInfo.annotations = {};

      // Force the XPIProvider provider to reload to better
      // simulate real-world usage.
      let XPIscope = Components.utils.import("resource://gre/modules/addons/XPIProvider.jsm");
      // This would be cleaner if I could get it as the rejection reason from
      // the AddonManagerInternal.shutdown() promise
      gXPISaveError = XPIscope.XPIProvider._shutdownError;
      do_print("gXPISaveError set to: " + gXPISaveError);
      AddonManagerPrivate.unregisterProvider(XPIscope.XPIProvider);
      Components.utils.unload("resource://gre/modules/addons/XPIProvider.jsm");
      if (hookErr) {
        throw hookErr;
      }
    });
}

function loadAddonsList() {
  function readDirectories(aSection) {
    var dirs = [];
    var keys = parser.getKeys(aSection);
    while (keys.hasMore()) {
      let descriptor = parser.getString(aSection, keys.getNext());
      try {
        let file = AM_Cc["@mozilla.org/file/local;1"].
                   createInstance(AM_Ci.nsIFile);
        file.persistentDescriptor = descriptor;
        dirs.push(file);
      }
      catch (e) {
        // Throws if the directory doesn't exist, we can ignore this since the
        // platform will too.
      }
    }
    return dirs;
  }

  gAddonsList = {
    extensions: [],
    themes: [],
    mpIncompatible: new Set()
  };

  if (!gExtensionsINI.exists())
    return;

  var factory = AM_Cc["@mozilla.org/xpcom/ini-parser-factory;1"].
                getService(AM_Ci.nsIINIParserFactory);
  var parser = factory.createINIParser(gExtensionsINI);
  gAddonsList.extensions = readDirectories("ExtensionDirs");
  gAddonsList.themes = readDirectories("ThemeDirs");
  var keys = parser.getKeys("MultiprocessIncompatibleExtensions");
  while (keys.hasMore()) {
    let id = parser.getString("MultiprocessIncompatibleExtensions", keys.getNext());
    gAddonsList.mpIncompatible.add(id);
  }
}

function isItemInAddonsList(aType, aDir, aId) {
  var path = aDir.clone();
  path.append(aId);
  var xpiPath = aDir.clone();
  xpiPath.append(aId + ".xpi");
  for (var i = 0; i < gAddonsList[aType].length; i++) {
    let file = gAddonsList[aType][i];
    if (!file.exists())
      do_throw("Non-existant path found in extensions.ini: " + file.path)
    if (file.isDirectory() && file.equals(path))
      return true;
    if (file.isFile() && file.equals(xpiPath))
      return true;
  }
  return false;
}

function isItemMarkedMPIncompatible(aId) {
  return gAddonsList.mpIncompatible.has(aId);
}

function isThemeInAddonsList(aDir, aId) {
  return isItemInAddonsList("themes", aDir, aId);
}

function isExtensionInAddonsList(aDir, aId) {
  return isItemInAddonsList("extensions", aDir, aId);
}

function check_startup_changes(aType, aIds) {
  var ids = aIds.slice(0);
  ids.sort();
  var changes = AddonManager.getStartupChanges(aType);
  changes = changes.filter(aEl => /@tests.mozilla.org$/.test(aEl));
  changes.sort();

  do_check_eq(JSON.stringify(ids), JSON.stringify(changes));
}

/**
 * Escapes any occurances of &, ", < or > with XML entities.
 *
 * @param   str
 *          The string to escape
 * @return  The escaped string
 */
function escapeXML(aStr) {
  return aStr.toString()
             .replace(/&/g, "&amp;")
             .replace(/"/g, "&quot;")
             .replace(/</g, "&lt;")
             .replace(/>/g, "&gt;");
}

function writeLocaleStrings(aData) {
  let rdf = "";
  ["name", "description", "creator", "homepageURL"].forEach(function(aProp) {
    if (aProp in aData)
      rdf += "<em:" + aProp + ">" + escapeXML(aData[aProp]) + "</em:" + aProp + ">\n";
  });

  ["developer", "translator", "contributor"].forEach(function(aProp) {
    if (aProp in aData) {
      aData[aProp].forEach(function(aValue) {
        rdf += "<em:" + aProp + ">" + escapeXML(aValue) + "</em:" + aProp + ">\n";
      });
    }
  });
  return rdf;
}

/**
 * Creates an update.rdf structure as a string using for the update data passed.
 *
 * @param   aData
 *          The update data as a JS object. Each property name is an add-on ID,
 *          the property value is an array of each version of the add-on. Each
 *          array value is a JS object containing the data for the version, at
 *          minimum a "version" and "targetApplications" property should be
 *          included to create a functional update manifest.
 * @return  the update.rdf structure as a string.
 */
function createUpdateRDF(aData) {
  var rdf = '<?xml version="1.0"?>\n';
  rdf += '<RDF xmlns="http://www.w3.org/1999/02/22-rdf-syntax-ns#"\n' +
         '     xmlns:em="http://www.mozilla.org/2004/em-rdf#">\n';

  for (let addon in aData) {
    rdf += '  <Description about="urn:mozilla:extension:' + escapeXML(addon) + '"><em:updates><Seq>\n';

    for (let versionData of aData[addon]) {
      rdf += '    <li><Description>\n';

      for (let prop of ["version", "multiprocessCompatible"]) {
        if (prop in versionData)
          rdf += "      <em:" + prop + ">" + escapeXML(versionData[prop]) + "</em:" + prop + ">\n";
      }

      if ("targetApplications" in versionData) {
        for (let app of versionData.targetApplications) {
          rdf += "      <em:targetApplication><Description>\n";
          for (let prop of ["id", "minVersion", "maxVersion", "updateLink", "updateHash"]) {
            if (prop in app)
              rdf += "        <em:" + prop + ">" + escapeXML(app[prop]) + "</em:" + prop + ">\n";
          }
          rdf += "      </Description></em:targetApplication>\n";
        }
      }

      rdf += '    </Description></li>\n';
    }

    rdf += '  </Seq></em:updates></Description>\n'
  }
  rdf += "</RDF>\n";

  return rdf;
}

function createInstallRDF(aData) {
  var rdf = '<?xml version="1.0"?>\n';
  rdf += '<RDF xmlns="http://www.w3.org/1999/02/22-rdf-syntax-ns#"\n' +
         '     xmlns:em="http://www.mozilla.org/2004/em-rdf#">\n';
  rdf += '<Description about="urn:mozilla:install-manifest">\n';

  ["id", "version", "type", "internalName", "updateURL", "updateKey",
   "optionsURL", "optionsType", "aboutURL", "iconURL", "icon64URL",
   "skinnable", "bootstrap", "unpack", "strictCompatibility", "multiprocessCompatible"].forEach(function(aProp) {
    if (aProp in aData)
      rdf += "<em:" + aProp + ">" + escapeXML(aData[aProp]) + "</em:" + aProp + ">\n";
  });

  rdf += writeLocaleStrings(aData);

  if ("targetPlatforms" in aData) {
    aData.targetPlatforms.forEach(function(aPlatform) {
      rdf += "<em:targetPlatform>" + escapeXML(aPlatform) + "</em:targetPlatform>\n";
    });
  }

  if ("targetApplications" in aData) {
    aData.targetApplications.forEach(function(aApp) {
      rdf += "<em:targetApplication><Description>\n";
      ["id", "minVersion", "maxVersion"].forEach(function(aProp) {
        if (aProp in aApp)
          rdf += "<em:" + aProp + ">" + escapeXML(aApp[aProp]) + "</em:" + aProp + ">\n";
      });
      rdf += "</Description></em:targetApplication>\n";
    });
  }

  if ("localized" in aData) {
    aData.localized.forEach(function(aLocalized) {
      rdf += "<em:localized><Description>\n";
      if ("locale" in aLocalized) {
        aLocalized.locale.forEach(function(aLocaleName) {
          rdf += "<em:locale>" + escapeXML(aLocaleName) + "</em:locale>\n";
        });
      }
      rdf += writeLocaleStrings(aLocalized);
      rdf += "</Description></em:localized>\n";
    });
  }

  rdf += "</Description>\n</RDF>\n";
  return rdf;
}

/**
 * Writes an install.rdf manifest into a directory using the properties passed
 * in a JS object. The objects should contain a property for each property to
 * appear in the RDF. The object may contain an array of objects with id,
 * minVersion and maxVersion in the targetApplications property to give target
 * application compatibility.
 *
 * @param   aData
 *          The object holding data about the add-on
 * @param   aDir
 *          The directory to add the install.rdf to
 * @param   aId
 *          An optional string to override the default installation aId
 * @param   aExtraFile
 *          An optional dummy file to create in the directory
 * @return  An nsIFile for the directory in which the add-on is installed.
 */
function writeInstallRDFToDir(aData, aDir, aId, aExtraFile) {
  var id = aId ? aId : aData.id

  var dir = aDir.clone();
  dir.append(id);

  var rdf = createInstallRDF(aData);
  if (!dir.exists())
    dir.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
  var file = dir.clone();
  file.append("install.rdf");
  if (file.exists())
    file.remove(true);
  var fos = AM_Cc["@mozilla.org/network/file-output-stream;1"].
            createInstance(AM_Ci.nsIFileOutputStream);
  fos.init(file,
           FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE,
           FileUtils.PERMS_FILE, 0);
  fos.write(rdf, rdf.length);
  fos.close();

  if (!aExtraFile)
    return dir;

  file = dir.clone();
  file.append(aExtraFile);
  file.create(AM_Ci.nsIFile.NORMAL_FILE_TYPE, FileUtils.PERMS_FILE);
  return dir;
}

/**
 * Writes an install.rdf manifest into an extension using the properties passed
 * in a JS object. The objects should contain a property for each property to
 * appear in the RDF. The object may contain an array of objects with id,
 * minVersion and maxVersion in the targetApplications property to give target
 * application compatibility.
 *
 * @param   aData
 *          The object holding data about the add-on
 * @param   aDir
 *          The install directory to add the extension to
 * @param   aId
 *          An optional string to override the default installation aId
 * @param   aExtraFile
 *          An optional dummy file to create in the extension
 * @return  A file pointing to where the extension was installed
 */
function writeInstallRDFForExtension(aData, aDir, aId, aExtraFile) {
  if (TEST_UNPACKED) {
    return writeInstallRDFToDir(aData, aDir, aId, aExtraFile);
  }
  return writeInstallRDFToXPI(aData, aDir, aId, aExtraFile);
}

/**
 * Writes a manifest.json manifest into an extension using the properties passed
 * in a JS object.
 *
 * @param   aManifest
 *          The data to write
 * @param   aDir
 *          The install directory to add the extension to
 * @param   aId
 *          An optional string to override the default installation aId
 * @return  A file pointing to where the extension was installed
 */
function writeWebManifestForExtension(aData, aDir, aId = undefined) {
  if (!aId)
    aId = aData.applications.gecko.id;

  if (TEST_UNPACKED) {
    let dir = aDir.clone();
    dir.append(aId);
    if (!dir.exists())
      dir.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);

    let file = dir.clone();
    file.append("manifest.json");
    if (file.exists())
      file.remove(true);

    let data = JSON.stringify(aData);
    let fos = AM_Cc["@mozilla.org/network/file-output-stream;1"].
              createInstance(AM_Ci.nsIFileOutputStream);
    fos.init(file,
             FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE,
             FileUtils.PERMS_FILE, 0);
    fos.write(data, data.length);
    fos.close();

    return dir;
  }
  else {
    let file = aDir.clone();
    file.append(aId + ".xpi");

    let stream = AM_Cc["@mozilla.org/io/string-input-stream;1"].
                 createInstance(AM_Ci.nsIStringInputStream);
    stream.setData(JSON.stringify(aData), -1);
    let zipW = AM_Cc["@mozilla.org/zipwriter;1"].
               createInstance(AM_Ci.nsIZipWriter);
    zipW.open(file, FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE);
    zipW.addEntryStream("manifest.json", 0, AM_Ci.nsIZipWriter.COMPRESSION_NONE,
                        stream, false);
    zipW.close();

    return file;
  }
}

/**
 * Writes an install.rdf manifest into a packed extension using the properties passed
 * in a JS object. The objects should contain a property for each property to
 * appear in the RDF. The object may contain an array of objects with id,
 * minVersion and maxVersion in the targetApplications property to give target
 * application compatibility.
 *
 * @param   aData
 *          The object holding data about the add-on
 * @param   aDir
 *          The install directory to add the extension to
 * @param   aId
 *          An optional string to override the default installation aId
 * @param   aExtraFile
 *          An optional dummy file to create in the extension
 * @return  A file pointing to where the extension was installed
 */
function writeInstallRDFToXPI(aData, aDir, aId, aExtraFile) {
  var id = aId ? aId : aData.id

  if (!aDir.exists())
    aDir.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);

  var file = aDir.clone();
  file.append(id + ".xpi");
  writeInstallRDFToXPIFile(aData, file, aExtraFile);

  return file;
}

/**
 * Writes an install.rdf manifest into an XPI file using the properties passed
 * in a JS object. The objects should contain a property for each property to
 * appear in the RDF. The object may contain an array of objects with id,
 * minVersion and maxVersion in the targetApplications property to give target
 * application compatibility.
 *
 * @param   aData
 *          The object holding data about the add-on
 * @param   aFile
 *          The XPI file to write to. Any existing file will be overwritten
 * @param   aExtraFile
 *          An optional dummy file to create in the extension
 */
function writeInstallRDFToXPIFile(aData, aFile, aExtraFile) {
  var rdf = createInstallRDF(aData);
  var stream = AM_Cc["@mozilla.org/io/string-input-stream;1"].
               createInstance(AM_Ci.nsIStringInputStream);
  stream.setData(rdf, -1);
  var zipW = AM_Cc["@mozilla.org/zipwriter;1"].
             createInstance(AM_Ci.nsIZipWriter);
  zipW.open(aFile, FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE);
  // Note these files are being created in the XPI archive with date "0" which is 1970-01-01.
  zipW.addEntryStream("install.rdf", 0, AM_Ci.nsIZipWriter.COMPRESSION_NONE,
                      stream, false);
  if (aExtraFile)
    zipW.addEntryStream(aExtraFile, 0, AM_Ci.nsIZipWriter.COMPRESSION_NONE,
                        stream, false);
  zipW.close();
}

var temp_xpis = [];
/**
 * Creates an XPI file for some manifest data in the temporary directory and
 * returns the nsIFile for it. The file will be deleted when the test completes.
 *
 * @param   aData
 *          The object holding data about the add-on
 * @return  A file pointing to the created XPI file
 */
function createTempXPIFile(aData) {
  var file = gTmpD.clone();
  file.append("foo.xpi");
  do {
    file.leafName = Math.floor(Math.random() * 1000000) + ".xpi";
  } while (file.exists());

  temp_xpis.push(file);
  writeInstallRDFToXPIFile(aData, file);
  return file;
}

/**
 * Creates an XPI file for some WebExtension data in the temporary directory and
 * returns the nsIFile for it. The file will be deleted when the test completes.
 *
 * @param   aData
 *          The object holding data about the add-on, as expected by
 *          |Extension.generateXPI|.
 * @return  A file pointing to the created XPI file
 */
function createTempWebExtensionFile(aData) {
  if (!aData.id) {
    const uuidGenerator = AM_Cc["@mozilla.org/uuid-generator;1"].getService(AM_Ci.nsIUUIDGenerator);
    aData.id = uuidGenerator.generateUUID().number;
  }

  let file = Extension.generateXPI(aData.id, aData);
  temp_xpis.push(file);
  return file;
}

/**
 * Sets the last modified time of the extension, usually to trigger an update
 * of its metadata. If the extension is unpacked, this function assumes that
 * the extension contains only the install.rdf file.
 *
 * @param aExt   a file pointing to either the packed extension or its unpacked directory.
 * @param aTime  the time to which we set the lastModifiedTime of the extension
 *
 * @deprecated Please use promiseSetExtensionModifiedTime instead
 */
function setExtensionModifiedTime(aExt, aTime) {
  aExt.lastModifiedTime = aTime;
  if (aExt.isDirectory()) {
    let entries = aExt.directoryEntries
                      .QueryInterface(AM_Ci.nsIDirectoryEnumerator);
    while (entries.hasMoreElements())
      setExtensionModifiedTime(entries.nextFile, aTime);
    entries.close();
  }
}
function promiseSetExtensionModifiedTime(aPath, aTime) {
  return Task.spawn(function* () {
    yield OS.File.setDates(aPath, aTime, aTime);
    let entries, iterator;
    try {
      let iterator = new OS.File.DirectoryIterator(aPath);
      entries = yield iterator.nextBatch();
    } catch (ex) {
      if (!(ex instanceof OS.File.Error))
        throw ex;
      return;
    } finally {
      if (iterator) {
        iterator.close();
      }
    }
    for (let entry of entries) {
      yield promiseSetExtensionModifiedTime(entry.path, aTime);
    }
  });
}

/**
 * Manually installs an XPI file into an install location by either copying the
 * XPI there or extracting it depending on whether unpacking is being tested
 * or not.
 *
 * @param aXPIFile
 *        The XPI file to install.
 * @param aInstallLocation
 *        The install location (an nsIFile) to install into.
 * @param aID
 *        The ID to install as.
 */
function manuallyInstall(aXPIFile, aInstallLocation, aID) {
  if (TEST_UNPACKED) {
    let dir = aInstallLocation.clone();
    dir.append(aID);
    dir.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
    let zip = AM_Cc["@mozilla.org/libjar/zip-reader;1"].
              createInstance(AM_Ci.nsIZipReader);
    zip.open(aXPIFile);
    let entries = zip.findEntries(null);
    while (entries.hasMore()) {
      let entry = entries.getNext();
      let target = dir.clone();
      entry.split("/").forEach(function(aPart) {
        target.append(aPart);
      });
      zip.extract(entry, target);
    }
    zip.close();

    return dir;
  }
  else {
    let target = aInstallLocation.clone();
    target.append(aID + ".xpi");
    aXPIFile.copyTo(target.parent, target.leafName);
    return target;
  }
}

/**
 * Manually uninstalls an add-on by removing its files from the install
 * location.
 *
 * @param aInstallLocation
 *        The nsIFile of the install location to remove from.
 * @param aID
 *        The ID of the add-on to remove.
 */
function manuallyUninstall(aInstallLocation, aID) {
  let file = getFileForAddon(aInstallLocation, aID);

  // In reality because the app is restarted a flush isn't necessary for XPIs
  // removed outside the app, but for testing we must flush manually.
  if (file.isFile())
    Services.obs.notifyObservers(file, "flush-cache-entry", null);

  file.remove(true);
}

/**
 * Gets the nsIFile for where an add-on is installed. It may point to a file or
 * a directory depending on whether add-ons are being installed unpacked or not.
 *
 * @param  aDir
 *         The nsIFile for the install location
 * @param  aId
 *         The ID of the add-on
 * @return an nsIFile
 */
function getFileForAddon(aDir, aId) {
  var dir = aDir.clone();
  dir.append(do_get_expected_addon_name(aId));
  return dir;
}

function registerDirectory(aKey, aDir) {
  var dirProvider = {
    getFile: function(aProp, aPersistent) {
      aPersistent.value = false;
      if (aProp == aKey)
        return aDir.clone();
      return null;
    },

    QueryInterface: XPCOMUtils.generateQI([AM_Ci.nsIDirectoryServiceProvider,
                                           AM_Ci.nsISupports])
  };
  Services.dirsvc.registerProvider(dirProvider);
}

var gExpectedEvents = {};
var gExpectedInstalls = [];
var gNext = null;

function getExpectedEvent(aId) {
  if (!(aId in gExpectedEvents))
    do_throw("Wasn't expecting events for " + aId);
  if (gExpectedEvents[aId].length == 0)
    do_throw("Too many events for " + aId);
  let event = gExpectedEvents[aId].shift();
  if (event instanceof Array)
    return event;
  return [event, true];
}

function getExpectedInstall(aAddon) {
  if (gExpectedInstalls instanceof Array)
    return gExpectedInstalls.shift();
  if (!aAddon || !aAddon.id)
    return gExpectedInstalls["NO_ID"].shift();
  let id = aAddon.id;
  if (!(id in gExpectedInstalls) || !(gExpectedInstalls[id] instanceof Array))
    do_throw("Wasn't expecting events for " + id);
  if (gExpectedInstalls[id].length == 0)
    do_throw("Too many events for " + id);
  return gExpectedInstalls[id].shift();
}

const AddonListener = {
  onPropertyChanged: function(aAddon, aProperties) {
    do_print(`Got onPropertyChanged event for ${aAddon.id}`);
    let [event, properties] = getExpectedEvent(aAddon.id);
    do_check_eq("onPropertyChanged", event);
    do_check_eq(aProperties.length, properties.length);
    properties.forEach(function(aProperty) {
      // Only test that the expected properties are listed, having additional
      // properties listed is not necessary a problem
      if (aProperties.indexOf(aProperty) == -1)
        do_throw("Did not see property change for " + aProperty);
    });
    return check_test_completed(arguments);
  },

  onEnabling: function(aAddon, aRequiresRestart) {
    do_print(`Got onEnabling event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onEnabling", event);
    do_check_eq(aRequiresRestart, expectedRestart);
    if (expectedRestart)
      do_check_true(hasFlag(aAddon.pendingOperations, AddonManager.PENDING_ENABLE));
    do_check_false(hasFlag(aAddon.permissions, AddonManager.PERM_CAN_ENABLE));
    return check_test_completed(arguments);
  },

  onEnabled: function(aAddon) {
    do_print(`Got onEnabled event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onEnabled", event);
    do_check_false(hasFlag(aAddon.permissions, AddonManager.PERM_CAN_ENABLE));
    return check_test_completed(arguments);
  },

  onDisabling: function(aAddon, aRequiresRestart) {
    do_print(`Got onDisabling event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onDisabling", event);
    do_check_eq(aRequiresRestart, expectedRestart);
    if (expectedRestart)
      do_check_true(hasFlag(aAddon.pendingOperations, AddonManager.PENDING_DISABLE));
    do_check_false(hasFlag(aAddon.permissions, AddonManager.PERM_CAN_DISABLE));
    return check_test_completed(arguments);
  },

  onDisabled: function(aAddon) {
    do_print(`Got onDisabled event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onDisabled", event);
    do_check_false(hasFlag(aAddon.permissions, AddonManager.PERM_CAN_DISABLE));
    return check_test_completed(arguments);
  },

  onInstalling: function(aAddon, aRequiresRestart) {
    do_print(`Got onInstalling event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onInstalling", event);
    do_check_eq(aRequiresRestart, expectedRestart);
    if (expectedRestart)
      do_check_true(hasFlag(aAddon.pendingOperations, AddonManager.PENDING_INSTALL));
    return check_test_completed(arguments);
  },

  onInstalled: function(aAddon) {
    do_print(`Got onInstalled event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onInstalled", event);
    return check_test_completed(arguments);
  },

  onUninstalling: function(aAddon, aRequiresRestart) {
    do_print(`Got onUninstalling event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onUninstalling", event);
    do_check_eq(aRequiresRestart, expectedRestart);
    if (expectedRestart)
      do_check_true(hasFlag(aAddon.pendingOperations, AddonManager.PENDING_UNINSTALL));
    return check_test_completed(arguments);
  },

  onUninstalled: function(aAddon) {
    do_print(`Got onUninstalled event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onUninstalled", event);
    return check_test_completed(arguments);
  },

  onOperationCancelled: function(aAddon) {
    do_print(`Got onOperationCancelled event for ${aAddon.id}`);
    let [event, expectedRestart] = getExpectedEvent(aAddon.id);
    do_check_eq("onOperationCancelled", event);
    return check_test_completed(arguments);
  }
};

const InstallListener = {
  onNewInstall: function(install) {
    if (install.state != AddonManager.STATE_DOWNLOADED &&
        install.state != AddonManager.STATE_DOWNLOAD_FAILED &&
        install.state != AddonManager.STATE_AVAILABLE)
      do_throw("Bad install state " + install.state);
    if (install.state != AddonManager.STATE_DOWNLOAD_FAILED)
      do_check_eq(install.error, 0);
    else
      do_check_neq(install.error, 0);
    do_check_eq("onNewInstall", getExpectedInstall());
    return check_test_completed(arguments);
  },

  onDownloadStarted: function(install) {
    do_check_eq(install.state, AddonManager.STATE_DOWNLOADING);
    do_check_eq(install.error, 0);
    do_check_eq("onDownloadStarted", getExpectedInstall());
    return check_test_completed(arguments);
  },

  onDownloadEnded: function(install) {
    do_check_eq(install.state, AddonManager.STATE_DOWNLOADED);
    do_check_eq(install.error, 0);
    do_check_eq("onDownloadEnded", getExpectedInstall());
    return check_test_completed(arguments);
  },

  onDownloadFailed: function(install) {
    do_check_eq(install.state, AddonManager.STATE_DOWNLOAD_FAILED);
    do_check_eq("onDownloadFailed", getExpectedInstall());
    return check_test_completed(arguments);
  },

  onDownloadCancelled: function(install) {
    do_check_eq(install.state, AddonManager.STATE_CANCELLED);
    do_check_eq(install.error, 0);
    do_check_eq("onDownloadCancelled", getExpectedInstall());
    return check_test_completed(arguments);
  },

  onInstallStarted: function(install) {
    do_check_eq(install.state, AddonManager.STATE_INSTALLING);
    do_check_eq(install.error, 0);
    do_check_eq("onInstallStarted", getExpectedInstall(install.addon));
    return check_test_completed(arguments);
  },

  onInstallEnded: function(install, newAddon) {
    do_check_eq(install.state, AddonManager.STATE_INSTALLED);
    do_check_eq(install.error, 0);
    do_check_eq("onInstallEnded", getExpectedInstall(install.addon));
    return check_test_completed(arguments);
  },

  onInstallFailed: function(install) {
    do_check_eq(install.state, AddonManager.STATE_INSTALL_FAILED);
    do_check_eq("onInstallFailed", getExpectedInstall(install.addon));
    return check_test_completed(arguments);
  },

  onInstallCancelled: function(install) {
    // If the install was cancelled by a listener returning false from
    // onInstallStarted, then the state will revert to STATE_DOWNLOADED.
    let possibleStates = [AddonManager.STATE_CANCELLED,
                          AddonManager.STATE_DOWNLOADED];
    do_check_true(possibleStates.indexOf(install.state) != -1);
    do_check_eq(install.error, 0);
    do_check_eq("onInstallCancelled", getExpectedInstall(install.addon));
    return check_test_completed(arguments);
  },

  onExternalInstall: function(aAddon, existingAddon, aRequiresRestart) {
    do_check_eq("onExternalInstall", getExpectedInstall(aAddon));
    do_check_false(aRequiresRestart);
    return check_test_completed(arguments);
  }
};

function hasFlag(aBits, aFlag) {
  return (aBits & aFlag) != 0;
}

// Just a wrapper around setting the expected events
function prepare_test(aExpectedEvents, aExpectedInstalls, aNext) {
  AddonManager.addAddonListener(AddonListener);
  AddonManager.addInstallListener(InstallListener);

  gExpectedInstalls = aExpectedInstalls;
  gExpectedEvents = aExpectedEvents;
  gNext = aNext;
}

// Checks if all expected events have been seen and if so calls the callback
function check_test_completed(aArgs) {
  if (!gNext)
    return undefined;

  if (gExpectedInstalls instanceof Array &&
      gExpectedInstalls.length > 0)
    return undefined;

  for (let id in gExpectedInstalls) {
    let installList = gExpectedInstalls[id];
    if (installList.length > 0)
      return undefined;
  }

  for (let id in gExpectedEvents) {
    if (gExpectedEvents[id].length > 0)
      return undefined;
  }

  return gNext.apply(null, aArgs);
}

// Verifies that all the expected events for all add-ons were seen
function ensure_test_completed() {
  for (let i in gExpectedEvents) {
    if (gExpectedEvents[i].length > 0)
      do_throw("Didn't see all the expected events for " + i);
  }
  gExpectedEvents = {};
  if (gExpectedInstalls)
    do_check_eq(gExpectedInstalls.length, 0);
}

/**
 * Returns a promise that resolves when the given add-on event is fired. The
 * resolved value is an array of arguments passed for the event.
 */
function promiseAddonEvent(event) {
  return new Promise(resolve => {
    let listener = {
      [event]: function(...args) {
        AddonManager.removeAddonListener(listener);
        resolve(args);
      }
    }

    AddonManager.addAddonListener(listener);
  });
}

/**
 * A helper method to install an array of AddonInstall to completion and then
 * call a provided callback.
 *
 * @param   aInstalls
 *          The array of AddonInstalls to install
 * @param   aCallback
 *          The callback to call when all installs have finished
 */
function completeAllInstalls(aInstalls, aCallback) {
  let count = aInstalls.length;

  if (count == 0) {
    aCallback();
    return;
  }

  function installCompleted(aInstall) {
    aInstall.removeListener(listener);

    if (--count == 0)
      do_execute_soon(aCallback);
  }

  let listener = {
    onDownloadFailed: installCompleted,
    onDownloadCancelled: installCompleted,
    onInstallFailed: installCompleted,
    onInstallCancelled: installCompleted,
    onInstallEnded: installCompleted,
    onInstallPostponed: installCompleted,
  };

  aInstalls.forEach(function(aInstall) {
    aInstall.addListener(listener);
    aInstall.install();
  });
}

function promiseCompleteAllInstalls(aInstalls) {
  return new Promise(resolve => {
    completeAllInstalls(aInstalls, resolve);
  });
}

/**
 * A helper method to install an array of files and call a callback after the
 * installs are completed.
 *
 * @param   aFiles
 *          The array of files to install
 * @param   aCallback
 *          The callback to call when all installs have finished
 * @param   aIgnoreIncompatible
 *          Optional parameter to ignore add-ons that are incompatible in
 *          aome way with the application
 */
function installAllFiles(aFiles, aCallback, aIgnoreIncompatible) {
  let count = aFiles.length;
  let installs = [];
  function callback() {
    if (aCallback) {
      aCallback();
    }
  }
  aFiles.forEach(function(aFile) {
    AddonManager.getInstallForFile(aFile, function(aInstall) {
      if (!aInstall)
        do_throw("No AddonInstall created for " + aFile.path);
      do_check_eq(aInstall.state, AddonManager.STATE_DOWNLOADED);

      if (!aIgnoreIncompatible || !aInstall.addon.appDisabled)
        installs.push(aInstall);

      if (--count == 0)
        completeAllInstalls(installs, callback);
    });
  });
}

function promiseInstallAllFiles(aFiles, aIgnoreIncompatible) {
  let deferred = Promise.defer();
  installAllFiles(aFiles, deferred.resolve, aIgnoreIncompatible);
  return deferred.promise;
}

// Get the profile directory for tests to use.
const gProfD = do_get_profile();

const EXTENSIONS_DB = "extensions.json";
var gExtensionsJSON = gProfD.clone();
gExtensionsJSON.append(EXTENSIONS_DB);

const EXTENSIONS_INI = "extensions.ini";
var gExtensionsINI = gProfD.clone();
gExtensionsINI.append(EXTENSIONS_INI);

// Enable more extensive EM logging
Services.prefs.setBoolPref("extensions.logging.enabled", true);

// By default only load extensions from the profile install location
Services.prefs.setIntPref("extensions.enabledScopes", AddonManager.SCOPE_PROFILE);

// By default don't disable add-ons from any scope
Services.prefs.setIntPref("extensions.autoDisableScopes", 0);

// By default, don't cache add-ons in AddonRepository.jsm
Services.prefs.setBoolPref("extensions.getAddons.cache.enabled", false);

// Disable the compatibility updates window by default
Services.prefs.setBoolPref("extensions.showMismatchUI", false);

// Point update checks to the local machine for fast failures
Services.prefs.setCharPref("extensions.update.url", "http://127.0.0.1/updateURL");
Services.prefs.setCharPref("extensions.update.background.url", "http://127.0.0.1/updateBackgroundURL");
Services.prefs.setCharPref("extensions.blocklist.url", "http://127.0.0.1/blocklistURL");
Services.prefs.setCharPref("services.settings.server", "http://localhost/dummy-kinto/v1");

// By default ignore bundled add-ons
Services.prefs.setBoolPref("extensions.installDistroAddons", false);

// By default use strict compatibility
Services.prefs.setBoolPref("extensions.strictCompatibility", true);

// By default don't check for hotfixes
Services.prefs.setCharPref("extensions.hotfix.id", "");

// By default, set min compatible versions to 0
Services.prefs.setCharPref(PREF_EM_MIN_COMPAT_APP_VERSION, "0");
Services.prefs.setCharPref(PREF_EM_MIN_COMPAT_PLATFORM_VERSION, "0");

// Ensure signature checks are enabled by default
Services.prefs.setBoolPref(PREF_XPI_SIGNATURES_REQUIRED, true);

// Register a temporary directory for the tests.
const gTmpD = gProfD.clone();
gTmpD.append("temp");
gTmpD.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
registerDirectory("TmpD", gTmpD);

// Create a replacement app directory for the tests.
const gAppDirForAddons = gProfD.clone();
gAppDirForAddons.append("appdir-addons");
gAppDirForAddons.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);
registerDirectory("XREAddonAppDir", gAppDirForAddons);

// Write out an empty blocklist.xml file to the profile to ensure nothing
// is blocklisted by default
var blockFile = gProfD.clone();
blockFile.append("blocklist.xml");
var stream = AM_Cc["@mozilla.org/network/file-output-stream;1"].
             createInstance(AM_Ci.nsIFileOutputStream);
stream.init(blockFile, FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE,
            FileUtils.PERMS_FILE, 0);

var data = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" +
           "<blocklist xmlns=\"http://www.mozilla.org/2006/addons-blocklist\">\n" +
           "</blocklist>\n";
stream.write(data, data.length);
stream.close();

// Copies blocklistFile (an nsIFile) to gProfD/blocklist.xml.
function copyBlocklistToProfile(blocklistFile) {
  var dest = gProfD.clone();
  dest.append("blocklist.xml");
  if (dest.exists())
    dest.remove(false);
  blocklistFile.copyTo(gProfD, "blocklist.xml");
  dest.lastModifiedTime = Date.now();
}

// Throw a failure and attempt to abandon the test if it looks like it is going
// to timeout
function timeout() {
  timer = null;
  do_throw("Test ran longer than " + TIMEOUT_MS + "ms");

  // Attempt to bail out of the test
  do_test_finished();
}

var timer = AM_Cc["@mozilla.org/timer;1"].createInstance(AM_Ci.nsITimer);
timer.init(timeout, TIMEOUT_MS, AM_Ci.nsITimer.TYPE_ONE_SHOT);

// Make sure that a given path does not exist
function pathShouldntExist(aPath) {
  if (aPath.exists()) {
    do_throw("Test cleanup: path " + aPath.path + " exists when it should not");
  }
}

do_register_cleanup(function addon_cleanup() {
  if (timer)
    timer.cancel();

  for (let file of temp_xpis) {
    if (file.exists())
      file.remove(false);
  }

  // Check that the temporary directory is empty
  var dirEntries = gTmpD.directoryEntries
                        .QueryInterface(AM_Ci.nsIDirectoryEnumerator);
  var entry;
  while ((entry = dirEntries.nextFile)) {
    do_throw("Found unexpected file in temporary directory: " + entry.leafName);
  }
  dirEntries.close();

  try {
    gAppDirForAddons.remove(true);
  } catch (ex) { do_print("Got exception removing addon app dir, " + ex); }

  var testDir = gProfD.clone();
  testDir.append("extensions");
  testDir.append("trash");
  pathShouldntExist(testDir);

  testDir.leafName = "staged";
  pathShouldntExist(testDir);

  shutdownManager();

  // Clear commonly set prefs.
  try {
    Services.prefs.clearUserPref(PREF_EM_CHECK_UPDATE_SECURITY);
  } catch (e) {}
  try {
    Services.prefs.clearUserPref(PREF_EM_STRICT_COMPATIBILITY);
  } catch (e) {}
});

/**
 * Creates a new HttpServer for testing, and begins listening on the
 * specified port. Automatically shuts down the server when the test
 * unit ends.
 *
 * @param port
 *        The port to listen on. If omitted, listen on a random
 *        port. The latter is the preferred behavior.
 *
 * @return HttpServer
 */
function createHttpServer(port = -1) {
  let server = new HttpServer();
  server.start(port);

  do_register_cleanup(() => {
    return new Promise(resolve => {
      server.stop(resolve);
    });
  });

  return server;
}

/**
 * Handler function that responds with the interpolated
 * static file associated to the URL specified by request.path.
 * This replaces the %PORT% entries in the file with the actual
 * value of the running server's port (stored in gPort).
 */
function interpolateAndServeFile(request, response) {
  try {
    let file = gUrlToFileMap[request.path];
    var data = "";
    var fstream = Components.classes["@mozilla.org/network/file-input-stream;1"].
    createInstance(Components.interfaces.nsIFileInputStream);
    var cstream = Components.classes["@mozilla.org/intl/converter-input-stream;1"].
    createInstance(Components.interfaces.nsIConverterInputStream);
    fstream.init(file, -1, 0, 0);
    cstream.init(fstream, "UTF-8", 0, 0);

    let str = {};
    let read = 0;
    do {
      // read as much as we can and put it in str.value
      read = cstream.readString(0xffffffff, str);
      data += str.value;
    } while (read != 0);
    data = data.replace(/%PORT%/g, gPort);

    response.write(data);
  } catch (e) {
    do_throw(`Exception while serving interpolated file: ${e}\n${e.stack}`);
  } finally {
    cstream.close(); // this closes fstream as well
  }
}

/**
 * Sets up a path handler for the given URL and saves the
 * corresponding file in the global url -> file map.
 *
 * @param  url
 *         the actual URL
 * @param  file
 *         nsILocalFile representing a static file
 */
function mapUrlToFile(url, file, server) {
  server.registerPathHandler(url, interpolateAndServeFile);
  gUrlToFileMap[url] = file;
}

function mapFile(path, server) {
  mapUrlToFile(path, do_get_file(path), server);
}

/**
 * Take out the port number in an URL
 *
 * @param url
 *        String that represents an URL with a port number in it
 */
function remove_port(url) {
  if (typeof url === "string")
    return url.replace(/:\d+/, "");
  return url;
}
// Wrap a function (typically a callback) to catch and report exceptions
function do_exception_wrap(func) {
  return function() {
    try {
      func.apply(null, arguments);
    }
    catch(e) {
      do_report_unexpected_exception(e);
    }
  };
}

/**
 * Change the schema version of the JSON extensions database
 */
function changeXPIDBVersion(aNewVersion, aMutator = undefined) {
  let jData = loadJSON(gExtensionsJSON);
  jData.schemaVersion = aNewVersion;
  if (aMutator)
    aMutator(jData);
  saveJSON(jData, gExtensionsJSON);
}

/**
 * Load a file into a string
 */
function loadFile(aFile) {
  let data = "";
  let fstream = Components.classes["@mozilla.org/network/file-input-stream;1"].
          createInstance(Components.interfaces.nsIFileInputStream);
  let cstream = Components.classes["@mozilla.org/intl/converter-input-stream;1"].
          createInstance(Components.interfaces.nsIConverterInputStream);
  fstream.init(aFile, -1, 0, 0);
  cstream.init(fstream, "UTF-8", 0, 0);
  let str = {};
  let read = 0;
  do {
    read = cstream.readString(0xffffffff, str); // read as much as we can and put it in str.value
    data += str.value;
  } while (read != 0);
  cstream.close();
  return data;
}

/**
 * Raw load of a JSON file
 */
function loadJSON(aFile) {
  let data = loadFile(aFile);
  do_print("Loaded JSON file " + aFile.path);
  return(JSON.parse(data));
}

/**
 * Raw save of a JSON blob to file
 */
function saveJSON(aData, aFile) {
  do_print("Starting to save JSON file " + aFile.path);
  let stream = FileUtils.openSafeFileOutputStream(aFile);
  let converter = AM_Cc["@mozilla.org/intl/converter-output-stream;1"].
    createInstance(AM_Ci.nsIConverterOutputStream);
  converter.init(stream, "UTF-8", 0, 0x0000);
  // XXX pretty print the JSON while debugging
  converter.writeString(JSON.stringify(aData, null, 2));
  converter.flush();
  // nsConverterOutputStream doesn't finish() safe output streams on close()
  FileUtils.closeSafeFileOutputStream(stream);
  converter.close();
  do_print("Done saving JSON file " + aFile.path);
}

/**
 * Create a callback function that calls do_execute_soon on an actual callback and arguments
 */
function callback_soon(aFunction) {
  return function(...args) {
    do_execute_soon(function() {
      aFunction.apply(null, args);
    }, aFunction.name ? "delayed callback " + aFunction.name : "delayed callback");
  }
}

/**
 * A promise-based variant of AddonManager.getAddonsByIDs.
 *
 * @param {array} list As the first argument of AddonManager.getAddonsByIDs
 * @return {promise}
 * @resolve {array} The list of add-ons sent by AddonManaget.getAddonsByIDs to
 * its callback.
 */
function promiseAddonsByIDs(list) {
  return new Promise(resolve => AddonManager.getAddonsByIDs(list, resolve));
}

/**
 * A promise-based variant of AddonManager.getAddonByID.
 *
 * @param {string} aId The ID of the add-on.
 * @return {promise}
 * @resolve {AddonWrapper} The corresponding add-on, or null.
 */
function promiseAddonByID(aId) {
  return new Promise(resolve => AddonManager.getAddonByID(aId, resolve));
}

/**
 * A promise-based variant of AddonManager.getAddonsWithOperationsByTypes
 *
 * @param {array} aTypes The first argument to
 *                       AddonManager.getAddonsWithOperationsByTypes
 * @return {promise}
 * @resolve {array} The list of add-ons sent by
 *                  AddonManaget.getAddonsWithOperationsByTypes to its callback.
 */
function promiseAddonsWithOperationsByTypes(aTypes) {
  return new Promise(resolve => AddonManager.getAddonsWithOperationsByTypes(aTypes, resolve));
}

/**
 * Returns a promise that will be resolved when an add-on update check is
 * complete. The value resolved will be an AddonInstall if a new version was
 * found.
 */
function promiseFindAddonUpdates(addon, reason = AddonManager.UPDATE_WHEN_PERIODIC_UPDATE) {
  return new Promise((resolve, reject) => {
    let result = {};
    addon.findUpdates({
      onNoCompatibilityUpdateAvailable: function(addon2) {
        if ("compatibilityUpdate" in result) {
          do_throw("Saw multiple compatibility update events");
        }
        equal(addon, addon2, "onNoCompatibilityUpdateAvailable");
        result.compatibilityUpdate = false;
      },

      onCompatibilityUpdateAvailable: function(addon2) {
        if ("compatibilityUpdate" in result) {
          do_throw("Saw multiple compatibility update events");
        }
        equal(addon, addon2, "onCompatibilityUpdateAvailable");
        result.compatibilityUpdate = true;
      },

      onNoUpdateAvailable: function(addon2) {
        if ("updateAvailable" in result) {
          do_throw("Saw multiple update available events");
        }
        equal(addon, addon2, "onNoUpdateAvailable");
        result.updateAvailable = false;
      },

      onUpdateAvailable: function(addon2, install) {
        if ("updateAvailable" in result) {
          do_throw("Saw multiple update available events");
        }
        equal(addon, addon2, "onUpdateAvailable");
        result.updateAvailable = install;
      },

      onUpdateFinished: function(addon2, error) {
        equal(addon, addon2, "onUpdateFinished");
        if (error == AddonManager.UPDATE_STATUS_NO_ERROR) {
          resolve(result);
        } else {
          result.error = error;
          reject(result);
        }
      }
    }, reason);
  });
}

/**
 * Monitors console output for the duration of a task, and returns a promise
 * which resolves to a tuple containing a list of all console messages
 * generated during the task's execution, and the result of the task itself.
 *
 * @param {function} aTask
 *                   The task to run while monitoring console output. May be
 *                   either a generator function, per Task.jsm, or an ordinary
 *                   function which returns promose.
 * @return {Promise<[Array<nsIConsoleMessage>, *]>}
 */
var promiseConsoleOutput = Task.async(function*(aTask) {
  const DONE = "=== xpcshell test console listener done ===";

  let listener, messages = [];
  let awaitListener = new Promise(resolve => {
    listener = msg => {
      if (msg == DONE) {
        resolve();
      } else {
        msg instanceof Components.interfaces.nsIScriptError;
        messages.push(msg);
      }
    }
  });

  Services.console.registerListener(listener);
  try {
    let result = yield aTask();

    Services.console.logStringMessage(DONE);
    yield awaitListener;

    return { messages, result };
  }
  finally {
    Services.console.unregisterListener(listener);
  }
});

/**
 * Creates an extension proxy file.
 * See: https://developer.mozilla.org/en-US/Add-ons/Setting_up_extension_development_environment#Firefox_extension_proxy_file
 * @param   aDir
 *          The directory to add the proxy file to.
 * @param   aAddon
 *          An nsIFile for the add-on file that this is a proxy file for.
 * @param   aId
 *          A string to use for the add-on ID.
 * @return  An nsIFile for the proxy file.
 */
function writeProxyFileToDir(aDir, aAddon, aId) {
  let dir = aDir.clone();

  if (!dir.exists())
    dir.create(AM_Ci.nsIFile.DIRECTORY_TYPE, FileUtils.PERMS_DIRECTORY);

  let file = dir.clone();
  file.append(aId);

  let addonPath = aAddon.path;

  let fos = AM_Cc["@mozilla.org/network/file-output-stream;1"].
            createInstance(AM_Ci.nsIFileOutputStream);
  fos.init(file,
           FileUtils.MODE_WRONLY | FileUtils.MODE_CREATE | FileUtils.MODE_TRUNCATE,
           FileUtils.PERMS_FILE, 0);
  fos.write(addonPath, addonPath.length);
  fos.close();

  return file;
}
