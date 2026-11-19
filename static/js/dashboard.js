let cachedLogs = [];
let cachedSpoofs = [];
let cachedStudents = [];
let cachedSchedules = [];

let editingStudentOriginalId = null;
let editingScheduleId = null;
let selectedScheduleDayFilter = "ALL";

let logViewMode = "table";
let calYear = new Date().getFullYear();
let calMonth = new Date().getMonth();

const DAYS_LIST = [
  "Monday",
  "Tuesday",
  "Wednesday",
  "Thursday",
  "Friday",
  "Saturday",
  "Sunday",
];
const DAY_ORDER = {
  Monday: 1,
  Tuesday: 2,
  Wednesday: 3,
  Thursday: 4,
  Friday: 5,
  Saturday: 6,
  Sunday: 7,
};

const ICONS = {
  calendar: `<svg class="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="1.8"><path stroke-linecap="round" stroke-linejoin="round" d="M8 7V3m8 4V3m-9 8h10M5 21h14a2 2 0 002-2V7a2 2 0 00-2-2H5a2 2 0 00-2 2v12a2 2 0 002 2z"/></svg>`,
  edit: `<svg class="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="1.8"><path stroke-linecap="round" stroke-linejoin="round" d="M16.862 4.487l1.687-1.688a1.875 1.875 0 112.652 2.652L10.582 16.07a4.5 4.5 0 01-1.897 1.13L6 18l.8-2.685a4.5 4.5 0 011.13-1.897l8.932-8.931zm0 0L19.5 7.125"/></svg>`,
  trash: `<svg class="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="1.8"><path stroke-linecap="round" stroke-linejoin="round" d="M14.74 9l-.346 9m-4.788 0L9.26 9m9.968-3.21c.342.052.682.107 1.022.166m-1.022-.165L18.16 19.673a2.25 2.25 0 01-2.244 2.077H8.084a2.25 2.25 0 01-2.244-2.077L4.772 5.79m14.456 0a48.108 48.108 0 00-3.478-.397m-12 .562c.34-.059.68-.114 1.022-.165m0 0a48.11 48.11 0 013.478-.397m7.5 0v-.916c0-1.18-.91-2.164-2.09-2.201a51.964 51.964 0 00-3.32 0c-1.18.037-2.09 1.022-2.09 2.201v.916m7.5 0a48.667 48.667 0 00-7.5 0"/></svg>`,
  photo: `<svg class="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="1.8"><path stroke-linecap="round" stroke-linejoin="round" d="M2.25 15.75l5.159-5.159a2.25 2.25 0 013.182 0l5.159 5.159m-1.5-1.5l1.409-1.409a2.25 2.25 0 013.182 0l2.909 2.909m-18 3.75h16.5a1.5 1.5 0 001.5-1.5V6a1.5 1.5 0 00-1.5-1.5H3.75A1.5 1.5 0 002.25 6v12a1.5 1.5 0 001.5 1.5zm10.5-11.25h.008v.008h-.008V8.25zm.375 0a.375.375 0 11-.75 0 .375.375 0 01.75 0z"/></svg>`,
};

const DAY_ACCENTS = {
  Monday: {
    header: "bg-sky-950/50 border-sky-800/60",
    title: "text-sky-300",
    badge: "bg-sky-500/15 text-sky-300 border-sky-500/30",
  },
  Tuesday: {
    header: "bg-violet-950/50 border-violet-800/60",
    title: "text-violet-300",
    badge: "bg-violet-500/15 text-violet-300 border-violet-500/30",
  },
  Wednesday: {
    header: "bg-cyan-950/50 border-cyan-800/60",
    title: "text-cyan-300",
    badge: "bg-cyan-500/15 text-cyan-300 border-cyan-500/30",
  },
  Thursday: {
    header: "bg-amber-950/45 border-amber-800/60",
    title: "text-amber-300",
    badge: "bg-amber-500/15 text-amber-300 border-amber-500/30",
  },
  Friday: {
    header: "bg-rose-950/45 border-rose-800/60",
    title: "text-rose-300",
    badge: "bg-rose-500/15 text-rose-300 border-rose-500/30",
  },
  Saturday: {
    header: "bg-indigo-950/50 border-indigo-800/60",
    title: "text-indigo-300",
    badge: "bg-indigo-500/15 text-indigo-300 border-indigo-500/30",
  },
  Sunday: {
    header: "bg-slate-800/70 border-slate-700",
    title: "text-slate-200",
    badge: "bg-slate-700/60 text-slate-300 border-slate-600",
  },
};

document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    closeManageModal();
    closeProofModal();
  }
});

function openManageModal() {
  document.getElementById("manageModal").classList.remove("hidden");
}

function closeManageModal() {
  document.getElementById("manageModal").classList.add("hidden");
}

function openProofModal(relPath, title, meta) {
  if (!relPath) return;
  document.getElementById("proofModalImg").src = `/proofs/${relPath}`;
  document.getElementById("proofModalTitle").textContent = title;
  document.getElementById("proofModalMeta").textContent = meta;
  document.getElementById("proofModal").classList.remove("hidden");
}

function closeProofModal() {
  document.getElementById("proofModal").classList.add("hidden");
  document.getElementById("proofModalImg").src = "";
}

function getTodayDateString() {
  return new Date().toLocaleDateString("en-CA");
}

function getYesterdayDateString() {
  const d = new Date();
  d.setDate(d.getDate() - 1);
  return d.toLocaleDateString("en-CA");
}

