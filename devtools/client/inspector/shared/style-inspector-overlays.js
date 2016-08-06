/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

// The style-inspector overlays are:
// - tooltips that appear when hovering over property values
// - editor tooltips that appear when clicking color swatches, etc.
// - in-content highlighters that appear when hovering over property values
// - etc.

const {getColor} = require("devtools/client/shared/theme");
const {HTMLTooltip} = require("devtools/client/shared/widgets/HTMLTooltip");
const {
  getImageDimensions,
  setImageTooltip,
  setBrokenImageTooltip,
} = require("devtools/client/shared/widgets/tooltip/ImageTooltipHelper");
const {
  CssDocsTooltip,
} = require("devtools/client/shared/widgets/tooltip/CssDocsTooltip");
const {
  SwatchColorPickerTooltip,
  SwatchCubicBezierTooltip,
  SwatchFilterTooltip
} = require("devtools/client/shared/widgets/Tooltip");
const EventEmitter = require("devtools/shared/event-emitter");
const promise = require("promise");
const {Task} = require("devtools/shared/task");
const Services = require("Services");

const PREF_IMAGE_TOOLTIP_SIZE = "devtools.inspector.imagePreviewTooltipSize";

// Types of existing tooltips
const TOOLTIP_IMAGE_TYPE = "image";
const TOOLTIP_FONTFAMILY_TYPE = "font-family";

// Types of nodes in the rule/computed-view
const VIEW_NODE_SELECTOR_TYPE = 1;
exports.VIEW_NODE_SELECTOR_TYPE = VIEW_NODE_SELECTOR_TYPE;
const VIEW_NODE_PROPERTY_TYPE = 2;
exports.VIEW_NODE_PROPERTY_TYPE = VIEW_NODE_PROPERTY_TYPE;
const VIEW_NODE_VALUE_TYPE = 3;
exports.VIEW_NODE_VALUE_TYPE = VIEW_NODE_VALUE_TYPE;
const VIEW_NODE_IMAGE_URL_TYPE = 4;
exports.VIEW_NODE_IMAGE_URL_TYPE = VIEW_NODE_IMAGE_URL_TYPE;
const VIEW_NODE_LOCATION_TYPE = 5;
exports.VIEW_NODE_LOCATION_TYPE = VIEW_NODE_LOCATION_TYPE;

/**
 * Manages all highlighters in the style-inspector.
 *
 * @param {CssRuleView|CssComputedView} view
 *        Either the rule-view or computed-view panel
 */
function HighlightersOverlay(view) {
  this.view = view;

  let {CssRuleView} = require("devtools/client/inspector/rules/rules");
  this.isRuleView = view instanceof CssRuleView;

  this.highlighterUtils = this.view.inspector.toolbox.highlighterUtils;

  this._onMouseMove = this._onMouseMove.bind(this);
  this._onMouseLeave = this._onMouseLeave.bind(this);

  this.highlighters = {};

  // Only initialize the overlay if at least one of the highlighter types is
  // supported
  this.supportsHighlighters =
    this.highlighterUtils.supportsCustomHighlighters();

  EventEmitter.decorate(this);
}

exports.HighlightersOverlay = HighlightersOverlay;

