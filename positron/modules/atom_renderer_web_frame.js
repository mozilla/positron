/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

'use strict';

const { classes: Cc, interfaces: Ci, results: Cr, utils: Cu } = Components;

const ipcRenderer = require('electron').ipcRenderer;
const positronUtil = process.binding('positron_util');
const v8Util = process.atomBinding('v8_util');
const cpmm = Cc['@mozilla.org/childprocessmessagemanager;1'].getService(Ci.nsISyncMessageSender);

exports.webFrame = {
  attachGuest: function(elementInstanceId, webView) {
    cpmm.sendSyncMessage('positron-register-web-view', null, { window, webView });
  },

  registerElementResizeCallback: positronUtil.makeStub('webFrame.registerElementResizeCallback'),

  registerEmbedderCustomElement: function(name, options) {
    return document.registerElement(name, options);
  },

};
