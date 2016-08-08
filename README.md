Positron: Electron-compatible runtime on top of Gecko
===
This project is an [Electron](http://electron.atom.io/)-compatible app shell for creating desktop apps based on Gecko, the rendering engine used in Firefox.

### Current status
This is a _work in progress_, and doesn't run Electron based apps yet.  Part of this project is being worked on in the [SpiderNode](https://github.com/mozilla/spidernode) repository.

We're actively working on this, so if you're interested in the status of this project, please check here again soon.

### How to download the source
To clone Positron and its submodules:
```
git clone --recursive https://github.com/mozilla/positron.git
```

### How to build
Before building please make sure you have the prerequisites for building Firefox as documented [here](https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_build#Build_prerequisites).

Build Command:
```bash
./mach build
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
