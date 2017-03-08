[![Development Status](https://img.shields.io/badge/status-discontinued-red.svg)]

Positron: Electron-compatible runtime on top of Gecko
===
This project is an [Electron](http://electron.atom.io/)-compatible app shell for creating desktop apps based on Gecko, the rendering engine used in Firefox.

### Current status
As noted in the blog post [Positron Discontinued](https://mykzilla.org/2017/03/08/positron-discontinued/), this project has been discontinued. The source remains available, and you're welcome to reuse it.

### How to download the source
To clone Positron and its submodules:
```
git clone --recursive https://github.com/mozilla/positron.git
```

### How to build
Before building please make sure you have the prerequisites for building Firefox as documented [here](https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_build#Build_prerequisites).

Build Command:
```bash
MOZCONFIG=positron/config/mozconfig ./mach build
```

### How to run
To run an app on Positron, invoke `./mach run` with the path to the app's directory. For example, to run a sample app:

```bash
./mach run positron/test/hello-world
```

You can also `npm link` the build directory and then run the app via the *positron* command:

```bash
(cd obj-x86_64-apple-darwin14.5.0/dist/; npm link) # Build dir name will vary.
positron positron/test/hello-world
```

### How to test
To start an Electron test run:

```bash
(cd positron/electron/spec; npm install) # You only have to do this the first time.
./mach run positron/electron/spec
```

*Note: these tests don't yet run.*
