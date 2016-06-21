/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
"use strict";

const { FrontClassWithSpec, Front } = require("devtools/shared/protocol");
const { cssPropertiesSpec } = require("devtools/shared/specs/css-properties");
const { Task } = require("devtools/shared/task");
const { CSS_PROPERTIES } = require("devtools/shared/css-properties-db");

/**
 * Build up a regular expression that matches a CSS variable token. This is an
 * ident token that starts with two dashes "--".
 *
 * https://www.w3.org/TR/css-syntax-3/#ident-token-diagram
 */
var NON_ASCII = "[^\\x00-\\x7F]";
var ESCAPE = "\\\\[^\n\r]";
var FIRST_CHAR = ["[_a-z]", NON_ASCII, ESCAPE].join("|");
var TRAILING_CHAR = ["[_a-z0-9-]", NON_ASCII, ESCAPE].join("|");
var IS_VARIABLE_TOKEN = new RegExp(`^--(${FIRST_CHAR})(${TRAILING_CHAR})*$`,
                                   "i");
/**
 * Check that this is a CSS variable.
 *
 * @param {String} input
 * @return {Boolean}
 */
function isCssVariable(input) {
  return !!input.match(IS_VARIABLE_TOKEN);
}

var cachedCssProperties = new WeakMap();

/**
 * The CssProperties front provides a mechanism to have a one-time asynchronous
 * load of a CSS properties database. This is then fed into the CssProperties
 * interface that provides synchronous methods for finding out what CSS
 * properties the current server supports.
 */
const CssPropertiesFront = FrontClassWithSpec(cssPropertiesSpec, {
  initialize: function (client, { cssPropertiesActor }) {
    Front.prototype.initialize.call(this, client, {actor: cssPropertiesActor});
    this.manage(this);
  }
});

exports.CssPropertiesFront = CssPropertiesFront;

/**
 * Ask questions to a CSS database. This class does not care how the database
 * gets loaded in, only the questions that you can ask to it.
 * Prototype functions are bound to 'this' so they can be passed around as helper
 * functions.
 *
 * @param {Array}  propertiesList
 *                 A list of known properties.
 * @param {Object} inheritedList
 *                 The key is the property name, the value is whether or not
 *                 that property is inherited.
 */
function CssProperties(properties) {
  this.properties = properties;

  this.isKnown = this.isKnown.bind(this);
  this.isInherited = this.isInherited.bind(this);
  this.supportsType = this.supportsType.bind(this);
}

CssProperties.prototype = {
  /**
   * Checks to see if the property is known by the browser. This function has
   * `this` already bound so that it can be passed around by reference.
   *
   * @param {String} property The property name to be checked.
   * @return {Boolean}
   */
  isKnown(property) {
    return !!this.properties[property] || isCssVariable(property);
  },

  /**
   * Checks to see if the property is an inherited one.
   *
   * @param {String} property The property name to be checked.
   * @return {Boolean}
   */
  isInherited(property) {
    return this.properties[property] && this.properties[property].isInherited;
  },

  /**
   * Checks if the property supports the given CSS type.
   * CSS types should come from devtools/shared/css-properties-db.js' CSS_TYPES.
   *
   * @param {String} property The property to be checked.
   * @param {Number} type One of the type values from CSS_TYPES.
   * @return {Boolean}
   */
  supportsType(property, type) {
    return this.properties[property] && this.properties[property].supports.includes(type);
  }
};

exports.CssProperties = CssProperties;

/**
 * Create a CssProperties object with a fully loaded CSS database. The
 * CssProperties interface can be queried synchronously, but the initialization
 * is potentially async and should be handled up-front when the tool is created.
 *
 * The front is returned only with this function so that it can be destroyed
 * once the toolbox is destroyed.
 *
 * @param {Toolbox} The current toolbox.
 * @returns {Promise} Resolves to {cssProperties, cssPropertiesFront}.
 */
exports.initCssProperties = Task.async(function* (toolbox) {
  let client = toolbox.target.client;
  if (cachedCssProperties.has(client)) {
    return cachedCssProperties.get(client);
  }

  let db, front;

  // Get the list dynamically if the cssProperties actor exists.
  if (toolbox.target.hasActor("cssProperties")) {
    front = CssPropertiesFront(client, toolbox.target.form);
    db = yield front.getCSSDatabase();

    // Even if the target has the cssProperties actor, it may not be the latest version.
    // So, the "supports" data may be missing.
    // Start with the server's list (because that's the correct one), and add the supports
    // information if required.
    if (!db.color.supports) {
      for (let name in db) {
        if (typeof CSS_PROPERTIES[name] === "object") {
          db[name].supports = CSS_PROPERTIES[name].supports;
        }
      }
    }
  } else {
    // The target does not support this actor, so require a static list of supported
    // properties.
    db = CSS_PROPERTIES;
  }

  const cssProperties = new CssProperties(db);
  cachedCssProperties.set(client, {cssProperties, front});
  return {cssProperties, front};
});

/**
 * Synchronously get a cached and initialized CssProperties.
 *
 * @param {Toolbox} The current toolbox.
 * @returns {CssProperties}
 */
exports.getCssProperties = function (toolbox) {
  if (!cachedCssProperties.has(toolbox.target.client)) {
    throw new Error("The CSS database has not been initialized, please make " +
                    "sure initCssDatabase was called once before for this " +
                    "toolbox.");
  }
  return cachedCssProperties.get(toolbox.target.client).cssProperties;
};

exports.CssPropertiesFront = CssPropertiesFront;
