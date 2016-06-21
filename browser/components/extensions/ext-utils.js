/* -*- Mode: indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set sts=2 sw=2 et tw=80: */
"use strict";

XPCOMUtils.defineLazyModuleGetter(this, "CustomizableUI",
                                  "resource:///modules/CustomizableUI.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "PrivateBrowsingUtils",
                                  "resource://gre/modules/PrivateBrowsingUtils.jsm");

Cu.import("resource://gre/modules/ExtensionUtils.jsm");
Cu.import("resource://gre/modules/AddonManager.jsm");
Cu.import("resource://gre/modules/AppConstants.jsm");

const XUL_NS = "http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul";

const INTEGER = /^[1-9]\d*$/;

var {
  EventManager,
  instanceOf,
} = ExtensionUtils;

// This file provides some useful code for the |tabs| and |windows|
// modules. All of the code is installed on |global|, which is a scope
// shared among the different ext-*.js scripts.


// Manages icon details for toolbar buttons in the |pageAction| and
// |browserAction| APIs.
global.IconDetails = {
  // Normalizes the various acceptable input formats into an object
  // with icon size as key and icon URL as value.
  //
  // If a context is specified (function is called from an extension):
  // Throws an error if an invalid icon size was provided or the
  // extension is not allowed to load the specified resources.
  //
  // If no context is specified, instead of throwing an error, this
  // function simply logs a warning message.
  normalize(details, extension, context = null) {
    let result = {};

    try {
      if (details.imageData) {
        let imageData = details.imageData;

        // The global might actually be from Schema.jsm, which
        // normalizes most of our arguments. In that case it won't have
        // an ImageData property. But Schema.jsm doesn't normalize
        // actual ImageData objects, so they will come from a global
        // with the right property.
        if (instanceOf(imageData, "ImageData")) {
          imageData = {"19": imageData};
        }

        for (let size of Object.keys(imageData)) {
          if (!INTEGER.test(size)) {
            throw new Error(`Invalid icon size ${size}, must be an integer`);
          }
          result[size] = this.convertImageDataToPNG(imageData[size], context);
        }
      }

      if (details.path) {
        let path = details.path;
        if (typeof path != "object") {
          path = {"19": path};
        }

        let baseURI = context ? context.uri : extension.baseURI;

        for (let size of Object.keys(path)) {
          if (!INTEGER.test(size)) {
            throw new Error(`Invalid icon size ${size}, must be an integer`);
          }

          let url = baseURI.resolve(path[size]);

          // The Chrome documentation specifies these parameters as
          // relative paths. We currently accept absolute URLs as well,
          // which means we need to check that the extension is allowed
          // to load them. This will throw an error if it's not allowed.
          Services.scriptSecurityManager.checkLoadURIStrWithPrincipal(
            extension.principal, url,
            Services.scriptSecurityManager.DISALLOW_SCRIPT);

          result[size] = url;
        }
      }
    } catch (e) {
      // Function is called from extension code, delegate error.
      if (context) {
        throw e;
      }
      // If there's no context, it's because we're handling this
      // as a manifest directive. Log a warning rather than
      // raising an error.
      extension.manifestError(`Invalid icon data: ${e}`);
    }

    return result;
  },

  // Returns the appropriate icon URL for the given icons object and the
  // screen resolution of the given window.
  getURL(icons, window, extension, size = 16) {
    const DEFAULT = "chrome://browser/content/extension.svg";

    size *= window.devicePixelRatio;

    let bestSize = null;
    if (icons[size]) {
      bestSize = size;
    } else if (icons[2 * size]) {
      bestSize = 2 * size;
    } else {
      let sizes = Object.keys(icons)
                        .map(key => parseInt(key, 10))
                        .sort((a, b) => a - b);

      bestSize = sizes.find(candidate => candidate > size) || sizes.pop();
    }

    if (bestSize) {
      return {size: bestSize, icon: icons[bestSize]};
    }

    return {size, icon: DEFAULT};
  },

  convertImageDataToPNG(imageData, context) {
    let document = context.contentWindow.document;
    let canvas = document.createElementNS("http://www.w3.org/1999/xhtml", "canvas");
    canvas.width = imageData.width;
    canvas.height = imageData.height;
    canvas.getContext("2d").putImageData(imageData, 0, 0);

    return canvas.toDataURL("image/png");
  },
};

