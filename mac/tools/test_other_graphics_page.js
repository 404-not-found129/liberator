// The page on a Mac without an NVIDIA card. Golden Gate does not need one: with AMD or Intel graphics the buttons
// work and the verdict says what the move means on that chip. A Mac with an NVIDIA card is shown what it always was,
// and a Mac whose NVIDIA card is only hidden or switched off is still told how to get it back.
//
//   node mac/tools/test_other_graphics_page.js mac/Resources/app.js mac/Resources/index.html
//
// The real page script runs against a stand-in document whose elements carry the data- attributes of the real page.
const fs = require("fs"), vm = require("vm"), path = require("path");
const here = path.resolve(__dirname, "..", "Resources");
const source = fs.readFileSync(process.argv[2] || path.join(here, "app.js"), "utf8");
const html = fs.readFileSync(process.argv[3] || path.join(here, "index.html"), "utf8");

function dataOf(id) {
  const at = html.indexOf(`id="${id}"`);
  if (at < 0) return {};
  const open = html.lastIndexOf("<", at), close = html.indexOf(">", at);
  const out = {};
  for (const m of html.slice(open, close).matchAll(/data-([a-z0-9-]+)="([^"]*)"/g)) {
    out[m[1].replace(/-([a-z0-9])/g, (_, c) => c.toUpperCase())] = m[2].replace(/&amp;/g, "&");
  }
  return out;
}
function page() {
  const nodes = new Map();
  const node = (id) => {
    if (!nodes.has(id)) nodes.set(id, { textContent: "", innerHTML: "", value: "", disabled: true, hidden: true, checked: false, dataset: dataOf(id), style: {}, options: [],
      classList: { toggle() {}, add() {}, remove() {}, contains() { return false; } }, setAttribute() {}, querySelectorAll() { return []; }, insertAdjacentHTML() {}, addEventListener() {} });
    return nodes.get(id);
  };
  const window = { webkit: { messageHandlers: { nm: { postMessage() {} } } }, matchMedia() { return { matches: false, addEventListener() {} }; } };
  const context = vm.createContext({ window, document: { getElementById: node, querySelectorAll() { return []; }, addEventListener() {} },
    confirm() { return false; }, setTimeout() {}, localStorage: { getItem() { return null; }, setItem() {} }, console });
  vm.runInContext(source, context);
  return { node, scan: (m) => window.NM.on({ event: "scan", data: m }) };
}
const base = { version: "0.3.0", macos: "26.7.3", major: 26, arch: "x86_64", translated: false, opencore: "DBG-108-2026-09-27", nativeMacPro: false,
  kexts: 0, metal: [], files: false, installedDriverVersion: "", gpuHiding: [], hiddenNvidia: [], hwModel: "MacPro7,1", packages: [],
  goldenGate: true, ggKit: "absent", ggKeptMB: 0, ggKitMB: 235, usb: [], efis: [], profile: {} };
const RX570 = { vendor: "1002", device: "67DF", name: "AMD Radeon RX 570", supported: false, tested: false };
const UHD = { vendor: "8086", device: "3E92", name: "Intel UHD Graphics 630", supported: false, tested: false };
const RTX = { vendor: "10DE", device: "2D05", name: "NVIDIA GeForce RTX 5060", supported: true, tested: true };
const GTX = { vendor: "10DE", device: "1C03", name: "NVIDIA 1C03", supported: false, tested: false };

let fails = 0;
function arm(name, ok, detail) { if (!ok) fails++; console.log((ok ? "ok   " : "FAIL ") + name + (ok ? "" : " (" + String(detail).slice(-500) + ")")); }
function show(m) { const p = page(); p.scan(Object.assign({}, base, m)); return { v: p.node("verdict").innerHTML, prep: p.node("ggprep").disabled, cancel: p.node("ggcancel").disabled, steps: p.node("steps").hidden, need: p.node("ggt").textContent }; }

let r = show({ gpus: [RX570] });
arm("a PC on a Radeon and no NVIDIA card is not turned away: the Golden Gate button works and the verdict names AMD graphics",
  !r.prep && !r.cancel && r.v.includes("Its AMD graphics can still make the move") && !r.v.includes("can't be used here"), JSON.stringify(r));
arm("it is told that the desktop is drawn without graphics acceleration at first, and that this is untried", r.v.includes("without graphics acceleration") && r.v.includes("untried on real hardware"), r.v);
arm("nothing of the NVIDIA driver's steps is shown there", r.steps === true, JSON.stringify(r));
r = show({ gpus: [UHD] });
arm("the same on Intel graphics alone, named as Intel", !r.prep && r.v.includes("Its Intel graphics can still make the move"), JSON.stringify(r));
r = show({ gpus: [GTX, UHD] });
arm("an NVIDIA card the driver does not take, beside Intel graphics, leaves the move on the Intel chip open", !r.prep && r.v.includes("Its Intel graphics can still make the move"), JSON.stringify(r));
r = show({ gpus: [RX570], opencore: "" });
arm("without OpenCore that PC is still refused, for OpenCore and not for its card", r.prep && r.v.includes("can't be used here") && r.v.includes("does not report OpenCore") && !r.v.includes("NVIDIA card was found"), JSON.stringify(r));
r = show({ gpus: [RX570], major: 14, macos: "14.6" });
arm("on a macOS the move does not start from it is refused as before", r.prep && r.v.includes("can't be used here"), JSON.stringify(r));
r = show({ gpus: [RX570], goldenGate: false, ggKit: "none" });
arm("a build that offers no macOS 27 keeps every Golden Gate button off there too", r.prep && r.cancel, JSON.stringify(r));
r = show({ gpus: [RX570], major: 27, macos: "27.2" });
arm("on macOS 27 itself the verdict says it runs on AMD graphics and that there is nothing to install", r.v.includes("is running on this Mac's AMD graphics") && r.steps === true && r.prep, JSON.stringify(r));

