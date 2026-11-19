let cachedLogs = [];
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
  if (e.key === "Escape") closeManageModal();
});

function openManageModal() {
  document.getElementById("manageModal").classList.remove("hidden");
}

function closeManageModal() {
  document.getElementById("manageModal").classList.add("hidden");
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

setInterval(loadDashboardData, 10000);

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

// --- DROPDOWN-FILTERED RECENT SCANS (DEFAULTS TO TODAY) ---
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
        return `
      <div class="p-3 rounded-lg bg-slate-800/70 border border-slate-700/60 flex justify-between items-center gap-2">
        <div class="min-w-0">
          <p class="font-semibold text-slate-100 truncate">${l.full_name}</p>
          <p class="text-xs text-slate-400 truncate">${subjBadge} • ${datePrefix}${l.time_str}</p>
        </div>
        <span class="px-2 py-0.5 rounded text-xs font-bold shrink-0 ${badgeClass(l.status)}">${l.status}</span>
      </div>
    `;
      })
      .join("") ||
    `<p class="text-slate-500 text-xs py-6 text-center">${targetDate ? `No scans recorded on ${formatHumanDate(targetDate)}.` : "No scans recorded yet."}</p>`;
}

async function loadDashboardData() {
  const res = await fetch("/api/dashboard_data");
  const data = await res.json();

  cachedLogs = data.logs;
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
  refreshLogsDisplay();
  renderStudentsTable();
  renderDayGroupedSchedules();
}

function renderStudentsTable() {
  document.getElementById("studentsTableBody").innerHTML =
    cachedStudents
      .map((s) => {
        const isEditing = editingStudentOriginalId === s.student_id;
        const rowHighlight = isEditing
          ? "bg-amber-500/10 border-l-2 border-l-amber-400"
          : "hover:bg-slate-800/40";
        return `
      <tr class="${rowHighlight} transition">
        <td class="py-3 px-4 font-mono text-xs text-sky-300">${s.student_id}</td>
        <td class="py-3 px-4 font-medium text-slate-100">${s.full_name}</td>
        <td class="py-3 px-4"><span class="px-2 py-0.5 rounded text-xs font-semibold bg-indigo-500/15 text-indigo-300 border border-indigo-500/30">${s.section}</span></td>
        <td class="py-3 px-4 text-xs text-slate-400">${s.created_at}</td>
        <td class="py-3 px-4 text-right space-x-2">
          <button onclick="openStudentCalendar('${s.student_id}')" class="text-sky-400 hover:text-sky-300 text-xs font-medium">Calendar</button>
          <button onclick="startEditStudent('${s.student_id}')" class="text-amber-400 hover:text-amber-300 text-xs font-medium">${isEditing ? "Editing..." : "Edit"}</button>
          <button onclick="deleteStudent('${s.student_id}')" class="text-rose-400 hover:text-rose-300 text-xs font-medium">Remove</button>
        </td>
      </tr>
    `;
      })
      .join("") ||
    `<tr><td colspan="5" class="py-4 px-4 text-center text-xs text-slate-500">No enrolled students.</td></tr>`;
}

// --- DAY-GROUPED SCHEDULES WITH MULTI-COLOR PALETTE ---
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
                const rowHighlight = isEditing
                  ? "bg-amber-500/10 border-l-2 border-l-amber-400"
                  : "hover:bg-slate-800/40";
                return `
            <tr class="${rowHighlight} transition">
              <td class="py-3 px-4 w-32">
                <span class="inline-block px-2.5 py-0.5 rounded-md font-mono text-xs font-bold bg-indigo-500/15 text-indigo-300 border border-indigo-500/30">
                  ${s.subject_code || "—"}
                </span>
              </td>
              <td class="py-3 px-4 font-medium text-slate-100">${s.subject}</td>
              <td class="py-3 px-4 font-mono text-xs text-slate-300">${format12Hour(s.start_time)} - ${format12Hour(s.end_time)}</td>
              <td class="py-3 px-4 text-right space-x-2">
                <button onclick="startEditSchedule(${s.id})" class="text-amber-400 hover:text-amber-300 text-xs font-medium">${isEditing ? "Editing..." : "Edit"}</button>
                <button onclick="deleteSchedule(${s.id})" class="text-rose-400 hover:text-rose-300 text-xs font-medium">Delete</button>
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

// --- ATTENDANCE LOGS (DROPDOWN SCOPE, DEFAULTS TO TODAY) & CALENDAR ---
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
          return `
        <tr class="hover:bg-slate-800/35 transition">
          <td class="py-2.5 px-4">
            <div class="font-medium text-slate-100">${l.full_name}</div>
            <div class="font-mono text-[11px] text-slate-400">${l.student_id}</div>
          </td>
          <td class="py-2.5 px-4 text-slate-200">${codeBadge}${l.subject}</td>
          <td class="py-2.5 px-4"><span class="px-2.5 py-0.5 rounded-full text-xs font-bold ${badgeClass(l.status)}">${l.status}</span></td>
          <td class="py-2.5 px-4 font-mono text-xs text-slate-300 text-right">${l.time_str}</td>
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
                <th class="py-2 px-4 text-right">Time</th>
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
          <div class="flex items-center gap-2 shrink-0">
            <span class="px-2 py-0.5 rounded text-[10px] font-bold ${badgeClass(l.status)}">${l.status}</span>
            <span class="font-mono text-[11px] text-slate-400">${l.time_str}</span>
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

