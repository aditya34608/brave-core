// Copyright (c) 2026 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <set>
#include <string>

#include "base/functional/bind.h"
#include "base/synchronization/lock.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/thread_annotations.h"
#include "brave/browser/brave_shields/ad_block_browser_test_helper.h"
#include "brave/browser/brave_shields/ad_block_service_browsertest.h"
#include "brave/components/brave_shields/core/browser/brave_shields_utils.h"
#include "brave/components/brave_shields/core/common/features.h"
#include "brave/components/brave_shields/core/common/pref_names.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "url/gurl.h"

using brave_shields::ControlType;
using brave_shields::features::kBraveAdblockPopupBlocking;
using brave_shields::prefs::kAdsBlocked;

namespace {

class CommitObserver : public content::WebContentsObserver {
 public:
  explicit CommitObserver(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents) {}

  bool committed() const { return committed_; }

 private:
  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (handle->IsInPrimaryMainFrame() && handle->HasCommitted() &&
        handle->GetURL().SchemeIsHTTPOrHTTPS()) {
      committed_ = true;
    }
  }

  bool committed_ = false;
};

// Waits until `popup` either commits an http(s) page or is closed, and returns
// true if it was closed.
bool WaitForBlocked(content::WebContents* popup) {
  content::WebContentsDestroyedWatcher destroyed(popup);
  CommitObserver commit(popup);
  EXPECT_TRUE(base::test::RunUntil(
      [&] { return destroyed.IsDestroyed() || commit.committed(); }));
  return destroyed.IsDestroyed();
}

}  // namespace

class AdBlockPopupTestBase : public AdBlockServiceTest {
 public:
  GURL Url(const std::string& host) {
    return embedded_test_server()->GetURL(host, "/simple.html");
  }

  // Runs `script` in `opener`, which must open a window, and returns true if
  // that window was closed before it committed a page. Otherwise closes it.
  bool OpensBlockedPopup(const content::ToRenderFrameHost& opener,
                         const std::string& script) {
    TabStripModel* tabs = browser()->tab_strip_model();
    const int tab_count = tabs->count();
    content::WebContents* popup = nullptr;
    {
      content::WebContentsAddedObserver added;
      content::ExecuteScriptAsync(opener, script);
      popup = added.GetWebContents();
    }

    const bool blocked = WaitForBlocked(popup);

    if (!blocked) {
      tabs->CloseWebContentsAt(tabs->GetIndexOfWebContents(popup), 0);
    }

    EXPECT_EQ(tab_count, tabs->count());
    return blocked;
  }

  bool OpensBlockedPopup(const std::string& script) {
    return OpensBlockedPopup(web_contents(), script);
  }

  bool WindowOpenIsBlocked(const content::ToRenderFrameHost& opener,
                           const GURL& url) {
    return OpensBlockedPopup(opener,
                             content::JsReplace("window.open($1)", url));
  }

  bool WindowOpenIsBlocked(const GURL& url) {
    return WindowOpenIsBlocked(web_contents(), url);
  }

  // Loads a page with a `target=_blank` link to `url` with the id `link`.
  void AddLink(const GURL& url) {
    NavigateToURL(Url("a.com"));
    ASSERT_TRUE(content::ExecJs(
        web_contents(),
        content::JsReplace("const a = document.createElement('a');"
                           "a.id = 'link';"
                           "a.href = $1;"
                           "a.target = '_blank';"
                           "a.textContent = 'link';"
                           "document.body.appendChild(a);",
                           url)));
  }

  // Adds a frame to the active tab and returns it once it has loaded.
  content::RenderFrameHost* AddFrame(const GURL& url,
                                     const std::string& sandbox) {
    const std::string script =
        "new Promise(resolve => {"
        "  const e = document.createElement('iframe');"
        "  if ($2) {"
        "    e.sandbox = $2;"
        "  }"
        "  e.src = $1;"
        "  e.onload = () =>"
        "      resolve(document.querySelectorAll('iframe').length);"
        "  document.body.appendChild(e);"
        "})";
    const int count = content::EvalJs(web_contents(),
                                      content::JsReplace(script, url, sandbox))
                          .ExtractInt();
    return content::ChildFrameAt(web_contents()->GetPrimaryMainFrame(),
                                 count - 1);
  }
};

