#!/usr/bin/env node

var path = require('path');
var proc = require('child_process');

// XXX Preprocess file to use MOZ_MACBUNDLE_NAME on Mac.
var binDir = process.platform === 'darwin' ? 'Positron.app/Contents/MacOS' : 'bin';

var binSuffix = process.platform === 'win32' ? '.exe' : '';

// XXX Preprocess file to use BINARY on Mac.
var binName = 'positron' + binSuffix;

var binPath = path.join(__dirname, binDir, binName);

var child = proc.spawn(binPath, process.argv.slice(2), {stdio: 'inherit'});
child.on('close', function(code) {
  process.exit(code);
});
