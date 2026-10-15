// SPDX-License-Identifier: Apache-2.0
// liberps docs: theme toggle and the Appendix III ring replay on the home page.
(function () {
  "use strict";

  // ---- theme: explicit choice stored per browser, otherwise follow the system ----
  var root = document.documentElement;
  try {
    var saved = localStorage.getItem("liberps-theme");
    if (saved === "dark" || saved === "light") root.setAttribute("data-theme", saved);
  } catch (e) { /* storage unavailable: follow the system */ }
  var btn = document.getElementById("theme-toggle");
  if (btn) {
    btn.addEventListener("click", function () {
      var dark = root.getAttribute("data-theme") === "dark" ||
        (!root.getAttribute("data-theme") && window.matchMedia("(prefers-color-scheme: dark)").matches);
      var next = dark ? "light" : "dark";
      root.setAttribute("data-theme", next);
      try { localStorage.setItem("liberps-theme", next); } catch (e) { /* ignore */ }
    });
  }

  // ---- ring replay: Appendix III scenario A, events from examples/appendix3.erps ----
  var log = document.getElementById("ring-log");
  if (!log) return;
  var steps = {
    idle: {
      blocked: ["6-1", "0-0"], failed: [],
      state: "<b>Idle (A)</b> on every node. RPL blocked at both ends.",
      log: [
        "300.000  G  Pending -> Idle      row 66  WTR expires: Tx R-APS(NR,RB,DNF)",
        "300.000  A..F  Pending -> Idle   row 70  R-APS(NR,RB)",
        "         A keeps its RPL end blocked (neighbour)",
        "         G repeats R-APS(NR,RB) every 5 s"
      ]
    },
    fail: {
      blocked: ["2-1", "3-0"], failed: [2],
      state: "<b>Protection (B)</b> on every node. C and D block the failed link; the RPL is open.",
      log: [
        "301.001000  C  Idle -> Protection   row 5  local SF: block port 1, Tx R-APS(SF), flush",
        "301.001000  D  Idle -> Protection   row 5  local SF: block port 0, Tx R-APS(SF), flush",
        "301.001050  B  Idle -> Protection   row 7  R-APS(SF)",
        "301.001050  E  Idle -> Protection   row 7  R-APS(SF)",
        "301.001100  A  port 0 unblocked     row 7  RPL opens at the neighbour",
        "301.001100  F  Idle -> Protection   row 7  R-APS(SF)",
        "301.001150  G  port 1 unblocked     row 7  RPL opens at the owner"
      ]
    },
    pend: {
      blocked: ["2-1"], failed: [],
      state: "<b>Pending (E)</b> on every node. C (ID 89) keeps one block; WTR runs at G.",
      log: [
        "302.001000  C  Protection -> Pending  row 20  clear SF: guard, Tx R-APS(NR)",
        "302.001000  D  Protection -> Pending  row 20  clear SF: guard, Tx R-APS(NR)",
        "302.001050  B, E  -> Pending          row 29  R-APS(NR)",
        "302.001100  A, F  -> Pending          row 29  R-APS(NR)",
        "302.001150  G  -> Pending             row 29  R-APS(NR): start WTR (5 min)",
        "307.007716  D  port 0 unblocked       row 71  R-APS(NR) from C: 89 > 62"
      ]
    },
    wtr: {
      blocked: ["6-1", "0-0"], failed: [],
      state: "<b>Idle (A)</b> again. The owner reverted; C unblocked; the RPL is blocked at both ends.",
      log: [
        "602.001150  G  Pending -> Idle  row 66  WTR expires: block RPL, Tx R-APS(NR,RB), flush",
        "            A..F  -> Idle       row 70  R-APS(NR,RB)",
        "            A  blocks its RPL end (neighbour); C unblocks its port 1",
        "            every node flushes once more: new (node ID, BPR) pair"
      ]
    }
  };
  var buttons = document.querySelectorAll(".steps button");
  function show(name) {
    var s = steps[name];
    if (!s) return;
    for (var i = 0; i < 7; i++) {
      for (var p = 0; p < 2; p++) {
        var el = document.getElementById("b" + i + "-" + p);
        if (el) el.classList.toggle("on", s.blocked.indexOf(i + "-" + p) >= 0);
      }
      var failed = s.failed.indexOf(i) >= 0;
      var lk = document.getElementById("l" + i);
      var x = document.getElementById("x" + i);
      if (lk) lk.classList.toggle("failed", failed);
      if (x) { if (failed) x.removeAttribute("visibility"); else x.setAttribute("visibility", "hidden"); }
    }
    document.getElementById("ring-state").innerHTML = s.state;
    log.textContent = s.log.join("\n");
    for (var b = 0; b < buttons.length; b++)
      buttons[b].setAttribute("aria-pressed", String(buttons[b].getAttribute("data-step") === name));
  }
  for (var k = 0; k < buttons.length; k++) {
    buttons[k].addEventListener("click", function () { show(this.getAttribute("data-step")); });
  }
  show("idle");
})();
