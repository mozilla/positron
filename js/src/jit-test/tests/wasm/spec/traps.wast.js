// |jit-test| test-also-wasm-baseline
// TODO trap on OOB
quit();
var importedArgs = ['traps.wast']; load(scriptdir + '../spec.js');
