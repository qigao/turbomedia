const signals = {
  connecting: "连接中", live: "在线", offline: "断流", unsupported: "不支持的编码"
};
const archiveStates = {
  ready: "", pending: "正在保存故障前录像", unavailable: "没有可回放的片段", none: ""
};
const feeds = document.querySelector("#feeds");
const list = document.querySelector("#incidents");
const connection = document.querySelector("#connection");
const notice = document.querySelector("#notice");
const pause = document.querySelector("#pause");
const playback = document.querySelector("#playback");
const playbackTitle = document.querySelector("#playback-title");
const video = document.querySelector("#clip");
const cards = new Map();
const entries = new Map();
const clock = new Intl.DateTimeFormat(undefined, {timeStyle: "medium"});
let paused = false;
let playbackTrigger;

function showClip(source, trigger) {
  playbackTitle.textContent = `${source.name} · 故障前录像`;
  video.src = `/api/recording/${source.id}/${source.incident}`;
  playback.hidden = false;
  playbackTrigger = trigger;
  playbackTitle.focus();
}

function closeClip() {
  video.pause();
  video.removeAttribute("src");
  video.load();
  playback.hidden = true;
  if (playbackTrigger?.isConnected) playbackTrigger.focus();
  else pause.focus();
}

async function acknowledge(source, button) {
  button.disabled = true;
  button.dataset.busy = "true";
  try {
    const response = await fetch(`/api/ack/${source.id}/${source.incident}`, {
      method: "POST",
      headers: {"X-Monitor-Action": "acknowledge"}
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    button.textContent = "已确认";
    notice.textContent = `${source.name}告警已确认`;
  } catch {
    notice.textContent = "确认失败，请刷新状态后重试";
    button.disabled = false;
  } finally {
    delete button.dataset.busy;
  }
}

function updateFeed(source) {
  let card = cards.get(source.id);
  if (!card) {
    card = document.querySelector("#feed-template").content.firstElementChild.cloneNode(true);
    card.querySelector("h3").textContent = source.name;
    card.querySelector("img").alt = `${source.name}的视频预览`;
    feeds.append(card);
    cards.set(source.id, card);
  }
  card.dataset.signal = source.signal;
  card.querySelector(".signal").textContent = signals[source.signal];
  card.querySelector(".last-frame").textContent = source.lastFrame
    ? `最后收到画面 ${clock.format(source.lastFrame)}` : "尚未收到画面";
  if (!paused && source.preview && card.dataset.preview !== String(source.preview)) {
    const image = card.querySelector("img");
    image.src = `/api/preview/${source.id}?v=${source.preview}`;
    image.hidden = false;
    card.querySelector(".placeholder").hidden = true;
    card.dataset.preview = String(source.preview);
  }
}

function updateIncident(source) {
  const key = `${source.id}/${source.incident}`;
  let item = entries.get(key);
  if (!item) {
    item = document.querySelector("#incident-template").content.firstElementChild.cloneNode(true);
    item.querySelector("h3").textContent = source.name;
    list.append(item);
    entries.set(key, item);
  }
  item.querySelector(".description").textContent =
    `${source.recovered ? "已恢复" : "待恢复"} · ${clock.format(source.opened)}`;
  item.querySelector(".archive").textContent = archiveStates[source.clip];
  const review = item.querySelector(".review");
  const ack = item.querySelector(".ack");
  review.hidden = source.clip !== "ready";
  review.onclick = () => showClip(source, review);
  ack.textContent = source.acknowledged ? "已确认" : "确认";
  ack.disabled = source.acknowledged || ack.dataset.busy === "true";
  ack.onclick = () => acknowledge(source, ack);
  return key;
}

async function refresh() {
  try {
    const response = await fetch("/api/status", {cache: "no-store"});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const sources = await response.json();
    const active = new Set();
    let online = 0;
    for (const source of sources) {
      updateFeed(source);
      if (source.signal === "live") online++;
      if (source.incident) active.add(updateIncident(source));
    }
    const summary = `${online} / ${sources.length} 路在线`;
    if (connection.textContent !== summary) connection.textContent = summary;
    for (const [key, item] of entries) {
      if (!active.has(key)) {
        item.remove();
        entries.delete(key);
      }
    }
    document.querySelector("#empty").hidden = active.size > 0;
    const count = document.querySelector("#incident-count");
    const total = active.size ? `${active.size} 条` : "";
    if (count.textContent !== total) count.textContent = total;
  } catch {
    if (connection.textContent !== "监控服务连接中断")
      connection.textContent = "监控服务连接中断";
  } finally {
    window.setTimeout(refresh, 500);
  }
}

pause.addEventListener("click", () => {
  paused = !paused;
  pause.setAttribute("aria-pressed", String(paused));
  pause.textContent = paused ? "继续预览" : "暂停预览";
});
document.querySelector("#close-playback").addEventListener("click", closeClip);
refresh();
