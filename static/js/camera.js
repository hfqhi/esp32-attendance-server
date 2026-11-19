let kioskStream = null;
let enrollStream = null;
let scanInterval = null;
let scanCooldown = false;
let selectedCameraId = "";
let uploadedEnrollBase64 = null;
let esp32Info = { online: false, ip: null, stream_url: null };
let localVideoDevices = [];

const LCD_THEMES = {
  STANDBY: {
    box: "bg-slate-800/90 border-slate-700 text-slate-300",
    dot: "bg-slate-500",
    badge: "STANDBY",
  },
  NO_FACE: {
    box: "bg-cyan-950/50 border-cyan-500/40 text-cyan-200",
    dot: "bg-cyan-400 animate-pulse",
    badge: "SCANNING",
  },
  CHALLENGE: {
    box: "bg-amber-950/60 border-amber-500/60 text-amber-200 shadow-lg shadow-amber-500/10",
    dot: "bg-amber-400 animate-ping",
    badge: "VERIFYING",
  },
  LOGGED: {
    box: "bg-emerald-950/70 border-emerald-500/70 text-emerald-200 shadow-lg shadow-emerald-500/15",
    dot: "bg-emerald-400",
    badge: "SUCCESS",
  },
  ALREADY_LOGGED: {
    box: "bg-teal-950/60 border-teal-500/50 text-teal-200",
    dot: "bg-teal-400",
    badge: "LOGGED",
  },
  UNKNOWN: {
    box: "bg-rose-950/75 border-rose-500/70 text-rose-200 shadow-lg shadow-rose-500/15",
    dot: "bg-rose-500 animate-pulse",
    badge: "DENIED",
  },
  SPOOF: {
    box: "bg-rose-950/85 border-rose-500 text-rose-200 shadow-lg shadow-rose-500/20",
    dot: "bg-rose-500 animate-ping",
    badge: "ALERT",
  },
  NO_CLASS: {
    box: "bg-purple-950/60 border-purple-500/50 text-purple-200",
    dot: "bg-purple-400",
    badge: "OFF-HOURS",
  },
  ERROR: {
    box: "bg-rose-950/75 border-rose-500/60 text-rose-200",
    dot: "bg-rose-500",
    badge: "ERROR",
  },
};

function updateLcdDisplay(status, message) {
  const banner = document.getElementById("lcdBanner");
  const dot = document.getElementById("lcdDot");
  const tag = document.getElementById("lcdStatusTag");
  const text = document.getElementById("lcdMessageText");

  const theme = LCD_THEMES[status] || LCD_THEMES.STANDBY;
  banner.className = `lcd-banner mt-4 px-4 py-3.5 rounded-xl border font-mono text-sm flex items-center justify-between gap-3 ${theme.box}`;
  dot.className = `lcd-dot ${theme.dot}`;
  tag.textContent = theme.badge;
  text.textContent = message;
}

function updateEsp32CameraStatus(espData) {
  esp32Info = espData || { online: false, ip: null, stream_url: null };
  rebuildCameraDropdown();
}

function rebuildCameraDropdown() {
  const select = document.getElementById("cameraDeviceSelect");
  if (!select) return;

  const prev = selectedCameraId || select.value;
  let options = "";

  if (esp32Info.online) {
    options += `<option value="esp32">ESP32-CAM (${esp32Info.ip})</option>`;
  }

  localVideoDevices.forEach((d, idx) => {
    options += `<option value="${d.deviceId}">${d.label || `Laptop Camera ${idx + 1}`}</option>`;
  });

  if (!options) {
    options = `<option value="default">Default Camera</option>`;
  }

  select.innerHTML = options;
  if (prev && [...select.options].some((o) => o.value === prev)) {
    select.value = prev;
  } else if (esp32Info.online) {
    select.value = "esp32";
  }
  selectedCameraId = select.value;
  select.classList.remove("hidden");
}

async function initCameraPermissions() {
  const badge = document.getElementById("camPermissionBadge");
  const kioskBtn = document.getElementById("kioskGrantPermBtn");
  const enrollBtn = document.getElementById("enrollGrantPermBtn");
  const kioskText = document.getElementById("cameraStatusText");
  const enrollText = document.getElementById("enrollCamStatusText");

  if (
    !window.isSecureContext ||
    !navigator.mediaDevices ||
    !navigator.mediaDevices.getUserMedia
  ) {
    const banner = document.getElementById("insecureContextBanner");
    document.getElementById("currentOriginLabel").textContent =
      window.location.origin;
    const port = window.location.port || "5000";
    document.getElementById("switchLocalhostBtn").href =
      `http://localhost:${port}`;
    banner.classList.remove("hidden");

    badge.textContent = "Local Cam Blocked";
    badge.className =
      "px-2 py-0.5 rounded text-[11px] font-medium bg-rose-500/20 text-rose-300 border border-rose-500/40";
    kioskText.textContent =
      "Select ESP32-CAM or open via http://localhost:5000.";
    enrollText.textContent = "Select ESP32-CAM or open via localhost.";
    rebuildCameraDropdown();
    return;
  }

  if (navigator.permissions && navigator.permissions.query) {
    try {
      const permStatus = await navigator.permissions.query({ name: "camera" });
      applyPermissionState(permStatus.state);
      permStatus.onchange = () => applyPermissionState(permStatus.state);
      return;
    } catch (_) {}
  }

  badge.textContent = "Ready";
  badge.className =
    "px-2 py-0.5 rounded text-[11px] font-medium bg-slate-800 text-slate-300 border border-slate-700";
  kioskBtn.classList.remove("hidden");
  enrollBtn.classList.remove("hidden");
  rebuildCameraDropdown();
}

