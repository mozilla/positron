'use strict';

// In Node, process.js is basically a noop, as it just re-exports the existing
// `process` global.  We use it to implement some bits and pieces that Electron
// expects, then construct the `process` global from it (instead of the other
// way around).  We can presumably stop doing that once we're using real Node.

// Because we construct the `process` global in this module, and we also inject
// the global into this module, we don't actually have to export its symbols
// here.  We just have to attach them to this module's own `process` global.
// XXX Figure out if that's really the best way to implement this functionality.

// Re-export process as a native module
// module.exports = process;

// This comes from Electron, where it's defined by common/init.js, which we
// currently don't load.  Once we enable loading of init.js modules, we should
// remove it from this module.
process.atomBinding = function(name) {
  try {
    return process.binding("atom_" + process.type + "_" + name);
  } catch (error) {
    if (/No such module/.test(error.message)) {
      return process.binding("atom_common_" + name);
    } else {
      throw error;
    }
  }
};

// This loads a native binding in Node, but we aren't using real Node yet,
// so we simply require the module with the same name, so we can implement
// bindings in JavaScript.
process.binding = function(name) {
  return require(name);
}
