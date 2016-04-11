Positron: Electron-compatible runtime on top of Gecko
===
This project is an [Electron](http://electron.atom.io/)-compatible app shell for creating desktop apps based on Gecko, the rendering engine used in Firefox.

### Current status
This is a _work in progress_, and doesn't run Electron based apps yet.  Part of this project is being worked on in the [SpiderNode](https://github.com/mozilla/spidernode) repository.

We're actively working on this, so if you're interested in the status of this project, please check here again soon.

### How to build
Before building please make sure you have the prerequisites for building Firefox as documented [here](https://developer.mozilla.org/en-US/docs/Mozilla/Developer_guide/Build_Instructions/Simple_Firefox_build#Build_prerequisites).

You need to add the following line to your mozconfig file:
```
ac_add_options --enable-application=positron
```

Build Command:
```bash
./mach build
```
