// Thin-shell loader: the router files (login/status.html) carry only
// client vars + this script. Everything else lives in portal.html, which is
// fetched, injected into #app, and executed in document order.
// No fetch/Promise dependency: old captive-portal browsers fall back to XHR
// and callback-chained script injection instead of sticking on the loader.
(function () {
  function fail(msg) {
    var bt = document.getElementById("bootText");
    if (!bt) { return; }
    bt.textContent = msg || "Failed to load portal. Please reload.";
    // portal.html's own RETRY button never runs when portal.html or core.js
    // is what failed, so without one here every partial file-set failure is a
    // dead-end spinner with no way forward.
    var host = bt.parentNode;
    if (!host || host.getElementsByClassName("boot-retry").length) { return; }
    var rb = document.createElement("button");
    rb.type = "button";
    rb.className = "boot-retry";
    rb.textContent = "RETRY";
    rb.onclick = function () { location.reload(); };
    host.appendChild(rb);
  }
  function checkLibs() {
    // portal.html order guarantees jQuery before core.js; a 404 on either
    // leaves $ undefined and every coin/session flow throws. Fail loud
    // with a retry instead of revealing a dead page.
    if (typeof window.jQuery === "undefined") {
      fail("jQuery failed to load.");
      return false;
    }
    if (typeof window.hexMD5 === "undefined") {
      fail("Login library failed to load.");
      return false;
    }
    return true;
  }
  function injectScripts(scripts, i) {
    while (i < scripts.length) {
      var s = scripts[i];
      if (!s.src) {
        var el = document.createElement("script");
        el.text = s.textContent;
        document.body.appendChild(el);
        i += 1;
        continue;
      }
      // Download consecutive external scripts in parallel while preserving
      // execution order (async=false), then continue after the group completes.
      var group = [];
      var j = i;
      while (j < scripts.length && scripts[j].src) { group.push(scripts[j]); j += 1; }
      var remaining = group.length;
      var advanced = false;
      function groupDone() {
        if (advanced) { return; }
        remaining -= 1;
        if (remaining <= 0) {
          advanced = true;
          injectScripts(scripts, j);
        }
      }
      for (var k = 0; k < group.length; k++) {
        var gs = group[k];
        var gel = document.createElement("script");
        gel.async = false;
        gel.src = gs.src;
        gel.onload = groupDone;
        gel.onerror = groupDone;
        document.body.appendChild(gel);
      }
      return;
    }
    checkLibs();
  }
  function inject(html) {
    var doc = new DOMParser().parseFromString(html, "text/html");
    var app = doc.getElementById("app");
    if (!app) { fail("Portal page is empty."); return; }
    // The live #app, not the parsed one. Shells load boot.js last so the body
    // is parsed, but the guard keeps a too-early run from throwing.
    var host = document.getElementById("app");
    if (!host) { fail("Portal page is empty."); return; }
    host.innerHTML = app.innerHTML;
    injectScripts(Array.prototype.slice.call(doc.querySelectorAll("script")), 0);
  }
  // status 0 is NOT success (file:// false positive / dead radio): the
  // portal is always served over http by the router, so demand 200.
  if (window.fetch) {
    var done = false;
    setTimeout(function () { if (!done) { done = true; fail("Portal is taking too long."); } }, 10000);
  // Default cache (no no-store): portal.html revalidates via HTTP cache,
  // and the ?v=231 query busts it on deploy alongside core.js?v=231
    fetch("portal.html?v=231").then(function (r) {
      if (done) { return null; }
      done = true;
      if (!r.ok) { throw 0; }
      return r.text();
    }).then(function (t) { if (t !== null) { inject(t); } })
    .catch(function () { fail(); });
  } else {
    try {
      var x = new XMLHttpRequest();
      var done = false;
      function settle(fn) { if (done) { return; } done = true; fn(); }
      // Same 10s overall deadline the fetch path has: a browser that ignores
      // x.timeout or throws on the assignment must not spin forever.
      setTimeout(function () { settle(function () { fail("Portal is taking too long."); }); }, 10000);
      x.open("GET", "portal.html?v=231", true);
      try { x.timeout = 8000; } catch (e) {}
      x.onreadystatechange = function () {
        if (x.readyState === 4) {
          settle(function () {
            if (x.status === 200) { inject(x.responseText); }
            else { fail(); }
          });
        }
      };
      x.onerror = function () { settle(function () { fail(); }); };
      try { x.ontimeout = function () { settle(function () { fail("Portal is taking too long."); }); }; } catch (e) {}
      x.send();
    } catch (e) { fail(); }
  }
})();