function formatHumanDate(yyyyMmDd) {
  const [y, m, d] = yyyyMmDd.split("-").map(Number);
  const dateObj = new Date(y, m - 1, d);
  const formatted = dateObj.toLocaleDateString("en-US", {
    weekday: "short",
    month: "short",
    day: "numeric",
    year: "numeric",
  });
  return yyyyMmDd === getTodayDateString() ? `Today • ${formatted}` : formatted;
}

setInterval(() => {
  const now = new Date();
  const dayName = now.toLocaleDateString("en-US", { weekday: "short" });
  document.getElementById("liveClock").textContent =
    `${dayName} • ${now.toLocaleTimeString()}`;
}, 1000);

setInterval(loadDashboardData, 8000);

function format12Hour(time24) {
  const [hStr, mStr] = time24.split(":");
  let h = parseInt(hStr, 10);
  const ampm = h >= 12 ? "PM" : "AM";
  h = h % 12 || 12;
  return `${h}:${mStr} ${ampm}`;
}

function switchTab(tab) {
  if (tab !== "kiosk") stopKioskStream();
  if (tab !== "enroll") stopEnrollStream();

  ["kiosk", "logs", "enroll", "schedules"].forEach((t) => {
    document.getElementById(`view-${t}`).classList.toggle("hidden", t !== tab);
    const btn = document.getElementById(`tab-${t}`);
    btn.classList.toggle("border-sky-400", t === tab);
    btn.classList.toggle("text-sky-400", t === tab);
    btn.classList.toggle("border-transparent", t !== tab);
    btn.classList.toggle("text-slate-400", t !== tab);
  });
  loadDashboardData();
}

// --- RECENT SCANS & SPOOF ALERTS ---
function onRecentScansScopeChange() {
  const scope = document.getElementById("recentScansScope").value;
  const dateInput = document.getElementById("recentScansDate");
  if (scope === "custom") {
    if (!dateInput.value) dateInput.value = getTodayDateString();
    dateInput.classList.remove("hidden");
  } else {
    dateInput.classList.add("hidden");
  }
  renderRecentScans();
}

function renderRecentScans() {
  const scopeEl = document.getElementById("recentScansScope");
  const dateInput = document.getElementById("recentScansDate");
  const countBadge = document.getElementById("recentScansCountBadge");
  const recentFeed = document.getElementById("recentFeed");
  if (!scopeEl || !recentFeed) return;

  const scope = scopeEl.value;
  let targetDate = "";
  if (scope === "today") targetDate = getTodayDateString();
  else if (scope === "yesterday") targetDate = getYesterdayDateString();
  else if (scope === "custom") targetDate = dateInput.value;

  const filtered = targetDate
    ? cachedLogs.filter((l) => l.date_str === targetDate)
    : cachedLogs;

  countBadge.textContent = filtered.length;

  recentFeed.innerHTML =
    filtered
      .slice(0, 15)
      .map((l) => {
        const subjBadge = l.subject_code
          ? `<span class="font-mono text-[11px] font-semibold text-indigo-300">${l.subject_code}</span>`
          : `<span class="text-xs text-slate-300">${l.subject}</span>`;
        const datePrefix =
          scope === "all"
            ? `<span class="text-slate-500">${l.date_str} • </span>`
            : "";
        const proofBtn = l.proof_image
          ? `<button type="button" onclick="openProofModal('${l.proof_image}', '${l.full_name.replace(/'/g, "\\'")}', '${l.subject_code || l.subject} • ${l.date_str} ${l.time_str}')" title="View Capture Proof" class="p-1 rounded text-slate-400 hover:text-sky-300 hover:bg-slate-700/60 transition">${ICONS.photo}</button>`
          : "";

        return `
      <div class="p-3 rounded-lg bg-slate-800/70 border border-slate-700/60 flex justify-between items-center gap-2">
        <div class="min-w-0">
          <p class="font-semibold text-slate-100 truncate">${l.full_name}</p>
          <p class="text-xs text-slate-400 truncate">${subjBadge} • ${datePrefix}${l.time_str}</p>
        </div>
        <div class="flex items-center gap-1.5 shrink-0">
          ${proofBtn}
          <span class="px-2 py-0.5 rounded text-xs font-bold ${badgeClass(l.status)}">${l.status}</span>
        </div>
      </div>
    `;
      })
      .join("") ||
    `<p class="text-slate-500 text-xs py-6 text-center">${targetDate ? `No scans recorded on ${formatHumanDate(targetDate)}.` : "No scans recorded yet."}</p>`;
}

function renderSpoofAlerts() {
  const panel = document.getElementById("spoofAlertPanel");
  const list = document.getElementById("spoofAlertsList");
  if (!panel || !list) return;

  if (!cachedSpoofs || cachedSpoofs.length === 0) {
    panel.classList.add("hidden");
    return;
  }

  panel.classList.remove("hidden");
  list.innerHTML = cachedSpoofs
    .map(
      (sp) => `
    <div class="p-2.5 rounded-lg bg-slate-900/90 border border-rose-500/30 flex items-center justify-between gap-2">
      <div class="min-w-0">
        <p class="font-semibold text-rose-200 truncate">${sp.suspected_name}</p>
        <p class="text-[11px] text-slate-400 truncate">${sp.source} • ${sp.date_str} ${sp.time_str}</p>
      </div>
      <button type="button" onclick="openProofModal('${sp.proof_image}', 'Spoof Attempt: ${sp.suspected_name.replace(/'/g, "\\'")}', '${sp.source} • Texture ${sp.texture_score} • ${sp.date_str} ${sp.time_str}')" class="shrink-0 px-2 py-1 rounded bg-rose-500/20 hover:bg-rose-500/30 text-rose-300 font-semibold text-[11px] transition">
        Inspect
      </button>
    </div>
  `,
    )
    .join("");
}

