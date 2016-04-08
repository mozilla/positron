/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

let wrapWebContents = null;

exports._setWrapWebContents = function(aWrapWebContents) {
  wrapWebContents = aWrapWebContents;
};

// We can't attach properties to a WebContents instance by setting
// WebContents.prototype, because wrapWebContents sets the instance's __proto__
// property to EventEmitter.prototype.
//
// So instead we attach "prototype" properties directly to the instance itself
// inside the WebContents constructor, using this WebContents_prototype object
// to collect the set of properties that should be assigned to the instance.
//
let WebContents_prototype = {
  getURL: function() {
    if (this._browserWindow._domWindow) {
      return this._browserWindow._domWindow.location;
    }
    return null;
  },

  loadURL: function(url) {
    this._browserWindow._domWindow.location = url;
  },
};

function WebContents(options) {
  // XXX Consider using WeakMap to hide private properties from consumers.
  this._browserWindow = options.browserWindow;

  // XXX Iterate WebContents_prototype and automagically assign each property
  // to the instance (after which we'll need to assign _getURL specially, since
  // it's an alias for getURL).
  this.getURL = this._getURL = WebContents_prototype.getURL;
  this.loadURL = WebContents_prototype.loadURL;
}

exports.create = function(options) {
  // Sadly, wrapWebContents returns one of the functions it attaches
  // to the WebContents instance rather than the instance itself, so we can't
  // simply return the result of wrapping a new instance.
  // XXX Request a pull to fix this upstream, as this seems unintentional.
  // return wrapWebContents(new WebContents(options));
  let webContents = new WebContents(options);
  wrapWebContents(webContents);
  return webContents;
};