function applyPermissionState(state) {
  const badge = document.getElementById("camPermissionBadge");
  const kioskBtn = document.getElementById("kioskGrantPermBtn");
  const enrollBtn = document.getElementById("enrollGrantPermBtn");
  const kioskText = document.getElementById("cameraStatusText");
  const enrollText = document.getElementById("enrollCamStatusText");

  if (state === "granted") {
    badge.textContent = "Ready";
    badge.className =
      "px-2 py-0.5 rounded text-[11px] font-medium bg-emerald-500/15 text-emerald-300 border border-emerald-500/30";
    kioskBtn.classList.add("hidden");
    enrollBtn.classList.add("hidden");
    kioskText.textContent = "Camera Standby";
    enrollText.textContent = "Camera Preview";
    populateCameraDevices();
  } else if (state === "prompt") {
    badge.textContent = "Permission Needed";
    badge.className =
      "px-2 py-0.5 rounded text-[11px] font-medium bg-cyan-500/15 text-cyan-300 border border-cyan-500/30";
    kioskText.textContent = "Allow camera access or select ESP32-CAM.";
    enrollText.textContent = "Allow camera access or select ESP32-CAM.";
    kioskBtn.textContent = "Allow Camera Access";
    enrollBtn.textContent = "Allow Camera Access";
    kioskBtn.classList.remove("hidden");
    enrollBtn.classList.remove("hidden");
  } else if (state === "denied") {
    badge.textContent = "Blocked";
    badge.className =
      "px-2 py-0.5 rounded text-[11px] font-medium bg-rose-500/15 text-rose-300 border border-rose-500/30";
    kioskText.textContent = "Local camera blocked in browser bar.";
    enrollText.textContent = "Local camera blocked in browser settings.";
    kioskBtn.textContent = "Re-check Camera";
    enrollBtn.textContent = "Re-check Camera";
    kioskBtn.classList.remove("hidden");
    enrollBtn.classList.remove("hidden");
  }
}

async function populateCameraDevices() {
  if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices)
    return;
  const devices = await navigator.mediaDevices.enumerateDevices();
  localVideoDevices = devices.filter((d) => d.kind === "videoinput");
  rebuildCameraDropdown();
}

function onCameraDeviceChange() {
  selectedCameraId = document.getElementById("cameraDeviceSelect").value;
  if (scanInterval || kioskStream) {
    stopKioskStream();
    toggleScanner();
  }
}

async function requestCameraPermission(target) {
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) return;
  try {
    const tempStream = await navigator.mediaDevices.getUserMedia({
      video: true,
      audio: false,
    });
    tempStream.getTracks().forEach((t) => t.stop());
    applyPermissionState("granted");
    if (target === "kiosk") await toggleScanner();
    else if (target === "enroll") await startEnrollCamera();
  } catch (err) {
    if (
      err.name === "NotAllowedError" ||
      err.name === "PermissionDeniedError"
    ) {
      applyPermissionState("denied");
    }
  }
}

function getVideoConstraints() {
  return selectedCameraId &&
    selectedCameraId !== "default" &&
    selectedCameraId !== "esp32"
    ? { deviceId: { exact: selectedCameraId } }
    : true;
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
  const video = document.getElementById("kioskVideo");
  const espImg = document.getElementById("kioskEspImg");
  const placeholder = document.getElementById("cameraPlaceholder");
  const guide = document.getElementById("faceGuide");
  const btn = document.getElementById("toggleScanBtn");

  video.srcObject = null;
  video.classList.add("hidden");
  espImg.src = "";
  espImg.classList.add("hidden");
  guide.classList.add("hidden");
  placeholder.classList.remove("hidden");

  btn.textContent = "Start Scanner";
  btn.classList.remove("bg-rose-600");
  btn.classList.add("bg-emerald-600");
  updateLcdDisplay("STANDBY", "Standby — Press Start Scanner");
}

function stopEnrollStream() {
  if (enrollStream) {
    enrollStream.getTracks().forEach((t) => t.stop());
    enrollStream = null;
  }
  const video = document.getElementById("enrollVideo");
  const espImg = document.getElementById("enrollEspImg");
  const hint = document.getElementById("enrollCamHint");

  video.srcObject = null;
  video.classList.add("hidden");
  espImg.src = "";
  espImg.classList.add("hidden");
  hint.classList.remove("hidden");
}