global.makeWidgetId = id => {
  id = id.toLowerCase();
  // FIXME: This allows for collisions.
  return id.replace(/[^a-z0-9_-]/g, "_");
};

function promisePopupShown(popup) {
  return new Promise(resolve => {
    if (popup.state == "open") {
      resolve();
    } else {
      popup.addEventListener("popupshown", function onPopupShown(event) {
        popup.removeEventListener("popupshown", onPopupShown);
        resolve();
      });
    }
  });
}

XPCOMUtils.defineLazyGetter(global, "stylesheets", () => {
  let styleSheetService = Cc["@mozilla.org/content/style-sheet-service;1"]
      .getService(Components.interfaces.nsIStyleSheetService);
  let styleSheetURI = Services.io.newURI("chrome://browser/content/extension.css",
                                         null, null);
  let styleSheet = styleSheetService.preloadSheet(styleSheetURI,
                                                  styleSheetService.AGENT_SHEET);
  let stylesheets = [styleSheet];

  if (AppConstants.platform === "macosx") {
    styleSheetURI = Services.io.newURI("chrome://browser/content/extension-mac.css",
                                       null, null);
    let macStyleSheet = styleSheetService.preloadSheet(styleSheetURI,
                                                       styleSheetService.AGENT_SHEET);
    stylesheets.push(macStyleSheet);
  }
  return stylesheets;
});

class BasePopup {
  constructor(extension, viewNode, popupURL, browserStyle) {
    let popupURI = Services.io.newURI(popupURL, null, extension.baseURI);

    Services.scriptSecurityManager.checkLoadURIWithPrincipal(
      extension.principal, popupURI,
      Services.scriptSecurityManager.DISALLOW_SCRIPT);

    this.extension = extension;
    this.popupURI = popupURI;
    this.viewNode = viewNode;
    this.browserStyle = browserStyle;
    this.window = viewNode.ownerDocument.defaultView;

    this.contentReady = new Promise(resolve => {
      this._resolveContentReady = resolve;
    });

    this.viewNode.addEventListener(this.DESTROY_EVENT, this);

    this.browser = null;
    this.browserReady = this.createBrowser(viewNode, popupURI);
  }

  destroy() {
    this.browserReady.then(() => {
      this.browser.removeEventListener("DOMWindowCreated", this, true);
      this.browser.removeEventListener("load", this, true);
      this.browser.removeEventListener("DOMTitleChanged", this, true);
      this.browser.removeEventListener("DOMWindowClose", this, true);

      this.viewNode.removeEventListener(this.DESTROY_EVENT, this);

      this.browser.remove();

      this.browser = null;
      this.viewNode = null;
    });
  }

  // Returns the name of the event fired on `viewNode` when the popup is being
  // destroyed. This must be implemented by every subclass.
  get DESTROY_EVENT() {
    throw new Error("Not implemented");
  }

