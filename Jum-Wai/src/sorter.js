/* sorter.js */
(function (global) {
  function timeKey(t) {
    if (!t) return "99:99";
    const [h = "", m = ""] = String(t).split(":");
    return `${h.padStart(2, "0")}:${m.padStart(2, "0")}`;
  }

  // Date ASC แล้วค่อย Time ASC
  function cmpDateAsc(a, b) {
    const d = a.date.localeCompare(b.date);
    if (d !== 0) return d;
    return timeKey(a.time).localeCompare(timeKey(b.time));
  }

  // Date DESC แล้วค่อย Time DESC
  function cmpDateDesc(a, b) {
    const d = b.date.localeCompare(a.date);
    if (d !== 0) return d;
    return timeKey(b.time).localeCompare(timeKey(a.time));
  }

  // ใหม่สุดก่อน อิง timestamp
  function cmpNewest(a, b) {
    const ta = parseInt(String(a.id || "").split("_")[0], 10) || 0;
    const tb = parseInt(String(b.id || "").split("_")[0], 10) || 0;
    return tb - ta;
  }

  // เก่าสุดก่อน
  function cmpOldest(a, b) {
    const ta = parseInt(String(a.id || "").split("_")[0], 10) || 0;
    const tb = parseInt(String(b.id || "").split("_")[0], 10) || 0;
    return ta - tb;
  }

  function sortBy(tasks, mode = "date-asc") {
    const arr = [...tasks];
    switch (mode) {
      case "date-asc":  arr.sort(cmpDateAsc);  break;
      case "date-desc": arr.sort(cmpDateDesc); break;
      case "newest":    arr.sort(cmpNewest);   break;
      case "oldest":    arr.sort(cmpOldest);   break;
      default:          arr.sort(cmpDateAsc);
    }
    return arr;
  }

  global.TaskSorter = { sortBy };
})(window);
