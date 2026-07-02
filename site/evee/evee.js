// EVEE landing — the firmware's design system, live.
// THEMES mirrors src/ui/Theme.cpp exactly (bg/border/dim/label/white/green/
// yellow/accent). "page" is the only web-side addition: the page background
// sits one stop past the device bg so the screenshots glow.
document.documentElement.classList.add("js");
document.getElementById("year").textContent = new Date().getFullYear();

const THEMES = {
  cam:       { page: "#0e0e10", panel: "#1a1a1a", line: "#3a3a3c", text: "#ffffff", label: "#aaaaaa", dim: "#8a8f96", accent: "#b950d7", green: "#00c864", yellow: "#ffcd00", light: false, cap: "shipping default" },
  ember:     { page: "#0d0a08", panel: "#14100e", line: "#483a30", text: "#fff6ec", label: "#c0a28a", dim: "#967a64", accent: "#ff8c28", green: "#78c85a", yellow: "#ffcd00", light: false, cap: "warm amber" },
  ice:       { page: "#0b0e11", panel: "#12161a", line: "#36424a", text: "#f0f8ff", label: "#a2b6c0", dim: "#788c96", accent: "#00c8e6", green: "#00d296", yellow: "#ffd25a", light: false, cap: "true cyan" },
  light:     { page: "#f6f6f8", panel: "#ececf0", line: "#b0b0b6", text: "#18181c", label: "#4a4a50", dim: "#78787e", accent: "#962cbe", green: "#009648", yellow: "#be8c00", light: true,  cap: "daylight" },
  cyber:     { page: "#060510", panel: "#0a0812", line: "#3e1e54", text: "#e4e6ff", label: "#ba8ee0", dim: "#7c5ca2", accent: "#ff2ccc", green: "#00ffb4", yellow: "#ffee3c", light: false, cap: "neon on near-black" },
  synthwave: { page: "#100819", panel: "#160c22", line: "#46285a", text: "#faf0ff", label: "#d296c8", dim: "#966eaa", accent: "#ff5aaa", green: "#3cf0c8", yellow: "#ffd264", light: false, cap: "retro sunset" },
  mono:      { page: "#0a0a0a", panel: "#101010", line: "#404040", text: "#f5f5f5", label: "#b4b4b4", dim: "#828282", accent: "#ffffff", green: "#c8c8c8", yellow: "#d2d2d2", light: false, cap: "grayscale" },
  forest:    { page: "#090f0a", panel: "#0e1610", line: "#304634", text: "#ecf6ee", label: "#a0bea4", dim: "#6e8c74", accent: "#78d26e", green: "#5adc78", yellow: "#dcc85a", light: false, cap: "earthy greens" },
};

const root = document.documentElement;
const chipsBox = document.getElementById("theme-chips");
const themeShot = document.getElementById("theme-shot");
const themeShotCap = document.getElementById("theme-shot-cap");

function applyTheme(name) {
  const t = THEMES[name] || THEMES.cam;
  const map = { "--bg": t.page, "--panel": t.panel, "--line": t.line, "--text": t.text,
                "--label": t.label, "--dim": t.dim, "--accent": t.accent,
                "--green": t.green, "--yellow": t.yellow };
  for (const [k, v] of Object.entries(map)) root.style.setProperty(k, v);
  root.classList.toggle("light", t.light);
  document.querySelector('meta[name="theme-color"]').content = t.page;
  themeShot.src = `img/theme_${name}.png`;
  themeShotCap.textContent = `${name} · ${t.cap}`;
  for (const b of chipsBox.querySelectorAll("button"))
    b.setAttribute("aria-pressed", String(b.dataset.theme === name));
  try { localStorage.setItem("evee-theme", name); } catch (_) {}
}

for (const [name, t] of Object.entries(THEMES)) {
  const b = document.createElement("button");
  b.type = "button";
  b.className = "theme-chip";
  b.dataset.theme = name;
  b.innerHTML = `<span class="theme-swatch" style="background:linear-gradient(135deg,${t.accent} 50%,${t.panel} 50%)"></span>${name}`;
  b.addEventListener("click", () => applyTheme(name));
  chipsBox.appendChild(b);
}
let saved = new URLSearchParams(location.search).get("theme");
if (!(saved in THEMES)) {
  try { saved = localStorage.getItem("evee-theme"); } catch (_) {}
}
applyTheme(saved in THEMES ? saved : "cam");

