/* xDrip CGM - PebbleKit JS side.
 *
 * Pulls glucose from the xDrip+ local web server (Nightscout-style API).
 * Enable it in xDrip+:
 *   Settings > Inter-app settings > "xDrip Web Service" (ON)
 * Default bind is http://127.0.0.1:17580 on the phone itself.
 */

var BASE = 'http://127.0.0.1:17580';
var HISTORY_MAX = 24;

// Nightscout "direction" string -> trend arrow code (1..7), matches xDrip.
var DIRECTION_TREND = {
  DoubleUp: 1, SingleUp: 2, FortyFiveUp: 3, Flat: 4,
  FortyFiveDown: 5, SingleDown: 6, DoubleDown: 7
};

function clampByte(n) {
  n = Math.round(n / 2);          // 0..510 mg/dL -> one byte, 2 mg/dL resolution
  if (n < 0) n = 0;
  if (n > 255) n = 255;
  return n;
}

function sendError(code) {
  console.log('xdrip: error ' + code);
  Pebble.sendAppMessage({ ERR: code });
}

function fetchBG() {
  var req = new XMLHttpRequest();
  req.open('GET', BASE + '/sgv.json?count=' + HISTORY_MAX, true);
  req.timeout = 8000;
  req.onload = function () {
    console.log('xdrip: HTTP ' + req.status + ' len=' + req.responseText.length);
    if (req.status !== 200) { sendError(req.status || 1); return; }

    var rows;
    try { rows = JSON.parse(req.responseText); }
    catch (e) { sendError(2); return; }
    if (!rows || !rows.length) { sendError(3); return; }

    // Nightscout entries are newest-first.
    var latest = rows[0];
    var sgv = parseInt(latest.sgv, 10) || 0;

    var trend = parseInt(latest.trend, 10);
    if (!trend || trend < 1 || trend > 7) {
      trend = DIRECTION_TREND[latest.direction] || 0;
    }

    var delta = 0;
    if (rows.length > 1) {
      delta = sgv - (parseInt(rows[1].sgv, 10) || sgv);
    }

    var ageMs = Date.now() - (parseInt(latest.date, 10) || Date.now());
    var age = Math.max(0, Math.round(ageMs / 1000));

    var hist = [];
    for (var i = Math.min(rows.length, HISTORY_MAX) - 1; i >= 0; i--) {
      hist.push(clampByte(parseInt(rows[i].sgv, 10) || 0));
    }

    console.log('xdrip: sgv=' + sgv + ' delta=' + delta + ' trend=' + trend +
                ' age=' + age + ' hist=' + hist.length);
    Pebble.sendAppMessage({
      SGV: sgv,
      DELTA: delta,
      TREND: trend,
      AGE: age,
      HIST_COUNT: hist.length,
      HISTORY: hist
    });
  };
  req.ontimeout = function () { sendError(4); };
  req.onerror = function () { sendError(5); };
  req.send();
}

Pebble.addEventListener('ready', fetchBG);
// The watch asks for a refresh by sending any AppMessage.
Pebble.addEventListener('appmessage', fetchBG);
