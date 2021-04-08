/* Copyright 2012-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "watchman_system.h"
#include "watchman.h"
#include <folly/String.h>
#include <exception>
#include <optional>

namespace watchman {

CookieSync::Cookie::Cookie(uint64_t numCookies) : numPending(numCookies) {}

CookieSync::CookieSync(const w_string& dir) {
  char hostname[256];
  gethostname(hostname, sizeof(hostname));
  hostname[sizeof(hostname) - 1] = '\0';

  auto prefix =
      w_string::build(WATCHMAN_COOKIE_PREFIX, hostname, "-", ::getpid(), "-");

  auto guard = cookieDirs_.wlock();
  guard->cookiePrefix_ = prefix;
  guard->dirs_.insert(dir);
}

CookieSync::~CookieSync() {
  // Wake up anyone that might have been waiting on us
  abortAllCookies();
}

void CookieSync::addCookieDir(const w_string& dir) {
  auto guard = cookieDirs_.wlock();
  guard->dirs_.insert(dir);
}

void CookieSync::removeCookieDir(const w_string& dir) {
  {
    auto guard = cookieDirs_.wlock();
    guard->dirs_.erase(dir);
  }

  // Cancel the cookies in the removed directory. These are considered to be
  // serviced.
  auto cookies = cookies_.wlock();
  for (const auto& [cookiePath, cookie] : *cookies) {
    if (w_string_startswith(cookiePath, dir)) {
      cookie->notify();
      cookies->erase(cookiePath);
    }
  }
}

void CookieSync::setCookieDir(const w_string& dir) {
  auto guard = cookieDirs_.wlock();
  guard->dirs_.clear();
  guard->dirs_.insert(dir);
}

std::vector<w_string> CookieSync::getOutstandingCookieFileList() const {
  std::vector<w_string> result;
  for (auto& it : *cookies_.rlock()) {
    result.push_back(it.first);
  }

  return result;
}

folly::Future<folly::Unit> CookieSync::sync() {
  auto prefixes = cookiePrefix();
  auto serial = serial_++;

  auto cookie = std::make_shared<Cookie>(prefixes.size());

  // Even though we only write to the cookie at the end of the function, we
  // need to hold it while the files are written on disk to avoid a race where
  // cookies are detected on disk by the watcher, and notifyCookie is called
  // prior to all the pending cookies being added to cookies_. Holding the lock
  // will make sure that notifyCookie will be serialized with this code.
  auto cookiesLock = cookies_.wlock();

  CookieMap pendingCookies;
  std::optional<std::tuple<w_string, int>> lastError;

  for (const auto& prefix : prefixes) {
    auto path_str = w_string::build(prefix, serial);

    /* then touch the file */
    auto file = w_stm_open(
        path_str.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0700);
    if (!file) {
      auto errCode = errno;
      lastError = {path_str, errCode};
      cookie->numPending.fetch_sub(1, std::memory_order_acq_rel);
      logf(
          ERR,
          "sync cookie {} couldn't be created: {}\n",
          path_str,
          folly::errnoStr(errCode));
      continue;
    }

    /* insert the cookie into the temporary map */
    pendingCookies[path_str] = cookie;
    logf(DBG, "sync created cookie file {}\n", path_str);
  }

  if (pendingCookies.size() == 0) {
    w_assert(lastError.has_value(), "no cookies written, but no errors set");
    auto errCode = std::get<int>(*lastError);
    throw std::system_error(
        errCode,
        std::generic_category(),
        folly::to<std::string>(
            "sync: creat(",
            std::get<w_string>(*lastError),
            ") failed: ",
            folly::errnoStr(errCode)));
  }

  cookiesLock->insert(pendingCookies.begin(), pendingCookies.end());

  return cookie->promise.getFuture();
}

void CookieSync::syncToNow(std::chrono::milliseconds timeout) {
  /* compute deadline */
  using namespace std::chrono;
  auto deadline = system_clock::now() + timeout;

  while (true) {
    auto cookie = sync();

    if (!cookie.wait(timeout).isReady()) {
      auto why = folly::to<std::string>(
          "syncToNow: timed out waiting for cookie file to be "
          "observed by watcher within ",
          timeout.count(),
          " milliseconds");
      log(ERR, why, "\n");
      throw std::system_error(ETIMEDOUT, std::generic_category(), why);
    }

    if (cookie.result().hasValue()) {
      // Success!
      return;
    }

    // Sync was aborted by a recrawl; recompute the timeout
    // and wait again if we still have time
    timeout = duration_cast<milliseconds>(deadline - system_clock::now());
    if (timeout.count() <= 0) {
      cookie.result().throwUnlessValue();
    }
  }
}

void CookieSync::abortAllCookies() {
  std::unordered_map<w_string, std::shared_ptr<Cookie>> cookies;

  {
    auto map = cookies_.wlock();
    std::swap(*map, cookies);
  }

  for (auto& it : cookies) {
    if (it.second->numPending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      log(ERR, "syncToNow: aborting cookie ", it.first, "\n");
      it.second->promise.setException(
          folly::make_exception_wrapper<CookieSyncAborted>());
    }
  }
}

void CookieSync::Cookie::notify() {
  if (numPending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    promise.setValue();
  }
}

void CookieSync::notifyCookie(const w_string& path) {
  std::shared_ptr<Cookie> cookie;

  {
    auto map = cookies_.wlock();
    auto cookie_iter = map->find(path);
    log(DBG,
        "cookie for ",
        path,
        "? ",
        cookie_iter != map->end() ? "yes" : "no",
        "\n");

    if (cookie_iter != map->end()) {
      cookie = std::move(cookie_iter->second);
      map->erase(cookie_iter);
    }
  }

  if (cookie) {
    cookie->notify();

    // The file may not exist at this point; we're just taking this
    // opportunity to remove it if nothing else has done so already.
    // We don't care about the return code; best effort is fine.
    unlink(path.c_str());
  }
}

bool CookieSync::isCookiePrefix(const w_string& path) {
  auto cookieDirs = cookieDirs_.rlock();
  for (const auto& dir : cookieDirs->dirs_) {
    if (w_string_startswith(path, dir) &&
        w_string_startswith(path.baseName(), cookieDirs->cookiePrefix_)) {
      return true;
    }
  }
  return false;
}

bool CookieSync::isCookieDir(const w_string& path) {
  auto cookieDirs = cookieDirs_.rlock();
  for (const auto& dir : cookieDirs->dirs_) {
    if (w_string_equal(path, dir)) {
      return true;
    }
  }
  return false;
}

std::unordered_set<w_string> CookieSync::cookiePrefix() const {
  std::unordered_set<w_string> res;
  auto guard = cookieDirs_.rlock();
  for (const auto& dir : guard->dirs_) {
    res.insert(w_string::build(dir, "/", guard->cookiePrefix_));
  }
  return res;
}

std::unordered_set<w_string> CookieSync::cookieDirs() const {
  std::unordered_set<w_string> res;
  auto guard = cookieDirs_.rlock();
  for (const auto& dir : guard->dirs_) {
    res.insert(dir);
  }
  return res;
}

} // namespace watchman
