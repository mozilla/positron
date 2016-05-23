/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// URL constructor doesn't support about: scheme
let href = window.location.href.replace("about:", "http://");
let url = new window.URL(href);

// Only use this method to attach the toolbox if some query parameters are given
if (url.search.length > 1) {
  const Cu = Components.utils;
  const Ci = Components.interfaces;
  const { gDevTools } = Cu.import("resource://devtools/client/framework/gDevTools.jsm", {});
  const { require } = Cu.import("resource://devtools/shared/Loader.jsm", {});
  const { targetFromURL } = require("devtools/client/framework/target-from-url");
  const { Toolbox } = require("devtools/client/framework/toolbox");
  const { TargetFactory } = require("devtools/client/framework/target");
  const { DebuggerServer } = require("devtools/server/main");
  const { DebuggerClient } = require("devtools/shared/client/main");
  const { Task } = require("devtools/shared/task");

  // `host` is the frame element loading the toolbox.
  let host = window.QueryInterface(Ci.nsIInterfaceRequestor)
                   .getInterface(Ci.nsIDOMWindowUtils)
                   .containerElement;

  // If there's no containerElement, use the current window.
  if (!host) {
    host = {
      contentWindow: window,
      contentDocument: document,
      // toolbox.js wants to set attributes on the frame that contains it, but
      // that is fine to skip and doesn't make sense when using the current
      // window.
      setAttribute() {},
      ownerDocument: document,
    };
  }

  // Specify the default tool to open
  let tool = url.searchParams.get("tool");

  let toolboxOpened;
  if (url.searchParams.has("target")) {
    // Attach toolbox to a given browser iframe (<xul:browser> or <html:iframe
    // mozbrowser>) whose reference is set on the host iframe.

    // `iframe` is the targeted document to debug
    let iframe = host.wrappedJSObject ? host.wrappedJSObject.target
                                      : host.target;
    if (iframe) {
      // Need to use a xray and query some interfaces to have
      // attributes and behavior expected by devtools codebase
      iframe = XPCNativeWrapper(iframe);
      iframe.QueryInterface(Ci.nsIFrameLoaderOwner);

      // Fake a xul:tab object as we don't have one.
      // linkedBrowser is the only one attribute being queried by client.getTab
      let tab = { linkedBrowser: iframe };

      if (!DebuggerServer.initialized) {
        DebuggerServer.init();
        DebuggerServer.addBrowserActors();
      }
      let client = new DebuggerClient(DebuggerServer.connectPipe());
      toolboxOpened = Task.spawn(function*() {
        yield client.connect();
        // Creates a target for a given browser iframe.
        let response = yield client.getTab({ tab });
        let form = response.tab;
        let target = yield TargetFactory.forRemoteTab({
          client,
          form,
          chrome: false
        });
        let options = { customIframe: host };
        return gDevTools.showToolbox(target, tool, Toolbox.HostType.CUSTOM,
                                     options);
      });
    } else {
      toolboxOpened = Promise.reject(new Error("toolbox target is undefined"));
    }
  } else {
    toolboxOpened = Task.spawn(function*() {
      let target = yield targetFromURL(url);
      let options = { customIframe: host };
      return gDevTools.showToolbox(target, tool, Toolbox.HostType.CUSTOM,
                                   options);
    });
  }

  // Ensure the toolbox gets destroyed if the host unloads
  toolboxOpened.then(toolbox => {
    let hostUnload = () => {
      host.contentWindow.removeEventListener("unload", hostUnload);
      toolbox.destroy();
    };
    host.contentWindow.addEventListener("unload", hostUnload);
  });

  toolboxOpened.catch(e => {
    window.alert("Unable to start the toolbox: " + e.message);
  });
}