async function captureCurrentFrameBase64(mode) {
  if (selectedCameraId === "esp32") {
    const resp = await fetch("/api/esp32/proxy_capture");
    if (!resp.ok) return "";
    const blob = await resp.blob();
    return await new Promise((resolve) => {
      const reader = new FileReader();
      reader.onloadend = () => resolve(reader.result);
      reader.readAsDataURL(blob);
    });
  }

  const video = document.getElementById(
    mode === "kiosk" ? "kioskVideo" : "enrollVideo",
  );
  const canvas = document.getElementById(
    mode === "kiosk" ? "kioskCanvas" : "enrollCanvas",
  );
  if (!video || video.videoWidth === 0) return "";

  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  canvas.getContext("2d").drawImage(video, 0, 0);
  return canvas.toDataURL("image/jpeg", 0.85);
}

async function toggleScanner() {
  if (scanInterval || kioskStream) {
    stopKioskStream();
    return;
  }

  stopEnrollStream();
  const btn = document.getElementById("toggleScanBtn");
  const video = document.getElementById("kioskVideo");
  const espImg = document.getElementById("kioskEspImg");
  const placeholder = document.getElementById("cameraPlaceholder");
  const guide = document.getElementById("faceGuide");

  if (selectedCameraId === "esp32") {
    if (!esp32Info.online) {
      document.getElementById("cameraStatusText").textContent =
        "ESP32-CAM is offline. Check Wi-Fi connection.";
      return;
    }
    espImg.src = esp32Info.stream_url;
    espImg.classList.remove("hidden");
    video.classList.add("hidden");
  } else {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
      document.getElementById("cameraStatusText").textContent =
        "Open http://localhost:5000 to access laptop camera.";
      return;
    }
    try {
      kioskStream = await navigator.mediaDevices.getUserMedia({
        video: getVideoConstraints(),
        audio: false,
      });
      applyPermissionState("granted");
      video.srcObject = kioskStream;
      await video.play();
      video.classList.remove("hidden");
      espImg.classList.add("hidden");
    } catch (err) {
      if (
        err.name === "NotAllowedError" ||
        err.name === "PermissionDeniedError"
      ) {
        applyPermissionState("denied");
      } else {
        document.getElementById("cameraStatusText").textContent =
          `Camera Error (${err.name}): ${err.message}`;
      }
      return;
    }
  }

  guide.classList.remove("hidden");
  placeholder.classList.add("hidden");
  btn.textContent = "Stop Scanner";
  btn.classList.remove("bg-emerald-600");
  btn.classList.add("bg-rose-600");
  updateLcdDisplay("NO_FACE", "Ready to Scan");

  scanInterval = setInterval(async () => {
    if (scanCooldown) return;
    const image = await captureCurrentFrameBase64("kiosk");
    if (!image) return;

    const res = await fetch("/api/verify_frame", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        image,
        override_subject: document.getElementById("overrideSubject").value,
      }),
    });
    const result = await res.json();
    updateLcdDisplay(result.status, result.message);

    if (result.status === "LOGGED" || result.status === "ALREADY_LOGGED") {
      loadDashboardData();
      scanCooldown = true;
      setTimeout(() => {
        scanCooldown = false;
        if (scanInterval) updateLcdDisplay("NO_FACE", "Ready to Scan");
      }, 3000);
    }
  }, 1200);
}

async function startEnrollCamera() {
  stopKioskStream();
  stopEnrollStream();
  uploadedEnrollBase64 = null;

  const video = document.getElementById("enrollVideo");
  const espImg = document.getElementById("enrollEspImg");
  const hint = document.getElementById("enrollCamHint");
  const feedback = document.getElementById("enrollFeedback");

  if (selectedCameraId === "esp32") {
    if (!esp32Info.online) {
      feedback.textContent = "ESP32-CAM is offline.";
      feedback.className = "text-xs text-center font-medium text-rose-400";
      return;
    }
    espImg.src = esp32Info.stream_url;
    espImg.classList.remove("hidden");
    hint.classList.add("hidden");
    feedback.textContent = "ESP32-CAM live preview active.";
    feedback.className = "text-xs text-center font-medium text-cyan-400";
    return;
  }

  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    document.getElementById("enrollCamStatusText").textContent =
      "Open http://localhost:5000 to access camera.";
    return;
  }

  try {
    enrollStream = await navigator.mediaDevices.getUserMedia({
      video: getVideoConstraints(),
      audio: false,
    });
    applyPermissionState("granted");
    video.srcObject = enrollStream;
    await video.play();
    video.classList.remove("hidden");
    hint.classList.add("hidden");
    feedback.textContent = "";
  } catch (err) {
    if (
      err.name === "NotAllowedError" ||
      err.name === "PermissionDeniedError"
    ) {
      applyPermissionState("denied");
    }
    feedback.textContent = `Camera Error (${err.name}): ${err.message}`;
    feedback.className = "text-xs text-center font-medium text-rose-400";
  }
}

function uploadStudentImage(event) {
  const file = event.target.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = (e) => {
    uploadedEnrollBase64 = e.target.result;
    const feedback = document.getElementById("enrollFeedback");
    feedback.textContent = "Photo attached. Click Save.";
    feedback.className = "text-xs text-center font-medium text-cyan-400";
  };
  reader.readAsDataURL(file);
}