class AdBlockPopupTest : public AdBlockPopupTestBase {
 public:
  AdBlockPopupTest() {
    feature_list_.InitAndEnableFeature(kBraveAdblockPopupBlocking);
  }

  void SetUpOnMainThread() override {
    embedded_test_server()->RegisterRequestMonitor(base::BindRepeating(
        &AdBlockPopupTest::OnRequest, base::Unretained(this)));
    AdBlockPopupTestBase::SetUpOnMainThread();
  }

  // Whether the test server got a request for `host`.
  bool WasRequested(const std::string& host) {
    base::AutoLock lock(lock_);
    return requested_hosts_.contains(host);
  }

 private:
  void OnRequest(const net::test_server::HttpRequest& request) {
    auto host = request.headers.find("Host");

    if (host == request.headers.end()) {
      return;
    }

    base::AutoLock lock(lock_);
    requested_hosts_.emplace(GURL("http://" + host->second).host());
  }

  base::test::ScopedFeatureList feature_list_;
  base::Lock lock_;
  std::set<std::string> requested_hosts_ GUARDED_BY(lock_);
};

class AdBlockPopupDisabledTest : public AdBlockPopupTestBase {
 public:
  AdBlockPopupDisabledTest() {
    feature_list_.InitAndDisableFeature(kBraveAdblockPopupBlocking);
  }

 private:
  base::test::ScopedFeatureList feature_list_;
};

IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, WindowOpen) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));

  const uint64_t ads_blocked_count =
      profile()->GetPrefs()->GetUint64(kAdsBlocked);
  EXPECT_TRUE(WindowOpenIsBlocked(Url("b.com")));
  EXPECT_EQ(profile()->GetPrefs()->GetUint64(kAdsBlocked),
            ads_blocked_count + 1);
  EXPECT_FALSE(WasRequested("b.com"));

  EXPECT_FALSE(WindowOpenIsBlocked(Url("c.com")));
  EXPECT_TRUE(WasRequested("c.com"));
}

// The page gets a window that closes, as it does when uBO closes a popup.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, ReturnsHandle) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));

  content::WebContents* popup = nullptr;
  {
    content::WebContentsAddedObserver added;
    EXPECT_EQ(true, content::EvalJs(
                        web_contents(),
                        content::JsReplace(
                            "window.w = window.open($1);"
                            "w !== null && !navigator.userActivation.isActive",
                            Url("b.com"))));
    popup = added.GetWebContents();
  }
  EXPECT_TRUE(WaitForBlocked(popup));

  EXPECT_EQ(true, content::EvalJs(web_contents(),
                                  "new Promise(resolve => {"
                                  "  const id = setInterval(() => {"
                                  "    if (w.closed) {"
                                  "      clearInterval(id);"
                                  "      resolve(true);"
                                  "    }"
                                  "  }, 10);"
                                  "})"));
}

IN_PROC_BROWSER_TEST_F(AdBlockPopupDisabledTest, WindowOpen) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("b.com")));
}

// A popup rule doesn't block loading the same URL in the current tab.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, NotAPopup) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("b.com"));
  EXPECT_EQ(Url("b.com"), web_contents()->GetLastCommittedURL());

  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(content::NavigateToURLFromRenderer(web_contents(), Url("b.com")));
  EXPECT_EQ(Url("b.com"), web_contents()->GetLastCommittedURL());
}

