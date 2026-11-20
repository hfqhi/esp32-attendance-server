let kioskStream = null;
let enrollStream = null;
let scanInterval = null;
let uploadedEnrollBase64 = null;
let selectedCameraId = "";
let esp32Status = {
  online: false,
  ip: null,
  stream_url: null,
  capture_url: null,
};
let isVerifyingFrame = false;

const LCD_THEMES = {
  STANDBY: {
    box: "bg-slate-900 border-slate-800 text-slate-300",
    dot: "bg-slate-500",
    tag: "STANDBY",
  },
  NO_FACE: {
    box: "bg-sky-950/50 border-sky-700/60 text-sky-200",
    dot: "bg-sky-400 animate-pulse",
    tag: "SCANNING",
  },
  LOGGED: {
    box: "bg-emerald-950/70 border-emerald-500/70 text-emerald-200",
    dot: "bg-emerald-400",
    tag: "VERIFIED",
  },
  ALREADY_LOGGED: {
    box: "bg-teal-950/60 border-teal-500/60 text-teal-200",
    dot: "bg-teal-400",
    tag: "LOGGED",
  },
  UNKNOWN: {
    box: "bg-rose-950/70 border-rose-500/70 text-rose-200",
    dot: "bg-rose-400",
    tag: "UNREGISTERED",
  },
  SPOOF: {
    box: "bg-amber-950/70 border-amber-500/70 text-amber-200",
    dot: "bg-amber-400",
    tag: "BLOCKED",
  },
  NO_CLASS: {
    box: "bg-purple-950/60 border-purple-500/60 text-purple-200",
    dot: "bg-purple-400",
    tag: "OFF-HOURS",
  },
  ERROR: {
    box: "bg-rose-950/70 border-rose-500/70 text-rose-200",
    dot: "bg-rose-400",
    tag: "ERROR",
  },
};

function updateLcdBanner(statusKey, message) {
  const banner = document.getElementById("lcdBanner");
  const dot = document.getElementById("lcdDot");
  const msgEl = document.getElementById("lcdMessageText");
  const tagEl = document.getElementById("lcdStatusTag");
  if (!banner) return;

  const theme = LCD_THEMES[statusKey] || LCD_THEMES.STANDBY;
  banner.className = `lcd-banner mt-4 px-4 py-3.5 rounded-xl border font-mono text-sm flex items-center justify-between gap-3 ${theme.box}`;
  dot.className = `lcd-dot ${theme.dot}`;
  msgEl.textContent = message;
  tagEl.textContent = theme.tag;
}

function updateEsp32CameraStatus(statusObj) {
  const prevOnline = esp32Status.online;
  const prevIp = esp32Status.ip;
  esp32Status = statusObj || {
    online: false,
    ip: null,
    stream_url: null,
    capture_url: null,
  };

  if (prevOnline !== esp32Status.online || prevIp !== esp32Status.ip) {
    populateCameraList();
  }
}

async function initCameraPermissions() {
  const isLocal = ["localhost", "127.0.0.1", "::1"].includes(
    window.location.hostname,
  );
  const isSecure = window.isSecureContext || isLocal;

  if (!isSecure) {
    document.getElementById("insecureContextBanner").classList.remove("hidden");
    document.getElementById("currentOriginLabel").textContent =
      window.location.origin;
    document.getElementById("switchLocalhostBtn").href =
      `http://localhost:${window.location.port || 5000}`;
    setPermissionUI("blocked", "Localhost or ESP32-CAM required.");
    await populateCameraList();
    return;
  }

  if (navigator.permissions && navigator.permissions.query) {
    try {
      const status = await navigator.permissions.query({ name: "camera" });
      applyPermissionState(status.state);
      status.onchange = () => applyPermissionState(status.state);
      return;
    } catch (_) {
      // Fallback when Permissions API is unavailable
    }
  }

  await populateCameraList();
  setPermissionUI("prompt", "Ready to Scan");
}