async function clearSpoofAlerts() {
  await fetch("/api/spoofs/clear", { method: "DELETE" });
  loadDashboardData();
}

async function loadDashboardData() {
  const res = await fetch("/api/dashboard_data");
  const data = await res.json();

  cachedLogs = data.logs;
  cachedSpoofs = data.spoofs || [];
  cachedStudents = data.students;
  cachedSchedules = [...data.schedules].sort((a, b) => {
    const dDiff =
      (DAY_ORDER[a.day_of_week] || 9) - (DAY_ORDER[b.day_of_week] || 9);
    return dDiff !== 0 ? dDiff : a.start_time.localeCompare(b.start_time);
  });

  document.getElementById("activeClassBadge").textContent = data.active_subject;
  if (typeof updateEsp32CameraStatus === "function") {
    updateEsp32CameraStatus(data.esp32);
  }

  const banner = document.getElementById("pendingAbsenceBanner");
  if (data.pending_absences && data.pending_absences.length > 0) {
    const first = data.pending_absences[0];
    const codeTag = first.subject_code || first.subject;
    document.getElementById("pendingAbsenceText").textContent =
      `${codeTag} ended — ${first.missing_count} unlogged student(s).`;
    document.getElementById("pendingAbsenceBtn").onclick = () =>
      quickMarkAbsent(first.subject);
    banner.classList.remove("hidden");
  } else {
    banner.classList.add("hidden");
  }

  const overrideSel = document.getElementById("overrideSubject");
  const absenceSel = document.getElementById("absenceSubjectSelect");
  const filterSubSel = document.getElementById("filterSubject");
  const filterStuSel = document.getElementById("filterStudent");

  const prevOverride = overrideSel.value;
  const prevFilterSub = filterSubSel.value;
  const prevFilterStu = filterStuSel.value;

  overrideSel.innerHTML = `<option value="">Auto Schedule</option>`;
  absenceSel.innerHTML = "";
  filterSubSel.innerHTML = `<option value="">All Subjects</option>`;
  filterStuSel.innerHTML = `<option value="">All Students</option>`;

  const uniqueSubjects = new Set();
  cachedSchedules.forEach((s) => {
    if (!uniqueSubjects.has(s.subject)) {
      uniqueSubjects.add(s.subject);
      const optLabel = s.subject_code
        ? `${s.subject_code} • ${s.subject}`
        : s.subject;
      overrideSel.innerHTML += `<option value="${s.subject}">${optLabel}</option>`;
      absenceSel.innerHTML += `<option value="${s.subject}">${optLabel}</option>`;
      filterSubSel.innerHTML += `<option value="${s.subject}">${optLabel}</option>`;
    }
  });

  cachedStudents.forEach((st) => {
    filterStuSel.innerHTML += `<option value="${st.student_id}">${st.full_name}</option>`;
  });

  overrideSel.value = prevOverride;
  filterSubSel.value = prevFilterSub;
  filterStuSel.value = prevFilterStu;

  renderRecentScans();
  renderSpoofAlerts();
  refreshLogsDisplay();
  renderStudentsTable();
  renderDayGroupedSchedules();
}

function renderStudentsTable() {
  document.getElementById("studentsTableBody").innerHTML =
    cachedStudents
      .map((s) => {
        const isEditing = editingStudentOriginalId === s.student_id;
        const rowBg = isEditing ? "bg-sky-500/10" : "hover:bg-slate-800/40";
        const edgeAccent = isEditing ? "shadow-[inset_3px_0_0_0_#38bdf8]" : "";
        const editBtnStyle = isEditing
          ? "bg-sky-500/20 text-sky-300 ring-1 ring-sky-400/50"
          : "text-slate-400 hover:text-sky-300 hover:bg-slate-800";

        return `
      <tr class="${rowBg} transition">
        <td class="py-3 px-4 font-mono text-xs text-sky-300 ${edgeAccent}">${s.student_id}</td>
        <td class="py-3 px-4 font-medium text-slate-100">${s.full_name}</td>
        <td class="py-3 px-4"><span class="px-2 py-0.5 rounded text-xs font-semibold bg-indigo-500/15 text-indigo-300 border border-indigo-500/30">${s.section}</span></td>
        <td class="py-3 px-4 text-xs text-slate-400">${s.created_at}</td>
        <td class="py-3 px-4 text-right">
          <div class="inline-flex items-center justify-end gap-1">
            <button type="button" onclick="openStudentCalendar('${s.student_id}')" title="Attendance Calendar" aria-label="Attendance Calendar" class="p-1.5 rounded-lg text-slate-400 hover:text-indigo-300 hover:bg-slate-800 transition">
              ${ICONS.calendar}
            </button>
            <button type="button" onclick="toggleEditStudent('${s.student_id}')" title="${isEditing ? "Cancel Edit" : "Edit Student"}" aria-label="${isEditing ? "Cancel Edit" : "Edit Student"}" class="p-1.5 rounded-lg transition ${editBtnStyle}">
              ${ICONS.edit}
            </button>
            <button type="button" onclick="deleteStudent('${s.student_id}')" title="Remove Student" aria-label="Remove Student" class="p-1.5 rounded-lg text-slate-400 hover:text-rose-400 hover:bg-slate-800 transition">
              ${ICONS.trash}
            </button>
          </div>
        </td>
      </tr>
    `;
      })
      .join("") ||
    `<tr><td colspan="5" class="py-4 px-4 text-center text-xs text-slate-500">No enrolled students.</td></tr>`;
}

