/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

var gEMEHandler = {
  get uiEnabled() {
    let emeUIEnabled = Services.prefs.getBoolPref("browser.eme.ui.enabled");
    // Force-disable on WinXP:
    if (navigator.platform.toLowerCase().startsWith("win")) {
      emeUIEnabled = emeUIEnabled && parseFloat(Services.sysinfo.get("version")) >= 6;
    }
    return emeUIEnabled;
  },
  ensureEMEEnabled: function(browser, keySystem) {
    Services.prefs.setBoolPref("media.eme.enabled", true);
    if (keySystem) {
      if (keySystem.startsWith("com.adobe") &&
          Services.prefs.getPrefType("media.gmp-eme-adobe.enabled") &&
          !Services.prefs.getBoolPref("media.gmp-eme-adobe.enabled")) {
        Services.prefs.setBoolPref("media.gmp-eme-adobe.enabled", true);
      } else if (keySystem == "org.w3.clearkey" &&
                 Services.prefs.getPrefType("media.eme.clearkey.enabled") &&
                 !Services.prefs.getBoolPref("media.eme.clearkey.enabled")) {
        Services.prefs.setBoolPref("media.eme.clearkey.enabled", true);
      } else if (keySystem == "com.widevine.alpha" &&
                 Services.prefs.getPrefType("media.gmp-widevinecdm.enabled") &&
                 !Services.prefs.getBoolPref("media.gmp-widevinecdm.enabled")) {
        Services.prefs.setBoolPref("media.gmp-widevinecdm.enabled", true);
      }
    }
    browser.reload();
  },
  isKeySystemVisible: function(keySystem) {
    if (!keySystem) {
      return false;
    }
    if (keySystem.startsWith("com.adobe") &&
        Services.prefs.getPrefType("media.gmp-eme-adobe.visible")) {
      return Services.prefs.getBoolPref("media.gmp-eme-adobe.visible");
    }
    if (keySystem == "com.widevine.alpha" &&
        Services.prefs.getPrefType("media.gmp-widevinecdm.visible")) {
      return Services.prefs.getBoolPref("media.gmp-widevinecdm.visible");
    }
    return true;
  },
  getLearnMoreLink: function(msgId) {
    let text = gNavigatorBundle.getString("emeNotifications." + msgId + ".learnMoreLabel");
    let baseURL = Services.urlFormatter.formatURLPref("app.support.baseURL");
    return "<label class='text-link' href='" + baseURL + "drm-content'>" +
           text + "</label>";
  },
  receiveMessage: function({target: browser, data: data}) {
    let parsedData;
    try {
      parsedData = JSON.parse(data);
    } catch (ex) {
      Cu.reportError("Malformed EME video message with data: " + data);
      return;
    }
    let {status: status, keySystem: keySystem} = parsedData;
    // Don't need to show if disabled or keysystem not visible.
    if (!this.uiEnabled || !this.isKeySystemVisible(keySystem)) {
      return;
    }

    let notificationId;
    let buttonCallback;
    let params = [];
    switch (status) {
      case "available":
      case "cdm-created":
        this.showPopupNotificationForSuccess(browser, keySystem);
        // ... and bail!
        return;

      case "api-disabled":
      case "cdm-disabled":
        notificationId = "drmContentDisabled";
        buttonCallback = gEMEHandler.ensureEMEEnabled.bind(gEMEHandler, browser, keySystem)
        params = [this.getLearnMoreLink(notificationId)];
        break;

      case "cdm-insufficient-version":
        notificationId = "drmContentCDMInsufficientVersion";
        params = [this._brandShortName];
        break;

      case "cdm-not-installed":
        notificationId = "drmContentCDMInstalling";
        params = [this._brandShortName];
        break;

      case "cdm-not-supported":
        // Not to pop up user-level notification because they cannot do anything
        // about it.
      case "error":
        // Fall through and do the same for unknown messages:
      default:
        let typeOfIssue = status == "error" ? "error" : "message ('" + status + "')";
        Cu.reportError("Unknown " + typeOfIssue + " dealing with EME key request: " + data);
        return;
    }

    this.showNotificationBar(browser, notificationId, keySystem, params, buttonCallback);
  },
  showNotificationBar: function(browser, notificationId, keySystem, labelParams, callback) {
    let box = gBrowser.getNotificationBox(browser);
    if (box.getNotificationWithValue(notificationId)) {
      return;
    }

    let msgPrefix = "emeNotifications." + notificationId + ".";
    let msgId = msgPrefix + "message";

    let message = labelParams.length ?
                  gNavigatorBundle.getFormattedString(msgId, labelParams) :
                  gNavigatorBundle.getString(msgId);

    let buttons = [];
    if (callback) {
      let btnLabelId = msgPrefix + "button.label";
      let btnAccessKeyId = msgPrefix + "button.accesskey";
      buttons.push({
        label: gNavigatorBundle.getString(btnLabelId),
        accessKey: gNavigatorBundle.getString(btnAccessKeyId),
        callback: callback
      });
    }

    let iconURL = "chrome://browser/skin/drm-icon.svg#chains-black";

    // Do a little dance to get rich content into the notification:
    let fragment = document.createDocumentFragment();
    let descriptionContainer = document.createElement("description");
    descriptionContainer.innerHTML = message;
    while (descriptionContainer.childNodes.length) {
      fragment.appendChild(descriptionContainer.childNodes[0]);
    }

    box.appendNotification(fragment, notificationId, iconURL, box.PRIORITY_WARNING_MEDIUM,
                           buttons);
  },
  showPopupNotificationForSuccess: function(browser, keySystem) {
    // We're playing EME content! Remove any "we can't play because..." messages.
    var box = gBrowser.getNotificationBox(browser);
    ["drmContentDisabled",
     "drmContentCDMInsufficientVersion",
     "drmContentCDMInstalling"
     ].forEach(function (value) {
        var notification = box.getNotificationWithValue(value);
        if (notification)
          box.removeNotification(notification);
      });

    // Don't bother creating it if it's already there:
    if (PopupNotifications.getNotification("drmContentPlaying", browser)) {
      return;
    }

    let msgPrefix = "emeNotifications.drmContentPlaying.";
    let msgId = msgPrefix + "message2";
    let btnLabelId = msgPrefix + "button.label";
    let btnAccessKeyId = msgPrefix + "button.accesskey";

    let message = gNavigatorBundle.getFormattedString(msgId, [this._brandShortName]);
    let anchorId = "eme-notification-icon";
    let firstPlayPref = "browser.eme.ui.firstContentShown";
    if (!Services.prefs.getPrefType(firstPlayPref) ||
        !Services.prefs.getBoolPref(firstPlayPref)) {
      document.getElementById(anchorId).setAttribute("firstplay", "true");
      Services.prefs.setBoolPref(firstPlayPref, true);
    } else {
      document.getElementById(anchorId).removeAttribute("firstplay");
    }

    let mainAction = {
      label: gNavigatorBundle.getString(btnLabelId),
      accessKey: gNavigatorBundle.getString(btnAccessKeyId),
      callback: function() { openPreferences("paneContent"); },
      dismiss: true
    };
    let options = {
      dismissed: true,
      eventCallback: aTopic => aTopic == "swapping",
      learnMoreURL: Services.urlFormatter.formatURLPref("app.support.baseURL") + "drm-content",
    };
    PopupNotifications.show(browser, "drmContentPlaying", message, anchorId, mainAction, null, options);
  },
  QueryInterface: XPCOMUtils.generateQI([Ci.nsIMessageListener])
};