function applyPermissionState(state) {
  if (state === "granted") {
    setPermissionUI("granted", "Camera Ready");
    populateCameraList();
  } else if (state === "denied") {
    setPermissionUI("denied", "Camera blocked in browser settings.");
    populateCameraList();
  } else {
    setPermissionUI("prompt", "Click Start Scanner to allow camera.");
    populateCameraList();
  }
}

function setPermissionUI(state, statusMsg) {
  const badge = document.getElementById("camPermissionBadge");
  const kioskBtn = document.getElementById("kioskGrantPermBtn");
  const enrollBtn = document.getElementById("enrollGrantPermBtn");

  document.getElementById("cameraStatusText").textContent = statusMsg;
  document.getElementById("enrollCamStatusText").textContent = statusMsg;

  if (state === "granted") {
    badge.textContent = "Ready";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-emerald-500/15 text-emerald-400 border border-emerald-500/30";
    kioskBtn.classList.add("hidden");
    enrollBtn.classList.add("hidden");
  } else if (state === "denied" || state === "blocked") {
    badge.textContent = "Blocked";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-rose-500/15 text-rose-400 border border-rose-500/30";
    kioskBtn.classList.toggle("hidden", state === "blocked");
    enrollBtn.classList.toggle("hidden", state === "blocked");
  } else {
    badge.textContent = "Standby";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-amber-500/15 text-amber-300 border border-amber-500/30";
    kioskBtn.classList.remove("hidden");
    enrollBtn.classList.remove("hidden");
  }
}

async function populateCameraList() {
  const select = document.getElementById("cameraDeviceSelect");
  const prevChoice = selectedCameraId || select.value;

  let videoInputs = [];
  if (navigator.mediaDevices && navigator.mediaDevices.enumerateDevices) {
    try {
      const devices = await navigator.mediaDevices.enumerateDevices();
      videoInputs = devices.filter((d) => d.kind === "videoinput");
    } catch (_) {
      videoInputs = [];
    }
  }

  select.innerHTML = "";

  if (esp32Status.online) {
    const opt = document.createElement("option");
    opt.value = "esp32";
    opt.textContent = `ESP32-CAM (${esp32Status.ip})`;
    select.appendChild(opt);
  }

  videoInputs.forEach((cam, idx) => {
    const opt = document.createElement("option");
    opt.value = cam.deviceId;
    opt.textContent = cam.label || `Webcam ${idx + 1}`;
    select.appendChild(opt);
  });

  if (select.options.length === 0) {
    const opt = document.createElement("option");
    opt.value = "";
    opt.textContent = "Default Webcam";
    select.appendChild(opt);
  }

  const stillExists = Array.from(select.options).some(
    (o) => o.value === prevChoice,
  );
  if (stillExists) {
    select.value = prevChoice;
    selectedCameraId = prevChoice;
  } else {
    selectedCameraId = select.options[0].value;
    select.value = selectedCameraId;
  }
}

function onCameraDeviceChange() {
  selectedCameraId = document.getElementById("cameraDeviceSelect").value;
  if (
    kioskStream ||
    !document.getElementById("kioskEspImg").classList.contains("hidden")
  ) {
    stopKioskStream();
    startKioskStream();
  }
  if (
    enrollStream ||
    !document.getElementById("enrollEspImg").classList.contains("hidden")
  ) {
    stopEnrollStream();
    startEnrollCamera();
  }
}

async function requestCameraPermission(target) {
  try {
    const probeStream = await navigator.mediaDevices.getUserMedia({
      video: true,
      audio: false,
    });
    probeStream.getTracks().forEach((t) => t.stop());
    setPermissionUI("granted", "Camera Ready");
    await populateCameraList();
    if (target === "kiosk") startKioskStream();
    if (target === "enroll") startEnrollCamera();
  } catch (err) {
    handleCameraError(err, target);
  }
}

