let kioskStream = null;
let enrollStream = null;
let scanInterval = null;
let espStreamWatchdog = null;
let isScanning = false;
let isVerifying = false;
let uploadedEnrollBase64 = null;

let videoDevices = [];
let selectedCameraId = "";
let esp32Info = {
  online: false,
  ip: null,
  ssid: "",
  server_url: "",
  stream_url: null,
  capture_url: null,
};
let camPermissionState = "prompt";

function isLocalhostOrSecure() {
  const host = window.location.hostname;
  return window.isSecureContext || host === "localhost" || host === "127.0.0.1";
}

function updateEsp32CameraStatus(espData) {
  const wasOnline = esp32Info.online;
  esp32Info = espData || {
    online: false,
    ip: null,
    ssid: "",
    server_url: "",
    stream_url: null,
    capture_url: null,
  };

  if (
    !wasOnline &&
    esp32Info.online &&
    (!selectedCameraId || videoDevices.length === 0)
  ) {
    selectedCameraId = "esp32";
  }

  renderCameraOptions();
  updatePermissionBadge();
}

function renderCameraOptions() {
  const select = document.getElementById("cameraDeviceSelect");
  if (!select) return;

  const prev = selectedCameraId;
  select.innerHTML = "";

  const espOpt = document.createElement("option");
  espOpt.value = "esp32";
  espOpt.textContent = esp32Info.online
    ? `ESP32-CAM (${esp32Info.ip})`
    : "ESP32-CAM (Offline)";
  select.appendChild(espOpt);

  if (!isLocalhostOrSecure()) {
    const phoneOpt = document.createElement("option");
    phoneOpt.value = "phone_snap";
    phoneOpt.textContent = "Phone Camera (Photo Capture)";
    select.appendChild(phoneOpt);
  }

  videoDevices.forEach((dev, idx) => {
    const opt = document.createElement("option");
    opt.value = dev.deviceId;
    opt.textContent = dev.label || `Device Camera ${idx + 1}`;
    select.appendChild(opt);
  });

  if (prev && [...select.options].some((o) => o.value === prev)) {
    select.value = prev;
    selectedCameraId = prev;
  } else if (esp32Info.online) {
    select.value = "esp32";
    selectedCameraId = "esp32";
  } else if (videoDevices.length > 0) {
    select.value = videoDevices[0].deviceId;
    selectedCameraId = videoDevices[0].deviceId;
  } else if (!isLocalhostOrSecure()) {
    select.value = "phone_snap";
    selectedCameraId = "phone_snap";
  } else {
    select.value = "esp32";
    selectedCameraId = "esp32";
  }

  updatePhoneSnapState();
}

function updatePhoneSnapState() {
  const snapBtn = document.getElementById("phoneSnapTriggerBtn");
  const statusText = document.getElementById("cameraStatusText");
  if (!snapBtn || !statusText || isScanning) return;

  if (selectedCameraId === "phone_snap") {
    statusText.textContent = "Tap below to verify using your phone camera";
    snapBtn.classList.remove("hidden");
  } else {
    snapBtn.classList.add("hidden");
    statusText.textContent = "Camera Standby";
  }
}

async function refreshVideoDevices() {
  if (!navigator.mediaDevices || !navigator.mediaDevices.enumerateDevices)
    return;
  try {
    const devices = await navigator.mediaDevices.enumerateDevices();
    videoDevices = devices.filter((d) => d.kind === "videoinput");
    renderCameraOptions();
  } catch (err) {
    console.warn("Unable to enumerate cameras:", err);
  }
}

function updatePermissionBadge() {
  const badge = document.getElementById("camPermissionBadge");
  if (!badge) return;

  if (selectedCameraId === "esp32") {
    badge.textContent = esp32Info.online ? "Ready" : "Offline";
    badge.className = esp32Info.online
      ? "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-emerald-500/15 text-emerald-300 border border-emerald-500/30"
      : "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-slate-900 text-slate-400 border border-slate-800";
    return;
  }

  if (selectedCameraId === "phone_snap") {
    badge.textContent = "Phone Cam";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-indigo-500/15 text-indigo-300 border border-indigo-500/30";
    return;
  }

  if (camPermissionState === "granted") {
    badge.textContent = "Ready";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-emerald-500/15 text-emerald-300 border border-emerald-500/30";
  } else if (camPermissionState === "denied") {
    badge.textContent = "Blocked";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-rose-500/15 text-rose-300 border border-rose-500/30";
  } else {
    badge.textContent = "Standby";
    badge.className =
      "px-2.5 py-0.5 rounded-md text-[11px] font-medium bg-slate-900 text-slate-400 border border-slate-800";
  }
}