HighlightersOverlay.prototype = {
  /**
   * Add the highlighters overlay to the view. This will start tracking mouse
   * movements and display highlighters when needed
   */
  addToView: function () {
    if (!this.supportsHighlighters || this._isStarted || this._isDestroyed) {
      return;
    }

    let el = this.view.element;
    el.addEventListener("mousemove", this._onMouseMove, false);
    el.addEventListener("mouseleave", this._onMouseLeave, false);

    this._isStarted = true;
  },

  /**
   * Remove the overlay from the current view. This will stop tracking mouse
   * movement and showing highlighters
   */
  removeFromView: function () {
    if (!this.supportsHighlighters || !this._isStarted || this._isDestroyed) {
      return;
    }

    this._hideCurrent();

    let el = this.view.element;
    el.removeEventListener("mousemove", this._onMouseMove, false);
    el.removeEventListener("mouseleave", this._onMouseLeave, false);

    this._isStarted = false;
  },

  _onMouseMove: function (event) {
    // Bail out if the target is the same as for the last mousemove
    if (event.target === this._lastHovered) {
      return;
    }

    // Only one highlighter can be displayed at a time, hide the currently shown
    this._hideCurrent();

    this._lastHovered = event.target;

    let nodeInfo = this.view.getNodeInfo(event.target);
    if (!nodeInfo) {
      return;
    }

    // Choose the type of highlighter required for the hovered node
    let type;
    if (this._isRuleViewTransform(nodeInfo) ||
        this._isComputedViewTransform(nodeInfo)) {
      type = "CssTransformHighlighter";
    }

    if (type) {
      this.highlighterShown = type;
      let node = this.view.inspector.selection.nodeFront;
      this._getHighlighter(type)
          .then(highlighter => highlighter.show(node))
          .then(shown => {
            if (shown) {
              this.emit("highlighter-shown");
            }
          });
    }
  },

  _onMouseLeave: function () {
    this._lastHovered = null;
    this._hideCurrent();
  },

  /**
   * Is the current hovered node a css transform property value in the rule-view
   *
   * @param {Object} nodeInfo
   * @return {Boolean}
   */
  _isRuleViewTransform: function (nodeInfo) {
    let isTransform = nodeInfo.type === VIEW_NODE_VALUE_TYPE &&
                      nodeInfo.value.property === "transform";
    let isEnabled = nodeInfo.value.enabled &&
                    !nodeInfo.value.overridden &&
                    !nodeInfo.value.pseudoElement;
    return this.isRuleView && isTransform && isEnabled;
  },

  /**
   * Is the current hovered node a css transform property value in the
   * computed-view
   *
   * @param {Object} nodeInfo
   * @return {Boolean}
   */
  _isComputedViewTransform: function (nodeInfo) {
    let isTransform = nodeInfo.type === VIEW_NODE_VALUE_TYPE &&
                      nodeInfo.value.property === "transform";
    return !this.isRuleView && isTransform;
  },

  /**
   * Hide the currently shown highlighter
   */
  _hideCurrent: function () {
    if (!this.highlighterShown || !this.highlighters[this.highlighterShown]) {
      return;
    }

    // For some reason, the call to highlighter.hide doesn't always return a
    // promise. This causes some tests to fail when trying to install a
    // rejection handler on the result of the call. To avoid this, check
    // whether the result is truthy before installing the handler.
    let onHidden = this.highlighters[this.highlighterShown].hide();
    if (onHidden) {
      onHidden.then(null, e => console.error(e));
    }

    this.highlighterShown = null;
    this.emit("highlighter-hidden");
  },

  /**
   * Get a highlighter front given a type. It will only be initialized once
   * @param {String} type The highlighter type. One of this.highlighters
   * @return a promise that resolves to the highlighter
   */
  _getHighlighter: function (type) {
    let utils = this.highlighterUtils;

    if (this.highlighters[type]) {
      return promise.resolve(this.highlighters[type]);
    }

    return utils.getHighlighterByType(type).then(highlighter => {
      this.highlighters[type] = highlighter;
      return highlighter;
    });
  },

  /**
   * Destroy this overlay instance, removing it from the view and destroying
   * all initialized highlighters
   */
  destroy: function () {
    this.removeFromView();

    for (let type in this.highlighters) {
      if (this.highlighters[type]) {
        this.highlighters[type].finalize();
        this.highlighters[type] = null;
      }
    }

    this.view = null;
    this.highlighterUtils = null;

    this._isDestroyed = true;
  }
};

/**
 * Manages all tooltips in the style-inspector.
 *
 * @param {CssRuleView|CssComputedView} view
 *        Either the rule-view or computed-view panel
 */
function TooltipsOverlay(view) {
  this.view = view;

  let {CssRuleView} = require("devtools/client/inspector/rules/rules");
  this.isRuleView = view instanceof CssRuleView;

  this._onNewSelection = this._onNewSelection.bind(this);
  this.view.inspector.selection.on("new-node-front", this._onNewSelection);
}