r = show({ gpus: [RTX] });
arm("control: a PC with an NVIDIA card the driver takes is shown what it always was", !r.prep && r.v.includes("This Mac can make the move. Use Prepare for Golden Gate below.") && !r.v.includes("graphics can still make the move"), JSON.stringify(r));
r = show({ gpus: [RTX, UHD] });
arm("control: and so is one that also has Intel graphics", !r.prep && r.v.includes("This Mac can make the move.") && !r.v.includes("Intel graphics can still"), JSON.stringify(r));
r = show({ gpus: [UHD], gpuHiding: ["-wegnoegpu"] });
arm("control: a Mac whose discrete card is hidden by a boot argument is told so, and is not sent to its Intel graphics", r.prep && r.v.includes("can't be used here") && r.v.includes("-wegnoegpu"), JSON.stringify(r));
r = show({ gpus: [UHD], hiddenNvidia: ["10DE:2D05"] });
arm("control: and one whose NVIDIA card is switched off is told that", r.prep && r.v.includes("switched off for macOS"), JSON.stringify(r));
r = show({ gpus: [] });
arm("control: no graphics found at all is refused", r.prep && r.v.includes("can't be used here"), JSON.stringify(r));
r = show({ gpus: [GTX] });
arm("control: an NVIDIA card the driver does not take, alone, is refused as before", r.prep && r.v.includes("No supported NVIDIA card was found"), JSON.stringify(r));
r = show({ gpus: [RX570], arch: "arm64" });
arm("control: Apple silicon is refused whatever its graphics", r.prep && r.v.includes("Apple silicon"), JSON.stringify(r));

// A run that cannot tell which of two partitions OpenCore started from asks the user. The choice is a card of its own
// that is brought into view, and the line under the button says what to do: as one plain line between two cards it
// was not seen on the first PC that needed it.
{
  const html2 = html;
  const nodes = new Map(); let scrolled = 0;
  const node = (id) => {
    if (!nodes.has(id)) nodes.set(id, { textContent: "", innerHTML: "", value: "", disabled: true, hidden: true, checked: false, dataset: dataOf(id), style: {}, options: [],
      classList: { toggle() {}, add() {}, remove() {}, contains() { return false; } }, setAttribute() {}, querySelectorAll() { return []; }, addEventListener() {},
      insertAdjacentHTML(_w, t) { const m = /<option value="([^"]+)"/.exec(t); if (m) { this.options.push({ value: m[1] }); if (!this.value) this.value = m[1]; } },
      scrollIntoView() { scrolled++; } });
    return nodes.get(id);
  };
  const window = { webkit: { messageHandlers: { nm: { postMessage() {} } } }, matchMedia() { return { matches: false, addEventListener() {} }; } };
  vm.runInContext(source, vm.createContext({ window, document: { getElementById: node, querySelectorAll() { return []; }, addEventListener() {} }, confirm() { return false; }, setTimeout() {}, localStorage: { getItem() { return null; }, setItem() {} }, console }));
  window.NM.on({ event: "scan", data: Object.assign({}, base, { gpus: [RX570] }) });
  arm("before any run the choice of partition is hidden", node("efirow").hidden === true);
  window.NM.on({ event: "lines", data: ["STEP Finding the OpenCore partition", "NOTE candidate disk3s1", "NOTE candidate disk5s1", "STOP OpenCore's startup partition could not be confirmed - select the partition this Mac started from"] });
  arm("two candidates in a run's lines show the choice, with both partitions, the first one chosen, and bring it into view once",
    node("efirow").hidden === false && node("efi").options.map((o) => o.value).join() === "disk3s1,disk5s1" && node("efi").value === "disk3s1" && scrolled === 1, JSON.stringify([node("efi").options, scrolled]));
  window.NM.on({ event: "run", data: { state: "end", mode: "goldengate", ok: false } });
  arm("the line under the button then says to choose the partition and press again", /choose the startup partition/.test(node("ggt").textContent), node("ggt").textContent);
  arm("the page holds the choice as a card of its own, with its sentence", /<div id="efirow" class="pane card" hidden>/.test(html2) && html2.includes("Nothing was changed.</p>") && html2.includes('<select id="efi"></select>'));
}
console.log(fails ? `${fails} failed` : "all passed");
process.exit(fails ? 1 : 0);