async function initCameraPermissions() {
  renderCameraOptions();

  if (navigator.permissions && navigator.permissions.query) {
    try {
      const status = await navigator.permissions.query({ name: "camera" });
      camPermissionState = status.state;
      status.onchange = () => {
        camPermissionState = status.state;
        updatePermissionBadge();
        if (status.state === "granted") refreshVideoDevices();
      };
    } catch (_) {}
  }

  await refreshVideoDevices();
  updatePermissionBadge();
}

async function requestCameraPermission(target) {
  if (!isLocalhostOrSecure()) {
    showCameraError(
      target,
      "Use 'Phone Camera' mode or start server with --https for live video.",
    );
    return false;
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    showCameraError(target, "Camera API unavailable.");
    return false;
  }

  try {
    const tempStream = await navigator.mediaDevices.getUserMedia({
      video: true,
      audio: false,
    });
    tempStream.getTracks().forEach((t) => t.stop());
    camPermissionState = "granted";
    await refreshVideoDevices();
    updatePermissionBadge();

    if (target === "kiosk" && !isScanning) toggleScanner();
    else if (target === "enroll") startEnrollCamera();
    return true;
  } catch (err) {
    camPermissionState = err.name === "NotAllowedError" ? "denied" : "prompt";
    updatePermissionBadge();
    showCameraError(target, "Camera access blocked in browser settings.");
    return false;
  }
}

function showCameraError(target, message) {
  if (target === "kiosk") {
    document.getElementById("cameraPlaceholder").classList.remove("hidden");
    document.getElementById("cameraStatusText").textContent = message;
    const btn = document.getElementById("kioskGrantPermBtn");
    if (isLocalhostOrSecure() && selectedCameraId !== "esp32")
      btn.classList.remove("hidden");
    else btn.classList.add("hidden");
  } else {
    document.getElementById("enrollCamHint").classList.remove("hidden");
    document.getElementById("enrollCamStatusText").textContent = message;
    const btn = document.getElementById("enrollGrantPermBtn");
    if (isLocalhostOrSecure() && selectedCameraId !== "esp32")
      btn.classList.remove("hidden");
    else btn.classList.add("hidden");
  }
}

async function onCameraDeviceChange() {
  selectedCameraId = document.getElementById("cameraDeviceSelect").value;
  renderCameraOptions();
  updatePermissionBadge();

  if (isScanning) {
    stopKioskStream();
    if (selectedCameraId !== "phone_snap") {
      await toggleScanner();
    }
  }
  if (
    enrollStream ||
    !document.getElementById("enrollEspImg").classList.contains("hidden")
  ) {
    await startEnrollCamera();
  }
}

function setLcdBanner(status, message) {
  const banner = document.getElementById("lcdBanner");
  const dot = document.getElementById("lcdDot");
  const msgEl = document.getElementById("lcdMessageText");
  const tagEl = document.getElementById("lcdStatusTag");

  const styles = {
    STANDBY: {
      box: "bg-slate-900 border-slate-800 text-slate-300",
      dot: "bg-slate-500",
      tag: "STANDBY",
    },
    NO_FACE: {
      box: "bg-sky-950/30 border-sky-700/50 text-sky-200",
      dot: "bg-sky-400 animate-pulse",
      tag: "SCANNING",
    },
    LOGGED: {
      box: "bg-emerald-950/50 border-emerald-500/70 text-emerald-200",
      dot: "bg-emerald-400",
      tag: "VERIFIED",
    },
    ALREADY_LOGGED: {
      box: "bg-amber-950/40 border-amber-500/60 text-amber-200",
      dot: "bg-amber-400",
      tag: "LOGGED",
    },
    UNKNOWN: {
      box: "bg-rose-950/50 border-rose-500/70 text-rose-200",
      dot: "bg-rose-500",
      tag: "UNKNOWN",
    },
    SPOOF: {
      box: "bg-rose-950/60 border-rose-500 text-rose-200",
      dot: "bg-rose-500 animate-ping",
      tag: "BLOCKED",
    },
    NO_CLASS: {
      box: "bg-purple-950/40 border-purple-500/60 text-purple-200",
      dot: "bg-purple-400",
      tag: "OFF-HOURS",
    },
  };

  const current = styles[status] || styles.STANDBY;
  banner.className = `lcd-banner mt-3.5 sm:mt-4 px-3.5 sm:px-4 py-3 rounded-xl border font-mono text-xs sm:text-sm flex items-center justify-between gap-2.5 ${current.box}`;
  dot.className = `lcd-dot shrink-0 ${current.dot}`;
  msgEl.textContent = message;
  tagEl.textContent = current.tag;
}