function handleCameraError(err, target) {
  let msg = "Unable to access camera.";
  if (err.name === "NotAllowedError" || err.name === "PermissionDeniedError") {
    msg = "Permission denied. Allow camera in browser site settings.";
    setPermissionUI("denied", msg);
  } else if (
    err.name === "NotFoundError" ||
    err.name === "DevicesNotFoundError"
  ) {
    msg = "No webcam detected.";
    setPermissionUI("denied", msg);
  } else if (
    err.name === "NotReadableError" ||
    err.name === "TrackStartError"
  ) {
    msg = "Camera in use by another app.";
  }

  if (target === "kiosk") {
    updateLcdBanner("ERROR", msg);
  } else {
    const fb = document.getElementById("enrollFeedback");
    fb.textContent = msg;
    fb.className = "text-xs text-center font-medium text-rose-400";
  }
}

function toggleScanner() {
  const isRunning = Boolean(
    kioskStream ||
    !document.getElementById("kioskEspImg").classList.contains("hidden"),
  );
  if (isRunning) {
    stopKioskStream();
  } else {
    startKioskStream();
  }
}

async function startKioskStream() {
  const video = document.getElementById("kioskVideo");
  const espImg = document.getElementById("kioskEspImg");
  const placeholder = document.getElementById("cameraPlaceholder");
  const faceGuide = document.getElementById("faceGuide");
  const btn = document.getElementById("toggleScanBtn");

  if (selectedCameraId === "esp32") {
    if (!esp32Status.online || !esp32Status.stream_url) {
      updateLcdBanner("ERROR", "ESP32-CAM is offline.");
      return;
    }
    video.classList.add("hidden");
    espImg.src = `${esp32Status.stream_url}?t=${Date.now()}`;
    espImg.classList.remove("hidden");
    placeholder.classList.add("hidden");
    faceGuide.classList.remove("hidden");

    btn.textContent = "Stop Scanner";
    btn.className =
      "bg-rose-600 hover:bg-rose-500 text-white text-xs font-semibold px-4 py-1.5 rounded-lg shadow-sm transition";
    updateLcdBanner("NO_FACE", "ESP32-CAM Active — Center Face in Frame");
    scanInterval = setInterval(captureAndVerifyFrame, 450);
    return;
  }

  const videoConstraints = selectedCameraId
    ? {
        deviceId: { exact: selectedCameraId },
        width: { ideal: 640 },
        height: { ideal: 480 },
      }
    : { facingMode: "user", width: { ideal: 640 }, height: { ideal: 480 } };

  try {
    kioskStream = await navigator.mediaDevices.getUserMedia({
      video: videoConstraints,
      audio: false,
    });
    espImg.classList.add("hidden");
    espImg.src = "";
    video.srcObject = kioskStream;
    await video.play();

    video.classList.remove("hidden");
    placeholder.classList.add("hidden");
    faceGuide.classList.remove("hidden");
    setPermissionUI("granted", "Camera Ready");
    await populateCameraList();

    btn.textContent = "Stop Scanner";
    btn.className =
      "bg-rose-600 hover:bg-rose-500 text-white text-xs font-semibold px-4 py-1.5 rounded-lg shadow-sm transition";
    updateLcdBanner("NO_FACE", "Center Face in Frame");
    scanInterval = setInterval(captureAndVerifyFrame, 450);
  } catch (err) {
    handleCameraError(err, "kiosk");
  }
}

function stopKioskStream() {
  if (scanInterval) {
    clearInterval(scanInterval);
    scanInterval = null;
  }
  if (kioskStream) {
    kioskStream.getTracks().forEach((t) => t.stop());
    kioskStream = null;
  }
  const espImg = document.getElementById("kioskEspImg");
  espImg.src = "";
  espImg.classList.add("hidden");

  document.getElementById("kioskVideo").classList.add("hidden");
  document.getElementById("faceGuide").classList.add("hidden");
  document.getElementById("cameraPlaceholder").classList.remove("hidden");

  const btn = document.getElementById("toggleScanBtn");
  btn.textContent = "Start Scanner";
  btn.className =
    "bg-emerald-600 hover:bg-emerald-500 text-white text-xs font-semibold px-4 py-1.5 rounded-lg shadow-sm transition";
  updateLcdBanner("STANDBY", "Standby — Press Start Scanner");
}