exports.TooltipsOverlay = TooltipsOverlay;

TooltipsOverlay.prototype = {
  get isEditing() {
    return this.colorPicker.tooltip.isVisible() ||
           this.colorPicker.eyedropperOpen ||
           this.cubicBezier.tooltip.isVisible() ||
           this.filterEditor.tooltip.isVisible();
  },

  /**
   * Add the tooltips overlay to the view. This will start tracking mouse
   * movements and display tooltips when needed
   */
  addToView: function () {
    if (this._isStarted || this._isDestroyed) {
      return;
    }

    let { toolbox } = this.view.inspector;

    // Image, fonts, ... preview tooltip
    this.previewTooltip = new HTMLTooltip(toolbox, {
      type: "arrow",
      useXulWrapper: true
    });
    this.previewTooltip.startTogglingOnHover(this.view.element,
      this._onPreviewTooltipTargetHover.bind(this));

    // MDN CSS help tooltip
    this.cssDocs = new CssDocsTooltip(toolbox);

    if (this.isRuleView) {
      // Color picker tooltip
      this.colorPicker = new SwatchColorPickerTooltip(toolbox, this.view.inspector);
      // Cubic bezier tooltip
      this.cubicBezier = new SwatchCubicBezierTooltip(toolbox);
      // Filter editor tooltip
      this.filterEditor = new SwatchFilterTooltip(toolbox);
    }

    this._isStarted = true;
  },

  /**
   * Remove the tooltips overlay from the view. This will stop tracking mouse
   * movements and displaying tooltips
   */
  removeFromView: function () {
    if (!this._isStarted || this._isDestroyed) {
      return;
    }

    this.previewTooltip.stopTogglingOnHover(this.view.element);
    this.previewTooltip.destroy();

    if (this.colorPicker) {
      this.colorPicker.destroy();
    }

    if (this.cubicBezier) {
      this.cubicBezier.destroy();
    }

    if (this.cssDocs) {
      this.cssDocs.destroy();
    }

    if (this.filterEditor) {
      this.filterEditor.destroy();
    }

    this._isStarted = false;
  },

  /**
   * Given a hovered node info, find out which type of tooltip should be shown,
   * if any
   *
   * @param {Object} nodeInfo
   * @return {String} The tooltip type to be shown, or null
   */
  _getTooltipType: function ({type, value: prop}) {
    let tooltipType = null;
    let inspector = this.view.inspector;

    // Image preview tooltip
    if (type === VIEW_NODE_IMAGE_URL_TYPE &&
        inspector.hasUrlToImageDataResolver) {
      tooltipType = TOOLTIP_IMAGE_TYPE;
    }

    // Font preview tooltip
    if (type === VIEW_NODE_VALUE_TYPE && prop.property === "font-family") {
      let value = prop.value.toLowerCase();
      if (value !== "inherit" && value !== "unset" && value !== "initial") {
        tooltipType = TOOLTIP_FONTFAMILY_TYPE;
      }
    }

    return tooltipType;
  },

  /**
   * Executed by the tooltip when the pointer hovers over an element of the
   * view. Used to decide whether the tooltip should be shown or not and to
   * actually put content in it.
   * Checks if the hovered target is a css value we support tooltips for.
   *
   * @param {DOMNode} target The currently hovered node
   * @return {Promise}
   */
  _onPreviewTooltipTargetHover: Task.async(function* (target) {
    let nodeInfo = this.view.getNodeInfo(target);
    if (!nodeInfo) {
      // The hovered node isn't something we care about
      return false;
    }

    let type = this._getTooltipType(nodeInfo);
    if (!type) {
      // There is no tooltip type defined for the hovered node
      return false;
    }

    if (this.isRuleView && this.colorPicker.tooltip.isVisible()) {
      this.colorPicker.revert();
      this.colorPicker.hide();
    }

    if (this.isRuleView && this.cubicBezier.tooltip.isVisible()) {
      this.cubicBezier.revert();
      this.cubicBezier.hide();
    }

    if (this.isRuleView && this.cssDocs.tooltip.isVisible()) {
      this.cssDocs.hide();
    }

    if (this.isRuleView && this.filterEditor.tooltip.isVisible()) {
      this.filterEditor.revert();
      this.filterEdtior.hide();
    }

    let inspector = this.view.inspector;

    if (type === TOOLTIP_IMAGE_TYPE) {
      try {
        yield this._setImagePreviewTooltip(nodeInfo.value.url);
      } catch (e) {
        yield setBrokenImageTooltip(this.previewTooltip, this.view.inspector.panelDoc);
      }
      return true;
    }

    if (type === TOOLTIP_FONTFAMILY_TYPE) {
      let font = nodeInfo.value.value;
      let nodeFront = inspector.selection.nodeFront;
      yield this._setFontPreviewTooltip(font, nodeFront);
      return true;
    }

    return false;
  }),

  /**
   * Set the content of the preview tooltip to display an image preview. The image URL can
   * be relative, a call will be made to the debuggee to retrieve the image content as an
   * imageData URI.
   *
   * @param {String} imageUrl
   *        The image url value (may be relative or absolute).
   * @return {Promise} A promise that resolves when the preview tooltip content is ready
   */
  _setImagePreviewTooltip: Task.async(function* (imageUrl) {
    let doc = this.view.inspector.panelDoc;
    let maxDim = Services.prefs.getIntPref(PREF_IMAGE_TOOLTIP_SIZE);

    let naturalWidth, naturalHeight;
    if (imageUrl.startsWith("data:")) {
      // If the imageUrl already is a data-url, save ourselves a round-trip
      let size = yield getImageDimensions(doc, imageUrl);
      naturalWidth = size.naturalWidth;
      naturalHeight = size.naturalHeight;
    } else {
      let inspectorFront = this.view.inspector.inspector;
      let {data, size} = yield inspectorFront.getImageDataFromURL(imageUrl, maxDim);
      imageUrl = yield data.string();
      naturalWidth = size.naturalWidth;
      naturalHeight = size.naturalHeight;
    }

    yield setImageTooltip(this.previewTooltip, doc, imageUrl,
      {maxDim, naturalWidth, naturalHeight});
  }),

  /**
   * Set the content of the preview tooltip to display a font family preview.
   *
   * @param {String} font
   *        The font family value.
   * @param {object} nodeFront
   *        The NodeActor that will used to retrieve the dataURL for the font
   *        family tooltip contents.
   * @return {Promise} A promise that resolves when the preview tooltip content is ready
   */
  _setFontPreviewTooltip: Task.async(function* (font, nodeFront) {
    if (!font || !nodeFront || typeof nodeFront.getFontFamilyDataURL !== "function") {
      throw new Error("Unable to create font preview tooltip content.");
    }

    font = font.replace(/"/g, "'");
    font = font.replace("!important", "");
    font = font.trim();

    let fillStyle = getColor("body-color");
    let {data, size: maxDim} = yield nodeFront.getFontFamilyDataURL(font, fillStyle);

    let imageUrl = yield data.string();
    let doc = this.view.inspector.panelDoc;
    let {naturalWidth, naturalHeight} = yield getImageDimensions(doc, imageUrl);

    yield setImageTooltip(this.previewTooltip, doc, imageUrl,
      {hideDimensionLabel: true, maxDim, naturalWidth, naturalHeight});
  }),

  _onNewSelection: function () {
    if (this.previewTooltip) {
      this.previewTooltip.hide();
    }

    if (this.colorPicker) {
      this.colorPicker.hide();
    }

    if (this.cubicBezier) {
      this.cubicBezier.hide();
    }

    if (this.cssDocs) {
      this.cssDocs.hide();
    }

    if (this.filterEditor) {
      this.filterEditor.hide();
    }
  },

  /**
   * Destroy this overlay instance, removing it from the view
   */
  destroy: function () {
    this.removeFromView();

    this.view.inspector.selection.off("new-node-front", this._onNewSelection);
    this.view = null;

    this._isDestroyed = true;
  }
};
