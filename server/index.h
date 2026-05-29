// ================================================================
// index.h — Web UI for generator server
// Updated — pending state feedback added
// ================================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Generator Control</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }

    body {
      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
      background: #1a1a1a;
      color: #f0f0f0;
      display: flex;
      justify-content: center;
      align-items: flex-start;
      min-height: 100vh;
      padding: 40px 20px;
    }

    .card {
      background: #2a2a2a;
      border-radius: 16px;
      padding: 32px;
      width: 100%;
      max-width: 420px;
      box-shadow: 0 4px 24px rgba(0,0,0,0.4);
    }

    h1 {
      font-size: 22px;
      font-weight: 600;
      margin-bottom: 6px;
    }

    .subtitle {
      font-size: 13px;
      color: #888;
      margin-bottom: 32px;
    }

    /* Status badge */
    .status-row {
      display: flex;
      align-items: center;
      gap: 10px;
      margin-bottom: 16px;
      padding: 14px 18px;
      background: #333;
      border-radius: 10px;
    }

    .dot {
      width: 10px;
      height: 10px;
      border-radius: 50%;
      background: #555;
      flex-shrink: 0;
      transition: background 0.3s;
    }

    .dot.running { background: #22c55e; }
    .dot.stopped { background: #888; }
    .dot.error   { background: #ef4444; }
    .dot.unknown { background: #f59e0b; }
    .dot.pending { background: #f59e0b; }

    #status-text {
      font-size: 15px;
      font-weight: 500;
    }

    #status-detail {
      font-size: 12px;
      color: #888;
      margin-top: 2px;
    }

    /* Pending banner */
    #pending-banner {
      display: none;
      align-items: center;
      gap: 10px;
      background: #2d2a1a;
      border: 1px solid #5a4a00;
      border-radius: 10px;
      padding: 12px 16px;
      margin-bottom: 16px;
      font-size: 13px;
      color: #f59e0b;
    }

    #pending-banner.visible {
      display: flex;
    }

    .spinner {
      width: 14px;
      height: 14px;
      border: 2px solid #5a4a00;
      border-top-color: #f59e0b;
      border-radius: 50%;
      flex-shrink: 0;
      animation: spin 0.8s linear infinite;
    }

    @keyframes spin {
      to { transform: rotate(360deg); }
    }

    /* Section labels */
    .section-label {
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: #666;
      margin-bottom: 12px;
    }

    /* Preset buttons */
    .preset-grid {
      display: grid;
      grid-template-columns: 1fr 1fr 1fr;
      gap: 10px;
      margin-bottom: 12px;
    }

    .btn-preset {
      padding: 18px 8px;
      border: none;
      border-radius: 10px;
      background: #22c55e;
      color: #fff;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: opacity 0.2s, transform 0.1s;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 4px;
    }

    .btn-preset .mins {
      font-size: 22px;
      font-weight: 700;
      line-height: 1;
    }

    .btn-preset .unit {
      font-size: 11px;
      opacity: 0.85;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }

    .btn-preset:active:not(:disabled) { transform: scale(0.97); }

    /* Custom row */
    .custom-row {
      display: flex;
      gap: 10px;
      align-items: stretch;
      margin-bottom: 32px;
    }

    .custom-input-wrap {
      display: flex;
      align-items: center;
      gap: 8px;
      background: #333;
      border: 1px solid #444;
      border-radius: 10px;
      padding: 0 14px;
      flex: 1;
      transition: opacity 0.2s;
    }

    .custom-input-wrap input[type="number"] {
      width: 56px;
      background: transparent;
      border: none;
      color: #f0f0f0;
      font-size: 18px;
      font-weight: 600;
      text-align: center;
      padding: 12px 0;
      outline: none;
    }

    .custom-input-wrap input::-webkit-inner-spin-button,
    .custom-input-wrap input::-webkit-outer-spin-button { -webkit-appearance: none; }
    .custom-input-wrap input[type=number] { -moz-appearance: textfield; }

    .custom-input-wrap .unit-label {
      font-size: 13px;
      color: #888;
    }

    #btn-custom {
      padding: 14px 20px;
      border: none;
      border-radius: 10px;
      background: #3b82f6;
      color: #fff;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      transition: opacity 0.2s, transform 0.1s;
      white-space: nowrap;
    }

    #btn-custom:active:not(:disabled) { transform: scale(0.97); }

    /* Stop button */
    #btn-stop {
      width: 100%;
      padding: 16px;
      border: none;
      border-radius: 10px;
      background: #ef4444;
      color: #fff;
      font-size: 16px;
      font-weight: 600;
      cursor: pointer;
      transition: opacity 0.2s, transform 0.1s;
      margin-bottom: 32px;
    }

    #btn-stop:active:not(:disabled) { transform: scale(0.97); }

    /* Disabled state for ALL buttons and input wrap */
    button:disabled,
    .btn-preset:disabled {
      opacity: 0.35;
      cursor: not-allowed;
      transform: none;
    }

    .controls-disabled .custom-input-wrap {
      opacity: 0.35;
    }

    .controls-disabled input {
      pointer-events: none;
    }

    /* Log */
    #log {
      background: #111;
      border-radius: 8px;
      padding: 14px;
      font-family: 'SF Mono', 'Consolas', monospace;
      font-size: 12px;
      color: #4ade80;
      height: 160px;
      overflow-y: auto;
      line-height: 1.6;
    }

    .log-entry { margin-bottom: 2px; }
    .log-entry.error { color: #f87171; }
    .log-entry.info  { color: #60a5fa; }
    .log-entry.warn  { color: #f59e0b; }
  </style>
</head>
<body>
<div class="card">
  <h1>Generator Control</h1>
  <p class="subtitle">Remote start system</p>

  <div class="status-row">
    <div class="dot unknown" id="status-dot"></div>
    <div>
      <div id="status-text">Unknown</div>
      <div id="status-detail">Waiting for controller...</div>
    </div>
  </div>

  <!-- Pending banner — hidden until a command is in flight -->
  <div id="pending-banner">
    <div class="spinner"></div>
    <span id="pending-text">Contacting controller...</span>
  </div>

  <div id="controls">
    <p class="section-label">Start with timer</p>
    <div class="preset-grid">
      <button class="btn-preset" onclick="sendTimedCommand(20)">
        <span class="mins">20</span>
        <span class="unit">min</span>
      </button>
      <button class="btn-preset" onclick="sendTimedCommand(60)">
        <span class="mins">60</span>
        <span class="unit">min</span>
      </button>
      <button class="btn-preset" onclick="sendTimedCommand(90)">
        <span class="mins">90</span>
        <span class="unit">min</span>
      </button>
    </div>

    <div class="custom-row">
      <div class="custom-input-wrap">
        <input type="number" id="custom-minutes" value="120" min="1" max="360">
        <span class="unit-label">min</span>
      </div>
      <button id="btn-custom" onclick="sendCustomCommand()">Custom start</button>
    </div>

    <p class="section-label">Stop</p>
    <button id="btn-stop" onclick="sendStop()">Stop generator</button>
  </div>

  <p class="section-label">Activity log</p>
  <div id="log"></div>
</div>

<script>
  var timerInterval  = null;
  var timerRemaining = 0;
  var isPending      = false;
  var MAX_MINUTES    = 360;

  // ── Pending state management ──────────────────────────────────

  function setPending(on, message) {
    isPending = on;
    var banner   = document.getElementById('pending-banner');
    var controls = document.getElementById('controls');
    var buttons  = controls.querySelectorAll('button');

    if (on) {
      banner.classList.add('visible');
      document.getElementById('pending-text').textContent =
        message || 'Contacting controller...';
      controls.classList.add('controls-disabled');
      buttons.forEach(function(b) { b.disabled = true; });
    } else {
      banner.classList.remove('visible');
      controls.classList.remove('controls-disabled');
      buttons.forEach(function(b) { b.disabled = false; });
    }
  }

  // ── Commands ──────────────────────────────────────────────────

  function sendTimedCommand(minutes) {
    doStart(minutes);
  }

  function sendCustomCommand() {
    var minutes = parseInt(document.getElementById('custom-minutes').value);
    if (isNaN(minutes) || minutes < 1) {
      addLog('Enter a valid number of minutes', 'error');
      return;
    }
    if (minutes > MAX_MINUTES) {
      addLog('Maximum run time is ' + MAX_MINUTES + ' minutes (6 hours)', 'warn');
      document.getElementById('custom-minutes').value = MAX_MINUTES;
      minutes = MAX_MINUTES;
    }
    doStart(minutes);
  }

  function doStart(minutes) {
    setPending(true, 'Starting generator...');
    addLog('Starting — ' + formatDuration(minutes * 60) + ' timer', 'info');
    fetch('/command?cmd=start&timer=' + minutes)
      .then(r => r.json())
      .then(data => {
        addLog(data.message);
        // Don't clear pending here — wait for poll to confirm
        // Poll faster while waiting for confirmation
        startFastPoll();
        startCountdown(minutes * 60);
      })
      .catch(() => {
        addLog('Network error — board unreachable', 'error');
        setPending(false);
      });
  }

  function sendStop() {
    setPending(true, 'Stopping generator...');
    addLog('Sending stop command', 'info');
    if (timerInterval) {
      clearInterval(timerInterval);
      timerInterval = null;
    }
    fetch('/command?cmd=stop')
      .then(r => r.json())
      .then(data => {
        addLog(data.message);
        startFastPoll();
      })
      .catch(() => {
        addLog('Network error — board unreachable', 'error');
        setPending(false);
      });
  }

  // ── Fast poll while pending ───────────────────────────────────
  // Polls every 2 seconds until pending clears, then drops back to 10s

  var fastPollInterval = null;
  var normalPollInterval = null;

  function startFastPoll() {
    if (fastPollInterval) return;
    if (normalPollInterval) {
      clearInterval(normalPollInterval);
      normalPollInterval = null;
    }
    fastPollInterval = setInterval(pollStatus, 2000);
  }

  function stopFastPoll() {
    if (fastPollInterval) {
      clearInterval(fastPollInterval);
      fastPollInterval = null;
    }
    if (!normalPollInterval) {
      normalPollInterval = setInterval(pollStatus, 10000);
    }
  }

  // ── Status polling ────────────────────────────────────────────

  function pollStatus() {
    fetch('/status')
      .then(r => r.json())
      .then(data => {
        updateStatus(data.status, data.detail);

        if (data.timerActive && data.timerRemaining > 0) {
          // Only restart countdown if we don't already have one running
          // or if the server's value differs significantly from ours
          if (!timerInterval || Math.abs(timerRemaining - data.timerRemaining) > 5) {
            startCountdown(data.timerRemaining);
          }
        } else if (!data.timerActive && timerInterval) {
          clearInterval(timerInterval);
          timerInterval = null;
        }

        if (data.pending) {
          // Board still processing — stay in pending state
          setPending(true, 'Waiting for controller...');
        } else {
          // Board is done — clear pending
          if (isPending) {
            setPending(false);
            stopFastPoll();
            if (data.status === 'error') {
              addLog('Error — controller did not respond', 'error');
            } else {
              addLog('Confirmed: ' + data.status, 'info');
            }
          }
        }
      })
      .catch(() => {});
  }

  // ── Status display ────────────────────────────────────────────

  function updateStatus(status, detail) {
    var dot  = document.getElementById('status-dot');
    var text = document.getElementById('status-text');
    var det  = document.getElementById('status-detail');

    dot.className = 'dot';
    if (status === 'running') {
      dot.classList.add('running');
      text.textContent = 'Running';
    } else if (status === 'stopped') {
      dot.classList.add('stopped');
      text.textContent = 'Stopped';
    } else if (status === 'error') {
      dot.classList.add('error');
      text.textContent = 'Error';
    } else if (status === 'pending') {
      dot.classList.add('pending');
      text.textContent = 'Pending';
    } else {
      dot.classList.add('unknown');
      text.textContent = 'Unknown';
    }

    if (detail) det.textContent = detail;
  }

  // ── Countdown ─────────────────────────────────────────────────

  function startCountdown(seconds) {
    if (timerInterval) clearInterval(timerInterval);
    timerRemaining = seconds;
    updateCountdownDisplay();
    timerInterval = setInterval(function() {
      timerRemaining--;
      updateCountdownDisplay();
      if (timerRemaining <= 0) {
        clearInterval(timerInterval);
        timerInterval = null;
        addLog('Timer expired — stop command sent', 'warn');
      }
    }, 1000);
  }

  function updateCountdownDisplay() {
    if (timerRemaining > 0) {
      document.getElementById('status-detail').textContent =
        'Auto-stop in ' + formatDuration(timerRemaining);
    }
  }

  function formatDuration(seconds) {
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    var s = seconds % 60;
    if (h > 0) {
      return h + 'h ' + m + 'm ' + (s < 10 ? '0' : '') + s + 's';
    }
    return m + 'm ' + (s < 10 ? '0' : '') + s + 's';
  }

  // ── Log ───────────────────────────────────────────────────────

  function addLog(msg, type) {
    var log = document.getElementById('log');
    var entry = document.createElement('div');
    var time = new Date().toLocaleTimeString();
    entry.className = 'log-entry' + (type ? ' ' + type : '');
    entry.textContent = '[' + time + '] ' + msg;
    log.appendChild(entry);
    log.scrollTop = log.scrollHeight;
  }

  // ── Init ──────────────────────────────────────────────────────

  normalPollInterval = setInterval(pollStatus, 10000);
  addLog('Interface ready', 'info');
</script>
</body>
</html>
)rawliteral";