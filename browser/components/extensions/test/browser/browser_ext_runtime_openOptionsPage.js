/* -*- Mode: indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set sts=2 sw=2 et tw=80: */
"use strict";

requestLongerTimeout(2);

function add_tasks(task) {
  add_task(task.bind(null, {embedded: false}));

  add_task(task.bind(null, {embedded: true}));
}

function* loadExtension(options) {
  let extension = ExtensionTestUtils.loadExtension({
    useAddonManager: "temporary",

    embedded: options.embedded,

    manifest: Object.assign({
      "permissions": ["tabs"],
    }, options.manifest),

    files: {
      "options.html": `<!DOCTYPE html>
        <html>
          <head>
            <meta charset="utf-8">
            <script src="options.js" type="text/javascript"></script>
          </head>
        </html>`,

      "options.js": function() {
        browser.runtime.sendMessage("options.html");
        browser.runtime.onMessage.addListener((msg, sender, respond) => {
          if (msg == "ping") {
            respond("pong");
          }
        });
      },
    },

    background: options.background,
  });

  yield extension.startup();

  return extension;
}

add_tasks(function* test_inline_options(extraOptions) {
  info(`Test options opened inline (${JSON.stringify(extraOptions)})`);

  let tab = yield BrowserTestUtils.openNewForegroundTab(gBrowser, "http://example.com/");

  let extension = yield loadExtension(Object.assign({}, extraOptions, {
    manifest: {
      applications: {gecko: {id: "inline_options@tests.mozilla.org"}},
      "options_ui": {
        "page": "options.html",
      },
    },

    background: function() {
      let _optionsPromise;
      let awaitOptions = () => {
        browser.test.assertFalse(_optionsPromise, "Should not be awaiting options already");

        return new Promise(resolve => {
          _optionsPromise = {resolve};
        });
      };

      browser.runtime.onMessage.addListener((msg, sender) => {
        if (msg == "options.html") {
          if (_optionsPromise) {
            _optionsPromise.resolve(sender.tab);
            _optionsPromise = null;
          } else {
            browser.test.fail("Saw unexpected options page load");
          }
        }
      });

      let firstTab, optionsTab;
      browser.tabs.query({currentWindow: true, active: true}).then(tabs => {
        firstTab = tabs[0].id;

        browser.test.log("Open options page. Expect fresh load.");
        return Promise.all([
          browser.runtime.openOptionsPage(),
          awaitOptions(),
        ]);
      }).then(([, tab]) => {
        browser.test.assertEq("about:addons", tab.url, "Tab contains AddonManager");
        browser.test.assertTrue(tab.active, "Tab is active");
        browser.test.assertTrue(tab.id != firstTab, "Tab is a new tab");

        optionsTab = tab.id;

        browser.test.log("Switch tabs.");
        return browser.tabs.update(firstTab, {active: true});
      }).then(() => {
        browser.test.log("Open options page again. Expect tab re-selected, no new load.");

        return browser.runtime.openOptionsPage();
      }).then(() => {
        return browser.tabs.query({currentWindow: true, active: true});
      }).then(([tab]) => {
        browser.test.assertEq(optionsTab, tab.id, "Tab is the same as the previous options tab");
        browser.test.assertEq("about:addons", tab.url, "Tab contains AddonManager");

        browser.test.log("Ping options page.");
        return browser.runtime.sendMessage("ping");
      }).then((pong) => {
        browser.test.assertEq("pong", pong, "Got pong.");

        browser.test.log("Remove options tab.");
        return browser.tabs.remove(optionsTab);
      }).then(() => {
        browser.test.log("Open options page again. Expect fresh load.");
        return Promise.all([
          browser.runtime.openOptionsPage(),
          awaitOptions(),
        ]);
      }).then(([, tab]) => {
        browser.test.assertEq("about:addons", tab.url, "Tab contains AddonManager");
        browser.test.assertTrue(tab.active, "Tab is active");
        browser.test.assertTrue(tab.id != optionsTab, "Tab is a new tab");

        return browser.tabs.remove(tab.id);
      }).then(() => {
        browser.test.notifyPass("options-ui");
      }).catch(error => {
        browser.test.log(`Error: ${error} :: ${error.stack}`);
        browser.test.notifyFail("options-ui");
      });
    },
  }));

  yield extension.awaitFinish("options-ui");
  yield extension.unload();

  yield BrowserTestUtils.removeTab(tab);
});