  handleEvent(event) {
    switch (event.type) {
      case this.DESTROY_EVENT:
        this.destroy();
        break;

      case "DOMWindowCreated":
        if (this.browserStyle && event.target === this.browser.contentDocument) {
          let winUtils = this.browser.contentWindow
              .QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowUtils);
          for (let stylesheet of global.stylesheets) {
            winUtils.addSheet(stylesheet, winUtils.AGENT_SHEET);
          }
        }
        break;

      case "DOMWindowClose":
        if (event.target === this.browser.contentWindow) {
          event.preventDefault();
          this.closePopup();
        }
        break;

      case "DOMTitleChanged":
        this.viewNode.setAttribute("aria-label", this.browser.contentTitle);
        break;

      case "load":
        // We use a capturing listener, so we get this event earlier than any
        // load listeners in the content page. Resizing after a timeout ensures
        // that we calculate the size after the entire event cycle has completed
        // (unless someone spins the event loop, anyway), and hopefully after
        // the content has made any modifications.
        //
        // In the future, to match Chrome's behavior, we'll need to update this
        // dynamically, probably in response to MozScrolledAreaChanged events.
        this.window.setTimeout(() => this.resizeBrowser(), 0);
        break;
    }
  }

  createBrowser(viewNode, popupURI) {
    let document = viewNode.ownerDocument;
    this.browser = document.createElementNS(XUL_NS, "browser");
    this.browser.setAttribute("type", "content");
    this.browser.setAttribute("disableglobalhistory", "true");
    this.browser.setAttribute("webextension-view-type", "popup");

    // Note: When using noautohide panels, the popup manager will add width and
    // height attributes to the panel, breaking our resize code, if the browser
    // starts out smaller than 30px by 10px. This isn't an issue now, but it
    // will be if and when we popup debugging.

    // This overrides the content's preferred size when displayed in a
    // fixed-size, slide-in panel.
    this.browser.setAttribute("flex", "1");

    viewNode.appendChild(this.browser);

    return new Promise(resolve => {
      // The first load event is for about:blank.
      // We can't finish setting up the browser until the binding has fully
      // initialized. Waiting for the first load event guarantees that it has.
      let loadListener = event => {
        this.browser.removeEventListener("load", loadListener, true);
        resolve();
      };
      this.browser.addEventListener("load", loadListener, true);
    }).then(() => {
      let {contentWindow} = this.browser;

      contentWindow.QueryInterface(Ci.nsIInterfaceRequestor)
                   .getInterface(Ci.nsIDOMWindowUtils)
                   .allowScriptsToClose();

      this.browser.setAttribute("src", popupURI.spec);

      this.browser.addEventListener("DOMWindowCreated", this, true);
      this.browser.addEventListener("load", this, true);
      this.browser.addEventListener("DOMTitleChanged", this, true);
      this.browser.addEventListener("DOMWindowClose", this, true);
    });
  }

  // Resizes the browser to match the preferred size of the content.
  resizeBrowser() {
    if (!this.browser) {
      return;
    }

    let width, height;
    try {
      let w = {}, h = {};
      this.browser.docShell.contentViewer.getContentSize(w, h);

      width = w.value / this.window.devicePixelRatio;
      height = h.value / this.window.devicePixelRatio;

      // The width calculation is imperfect, and is often a fraction of a pixel
      // too narrow, even after taking the ceiling, which causes lines of text
      // to wrap.
      width += 1;
    } catch (e) {
      // getContentSize can throw
      [width, height] = [400, 400];
    }

    width = Math.ceil(Math.min(width, 800));
    height = Math.ceil(Math.min(height, 600));

    this.browser.style.width = `${width}px`;
    this.browser.style.height = `${height}px`;

    this._resolveContentReady();
  }
}

global.PanelPopup = class PanelPopup extends BasePopup {
  constructor(extension, imageNode, popupURL, browserStyle) {
    let document = imageNode.ownerDocument;

    let panel = document.createElement("panel");
    panel.setAttribute("id", makeWidgetId(extension.id) + "-panel");
    panel.setAttribute("class", "browser-extension-panel");
    panel.setAttribute("type", "arrow");
    panel.setAttribute("role", "group");

    document.getElementById("mainPopupSet").appendChild(panel);

    super(extension, panel, popupURL, browserStyle);

    this.contentReady.then(() => {
      panel.openPopup(imageNode, "bottomcenter topright", 0, 0, false, false);
    });
  }

  get DESTROY_EVENT() {
    return "popuphidden";
  }

  destroy() {
    super.destroy();
    this.viewNode.remove();
  }

  closePopup() {
    promisePopupShown(this.viewNode).then(() => {
      // Make sure we're not already destroyed.
      if (this.viewNode) {
        this.viewNode.hidePopup();
      }
    });
  }
};

global.ViewPopup = class ViewPopup extends BasePopup {
  get DESTROY_EVENT() {
    return "ViewHiding";
  }

  closePopup() {
    CustomizableUI.hidePanelForNode(this.viewNode);
  }
};

// Manages tab-specific context data, and dispatching tab select events
// across all windows.
global.TabContext = function TabContext(getDefaults, extension) {
  this.extension = extension;
  this.getDefaults = getDefaults;

  this.tabData = new WeakMap();

  AllWindowEvents.addListener("progress", this);
  AllWindowEvents.addListener("TabSelect", this);

  EventEmitter.decorate(this);
};

