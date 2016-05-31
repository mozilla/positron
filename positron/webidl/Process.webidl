/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

dictionary ReleaseDictionary {
  DOMString name;
  // Unimplemented keys that are part of the Node specification:
  // DOMString sourceUrl;
  // DOMString headersUrl;
  // DOMString libUrl; // Windows-only
};

dictionary VersionDictionary {
  DOMString node;
  DOMString chrome;
  DOMString electron;
};

// EventEmitter isn't instantiated, its methods are just accessed
// on processImpl.  So it doesn't need a real contract ID.
[ChromeOnly,
 JSImplementation="dummy"]
interface EventEmitter {
  [Throws]
  EventEmitter once(DOMString name, Function listener);
  // TODO: specify the rest of the interface.
};

// This currently specifies only a subset of the attributes and operations
// of the process global as specified by Node.
[ChromeOnly,
 JSImplementation="@mozilla.org/positron/process;1"]
interface processImpl : EventEmitter {
  [Cached, Pure] readonly attribute sequence<DOMString> argv;
  [Cached, Pure] readonly attribute object env;
  [Cached, Pure] readonly attribute DOMString execPath;
  [Cached, Pure] readonly attribute unsigned long pid;
  [Cached, Pure] readonly attribute DOMString platform;
  [Cached, Pure] readonly attribute ReleaseDictionary release;
  [Cached, Pure] readonly attribute DOMString type;
  [Cached, Pure] readonly attribute VersionDictionary versions;
  any atomBinding(DOMString name);
  any binding(DOMString name);
};