// `third-party` and `domain=` are checked against the page that opened the
// popup.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, OpenerIsSource) {
  UpdateAdBlockInstanceWithRules(
      "||b.com^$popup,third-party\n||c.com^$popup,domain=a.com");

  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(WindowOpenIsBlocked(Url("b.com")));
  EXPECT_TRUE(WindowOpenIsBlocked(Url("c.com")));

  NavigateToURL(Url("b.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("b.com")));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("c.com")));
}

// A popup opened from a cross-site frame is checked against the frame. A
// sandboxed frame has an opaque origin, so the page it was loaded from is used.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, OpenedFromFrame) {
  UpdateAdBlockInstanceWithRules("||c.com^$popup,domain=b.com");
  NavigateToURL(Url("a.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("c.com")));

  EXPECT_TRUE(WindowOpenIsBlocked(AddFrame(Url("b.com"), ""), Url("c.com")));
  EXPECT_TRUE(WindowOpenIsBlocked(
      AddFrame(Url("b.com"), "allow-scripts allow-popups"), Url("c.com")));
}

// A popup opened by a blank window uses the settings of the site that opened
// the blank window.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, OpenedFromBlankWindow) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup,domain=a.com");
  NavigateToURL(Url("a.com"));
  content::WebContents* blank = nullptr;
  {
    content::WebContentsAddedObserver added;
    content::ExecuteScriptAsync(web_contents(), "window.open()");
    blank = added.GetWebContents();
  }
  EXPECT_TRUE(WindowOpenIsBlocked(blank, Url("b.com")));
}

// In standard mode third-party popups are blocked and first-party popups are
// left alone, like other first-party requests.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, StandardMode) {
  UpdateAdBlockInstanceWithRules("||a.com^$popup\n||b.com^$popup");
  DisableAggressiveMode();
  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(WindowOpenIsBlocked(Url("b.com")));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("a.com")));

  brave_shields::SetCosmeticFilteringControlType(
      content_settings(), ControlType::BLOCK, Url("a.com"));
  EXPECT_TRUE(WindowOpenIsBlocked(Url("a.com")));
}

// Shields down on either the opener or the popup's site turns the check off.
// Ad blocking is read from the opener.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, ShieldsSettings) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));

  brave_shields::SetBraveShieldsEnabled(content_settings(), false,
                                        Url("b.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("b.com")));
  brave_shields::SetBraveShieldsEnabled(content_settings(), true, Url("b.com"));
  EXPECT_TRUE(WindowOpenIsBlocked(Url("b.com")));

  brave_shields::SetBraveShieldsEnabled(content_settings(), false,
                                        Url("a.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("b.com")));

  brave_shields::SetBraveShieldsEnabled(content_settings(), true, Url("a.com"));
  brave_shields::SetAdControlType(content_settings(), ControlType::ALLOW,
                                  Url("a.com"));
  EXPECT_FALSE(WindowOpenIsBlocked(Url("b.com")));
}

IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, BlankThenNavigate) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(OpensBlockedPopup(content::JsReplace(
      "{ const w = window.open(); w.location = $1; }", Url("b.com"))));
  EXPECT_TRUE(OpensBlockedPopup(content::JsReplace(
      "{ const w = window.open('about:blank'); w.location = $1; }",
      Url("b.com"))));
}

// Only the redirect target matches, so the first hop is requested and the
// redirect isn't followed.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, Redirect) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(OpensBlockedPopup(content::JsReplace(
      "window.open($1)", embedded_test_server()->GetURL(
                             "c.com", "/cross-site/b.com/simple.html"))));
  EXPECT_TRUE(WasRequested("c.com"));
  EXPECT_FALSE(WasRequested("b.com"));
}

IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, Noopener) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  NavigateToURL(Url("a.com"));
  EXPECT_TRUE(OpensBlockedPopup(content::JsReplace(
      "window.open($1, '_blank', 'noopener')", Url("b.com"))));
}

// A link the user clicks isn't a popup, but the same link clicked from script
// is.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, ClickedLink) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  AddLink(Url("b.com"));

  ASSERT_TRUE(OpensBlockedPopup("document.getElementById('link').click()"));

  content::WebContents* popup = nullptr;
  {
    content::WebContentsAddedObserver added;
    content::SimulateMouseClickOrTapElementWithId(web_contents(), "link");
    popup = added.GetWebContents();
  }
  EXPECT_FALSE(WaitForBlocked(popup));
}

// A link opened from the keyboard isn't a popup either.
IN_PROC_BROWSER_TEST_F(AdBlockPopupTest, KeyboardActivatedLink) {
  UpdateAdBlockInstanceWithRules("||b.com^$popup");
  AddLink(Url("b.com"));
  ASSERT_TRUE(content::ExecJs(web_contents(),
                              "document.getElementById('link').focus()"));

  content::WebContents* popup = nullptr;
  {
    content::WebContentsAddedObserver added;
    content::SimulateKeyPress(web_contents(), ui::DomKey::ENTER,
                              ui::DomCode::ENTER, ui::VKEY_RETURN, false, false,
                              false, false);
    popup = added.GetWebContents();
  }
  EXPECT_FALSE(WaitForBlocked(popup));
}