async function captureCurrentFrameBase64(target) {
  const isEsp = selectedCameraId === "esp32";
  if (isEsp) {
    const res = await fetch(`/api/esp32/proxy_capture?t=${Date.now()}`);
    if (!res.ok) return "";
    const blob = await res.blob();
    return await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onloadend = () => resolve(reader.result);
      reader.readAsDataURL(blob);
    });
  }

  const video = document.getElementById(
    target === "kiosk" ? "kioskVideo" : "enrollVideo",
  );
  const canvas = document.getElementById(
    target === "kiosk" ? "kioskCanvas" : "enrollCanvas",
  );
  if (!video || video.readyState < 2) return "";

  canvas.width = video.videoWidth || 640;
  canvas.height = video.videoHeight || 480;
  canvas.getContext("2d").drawImage(video, 0, 0, canvas.width, canvas.height);
  return canvas.toDataURL("image/jpeg", 0.85);
}

async function captureAndVerifyFrame() {
  if (isVerifyingFrame) return;
  isVerifyingFrame = true;

  try {
    const isEsp = selectedCameraId === "esp32";
    const imageBase64 = await captureCurrentFrameBase64("kiosk");
    if (!imageBase64) return;

    const res = await fetch("/api/verify_frame", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        image: imageBase64,
        source: isEsp ? "esp32" : "webcam",
      }),
    });
    const data = await res.json();

    updateLcdBanner(data.status, data.message);

    if (data.status === "LOGGED") {
      loadDashboardData();
    }
  } catch (_) {
    // Ignore transient network hiccups during continuous loop
  } finally {
    isVerifyingFrame = false;
  }
}

async function startEnrollCamera() {
  uploadedEnrollBase64 = null;
  const video = document.getElementById("enrollVideo");
  const espImg = document.getElementById("enrollEspImg");
  const hint = document.getElementById("enrollCamHint");

  if (selectedCameraId === "esp32") {
    if (!esp32Status.online || !esp32Status.stream_url) {
      const fb = document.getElementById("enrollFeedback");
      fb.textContent = "ESP32-CAM is offline.";
      fb.className = "text-xs text-center font-medium text-rose-400";
      return;
    }
    video.classList.add("hidden");
    espImg.src = `${esp32Status.stream_url}?t=${Date.now()}`;
    espImg.classList.remove("hidden");
    hint.classList.add("hidden");
    return;
  }

  const videoConstraints = selectedCameraId
    ? {
        deviceId: { exact: selectedCameraId },
        width: { ideal: 640 },
        height: { ideal: 480 },
      }
    : { facingMode: "user", width: { ideal: 640 }, height: { ideal: 480 } };

  try {
    enrollStream = await navigator.mediaDevices.getUserMedia({
      video: videoConstraints,
      audio: false,
    });
    espImg.classList.add("hidden");
    espImg.src = "";
    video.srcObject = enrollStream;
    await video.play();
    video.classList.remove("hidden");
    hint.classList.add("hidden");
    setPermissionUI("granted", "Camera Ready");
    await populateCameraList();
  } catch (err) {
    handleCameraError(err, "enroll");
  }
}

function stopEnrollStream() {
  if (enrollStream) {
    enrollStream.getTracks().forEach((t) => t.stop());
    enrollStream = null;
  }
  const espImg = document.getElementById("enrollEspImg");
  espImg.src = "";
  espImg.classList.add("hidden");

  document.getElementById("enrollVideo").classList.add("hidden");
  document.getElementById("enrollCamHint").classList.remove("hidden");
}

function uploadStudentImage(event) {
  const file = event.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    uploadedEnrollBase64 = reader.result;
    const fb = document.getElementById("enrollFeedback");
    fb.textContent = `Selected: ${file.name}`;
    fb.className = "text-xs text-center font-medium text-sky-400";
  };
  reader.readAsDataURL(file);
}