XPCOMUtils.defineLazyGetter(gEMEHandler, "_brandShortName", function() {
  return document.getElementById("bundle_brand").getString("brandShortName");
});

let gDecoderDoctorHandler = {
  shouldShowLearnMoreButton() {
    return AppConstants.platform == "win";
  },

  getLabelForNotificationBox(type) {
    if (type == "adobe-cdm-not-found" &&
        AppConstants.platform == "win") {
      if (AppConstants.isPlatformAndVersionAtMost("win", "5.9")) {
        // We supply our own Learn More button so we don't need to populate the message here.
        return gNavigatorBundle.getFormattedString("emeNotifications.drmContentDisabled.message", [""]);
      }
      return gNavigatorBundle.getString("decoder.noCodecs.message");
    }
    if (type == "adobe-cdm-not-activated" &&
        AppConstants.platform == "win") {
      if (AppConstants.isPlatformAndVersionAtMost("win", "5.9")) {
        return gNavigatorBundle.getString("decoder.noCodecsXP.message");
      }
      return gNavigatorBundle.getString("decoder.noCodecs.message");
    }
    if (type == "platform-decoder-not-found") {
      if (AppConstants.isPlatformAndVersionAtLeast("win", "6")) {
        return gNavigatorBundle.getString("decoder.noHWAcceleration.message");
      }
      if (AppConstants.platform == "linux") {
        return gNavigatorBundle.getString("decoder.noCodecsLinux.message");
      }
    }
    return "";
  },

  receiveMessage({target: browser, data: data}) {
    let box = gBrowser.getNotificationBox(browser);
    let notificationId = "decoder-doctor-notification";
    if (box.getNotificationWithValue(notificationId)) {
      return;
    }

    let parsedData;
    try {
      parsedData = JSON.parse(data);
    } catch (ex) {
      Cu.reportError("Malformed Decoder Doctor message with data: " + data);
      return;
    }
    let {type} = parsedData;
    type = type.toLowerCase();
    let title = gDecoderDoctorHandler.getLabelForNotificationBox(type);
    if (!title) {
      return;
    }

    let buttons = [];
    if (gDecoderDoctorHandler.shouldShowLearnMoreButton()) {
      buttons.push({
        label: gNavigatorBundle.getString("decoder.noCodecs.button"),
        accessKey: gNavigatorBundle.getString("decoder.noCodecs.accesskey"),
        callback() {
          let baseURL = Services.urlFormatter.formatURLPref("app.support.baseURL");
          openUILinkIn(baseURL + "fix-video-audio-problems-firefox-windows", "tab");
        }
      });
    }

    box.appendNotification(
      title,
      notificationId,
      "", // This uses the info icon as specified below.
      box.PRIORITY_INFO_LOW,
      buttons
    );
  },
}

window.messageManager.addMessageListener("DecoderDoctor:Notification", gDecoderDoctorHandler);
window.messageManager.addMessageListener("EMEVideo:ContentMediaKeysRequest", gEMEHandler);
window.addEventListener("unload", function() {
  window.messageManager.removeMessageListener("EMEVideo:ContentMediaKeysRequest", gEMEHandler);
  window.messageManager.removeMessageListener("DecoderDoctor:Notification", gDecoderDoctorHandler);
}, false);
