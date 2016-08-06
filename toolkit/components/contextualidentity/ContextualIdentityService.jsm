/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

this.EXPORTED_SYMBOLS = ["ContextualIdentityService"];

const {classes: Cc, interfaces: Ci, utils: Cu, results: Cr} = Components;

Cu.import("resource://gre/modules/XPCOMUtils.jsm");
Cu.import("resource://gre/modules/Services.jsm");

const DEFAULT_TAB_COLOR = "#909090";
const SAVE_DELAY_MS = 1500;

XPCOMUtils.defineLazyGetter(this, "gBrowserBundle", function() {
  return Services.strings.createBundle("chrome://browser/locale/browser.properties");
});

XPCOMUtils.defineLazyGetter(this, "gTextDecoder", function () {
  return new TextDecoder();
});

XPCOMUtils.defineLazyGetter(this, "gTextEncoder", function () {
  return new TextEncoder();
});

XPCOMUtils.defineLazyModuleGetter(this, "AsyncShutdown",
                                  "resource://gre/modules/AsyncShutdown.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "OS",
                                  "resource://gre/modules/osfile.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "DeferredTask",
                                  "resource://gre/modules/DeferredTask.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "FileUtils",
                                  "resource://gre/modules/FileUtils.jsm");

function _ContextualIdentityService(path) {
  this.init(path);
}