// --- DAY-GROUPED SCHEDULES ---
function setScheduleDayFilter(day) {
  selectedScheduleDayFilter = day;
  const sel = document.getElementById("scheduleDayFilter");
  if (sel) sel.value = day;
  renderDayGroupedSchedules();
}

function renderDayGroupedSchedules() {
  const container = document.getElementById("schedulesByDayContainer");
  const daysToRender =
    selectedScheduleDayFilter === "ALL"
      ? DAYS_LIST.filter((day) =>
          cachedSchedules.some((s) => s.day_of_week === day),
        )
      : [selectedScheduleDayFilter];

  if (daysToRender.length === 0) {
    container.innerHTML = `<div class="bg-slate-900 border border-slate-800 rounded-xl p-6 text-center text-xs text-slate-500">No schedules configured.</div>`;
    return;
  }

  container.innerHTML = daysToRender
    .map((day) => {
      const accent = DAY_ACCENTS[day] || DAY_ACCENTS.Sunday;
      const dayItems = cachedSchedules.filter((s) => s.day_of_week === day);
      const rowsHtml =
        dayItems.length > 0
          ? dayItems
              .map((s) => {
                const isEditing = editingScheduleId === s.id;
                const rowBg = isEditing
                  ? "bg-sky-500/10"
                  : "hover:bg-slate-800/40";
                const edgeAccent = isEditing
                  ? "shadow-[inset_3px_0_0_0_#38bdf8]"
                  : "";
                const editBtnStyle = isEditing
                  ? "bg-sky-500/20 text-sky-300 ring-1 ring-sky-400/50"
                  : "text-slate-400 hover:text-sky-300 hover:bg-slate-800";

                return `
            <tr class="${rowBg} transition">
              <td class="py-3 px-4 w-32 ${edgeAccent}">
                <span class="inline-block px-2.5 py-0.5 rounded-md font-mono text-xs font-bold bg-indigo-500/15 text-indigo-300 border border-indigo-500/30">
                  ${s.subject_code || "—"}
                </span>
              </td>
              <td class="py-3 px-4 font-medium text-slate-100">${s.subject}</td>
              <td class="py-3 px-4 font-mono text-xs text-slate-300">${format12Hour(s.start_time)} - ${format12Hour(s.end_time)}</td>
              <td class="py-3 px-4 text-right">
                <div class="inline-flex items-center justify-end gap-1">
                  <button type="button" onclick="toggleEditSchedule(${s.id})" title="${isEditing ? "Cancel Edit" : "Edit Schedule"}" aria-label="${isEditing ? "Cancel Edit" : "Edit Schedule"}" class="p-1.5 rounded-lg transition ${editBtnStyle}">
                    ${ICONS.edit}
                  </button>
                  <button type="button" onclick="deleteSchedule(${s.id})" title="Delete Schedule" aria-label="Delete Schedule" class="p-1.5 rounded-lg text-slate-400 hover:text-rose-400 hover:bg-slate-800 transition">
                    ${ICONS.trash}
                  </button>
                </div>
              </td>
            </tr>
          `;
              })
              .join("")
          : `<tr><td colspan="4" class="py-4 px-4 text-center text-xs text-slate-500">No classes scheduled on ${day}.</td></tr>`;

      return `
      <div class="bg-slate-900 border border-slate-800 rounded-xl overflow-hidden">
        <div class="${accent.header} px-4 py-2.5 border-b flex items-center justify-between">
          <span class="text-sm font-bold ${accent.title}">${day}</span>
          <span class="px-2 py-0.5 rounded-full text-[11px] font-semibold border ${accent.badge}">
            ${dayItems.length} class${dayItems.length === 1 ? "" : "es"}
          </span>
        </div>
        <div class="overflow-x-auto">
          <table class="w-full text-left text-sm">
            <thead>
              <tr class="border-b border-slate-800/70 text-slate-400 text-[11px] uppercase">
                <th class="py-2 px-4">Code</th>
                <th class="py-2 px-4">Subject Name</th>
                <th class="py-2 px-4">Time</th>
                <th class="py-2 px-4 text-right">Actions</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-slate-800/60">${rowsHtml}</tbody>
          </table>
        </div>
      </div>
    `;
    })
    .join("");
}

// --- ATTENDANCE LOGS (WITH SINGLE LOG DELETE & PROOF PREVIEW) ---
function onLogDateScopeChange() {
  const scope = document.getElementById("logDateScope").value;
  const dateInput = document.getElementById("filterDate");
  if (scope === "custom") {
    if (!dateInput.value) dateInput.value = getTodayDateString();
    dateInput.classList.remove("hidden");
  } else {
    dateInput.classList.add("hidden");
  }
  refreshLogsDisplay();
}

function getActiveLogDateFilter() {
  const scopeEl = document.getElementById("logDateScope");
  if (!scopeEl) return getTodayDateString();
  const scope = scopeEl.value;
  if (scope === "today") return getTodayDateString();
  if (scope === "custom") return document.getElementById("filterDate").value;
  return "";
}

