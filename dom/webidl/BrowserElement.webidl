/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

callback BrowserElementNextPaintEventCallback = void ();

enum BrowserFindCaseSensitivity { "case-sensitive", "case-insensitive" };
enum BrowserFindDirection { "forward", "backward" };

dictionary BrowserElementDownloadOptions {
  DOMString? filename;
  DOMString? referrer;
};

dictionary BrowserElementExecuteScriptOptions {
  DOMString? url;
  DOMString? origin;
};

[NoInterfaceObject]
interface BrowserElement {
};

BrowserElement implements BrowserElementCommon;
BrowserElement implements BrowserElementPrivileged;

[NoInterfaceObject]
interface BrowserElementCommon {
  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void setVisible(boolean visible);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getVisible();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void setActive(boolean active);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  boolean getActive();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void addNextPaintListener(BrowserElementNextPaintEventCallback listener);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void removeNextPaintListener(BrowserElementNextPaintEventCallback listener);
};

[NoInterfaceObject]
interface BrowserElementPrivileged {
  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void sendMouseEvent(DOMString type,
                      unsigned long x,
                      unsigned long y,
                      unsigned long button,
                      unsigned long clickCount,
                      unsigned long modifiers);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="TouchEvent::PrefEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void sendTouchEvent(DOMString type,
                      sequence<unsigned long> identifiers,
                      sequence<long> x,
                      sequence<long> y,
                      sequence<unsigned long> rx,
                      sequence<unsigned long> ry,
                      sequence<float> rotationAngles,
                      sequence<float> forces,
                      unsigned long count,
                      unsigned long modifiers);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void goBack();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void goForward();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void reload(optional boolean hardReload = false);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void stop();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest download(DOMString url,
                      optional BrowserElementDownloadOptions options);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest purgeHistory();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getScreenshot([EnforceRange] unsigned long width,
                           [EnforceRange] unsigned long height,
                           optional DOMString mimeType="");

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void zoom(float zoom);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getCanGoBack();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getCanGoForward();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getContentDimensions();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest setInputMethodActive(boolean isActive);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void setNFCFocus(boolean isFocus);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void findAll(DOMString searchString, BrowserFindCaseSensitivity caseSensitivity);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void findNext(BrowserFindDirection direction);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  void clearMatch();

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest executeScript(DOMString script,
                           optional BrowserElementExecuteScriptOptions options);

  [Throws,
   Pref="dom.mozBrowserFramesEnabled",
   Func="nsDocument::IsBrowserElementEnabled"]
  DOMRequest getWebManifest();

};
