// Strona działa bez internetu po pierwszym otwarciu (w terenie telefon łączy się tylko z Heltec).
// Obejrzane fragmenty mapy i biblioteka Leaflet zostają w pamięci telefonu.
const CACHE = "rid-phone-v16";
const TILES = "rid-tiles-v1";
const MAX_TILES = 1500;  // ok. 20-40 MB
const FILES = ["./", "index.html", "manifest.webmanifest", "favicon.png", "icon-192.png", "icon-512.png", "nosleep.mp4"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).then(() => self.skipWaiting()));
});

self.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k !== CACHE && k !== TILES).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

async function trimTiles(cache) {
  const keys = await cache.keys();
  for (let i = 0; i < keys.length - MAX_TILES; i++) await cache.delete(keys[i]);  // najstarsze pierwsze
}

// kafelki mapy i Leaflet: najpierw pamięć telefonu (rzadko się zmieniają), potem sieć
async function cacheFirst(req, name) {
  const cache = await caches.open(name);
  const hit = await cache.match(req);
  if (hit) return hit;
  const res = await fetch(req);
  if (res.ok || res.type === "opaque") {
    await cache.put(req, res.clone());
    if (name === TILES && Math.random() < 0.05) trimTiles(cache);
  }
  return res;
}

self.addEventListener("fetch", (e) => {
  if (e.request.method !== "GET") return;
  const url = new URL(e.request.url);
  if (url.hostname === "tile.openstreetmap.org" || /^mt[0-3]\.google\.com$/.test(url.hostname)) {
    e.respondWith(cacheFirst(e.request, TILES));
    return;
  }
  if (url.hostname === "unpkg.com") {
    e.respondWith(cacheFirst(e.request, CACHE));
    return;
  }
  // strona: najpierw sieć (nowa wersja), bez sieci - kopia z pamięci telefonu
  e.respondWith(
    fetch(e.request)
      .then((r) => {
        const copy = r.clone();
        caches.open(CACHE).then((c) => c.put(e.request, copy));
        return r;
      })
      .catch(() => caches.match(e.request).then((r) => r || caches.match("index.html"))),
  );
});