TabContext.prototype = {
  get(tab) {
    if (!this.tabData.has(tab)) {
      this.tabData.set(tab, this.getDefaults(tab));
    }

    return this.tabData.get(tab);
  },

  clear(tab) {
    this.tabData.delete(tab);
  },

  handleEvent(event) {
    if (event.type == "TabSelect") {
      let tab = event.target;
      this.emit("tab-select", tab);
      this.emit("location-change", tab);
    }
  },

  onLocationChange(browser, webProgress, request, locationURI, flags) {
    let gBrowser = browser.ownerDocument.defaultView.gBrowser;
    if (browser === gBrowser.selectedBrowser) {
      let tab = gBrowser.getTabForBrowser(browser);
      this.emit("location-change", tab, true);
    }
  },

  shutdown() {
    AllWindowEvents.removeListener("progress", this);
    AllWindowEvents.removeListener("TabSelect", this);
  },
};

// Manages tab mappings and permissions for a specific extension.
function ExtensionTabManager(extension) {
  this.extension = extension;

  // A mapping of tab objects to the inner window ID the extension currently has
  // the active tab permission for. The active permission for a given tab is
  // valid only for the inner window that was active when the permission was
  // granted. If the tab navigates, the inner window ID changes, and the
  // permission automatically becomes stale.
  //
  // WeakMap[tab => inner-window-id<int>]
  this.hasTabPermissionFor = new WeakMap();
}

ExtensionTabManager.prototype = {
  addActiveTabPermission(tab = TabManager.activeTab) {
    if (this.extension.hasPermission("activeTab")) {
      // Note that, unlike Chrome, we don't currently clear this permission with
      // the tab navigates. If the inner window is revived from BFCache before
      // we've granted this permission to a new inner window, the extension
      // maintains its permissions for it.
      this.hasTabPermissionFor.set(tab, tab.linkedBrowser.innerWindowID);
    }
  },

  // Returns true if the extension has the "activeTab" permission for this tab.
  // This is somewhat more permissive than the generic "tabs" permission, as
  // checked by |hasTabPermission|, in that it also allows programmatic script
  // injection without an explicit host permission.
  hasActiveTabPermission(tab) {
    // This check is redundant with addTabPermission, but cheap.
    if (this.extension.hasPermission("activeTab")) {
      return (this.hasTabPermissionFor.has(tab) &&
              this.hasTabPermissionFor.get(tab) === tab.linkedBrowser.innerWindowID);
    }
    return false;
  },

  hasTabPermission(tab) {
    return this.extension.hasPermission("tabs") || this.hasActiveTabPermission(tab);
  },

  convert(tab) {
    let window = tab.ownerDocument.defaultView;
    let browser = tab.linkedBrowser;

    let mutedInfo = {muted: tab.muted};
    if (tab.muteReason === null) {
      mutedInfo.reason = "user";
    } else if (tab.muteReason) {
      mutedInfo.reason = "extension";
      mutedInfo.extensionId = tab.muteReason;
    }

    let result = {
      id: TabManager.getId(tab),
      index: tab._tPos,
      windowId: WindowManager.getId(window),
      selected: tab.selected,
      highlighted: tab.selected,
      active: tab.selected,
      pinned: tab.pinned,
      status: TabManager.getStatus(tab),
      incognito: PrivateBrowsingUtils.isBrowserPrivate(browser),
      width: browser.frameLoader.lazyWidth || browser.clientWidth,
      height: browser.frameLoader.lazyHeight || browser.clientHeight,
      audible: tab.soundPlaying,
      mutedInfo,
    };

    if (this.hasTabPermission(tab)) {
      result.url = browser.currentURI.spec;
      if (browser.contentTitle) {
        result.title = browser.contentTitle;
      }
      let icon = window.gBrowser.getIcon(tab);
      if (icon) {
        result.favIconUrl = icon;
      }
    }

    return result;
  },

  getTabs(window) {
    return Array.from(window.gBrowser.tabs, tab => this.convert(tab));
  },
};