// --- KIOSK LIVE SCANNER ---
async function startKioskStream() {
  const video = document.getElementById("kioskVideo");
  const espImg = document.getElementById("kioskEspImg");
  const placeholder = document.getElementById("cameraPlaceholder");
  const guide = document.getElementById("faceGuide");

  if (selectedCameraId === "phone_snap") {
    document.getElementById("phoneSnapVerifyInput").click();
    return false;
  }

  if (selectedCameraId === "esp32") {
    if (!esp32Info.online || !esp32Info.stream_url) {
      showCameraError(
        "kiosk",
        "ESP32-CAM offline. Tap Wi-Fi icon to link or configure.",
      );
      return false;
    }
    video.classList.add("hidden");
    espImg.src = `${esp32Info.stream_url}?t=${Date.now()}`;
    espImg.classList.remove("hidden");
    placeholder.classList.add("hidden");
    guide.classList.remove("hidden");

    clearInterval(espStreamWatchdog);
    espStreamWatchdog = setInterval(() => {
      if (
        isScanning &&
        selectedCameraId === "esp32" &&
        esp32Info.online &&
        esp32Info.stream_url
      ) {
        if (espImg.naturalWidth === 0) {
          espImg.src = `${esp32Info.stream_url}?t=${Date.now()}`;
        }
      }
    }, 3000);
    return true;
  }

  if (!isLocalhostOrSecure()) {
    showCameraError("kiosk", "Select Phone Camera or run server with --https.");
    return false;
  }

  try {
    const constraints = selectedCameraId
      ? {
          video: {
            deviceId: { exact: selectedCameraId },
            width: 640,
            height: 480,
          },
        }
      : { video: { width: 640, height: 480 } };

    kioskStream = await navigator.mediaDevices.getUserMedia(constraints);
    camPermissionState = "granted";
    await refreshVideoDevices();
    updatePermissionBadge();

    espImg.classList.add("hidden");
    espImg.src = "";
    video.srcObject = kioskStream;
    video.classList.remove("hidden");
    placeholder.classList.add("hidden");
    guide.classList.remove("hidden");
    return true;
  } catch (err) {
    showCameraError("kiosk", "Unable to start device webcam.");
    return false;
  }
}

function stopKioskStream() {
  clearInterval(scanInterval);
  clearInterval(espStreamWatchdog);
  scanInterval = null;
  espStreamWatchdog = null;
  isScanning = false;

  if (kioskStream) {
    kioskStream.getTracks().forEach((track) => track.stop());
    kioskStream = null;
  }

  const video = document.getElementById("kioskVideo");
  const espImg = document.getElementById("kioskEspImg");
  video.srcObject = null;
  video.classList.add("hidden");
  espImg.src = "";
  espImg.classList.add("hidden");

  document.getElementById("faceGuide").classList.add("hidden");
  document.getElementById("cameraPlaceholder").classList.remove("hidden");
  document.getElementById("kioskGrantPermBtn").classList.add("hidden");
  updatePhoneSnapState();

  const btn = document.getElementById("toggleScanBtn");
  btn.textContent = "Start";
  btn.className =
    "col-span-3 sm:w-auto bg-emerald-600 hover:bg-emerald-500 text-white text-xs font-semibold px-3 sm:px-4 py-2 sm:py-1.5 rounded-lg shadow-sm transition text-center whitespace-nowrap";
  setLcdBanner("STANDBY", "Standby — Press Start");
}

async function toggleScanner() {
  if (isScanning) {
    stopKioskStream();
    return;
  }

  const started = await startKioskStream();
  if (!started) return;

  isScanning = true;
  const btn = document.getElementById("toggleScanBtn");
  btn.textContent = "Stop";
  btn.className =
    "col-span-3 sm:w-auto bg-rose-600 hover:bg-rose-500 text-white text-xs font-semibold px-3 sm:px-4 py-2 sm:py-1.5 rounded-lg shadow-sm transition text-center whitespace-nowrap";
  setLcdBanner("NO_FACE", "Ready to Scan");

  scanInterval = setInterval(captureAndVerifyFrame, 650);
}

async function verifyPhoneSnapPhoto(event) {
  const file = event.target.files[0];
  if (!file) return;

  setLcdBanner("NO_FACE", "Analyzing phone photo...");
  const reader = new FileReader();
  reader.onload = async () => {
    try {
      const res = await fetch("/api/verify_frame", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          image: reader.result,
          source: "webcam",
        }),
      });
      const data = await res.json();
      if (data.status && data.message) {
        setLcdBanner(data.status, data.message);
      }
      if (data.status === "LOGGED") {
        loadDashboardData();
      }
    } catch (err) {
      setLcdBanner("UNKNOWN", "Verification failed.");
    } finally {
      event.target.value = "";
    }
  };
  reader.readAsDataURL(file);
}