_ContextualIdentityService.prototype = {
  _defaultIdentities: [
    { userContextId: 1,
      public: true,
      icon: "chrome://browser/skin/usercontext/personal.svg",
      color: "#00a7e0",
      l10nID: "userContextPersonal.label",
      accessKey: "userContextPersonal.accesskey",
      telemetryId: 1,
    },
    { userContextId: 2,
      public: true,
      icon: "chrome://browser/skin/usercontext/work.svg",
      color: "#f89c24",
      l10nID: "userContextWork.label",
      accessKey: "userContextWork.accesskey",
      telemetryId: 2,
    },
    { userContextId: 3,
      public: true,
      icon: "chrome://browser/skin/usercontext/banking.svg",
      color: "#7dc14c",
      l10nID: "userContextBanking.label",
      accessKey: "userContextBanking.accesskey",
      telemetryId: 3,
    },
    { userContextId: 4,
      public: true,
      icon: "chrome://browser/skin/usercontext/shopping.svg",
      color: "#ee5195",
      l10nID: "userContextShopping.label",
      accessKey: "userContextShopping.accesskey",
      telemetryId: 4,
    },
    { userContextId: 5,
      public: false,
      icon: "",
      color: "",
      name: "userContextIdInternal.thumbnail",
      accessKey: "" },
  ],

  _identities: null,
  _openedIdentities: new Set(),
  _lastUserContextId: 0,

  _path: null,
  _dataReady: false,

  _saver: null,

  init(path) {
    this._path = path;
    this._saver = new DeferredTask(() => this.save(), SAVE_DELAY_MS);
    AsyncShutdown.profileBeforeChange.addBlocker("ContextualIdentityService: writing data",
                                                 () => this._saver.finalize());

    this.load();
  },

  load() {
    OS.File.read(this._path).then(bytes => {
      // If synchronous loading happened in the meantime, exit now.
      if (this._dataReady) {
        return;
      }

      try {
        let data = JSON.parse(gTextDecoder.decode(bytes));
        if (data.version != 1) {
          dump("ERROR - ContextualIdentityService - Unknown version found in " + this._path + "\n");
          this.loadError(null);
          return;
        }

        this._identities = data.identities;
        this._lastUserContextId = data.lastUserContextId;

        this._dataReady = true;
      } catch(error) {
        this.loadError(error);
      }
    }, (error) => {
      this.loadError(error);
    });
  },

  loadError(error) {
    if (error != null &&
        !(error instanceof OS.File.Error && error.becauseNoSuchFile) &&
        !(error instanceof Components.Exception &&
          error.result == Cr.NS_ERROR_FILE_NOT_FOUND)) {
      // Let's report the error.
      Cu.reportError(error);
    }

    // If synchronous loading happened in the meantime, exit now.
    if (this._dataReady) {
      return;
    }

    this._identities = this._defaultIdentities;
    this._lastUserContextId = this._defaultIdentities.length;

    this._dataReady = true;

    this.saveSoon();
  },

  saveSoon() {
    this._saver.arm();
  },

  save() {
   let object = {
     version: 1,
     lastUserContextId: this._lastUserContextId,
     identities: this._identities
   };

   let bytes = gTextEncoder.encode(JSON.stringify(object));
   return OS.File.writeAtomic(this._path, bytes,
                              { tmpPath: this._path + ".tmp" });
  },

  create(name, icon, color) {
    let identity = {
      userContextId: ++this._lastUserContextId,
      public: true,
      icon,
      color,
      name
    };

    this._identities.push(identity);
    this.saveSoon();

    return Cu.cloneInto(identity, {});
  },

  update(userContextId, name, icon, color) {
    let identity = this._identities.find(identity => identity.userContextId == userContextId &&
                                         identity.public);
    if (identity) {
      identity.name = name;
      identity.color = color;
      identity.icon = icon;
      delete identity.l10nID;
      delete identity.accessKey;
      this.saveSoon();
    }

    return !!identity;
  },

  remove(userContextId) {
    let index = this._identities.findIndex(i => i.userContextId == userContextId && i.public);
    if (index == -1) {
      return false;
    }

    Services.obs.notifyObservers(null, "clear-origin-data",
                                 JSON.stringify({ userContextId }));

    this._identities.splice(index, 1);
    this._openedIdentities.delete(userContextId);
    this.saveSoon();

    return true;
  },

  ensureDataReady() {
    if (this._dataReady) {
      return;
    }

    try {
      // This reads the file and automatically detects the UTF-8 encoding.
      let inputStream = Cc["@mozilla.org/network/file-input-stream;1"]
                          .createInstance(Ci.nsIFileInputStream);
      inputStream.init(new FileUtils.File(this._path),
                       FileUtils.MODE_RDONLY, FileUtils.PERMS_FILE, 0);
      try {
        let json = Cc["@mozilla.org/dom/json;1"].createInstance(Ci.nsIJSON);
        this._identities = json.decodeFromStream(inputStream,
                                                 inputStream.available());
        this._dataReady = true;
      } finally {
        inputStream.close();
      }
    } catch (error) {
      this.loadError(error);
      return;
    }
  },

  getIdentities() {
    this.ensureDataReady();
    return Cu.cloneInto(this._identities.filter(info => info.public), {});
  },

  getPrivateIdentity(name) {
    this.ensureDataReady();
    return Cu.cloneInto(this._identities.find(info => !info.public && info.name == name), {});
  },

  getIdentityFromId(userContextId) {
    this.ensureDataReady();
    return Cu.cloneInto(this._identities.find(info => info.userContextId == userContextId &&
                                              info.public), {});
  },

  getUserContextLabel(userContextId) {
    let identity = this.getIdentityFromId(userContextId);
    if (!identity.public) {
      return "";
    }

    // We cannot localize the user-created identity names.
    if (identity.name) {
      return identity.name;
    }

    return gBrowserBundle.GetStringFromName(identity.l10nID);
  },

  setTabStyle(tab) {
    // inline style is only a temporary fix for some bad performances related
    // to the use of CSS vars. This code will be removed in bug 1278177.
    if (!tab.hasAttribute("usercontextid")) {
      tab.style.removeProperty("background-image");
      tab.style.removeProperty("background-size");
      tab.style.removeProperty("background-repeat");
      return;
    }

    let userContextId = tab.getAttribute("usercontextid");
    let identity = this.getIdentityFromId(userContextId);

    let color = identity ? identity.color : DEFAULT_TAB_COLOR;
    tab.style.backgroundImage = "linear-gradient(to right, transparent 20%, " + color + " 30%, " + color + " 70%, transparent 80%)";
    tab.style.backgroundSize = "auto 2px";
    tab.style.backgroundRepeat = "no-repeat";
  },

  telemetry(userContextId) {
    let identity = this.getIdentityFromId(userContextId);

    // Let's ignore unknown identities for now.
    if (!identity || !identity.public) {
      return;
    }

    if (this._openedIdentities.has(userContextId)) {
      this._openedIdentities.add(userContextId);
      Services.telemetry.getHistogramById("UNIQUE_CONTAINERS_OPENED").add(1);
    }

    Services.telemetry.getHistogramById("TOTAL_CONTAINERS_OPENED").add(1);

    if (identity.telemetryId) {
      Services.telemetry.getHistogramById("CONTAINER_USED")
                        .add(identity.telemetryId);
    }
  },

  createNewInstanceForTesting(path) {
    return new _ContextualIdentityService(path);
  },
};

let path = OS.Path.join(OS.Constants.Path.profileDir, "containers.json");
this.ContextualIdentityService = new _ContextualIdentityService(path);