// --- STUDENT FORM & CRUD (DISTINCT EDIT MODE STYLING) ---
function startEditStudent(studentId) {
  const student = cachedStudents.find((s) => s.student_id === studentId);
  if (!student) return;

  editingStudentOriginalId = student.student_id;
  uploadedEnrollBase64 = null;
  stopEnrollStream();

  const card = document.getElementById("enrollFormCard");
  card.className =
    "bg-amber-950/15 border-2 border-amber-500/70 rounded-xl p-5 transition-colors";

  document.getElementById("enrollFormTitle").textContent = "Edit Student";
  document.getElementById("enrollFormTitle").className =
    "text-base font-bold text-amber-300";
  document.getElementById("enrollEditBadge").classList.remove("hidden");
  document.getElementById("cancelEditStudentBtn").classList.remove("hidden");

  const submitBtn = document.getElementById("enrollSubmitBtn");
  submitBtn.textContent = "Save Changes";
  submitBtn.className =
    "w-full bg-amber-500 hover:bg-amber-400 text-slate-950 font-bold py-2 rounded-lg transition";

  document.getElementById("enrollId").value = student.student_id;
  document.getElementById("enrollName").value = student.full_name;
  document.getElementById("enrollSection").value = student.section;
  document.getElementById("enrollFeedback").textContent =
    "Editing existing profile — new photo optional.";
  document.getElementById("enrollFeedback").className =
    "text-xs text-center font-medium text-amber-300";
  renderStudentsTable();
}

function resetStudentForm() {
  editingStudentOriginalId = null;
  uploadedEnrollBase64 = null;
  stopEnrollStream();

  const card = document.getElementById("enrollFormCard");
  card.className =
    "bg-slate-900 border border-slate-800 rounded-xl p-5 transition-colors";

  document.getElementById("enrollFormTitle").textContent = "Enroll Student";
  document.getElementById("enrollFormTitle").className =
    "text-base font-semibold text-slate-100";
  document.getElementById("enrollEditBadge").classList.add("hidden");
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

// --- SCHEDULE FORM & CRUD (DISTINCT EDIT MODE STYLING) ---
function startEditSchedule(schedId) {
  const sched = cachedSchedules.find((s) => s.id === schedId);
  if (!sched) return;

  editingScheduleId = sched.id;

  const card = document.getElementById("schedFormCard");
  card.className =
    "bg-amber-950/15 border-2 border-amber-500/70 rounded-xl p-5 self-start transition-colors";

  document.getElementById("schedFormTitle").textContent = "Edit Schedule";
  document.getElementById("schedFormTitle").className =
    "text-base font-bold text-amber-300";
  document.getElementById("schedEditBadge").classList.remove("hidden");
  document.getElementById("cancelEditSchedBtn").classList.remove("hidden");

  const submitBtn = document.getElementById("schedSubmitBtn");
  submitBtn.textContent = "Save Changes";
  submitBtn.className =
    "w-full bg-amber-500 hover:bg-amber-400 text-slate-950 font-bold py-2 rounded-lg transition";

  document.getElementById("schedDay").value = sched.day_of_week;
  document.getElementById("schedCode").value = sched.subject_code || "";
  document.getElementById("schedSubject").value = sched.subject;
  document.getElementById("schedStart").value = sched.start_time;
  document.getElementById("schedEnd").value = sched.end_time;
  document.getElementById("scheduleFeedback").textContent =
    `Editing ${sched.subject_code || sched.subject} (${sched.day_of_week}).`;
  document.getElementById("scheduleFeedback").className =
    "text-xs text-center font-medium text-amber-300";
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
  document.getElementById("schedEditBadge").classList.add("hidden");
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
