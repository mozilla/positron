/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

dictionary VersionDictionary {
  DOMString node;
  DOMString chrome;
  DOMString electron;
};

[ChromeOnly,
 JSImplementation="@mozilla.org/positron/process;1"]
interface processImpl {
  [Cached, Pure] readonly attribute VersionDictionary versions;
};
