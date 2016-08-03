const timers = require('timers');

if (process.type === 'browser') {
  process.positronBinding = function(name) {
    return require("../../../modules/" + name);
  };

  process.atomBinding = function(name) {
    try {
      return require("../../../modules/atom_" + process.type + "_" + name);
    } catch (error) {
      if (/Cannot find module/.test(error.message)) {
        return require("../../../modules/atom_common_" + name);
      }
    }
  };
}

// setImmediate and process.nextTick makes use of uv_check and uv_prepare to
// run the callbacks, however since we only run uv loop on requests, the
// callbacks wouldn't be called until something else activated the uv loop,
// which would delay the callbacks for arbitrary long time. So we should
// initiatively activate the uv loop once setImmediate and process.nextTick is
// called.
var wrapWithActivateUvLoop = function(func) {
  return function() {
    // TODO: add a binding to do this
    // process.activateUvLoop();
    return func.apply(this, arguments);
  };
};

process.nextTick = wrapWithActivateUvLoop(process.nextTick);

global.setImmediate = wrapWithActivateUvLoop(timers.setImmediate);

global.clearImmediate = timers.clearImmediate;

if (process.type === 'browser') {
  // setTimeout needs to update the polling timeout of the event loop, when
  // called under Chromium's event loop the node's event loop won't get a chance
  // to update the timeout, so we have to force the node's event loop to
  // recalculate the timeout in browser process.
  global.setTimeout = wrapWithActivateUvLoop(timers.setTimeout);
  global.setInterval = wrapWithActivateUvLoop(timers.setInterval);
}