function getFilteredLogs() {
  const stuFilter = document.getElementById("filterStudent").value;
  const subFilter = document.getElementById("filterSubject").value;
  const dateFilter = getActiveLogDateFilter();

  return cachedLogs.filter((l) => {
    if (stuFilter && l.student_id !== stuFilter) return false;
    if (subFilter && l.subject !== subFilter) return false;
    if (dateFilter && l.date_str !== dateFilter) return false;
    return true;
  });
}

function setLogViewMode(mode) {
  logViewMode = mode;
  document
    .getElementById("logsTableContainer")
    .classList.toggle("hidden", mode !== "table");
  document
    .getElementById("logsCalendarContainer")
    .classList.toggle("hidden", mode !== "calendar");

  document.getElementById("mode-table").className =
    mode === "table"
      ? "px-3 py-1 text-xs font-semibold rounded-md bg-sky-600 text-white"
      : "px-3 py-1 text-xs font-semibold rounded-md text-slate-400 hover:text-slate-200";
  document.getElementById("mode-calendar").className =
    mode === "calendar"
      ? "px-3 py-1 text-xs font-semibold rounded-md bg-sky-600 text-white"
      : "px-3 py-1 text-xs font-semibold rounded-md text-slate-400 hover:text-slate-200";

  refreshLogsDisplay();
}

function refreshLogsDisplay() {
  const filtered = getFilteredLogs();

  document.getElementById("countPresent").textContent = filtered.filter(
    (l) => l.status === "PRESENT",
  ).length;
  document.getElementById("countLate").textContent = filtered.filter(
    (l) => l.status === "LATE",
  ).length;
  document.getElementById("countAbsent").textContent = filtered.filter(
    (l) => l.status === "ABSENT",
  ).length;

  const stuFilter = document.getElementById("filterStudent").value;
  const subFilter = document.getElementById("filterSubject").value;
  const dateFilter = getActiveLogDateFilter();
  const params = new URLSearchParams();
  if (stuFilter) params.append("student_id", stuFilter);
  if (subFilter) params.append("subject", subFilter);
  if (dateFilter) params.append("date", dateFilter);
  const qs = params.toString();
  document.getElementById("exportCsvBtn").href = qs
    ? `/api/export_csv?${qs}`
    : "/api/export_csv";

  renderDayGroupedLogsTable(filtered);
  renderAttendanceCalendar(filtered);
}

function renderDayGroupedLogsTable(filteredLogs) {
  const container = document.getElementById("logsTableContainer");
  if (filteredLogs.length === 0) {
    const activeDate = getActiveLogDateFilter();
    container.innerHTML = `<div class="py-8 text-center text-xs text-slate-500">${activeDate ? `No attendance records found for ${formatHumanDate(activeDate)}.` : "No attendance records found."}</div>`;
    return;
  }

  const grouped = {};
  filteredLogs.forEach((log) => {
    if (!grouped[log.date_str]) grouped[log.date_str] = [];
    grouped[log.date_str].push(log);
  });

  container.innerHTML = Object.keys(grouped)
    .map((dateKey) => {
      const dayLogs = grouped[dateKey];
      const rowsHtml = dayLogs
        .map((l) => {
          const codeBadge = l.subject_code
            ? `<span class="inline-block px-2 py-0.5 rounded font-mono text-xs font-bold bg-indigo-500/15 text-indigo-300 border border-indigo-500/30 mr-2">${l.subject_code}</span>`
            : "";
          const proofBtn = l.proof_image
            ? `<button type="button" onclick="openProofModal('${l.proof_image}', '${l.full_name.replace(/'/g, "\\'")}', '${l.subject_code || l.subject} • ${l.date_str} ${l.time_str}')" title="View Capture Proof" class="p-1.5 rounded-lg text-slate-400 hover:text-sky-300 hover:bg-slate-800 transition">${ICONS.photo}</button>`
            : "";

          return `
        <tr class="hover:bg-slate-800/35 transition">
          <td class="py-2.5 px-4">
            <div class="font-medium text-slate-100">${l.full_name}</div>
            <div class="font-mono text-[11px] text-slate-400">${l.student_id}</div>
          </td>
          <td class="py-2.5 px-4 text-slate-200">${codeBadge}${l.subject}</td>
          <td class="py-2.5 px-4"><span class="px-2.5 py-0.5 rounded-full text-xs font-bold ${badgeClass(l.status)}">${l.status}</span></td>
          <td class="py-2.5 px-4 font-mono text-xs text-slate-300">${l.time_str}</td>
          <td class="py-2.5 px-4 text-right">
            <div class="inline-flex items-center justify-end gap-1">
              ${proofBtn}
              <button type="button" onclick="deleteSingleLog(${l.id})" title="Delete This Log" aria-label="Delete This Log" class="p-1.5 rounded-lg text-slate-400 hover:text-rose-400 hover:bg-slate-800 transition">
                ${ICONS.trash}
              </button>
            </div>
          </td>
        </tr>
      `;
        })
        .join("");

      return `
      <div class="border border-slate-800 rounded-xl overflow-hidden">
        <div class="bg-slate-800/60 px-4 py-2 border-b border-slate-800 flex items-center justify-between">
          <span class="text-xs font-bold text-sky-300">${formatHumanDate(dateKey)}</span>
          <span class="text-[11px] text-slate-400">${dayLogs.length} record${dayLogs.length === 1 ? "" : "s"}</span>
        </div>
        <div class="overflow-x-auto">
          <table class="w-full text-left text-sm border-collapse">
            <thead>
              <tr class="border-b border-slate-800/70 text-slate-400 text-[11px] uppercase">
                <th class="py-2 px-4">Student</th>
                <th class="py-2 px-4">Subject</th>
                <th class="py-2 px-4">Status</th>
                <th class="py-2 px-4">Time</th>
                <th class="py-2 px-4 text-right">Actions</th>
              </tr>
            </thead>
            <tbody class="divide-y divide-slate-800/60">${rowsHtml}</tbody>
          </table>
        </div>
      </div>
    `;
    })
    .join("");
}