async function captureCurrentFrameBase64(target) {
  const isKiosk = target === "kiosk";
  const canvas = document.getElementById(
    isKiosk ? "kioskCanvas" : "enrollCanvas",
  );
  const ctx = canvas.getContext("2d");

  if (selectedCameraId === "esp32") {
    try {
      const resp = await fetch(`/api/esp32/proxy_capture?t=${Date.now()}`);
      if (!resp.ok) return null;
      const blob = await resp.blob();
      return await new Promise((resolve) => {
        const reader = new FileReader();
        reader.onloadend = () => resolve(reader.result);
        reader.readAsDataURL(blob);
      });
    } catch (_) {
      return null;
    }
  }

  const video = document.getElementById(isKiosk ? "kioskVideo" : "enrollVideo");
  if (!video || !video.videoWidth) return null;
  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  return canvas.toDataURL("image/jpeg", 0.9);
}

async function captureAndVerifyFrame() {
  if (!isScanning || isVerifying) return;
  isVerifying = true;

  try {
    const dataUrl = await captureCurrentFrameBase64("kiosk");
    if (!dataUrl) return;

    const cameraSource = selectedCameraId === "esp32" ? "esp32" : "webcam";

    const res = await fetch("/api/verify_frame", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        image: dataUrl,
        source: cameraSource,
      }),
    });
    const data = await res.json();

    if (data.status && data.message) {
      setLcdBanner(data.status, data.message);
    }

    if (data.status === "LOGGED") {
      loadDashboardData();
    }
  } catch (err) {
    console.error("Verification error:", err);
  } finally {
    isVerifying = false;
  }
}

// --- ENROLLMENT CAMERA & PHOTO UPLOAD ---
async function startEnrollCamera() {
  stopEnrollStream();
  uploadedEnrollBase64 = null;
  const video = document.getElementById("enrollVideo");
  const espImg = document.getElementById("enrollEspImg");
  const hint = document.getElementById("enrollCamHint");

  if (selectedCameraId === "esp32") {
    if (!esp32Info.online || !esp32Info.stream_url) {
      showCameraError(
        "enroll",
        "ESP32-CAM offline. Tap Phone Camera or upload photo.",
      );
      return;
    }
    video.classList.add("hidden");
    espImg.src = `${esp32Info.stream_url}?t=${Date.now()}`;
    espImg.classList.remove("hidden");
    hint.classList.add("hidden");
    return;
  }

  if (!isLocalhostOrSecure()) {
    document.getElementById("enrollCameraCaptureInput").click();
    return;
  }

  try {
    const constraints =
      selectedCameraId && selectedCameraId !== "phone_snap"
        ? {
            video: {
              deviceId: { exact: selectedCameraId },
              width: 640,
              height: 480,
            },
          }
        : { video: true };
    enrollStream = await navigator.mediaDevices.getUserMedia(constraints);
    camPermissionState = "granted";
    await refreshVideoDevices();
    updatePermissionBadge();

    espImg.classList.add("hidden");
    espImg.src = "";
    video.srcObject = enrollStream;
    video.classList.remove("hidden");
    hint.classList.add("hidden");
  } catch (err) {
    showCameraError(
      "enroll",
      "Unable to open webcam. Use Phone Camera button.",
    );
  }
}

function stopEnrollStream() {
  if (enrollStream) {
    enrollStream.getTracks().forEach((track) => track.stop());
    enrollStream = null;
  }
  const video = document.getElementById("enrollVideo");
  const espImg = document.getElementById("enrollEspImg");
  const photoPreview = document.getElementById("enrollPhotoPreview");
  if (video) {
    video.srcObject = null;
    video.classList.add("hidden");
  }
  if (espImg) {
    espImg.src = "";
    espImg.classList.add("hidden");
  }
  if (photoPreview) {
    photoPreview.src = "";
    photoPreview.classList.add("hidden");
  }
  const hint = document.getElementById("enrollCamHint");
  if (hint) {
    hint.classList.remove("hidden");
    document.getElementById("enrollCamStatusText").textContent =
      "Camera or Photo Preview";
    document.getElementById("enrollGrantPermBtn").classList.add("hidden");
  }
}

function uploadStudentImage(event) {
  const file = event.target.files[0];
  if (!file) return;
  stopEnrollStream();
  const reader = new FileReader();
  reader.onload = () => {
    uploadedEnrollBase64 = reader.result;
    const preview = document.getElementById("enrollPhotoPreview");
    const hint = document.getElementById("enrollCamHint");
    if (preview && hint) {
      preview.src = uploadedEnrollBase64;
      preview.classList.remove("hidden");
      hint.classList.add("hidden");
    }
    const feedback = document.getElementById("enrollFeedback");
    feedback.textContent = `Loaded: ${file.name}`;
    feedback.className = "text-xs text-center font-medium text-sky-400";
  };
  reader.readAsDataURL(file);
}