// ── Hero device: cycle the five real HUD faces like the side button does ──
const FACE_NAMES = ["SPEED", "BATTERY %", "BATTERY V", "WATTS", "SAFETY"];
const screens = [...document.querySelectorAll("#hud-stack .device-screen")];
const faceLabel = document.getElementById("face-label");
const dotsBox = document.getElementById("face-dots");
const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;
let face = 0, hudTimer = null;

FACE_NAMES.forEach((n, i) => {
  const d = document.createElement("button");
  d.type = "button";
  d.setAttribute("role", "tab");
  d.setAttribute("aria-label", n + " face");
  d.setAttribute("aria-selected", i === 0 ? "true" : "false");
  d.addEventListener("click", () => { setFace(i); restartHud(); });
  dotsBox.appendChild(d);
});
const dots = [...dotsBox.children];

function setFace(i) {
  face = (i + screens.length) % screens.length;
  screens.forEach((img, k) => img.classList.toggle("active", k === face));
  dots.forEach((d, k) => d.setAttribute("aria-selected", String(k === face)));
  faceLabel.textContent = FACE_NAMES[face];
}
function restartHud() {
  clearInterval(hudTimer);
  if (!reduced) hudTimer = setInterval(() => setFace(face + 1), 3800);
}
document.getElementById("hud-btn").addEventListener("click", () => { setFace(face + 1); restartHud(); });
// only cycle while the device is on screen
new IntersectionObserver((en) => {
  if (en[0].isIntersecting) restartHud(); else clearInterval(hudTimer);
}, { threshold: 0.2 }).observe(document.querySelector(".device"));

// ── Topbar clock — same slot as the device status bar ──
const clockEl = document.getElementById("top-clock");
function tick() {
  const d = new Date();
  clockEl.textContent = String(d.getHours()).padStart(2, "0") + ":" + String(d.getMinutes()).padStart(2, "0");
}
tick(); setInterval(tick, 30000);

// ── Scroll-reveal sections (skipped for reduced motion via CSS) ──
const sections = document.querySelectorAll("main section");
sections.forEach((s) => s.classList.add("reveal"));
const io = new IntersectionObserver((entries) => {
  for (const en of entries) if (en.isIntersecting) { en.target.classList.add("in"); io.unobserve(en.target); }
}, { threshold: 0.12 });
sections.forEach((s) => io.observe(s));

// OTA boot-bar fills once the updates section arrives
new IntersectionObserver((en, obs) => {
  if (en[0].isIntersecting) { document.getElementById("boot-bar").classList.add("go"); obs.disconnect(); }
}, { threshold: 0.4 }).observe(document.getElementById("ota"));

// Active nav state follows the section in view.
const navLinks = [...document.querySelectorAll(".topnav a")];
const navIo = new IntersectionObserver((entries) => {
  for (const en of entries) {
    if (!en.isIntersecting) continue;
    navLinks.forEach((a) => a.classList.toggle("active", a.hash === "#" + en.target.id));
  }
}, { rootMargin: "-30% 0px -60% 0px" });
document.querySelectorAll("main section[id]").forEach((s) => navIo.observe(s));

// ── Live release feed ──
const FEED = "https://apps.zombie.digital/downloads/esk8os-firmware";
const LABELS = {
  "tdisplay-s3": ["LilyGo T-Display-S3", "color dashboard build"],
  "esp32s3-oled": ["ESP32-S3 + OLED", "mini display build"],
  "esp32s3-headless": ["ESP32-S3 headless", "BLE/app-only build"],
};

fetch(`${FEED}/latest.json`)
  .then((r) => r.json())
  .then((rel) => {
    document.getElementById("fw-version").textContent = "v" + rel.version;
    document.getElementById("fw-date").textContent =
      new Date(rel.publishedAt).toISOString().slice(0, 10);
    document.getElementById("fw-notes").textContent = rel.notes || "";
    document.getElementById("release-line").textContent =
      `firmware v${rel.version} · app soon · self-hosted`;
    const list = document.getElementById("fw-files");
    list.innerHTML = "";
    for (const [kind, file] of Object.entries(rel.files || {})) {
      const [name, sub] = LABELS[kind] || [kind, ""];
      const a = document.createElement("a");
      a.className = "dl-item";
      a.href = `${FEED}/${file}`;
      a.innerHTML = `<span><span class="k">${name}</span><br><span class="f mono">${file}</span></span>` +
                    `<span class="f mono">${sub}</span>`;
      list.appendChild(a);
    }
  })
  .catch(() => {
    document.getElementById("fw-notes").textContent =
      "Release feed unavailable right now — downloads live at apps.zombie.digital/downloads/esk8os-firmware/.";
  });