async function deleteSingleLog(logId) {
  if (!confirm("Delete this attendance record?")) return;
  await fetch(`/api/logs/${logId}`, { method: "DELETE" });
  loadDashboardData();
}

function renderAttendanceCalendar(filteredLogs) {
  const monthNames = [
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
  ];
  document.getElementById("calendarMonthLabel").textContent =
    `${monthNames[calMonth]} ${calYear}`;

  const calendarSourceLogs =
    document.getElementById("logDateScope").value === "today"
      ? cachedLogs.filter((l) => {
          const stuFilter = document.getElementById("filterStudent").value;
          const subFilter = document.getElementById("filterSubject").value;
          if (stuFilter && l.student_id !== stuFilter) return false;
          if (subFilter && l.subject !== subFilter) return false;
          return true;
        })
      : filteredLogs;

  const logsByDate = {};
  calendarSourceLogs.forEach((l) => {
    if (!logsByDate[l.date_str]) logsByDate[l.date_str] = [];
    logsByDate[l.date_str].push(l);
  });

  const firstDay = new Date(calYear, calMonth, 1);
  const startOffset = firstDay.getDay();
  const gridStart = new Date(calYear, calMonth, 1 - startOffset);

  const todayStr = getTodayDateString();
  const singleStudentSelected = Boolean(
    document.getElementById("filterStudent").value,
  );
  let html = "";

  for (let week = 0; week < 6; week++) {
    const weekStartDate = new Date(gridStart);
    weekStartDate.setDate(gridStart.getDate() + week * 7);

    if (week === 5 && weekStartDate.getMonth() !== calMonth) break;

    html += `<div class="att-calendar-grid border-b border-slate-800/80 last:border-b-0">`;

    for (let d = 0; d < 7; d++) {
      const cellDate = new Date(weekStartDate);
      cellDate.setDate(weekStartDate.getDate() + d);

      const yyyy = cellDate.getFullYear();
      const mm = String(cellDate.getMonth() + 1).padStart(2, "0");
      const dd = String(cellDate.getDate()).padStart(2, "0");
      const dateKey = `${yyyy}-${mm}-${dd}`;
      const inCurrentMonth = cellDate.getMonth() === calMonth;
      const isToday = dateKey === todayStr;

      const dayLogs = logsByDate[dateKey] || [];
      const barsHtml = dayLogs
        .slice(0, 3)
        .map((item) => {
          let barStyle =
            "bg-emerald-500/20 text-emerald-300 border-emerald-500/40";
          if (item.status === "LATE")
            barStyle = "bg-amber-500/20 text-amber-300 border-amber-500/40";
          if (item.status === "ABSENT")
            barStyle = "bg-rose-500/20 text-rose-300 border-rose-500/40";
          const subjCode = item.subject_code || item.subject;
          const label = singleStudentSelected
            ? subjCode
            : `${item.full_name.split(" ")[0]} • ${subjCode}`;
          return `<div class="px-1.5 py-0.5 rounded border text-[10px] font-medium truncate ${barStyle}">${label}</div>`;
        })
        .join("");

      const extraBadge =
        dayLogs.length > 3
          ? `<div class="text-[10px] text-slate-400 pl-1">+${dayLogs.length - 3} more</div>`
          : "";

      html += `
        <div onclick="inspectCalendarDate('${dateKey}')" class="min-h-[94px] p-1.5 border-r last:border-r-0 border-slate-800/80 cursor-pointer transition hover:bg-slate-800/50 ${inCurrentMonth ? "bg-slate-950/40" : "bg-slate-900/40 opacity-45"}">
          <div class="flex justify-between items-center mb-1">
            <span class="text-xs font-semibold ${isToday ? "px-1.5 py-0.5 rounded bg-sky-500 text-slate-950" : "text-slate-300"}">${cellDate.getDate()}</span>
          </div>
          <div class="space-y-1">${barsHtml}${extraBadge}</div>
        </div>
      `;
    }
    html += `</div>`;
  }

  document.getElementById("calendarGridBody").innerHTML = html;
}