add_tasks(function* test_tab_options(extraOptions) {
  info(`Test options opened in a tab (${JSON.stringify(extraOptions)})`);

  let tab = yield BrowserTestUtils.openNewForegroundTab(gBrowser, "http://example.com/");

  let extension = yield loadExtension(Object.assign({}, extraOptions, {
    manifest: {
      applications: {gecko: {id: "tab_options@tests.mozilla.org"}},
      "options_ui": {
        "page": "options.html",
        "open_in_tab": true,
      },
    },

    background: function() {
      let _optionsPromise;
      let awaitOptions = () => {
        browser.test.assertFalse(_optionsPromise, "Should not be awaiting options already");

        return new Promise(resolve => {
          _optionsPromise = {resolve};
        });
      };

      browser.runtime.onMessage.addListener((msg, sender) => {
        if (msg == "options.html") {
          if (_optionsPromise) {
            _optionsPromise.resolve(sender.tab);
            _optionsPromise = null;
          } else {
            browser.test.fail("Saw unexpected options page load");
          }
        }
      });

      let optionsURL = browser.extension.getURL("options.html");

      let firstTab, optionsTab;
      browser.tabs.query({currentWindow: true, active: true}).then(tabs => {
        firstTab = tabs[0].id;

        browser.test.log("Open options page. Expect fresh load.");
        return Promise.all([
          browser.runtime.openOptionsPage(),
          awaitOptions(),
        ]);
      }).then(([, tab]) => {
        browser.test.assertEq(optionsURL, tab.url, "Tab contains options.html");
        browser.test.assertTrue(tab.active, "Tab is active");
        browser.test.assertTrue(tab.id != firstTab, "Tab is a new tab");

        optionsTab = tab.id;

        browser.test.log("Switch tabs.");
        return browser.tabs.update(firstTab, {active: true});
      }).then(() => {
        browser.test.log("Open options page again. Expect tab re-selected, no new load.");

        return browser.runtime.openOptionsPage();
      }).then(() => {
        return browser.tabs.query({currentWindow: true, active: true});
      }).then(([tab]) => {
        browser.test.assertEq(optionsTab, tab.id, "Tab is the same as the previous options tab");
        browser.test.assertEq(optionsURL, tab.url, "Tab contains options.html");

        // Unfortunately, we can't currently do this, since onMessage doesn't
        // currently support responses when there are multiple listeners.
        //
        // browser.test.log("Ping options page.");
        // return new Promise(resolve => browser.runtime.sendMessage("ping", resolve));

        browser.test.log("Remove options tab.");
        return browser.tabs.remove(optionsTab);
      }).then(() => {
        browser.test.log("Open options page again. Expect fresh load.");
        return Promise.all([
          browser.runtime.openOptionsPage(),
          awaitOptions(),
        ]);
      }).then(([, tab]) => {
        browser.test.assertEq(optionsURL, tab.url, "Tab contains options.html");
        browser.test.assertTrue(tab.active, "Tab is active");
        browser.test.assertTrue(tab.id != optionsTab, "Tab is a new tab");

        return browser.tabs.remove(tab.id);
      }).then(() => {
        browser.test.notifyPass("options-ui-tab");
      }).catch(error => {
        browser.test.log(`Error: ${error} :: ${error.stack}`);
        browser.test.notifyFail("options-ui-tab");
      });
    },
  }));

  yield extension.awaitFinish("options-ui-tab");
  yield extension.unload();

  yield BrowserTestUtils.removeTab(tab);
});

add_tasks(function* test_options_no_manifest(extraOptions) {
  info(`Test with no manifest key (${JSON.stringify(extraOptions)})`);

  let extension = yield loadExtension(Object.assign({}, extraOptions, {
    manifest: {
      applications: {gecko: {id: "no_options@tests.mozilla.org"}},
    },

    background: function() {
      browser.test.log("Try to open options page when not specified in the manifest.");

      browser.runtime.openOptionsPage().then(
        () => {
          browser.test.fail("Opening options page without one specified in the manifest generated an error");
          browser.test.notifyFail("options-no-manifest");
        },
        error => {
          let expected = "No `options_ui` declared";
          browser.test.assertTrue(
            error.message.includes(expected),
            `Got expected error (got: '${error.message}', expected: '${expected}'`);
        }
      ).then(() => {
        browser.test.notifyPass("options-no-manifest");
      }).catch(error => {
        browser.test.log(`Error: ${error} :: ${error.stack}`);
        browser.test.notifyFail("options-no-manifest");
      });
    },
  }));

  yield extension.awaitFinish("options-no-manifest");
  yield extension.unload();
});
