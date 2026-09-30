import { deriveOwnerKey, sealOwnerCommand } from "./crypto.js";

const modes = [
  "nametag",
  "slideshow (all)",
  "wifi scanner",
  "cube",
  "pyramid",
  "tunnel",
  "rings",
  "pop-out text",
];
const elements = {
  connectForm: document.querySelector("#connect-form"),
  controls: document.querySelector("#controls"),
  status: document.querySelector("#connection-status"),
  error: document.querySelector("#connection-error"),
  badgeId: document.querySelector("#badge-id"),
  badgeCode: document.querySelector("#badge-code"),
  brokerUrl: document.querySelector("#broker-url"),
  brokerUser: document.querySelector("#broker-user"),
  brokerPassword: document.querySelector("#broker-password"),
  badgeState: document.querySelector("#badge-state"),
  name: document.querySelector("#name"),
  show: document.querySelector("#show"),
  toast: document.querySelector("#toast"),
};

let client;
let ownerKey;
let activeBadgeId;
let toastTimer;

elements.brokerUrl.value = localStorage.getItem("dc34-broker-url") || "wss://";
elements.badgeId.value = localStorage.getItem("dc34-badge-id") || "";
elements.show.replaceChildren(...modes.map((mode, index) => new Option(mode, String(index))));
elements.badgeCode.addEventListener("input", () => {
  elements.badgeCode.value = elements.badgeCode.value.toUpperCase().replace(/[ILOU]/g, "");
});
elements.badgeId.addEventListener("input", () => {
  elements.badgeId.value = elements.badgeId.value.toLowerCase().replace(/[^0-9a-f]/g, "");
});

function setStatus(text, state = "offline") {
  elements.status.value = text;
  elements.status.dataset.state = state;
}

function showToast(text) {
  elements.toast.textContent = text;
  elements.toast.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => elements.toast.classList.remove("visible"), 2200);
}

function disconnect() {
  if (client) client.end(true);
  client = undefined;
  ownerKey = undefined;
  elements.controls.inert = true;
  setStatus("Offline");
}

function connectMqtt(options) {
  return new Promise((resolve, reject) => {
    let settled = false;
    const candidate = mqtt.connect(options.url, {
      username: options.username,
      password: options.password,
      clientId: `dc34-web-${crypto.randomUUID().slice(0, 8)}`,
      clean: true,
      connectTimeout: 7000,
      reconnectPeriod: 3000,
    });
    const fail = (error) => {
      if (!settled) {
        settled = true;
        candidate.end(true);
        reject(error);
      }
    };
    candidate.once("connect", () => {
      settled = true;
      client = candidate;
      resolve();
    });
    candidate.once("error", fail);
  });
}

elements.connectForm.addEventListener("submit", async (event) => {
  event.preventDefault();
  elements.error.textContent = "";
  const button = event.submitter;
  button.disabled = true;
  setStatus("Deriving key");
  disconnect();

  activeBadgeId = elements.badgeId.value.trim().toLowerCase();
  const code = elements.badgeCode.value.trim().toUpperCase();
  const brokerUrl = elements.brokerUrl.value.trim();
  try {
    ownerKey = await deriveOwnerKey(code, activeBadgeId);
    setStatus("Connecting");
    await connectMqtt({
      url: brokerUrl,
      username: elements.brokerUser.value.trim(),
      password: elements.brokerPassword.value,
    });
    client.on("close", () => setStatus("Reconnecting"));
    client.on("connect", () => setStatus("Connected", "online"));
    client.on("error", (error) => { elements.error.textContent = error.message; });
    client.on("message", (_topic, payload) => {
      try {
        const state = JSON.parse(payload.toString());
        elements.badgeState.textContent = `${state.name} · ${state.show} · firmware ${state.v}`;
        elements.name.value = state.name || "";
        const modeIndex = modes.indexOf(state.show);
        if (modeIndex >= 0) {
          elements.show.value = String(modeIndex);
        } else if (state.show && !elements.show.querySelector('[data-runtime]')) {
          const runtimeMode = new Option(`${state.show} (on badge)`, "");
          runtimeMode.dataset.runtime = "true";
          runtimeMode.disabled = true;
          elements.show.append(runtimeMode);
          runtimeMode.selected = true;
        }
      } catch { elements.badgeState.textContent = "Badge state could not be read"; }
    });
    client.subscribe(`dc34/badge/${activeBadgeId}/state`);
    elements.controls.inert = false;
    setStatus("Connected", "online");
    localStorage.setItem("dc34-badge-id", activeBadgeId);
    localStorage.setItem("dc34-broker-url", brokerUrl);
  } catch (error) {
    disconnect();
    elements.error.textContent = error.message || "Could not connect";
  } finally {
    button.disabled = false;
  }
});

async function publish(command, confirmation) {
  if (!client?.connected || !ownerKey) throw new Error("Connect a badge first");
  const envelope = await sealOwnerCommand(ownerKey, activeBadgeId, command);
  await new Promise((resolve, reject) => {
    client.publish(`dc34/badge/${activeBadgeId}/owner`, envelope, { qos: 1 }, (error) => error ? reject(error) : resolve());
  });
  showToast(confirmation);
}

async function submitCommand(action) {
  elements.error.textContent = "";
  try {
    await action();
  } catch (error) {
    elements.error.textContent = error.message || "Command could not be sent";
  }
}

document.querySelector("#name-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const name = elements.name.value.trim();
  if (name) submitCommand(() => publish({ setName: name }, "Name sent"));
});

document.querySelector("#show-form").addEventListener("submit", (event) => {
  event.preventDefault();
  submitCommand(() => publish({ setShow: Number(elements.show.value) }, "Show changed"));
});

document.querySelector("#popup-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const message = document.querySelector("#popup-message").value.trim();
  const seconds = Number(document.querySelector("#popup-seconds").value);
  if (message) submitCommand(() => publish({ msg: message, secs: seconds }, "Popup sent"));
});

addEventListener("beforeunload", disconnect);
