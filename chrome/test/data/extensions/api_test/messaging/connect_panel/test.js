// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

var panelWindowId = 0;

chrome.test.runTests([
  function openPanelThatConnectsToExtension() {
    chrome.test.listenOnce(chrome.extension.onConnect, function(port) {
      chrome.test.assertEq(panelWindowId, port.sender.tab.windowId);
      chrome.test.assertTrue(port.sender.tab.id > 0);
    });
    chrome.windows.create(
        { 'url': chrome.extension.getURL('panel.html'), 'type': 'panel' },
        chrome.test.callbackPass(function(win) {
          chrome.test.assertEq('panel', win.type);
          panelWindowId = win.id;
        }));
  }
]);