function inspectCalendarDate(dateKey) {
  const stuFilter = document.getElementById("filterStudent").value;
  const subFilter = document.getElementById("filterSubject").value;
  const filtered = cachedLogs.filter((l) => {
    if (l.date_str !== dateKey) return false;
    if (stuFilter && l.student_id !== stuFilter) return false;
    if (subFilter && l.subject !== subFilter) return false;
    return true;
  });

  const detailBox = document.getElementById("calendarDayDetail");

  if (filtered.length === 0) {
    detailBox.innerHTML = `<span class="font-semibold text-slate-200">${formatHumanDate(dateKey)}:</span> No records.`;
    return;
  }

  detailBox.innerHTML = `
    <div class="flex items-center justify-between mb-2.5">
      <span class="font-bold text-sky-300">${formatHumanDate(dateKey)}</span>
      <span class="text-[11px] text-slate-400">${filtered.length} record${filtered.length > 1 ? "s" : ""}</span>
    </div>
    <div class="grid grid-cols-1 sm:grid-cols-2 gap-2">
      ${filtered
        .map(
          (l) => `
        <div class="px-3 py-2 rounded-lg bg-slate-900 border border-slate-800 flex items-center justify-between gap-2">
          <div class="min-w-0 truncate">
            <span class="font-semibold text-slate-100">${l.full_name}</span>
            <span class="font-mono text-indigo-300 ml-1.5">${l.subject_code || l.subject}</span>
          </div>
          <div class="flex items-center gap-1.5 shrink-0">
            <span class="px-2 py-0.5 rounded text-[10px] font-bold ${badgeClass(l.status)}">${l.status}</span>
            <span class="font-mono text-[11px] text-slate-400">${l.time_str}</span>
            <button type="button" onclick="deleteSingleLog(${l.id})" title="Delete This Log" class="p-1 rounded text-slate-400 hover:text-rose-400 transition">
              ${ICONS.trash}
            </button>
          </div>
        </div>
      `,
        )
        .join("")}
    </div>
  `;
}

function changeCalendarMonth(delta) {
  calMonth += delta;
  if (calMonth < 0) {
    calMonth = 11;
    calYear--;
  }
  if (calMonth > 11) {
    calMonth = 0;
    calYear++;
  }
  refreshLogsDisplay();
}

function jumpCalendarToday() {
  const now = new Date();
  calYear = now.getFullYear();
  calMonth = now.getMonth();
  refreshLogsDisplay();
}

function openStudentCalendar(studentId) {
  switchTab("logs");
  setTimeout(() => {
    document.getElementById("logDateScope").value = "all";
    document.getElementById("filterDate").classList.add("hidden");
    document.getElementById("filterStudent").value = studentId;
    setLogViewMode("calendar");
  }, 100);
}

function badgeClass(status) {
  if (status === "PRESENT") return "bg-emerald-500/20 text-emerald-400";
  if (status === "LATE") return "bg-amber-500/20 text-amber-400";
  return "bg-rose-500/20 text-rose-400";
}

// --- STUDENT FORM & CRUD ---
function toggleEditStudent(studentId) {
  if (editingStudentOriginalId === studentId) {
    resetStudentForm();
    return;
  }

  const student = cachedStudents.find((s) => s.student_id === studentId);
  if (!student) return;

  editingStudentOriginalId = student.student_id;
  uploadedEnrollBase64 = null;
  stopEnrollStream();

  const card = document.getElementById("enrollFormCard");
  card.className =
    "bg-sky-950/15 border border-sky-500/50 ring-1 ring-sky-500/20 rounded-xl p-5 self-start transition-colors";

  document.getElementById("enrollFormTitle").textContent = "Edit Student";
  document.getElementById("enrollFormTitle").className =
    "text-base font-semibold text-sky-300";
  document.getElementById("cancelEditStudentBtn").classList.remove("hidden");

  const submitBtn = document.getElementById("enrollSubmitBtn");
  submitBtn.textContent = "Save Changes";
  submitBtn.className =
    "w-full bg-sky-600 hover:bg-sky-500 text-white font-semibold py-2 rounded-lg transition";

  document.getElementById("enrollId").value = student.student_id;
  document.getElementById("enrollName").value = student.full_name;
  document.getElementById("enrollSection").value = student.section;
  document.getElementById("enrollFeedback").textContent =
    "Photo optional when updating details.";
  document.getElementById("enrollFeedback").className =
    "text-xs text-center font-medium text-slate-400";
  renderStudentsTable();
}

function resetStudentForm() {
  editingStudentOriginalId = null;
  uploadedEnrollBase64 = null;
  stopEnrollStream();

  const card = document.getElementById("enrollFormCard");
  card.className =
    "bg-slate-900 border border-slate-800 rounded-xl p-5 self-start transition-colors";

  document.getElementById("enrollFormTitle").textContent = "Enroll Student";
  document.getElementById("enrollFormTitle").className =
    "text-base font-semibold text-slate-100";
  document.getElementById("cancelEditStudentBtn").classList.add("hidden");

  const submitBtn = document.getElementById("enrollSubmitBtn");
  submitBtn.textContent = "Capture & Save";
  submitBtn.className =
    "w-full bg-emerald-600 hover:bg-emerald-500 text-white font-semibold py-2 rounded-lg transition";

  document.getElementById("enrollId").value = "";
  document.getElementById("enrollName").value = "";
  document.getElementById("enrollSection").value = "CPE-3B";
  document.getElementById("enrollFileInput").value = "";
  document.getElementById("enrollFeedback").textContent = "";
  renderStudentsTable();
}