// Manages global mappings between XUL tabs and extension tab IDs.
global.TabManager = {
  _tabs: new WeakMap(),
  _nextId: 1,
  _initialized: false,

  // We begin listening for TabOpen and TabClose events once we've started
  // assigning IDs to tabs, so that we can remap the IDs of tabs which are moved
  // between windows.
  initListener() {
    if (this._initialized) {
      return;
    }

    AllWindowEvents.addListener("TabOpen", this);
    AllWindowEvents.addListener("TabClose", this);
    WindowListManager.addOpenListener(this.handleWindowOpen.bind(this));

    this._initialized = true;
  },

  handleEvent(event) {
    if (event.type == "TabOpen") {
      let {adoptedTab} = event.detail;
      if (adoptedTab) {
        // This tab is being created to adopt a tab from a different window.
        // Copy the ID from the old tab to the new.
        this._tabs.set(event.target, this.getId(adoptedTab));
      }
    } else if (event.type == "TabClose") {
      let {adoptedBy} = event.detail;
      if (adoptedBy) {
        // This tab is being closed because it was adopted by a new window.
        // Copy its ID to the new tab, in case it was created as the first tab
        // of a new window, and did not have an `adoptedTab` detail when it was
        // opened.
        this._tabs.set(adoptedBy, this.getId(event.target));
      }
    }
  },

  handleWindowOpen(window) {
    if (window.arguments && window.arguments[0] instanceof window.XULElement) {
      // If the first window argument is a XUL element, it means the
      // window is about to adopt a tab from another window to replace its
      // initial tab.
      let adoptedTab = window.arguments[0];

      this._tabs.set(window.gBrowser.tabs[0], this.getId(adoptedTab));
    }
  },

  getId(tab) {
    if (this._tabs.has(tab)) {
      return this._tabs.get(tab);
    }
    this.initListener();

    let id = this._nextId++;
    this._tabs.set(tab, id);
    return id;
  },

  getBrowserId(browser) {
    let gBrowser = browser.ownerDocument.defaultView.gBrowser;
    // Some non-browser windows have gBrowser but not
    // getTabForBrowser!
    if (gBrowser && gBrowser.getTabForBrowser) {
      let tab = gBrowser.getTabForBrowser(browser);
      if (tab) {
        return this.getId(tab);
      }
    }
    return -1;
  },

  getTab(tabId) {
    // FIXME: Speed this up without leaking memory somehow.
    for (let window of WindowListManager.browserWindows()) {
      if (!window.gBrowser) {
        continue;
      }
      for (let tab of window.gBrowser.tabs) {
        if (this.getId(tab) == tabId) {
          return tab;
        }
      }
    }
    return null;
  },

  get activeTab() {
    let window = WindowManager.topWindow;
    if (window && window.gBrowser) {
      return window.gBrowser.selectedTab;
    }
    return null;
  },

  getStatus(tab) {
    return tab.getAttribute("busy") == "true" ? "loading" : "complete";
  },

  convert(extension, tab) {
    return TabManager.for(extension).convert(tab);
  },
};

// WeakMap[Extension -> ExtensionTabManager]
let tabManagers = new WeakMap();

// Returns the extension-specific tab manager for the given extension, or
// creates one if it doesn't already exist.
TabManager.for = function(extension) {
  if (!tabManagers.has(extension)) {
    tabManagers.set(extension, new ExtensionTabManager(extension));
  }
  return tabManagers.get(extension);
};

/* eslint-disable mozilla/balanced-listeners */
extensions.on("shutdown", (type, extension) => {
  tabManagers.delete(extension);
});
/* eslint-enable mozilla/balanced-listeners */

