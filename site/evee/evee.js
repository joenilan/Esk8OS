// EVEE landing — live release feed + the animated HUD replica.
document.documentElement.classList.add("js");
document.getElementById("year").textContent = new Date().getFullYear();

// Scroll-reveal sections (skipped for reduced motion via CSS).
const sections = document.querySelectorAll("main section");
sections.forEach((s) => s.classList.add("reveal"));
const io = new IntersectionObserver((entries) => {
  for (const en of entries) if (en.isIntersecting) { en.target.classList.add("in"); io.unobserve(en.target); }
}, { threshold: 0.12 });
sections.forEach((s) => io.observe(s));

// Active nav state follows the section in view.
const navLinks = [...document.querySelectorAll(".topnav a")];
const navIo = new IntersectionObserver((entries) => {
  for (const en of entries) {
    if (!en.isIntersecting) continue;
    navLinks.forEach((a) => a.classList.toggle("active", a.hash === "#" + en.target.id));
  }
}, { rootMargin: "-30% 0px -60% 0px" });
document.querySelectorAll("main section[id]").forEach((s) => navIo.observe(s));

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