async function submitStudentForm() {
  const feedback = document.getElementById("enrollFeedback");
  const enrollEspActive = !document
    .getElementById("enrollEspImg")
    .classList.contains("hidden");

  let imageBase64 = uploadedEnrollBase64 || "";
  if (!imageBase64 && (enrollStream || enrollEspActive)) {
    imageBase64 = await captureCurrentFrameBase64("enroll");
  }

  if (!editingStudentOriginalId && !imageBase64) {
    feedback.textContent = "Open camera or upload a photo first.";
    feedback.className = "text-xs text-center font-medium text-amber-400";
    return;
  }

  const cameraSource = selectedCameraId === "esp32" ? "esp32" : "webcam";
  const url = editingStudentOriginalId
    ? `/api/students/${encodeURIComponent(editingStudentOriginalId)}`
    : "/api/enroll";
  const method = editingStudentOriginalId ? "PUT" : "POST";

  const res = await fetch(url, {
    method,
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      student_id: document.getElementById("enrollId").value,
      full_name: document.getElementById("enrollName").value,
      section: document.getElementById("enrollSection").value,
      image: imageBase64,
      source: cameraSource,
    }),
  });
  const data = await res.json();

  if (data.success) {
    resetStudentForm();
    feedback.textContent = data.message;
    feedback.className = "text-xs text-center font-medium text-emerald-400";
    loadDashboardData();
  } else {
    feedback.textContent = data.message;
    feedback.className = "text-xs text-center font-medium text-rose-400";
  }
}

async function deleteStudent(id) {
  if (!confirm("Remove this student and their biometric profile?")) return;
  await fetch(`/api/students/${encodeURIComponent(id)}`, { method: "DELETE" });
  if (editingStudentOriginalId === id) resetStudentForm();
  loadDashboardData();
}

// --- SCHEDULE FORM & CRUD ---
function toggleEditSchedule(schedId) {
  if (editingScheduleId === schedId) {
    resetScheduleForm();
    return;
  }

  const sched = cachedSchedules.find((s) => s.id === schedId);
  if (!sched) return;

  editingScheduleId = sched.id;

  const card = document.getElementById("schedFormCard");
  card.className =
    "bg-sky-950/15 border border-sky-500/50 ring-1 ring-sky-500/20 rounded-xl p-5 self-start transition-colors";

  document.getElementById("schedFormTitle").textContent = "Edit Schedule";
  document.getElementById("schedFormTitle").className =
    "text-base font-semibold text-sky-300";
  document.getElementById("cancelEditSchedBtn").classList.remove("hidden");

  const submitBtn = document.getElementById("schedSubmitBtn");
  submitBtn.textContent = "Save Changes";
  submitBtn.className =
    "w-full bg-sky-600 hover:bg-sky-500 text-white font-semibold py-2 rounded-lg transition";

  document.getElementById("schedDay").value = sched.day_of_week;
  document.getElementById("schedCode").value = sched.subject_code || "";
  document.getElementById("schedSubject").value = sched.subject;
  document.getElementById("schedStart").value = sched.start_time;
  document.getElementById("schedEnd").value = sched.end_time;
  document.getElementById("scheduleFeedback").textContent = "";
  renderDayGroupedSchedules();
}

function resetScheduleForm() {
  editingScheduleId = null;

  const card = document.getElementById("schedFormCard");
  card.className =
    "bg-slate-900 border border-slate-800 rounded-xl p-5 self-start transition-colors";

  document.getElementById("schedFormTitle").textContent = "Add Schedule";
  document.getElementById("schedFormTitle").className =
    "text-base font-semibold text-slate-100";
  document.getElementById("cancelEditSchedBtn").classList.add("hidden");

  const submitBtn = document.getElementById("schedSubmitBtn");
  submitBtn.textContent = "Save Schedule";
  submitBtn.className =
    "w-full bg-emerald-600 hover:bg-emerald-500 text-white font-semibold py-2 rounded-lg transition";

  document.getElementById("schedCode").value = "";
  document.getElementById("schedSubject").value = "";
  document.getElementById("schedStart").value = "";
  document.getElementById("schedEnd").value = "";
  document.getElementById("scheduleFeedback").textContent = "";
  renderDayGroupedSchedules();
}

async function submitScheduleForm() {
  const feedback = document.getElementById("scheduleFeedback");
  const url = editingScheduleId
    ? `/api/schedules/${editingScheduleId}`
    : "/api/schedules";
  const method = editingScheduleId ? "PUT" : "POST";

  const res = await fetch(url, {
    method,
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      day_of_week: document.getElementById("schedDay").value,
      subject_code: document.getElementById("schedCode").value,
      subject: document.getElementById("schedSubject").value,
      start_time: document.getElementById("schedStart").value,
      end_time: document.getElementById("schedEnd").value,
    }),
  });
  const data = await res.json();

  if (data.success) {
    resetScheduleForm();
    feedback.textContent = data.message;
    feedback.className = "text-xs text-center font-medium text-emerald-400";
    loadDashboardData();
  } else {
    feedback.textContent = data.message;
    feedback.className = "text-xs text-center font-medium text-rose-400";
  }
}

async function deleteSchedule(id) {
  await fetch(`/api/schedules/${id}`, { method: "DELETE" });
  if (editingScheduleId === id) resetScheduleForm();
  loadDashboardData();
}

async function quickMarkAbsent(subject) {
  const res = await fetch("/api/mark_absences", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ subject }),
  });
  const data = await res.json();
  alert(data.message);
  loadDashboardData();
}

async function markRemainingAbsent() {
  const subject = document.getElementById("absenceSubjectSelect").value;
  await quickMarkAbsent(subject);
  closeManageModal();
}

async function clearTestLogs() {
  if (!confirm("Clear all attendance logs?")) return;
  await fetch("/api/logs/clear", { method: "DELETE" });
  closeManageModal();
  document.getElementById("calendarDayDetail").textContent =
    "Click a date above to view attendance details.";
  loadDashboardData();
}

// Initialize app
initCameraPermissions();
loadDashboardData();