// Manages mapping between XUL windows and extension window IDs.
global.WindowManager = {
  _windows: new WeakMap(),
  _nextId: 0,

  // Note: These must match the values in windows.json.
  WINDOW_ID_NONE: -1,
  WINDOW_ID_CURRENT: -2,

  get topWindow() {
    return Services.wm.getMostRecentWindow("navigator:browser");
  },

  windowType(window) {
    // TODO: Make this work.

    let {chromeFlags} = window.QueryInterface(Ci.nsIInterfaceRequestor)
                              .getInterface(Ci.nsIDocShell)
                              .treeOwner.QueryInterface(Ci.nsIInterfaceRequestor)
                              .getInterface(Ci.nsIXULWindow);

    if (chromeFlags & Ci.nsIWebBrowserChrome.CHROME_OPENAS_DIALOG) {
      return "popup";
    }

    return "normal";
  },

  updateGeometry(window, options) {
    if (options.left !== null || options.top !== null) {
      let left = options.left !== null ? options.left : window.screenX;
      let top = options.top !== null ? options.top : window.screenY;
      window.moveTo(left, top);
    }

    if (options.width !== null || options.height !== null) {
      let width = options.width !== null ? options.width : window.outerWidth;
      let height = options.height !== null ? options.height : window.outerHeight;
      window.resizeTo(width, height);
    }
  },

  getId(window) {
    if (this._windows.has(window)) {
      return this._windows.get(window);
    }
    let id = this._nextId++;
    this._windows.set(window, id);
    return id;
  },

  getWindow(id, context) {
    if (id == this.WINDOW_ID_CURRENT) {
      return currentWindow(context);
    }

    for (let window of WindowListManager.browserWindows(true)) {
      if (this.getId(window) == id) {
        return window;
      }
    }
    return null;
  },

  setState(window, state) {
    if (state != "fullscreen" && window.fullScreen) {
      window.fullScreen = false;
    }

    switch (state) {
      case "maximized":
        window.maximize();
        break;

      case "minimized":
      case "docked":
        window.minimize();
        break;

      case "normal":
        // Restore sometimes returns the window to its previous state, rather
        // than to the "normal" state, so it may need to be called anywhere from
        // zero to two times.
        window.restore();
        if (window.windowState != window.STATE_NORMAL) {
          window.restore();
        }
        if (window.windowState != window.STATE_NORMAL) {
          // And on OS-X, where normal vs. maximized is basically a heuristic,
          // we need to cheat.
          window.sizeToContent();
        }
        break;

      case "fullscreen":
        window.fullScreen = true;
        break;

      default:
        throw new Error(`Unexpected window state: ${state}`);
    }
  },

  convert(extension, window, getInfo) {
    const STATES = {
      [window.STATE_MAXIMIZED]: "maximized",
      [window.STATE_MINIMIZED]: "minimized",
      [window.STATE_NORMAL]: "normal",
    };
    let state = STATES[window.windowState];
    if (window.fullScreen) {
      state = "fullscreen";
    }

    let xulWindow = window.QueryInterface(Ci.nsIInterfaceRequestor)
                          .getInterface(Ci.nsIDocShell)
                          .treeOwner.QueryInterface(Ci.nsIInterfaceRequestor)
                          .getInterface(Ci.nsIXULWindow);

    let result = {
      id: this.getId(window),
      focused: window.document.hasFocus(),
      top: window.screenY,
      left: window.screenX,
      width: window.outerWidth,
      height: window.outerHeight,
      incognito: PrivateBrowsingUtils.isWindowPrivate(window),
      type: this.windowType(window),
      state,
      alwaysOnTop: xulWindow.zLevel >= Ci.nsIXULWindow.raisedZ,
    };

    if (getInfo && getInfo.populate) {
      result.tabs = TabManager.for(extension).getTabs(window);
    }

    return result;
  },
};

// Manages listeners for window opening and closing. A window is
// considered open when the "load" event fires on it. A window is
// closed when a "domwindowclosed" notification fires for it.
global.WindowListManager = {
  _openListeners: new Set(),
  _closeListeners: new Set(),

  // Returns an iterator for all browser windows. Unless |includeIncomplete| is
  // true, only fully-loaded windows are returned.
  * browserWindows(includeIncomplete = false) {
    // The window type parameter is only available once the window's document
    // element has been created. This means that, when looking for incomplete
    // browser windows, we need to ignore the type entirely for windows which
    // haven't finished loading, since we would otherwise skip browser windows
    // in their early loading stages.
    // This is particularly important given that the "domwindowcreated" event
    // fires for browser windows when they're in that in-between state, and just
    // before we register our own "domwindowcreated" listener.

    let e = Services.wm.getEnumerator("");
    while (e.hasMoreElements()) {
      let window = e.getNext();

      let ok = includeIncomplete;
      if (window.document.readyState == "complete") {
        ok = window.document.documentElement.getAttribute("windowtype") == "navigator:browser";
      }

      if (ok) {
        yield window;
      }
    }
  },

  addOpenListener(listener) {
    if (this._openListeners.size == 0 && this._closeListeners.size == 0) {
      Services.ww.registerNotification(this);
    }
    this._openListeners.add(listener);

    for (let window of this.browserWindows(true)) {
      if (window.document.readyState != "complete") {
        window.addEventListener("load", this);
      }
    }
  },

  removeOpenListener(listener) {
    this._openListeners.delete(listener);
    if (this._openListeners.size == 0 && this._closeListeners.size == 0) {
      Services.ww.unregisterNotification(this);
    }
  },

  addCloseListener(listener) {
    if (this._openListeners.size == 0 && this._closeListeners.size == 0) {
      Services.ww.registerNotification(this);
    }
    this._closeListeners.add(listener);
  },

  removeCloseListener(listener) {
    this._closeListeners.delete(listener);
    if (this._openListeners.size == 0 && this._closeListeners.size == 0) {
      Services.ww.unregisterNotification(this);
    }
  },

  handleEvent(event) {
    event.currentTarget.removeEventListener(event.type, this);
    let window = event.target.defaultView;
    if (window.document.documentElement.getAttribute("windowtype") != "navigator:browser") {
      return;
    }

    for (let listener of this._openListeners) {
      listener(window);
    }
  },

  observe(window, topic, data) {
    if (topic == "domwindowclosed") {
      if (window.document.documentElement.getAttribute("windowtype") != "navigator:browser") {
        return;
      }

      window.removeEventListener("load", this);
      for (let listener of this._closeListeners) {
        listener(window);
      }
    } else {
      window.addEventListener("load", this);
    }
  },
};

// Provides a facility to listen for DOM events across all XUL windows.
global.AllWindowEvents = {
  _listeners: new Map(),

  // If |type| is a normal event type, invoke |listener| each time
  // that event fires in any open window. If |type| is "progress", add
  // a web progress listener that covers all open windows.
  addListener(type, listener) {
    if (type == "domwindowopened") {
      return WindowListManager.addOpenListener(listener);
    } else if (type == "domwindowclosed") {
      return WindowListManager.addCloseListener(listener);
    }

    if (this._listeners.size == 0) {
      WindowListManager.addOpenListener(this.openListener);
    }

    if (!this._listeners.has(type)) {
      this._listeners.set(type, new Set());
    }
    let list = this._listeners.get(type);
    list.add(listener);

    // Register listener on all existing windows.
    for (let window of WindowListManager.browserWindows()) {
      this.addWindowListener(window, type, listener);
    }
  },

  removeListener(eventType, listener) {
    if (eventType == "domwindowopened") {
      return WindowListManager.removeOpenListener(listener);
    } else if (eventType == "domwindowclosed") {
      return WindowListManager.removeCloseListener(listener);
    }

    let listeners = this._listeners.get(eventType);
    listeners.delete(listener);
    if (listeners.size == 0) {
      this._listeners.delete(eventType);
      if (this._listeners.size == 0) {
        WindowListManager.removeOpenListener(this.openListener);
      }
    }

    // Unregister listener from all existing windows.
    let useCapture = eventType === "focus" || eventType === "blur";
    for (let window of WindowListManager.browserWindows()) {
      if (eventType == "progress") {
        window.gBrowser.removeTabsProgressListener(listener);
      } else {
        window.removeEventListener(eventType, listener, useCapture);
      }
    }
  },

  /* eslint-disable mozilla/balanced-listeners */
  addWindowListener(window, eventType, listener) {
    let useCapture = eventType === "focus" || eventType === "blur";

    if (eventType == "progress") {
      window.gBrowser.addTabsProgressListener(listener);
    } else {
      window.addEventListener(eventType, listener, useCapture);
    }
  },
  /* eslint-enable mozilla/balanced-listeners */

  // Runs whenever the "load" event fires for a new window.
  openListener(window) {
    for (let [eventType, listeners] of AllWindowEvents._listeners) {
      for (let listener of listeners) {
        this.addWindowListener(window, eventType, listener);
      }
    }
  },
};

AllWindowEvents.openListener = AllWindowEvents.openListener.bind(AllWindowEvents);

// Subclass of EventManager where we just need to call
// add/removeEventListener on each XUL window.
global.WindowEventManager = function(context, name, event, listener) {
  EventManager.call(this, context, name, fire => {
    let listener2 = (...args) => listener(fire, ...args);
    AllWindowEvents.addListener(event, listener2);
    return () => {
      AllWindowEvents.removeListener(event, listener2);
    };
  });
};

WindowEventManager.prototype = Object.create(EventManager.prototype);
