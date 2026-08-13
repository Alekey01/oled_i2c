/* Service worker de BitCat Watch.
 *
 * El unico objetivo es que la app abra sin red: el enlace BLE con el reloj no
 * necesita internet, asi que poner el reloj en hora tiene que funcionar aunque
 * el celular este sin datos. Solo el clima requiere salir a Open-Meteo.
 *
 * Sube CACHE al cambiar cualquier archivo del casco: el nombre nuevo es lo que
 * dispara el borrado del anterior en activate.
 */
// Sube este numero al publicar: es lo que dispara el borrado del cache anterior.
// Debe ir a la par con "version" en manifest.json, que es lo que ve el usuario.
const CACHE = 'bitcat-watch-v1.4.0';

const CASCO = [
  './',
  './index.html',
  './manifest.json',
  './icon-192.png',
  './icon-512.png',
  './icon-maskable-512.png',
  './apple-touch-icon.png',
];

self.addEventListener('install', ev => {
  ev.waitUntil(
    caches.open(CACHE)
      .then(c => c.addAll(CASCO))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', ev => {
  ev.waitUntil(
    caches.keys()
      .then(claves => Promise.all(
        claves.filter(k => k !== CACHE).map(k => caches.delete(k))
      ))
      .then(() => self.clients.claim())
  );
});

// El cliente pide tomar el control cuando el usuario acepta actualizar.
self.addEventListener('message', ev => {
  if (ev.data === 'actualizar') self.skipWaiting();
});

self.addEventListener('fetch', ev => {
  const req = ev.request;

  // Open-Meteo y la geolocalizacion nunca se cachean: un clima viejo servido
  // desde disco seria peor que un error honesto.
  if (req.method !== 'GET' || new URL(req.url).origin !== self.location.origin) return;

  // El firmware tampoco: medio mega que cambia en cada deploy no tiene nada que
  // hacer en el cache, y servir una version vieja seria peor que no servir nada.
  if (new URL(req.url).pathname.includes('/firmware/')) return;

  // Navegacion: red primero, para que un deploy nuevo se vea al recargar con
  // señal. Sin red, cae al casco cacheado.
  if (req.mode === 'navigate') {
    ev.respondWith(
      fetch(req)
        .then(r => {
          const copia = r.clone();
          caches.open(CACHE).then(c => c.put('./index.html', copia));
          return r;
        })
        .catch(() => caches.match('./index.html'))
    );
    return;
  }

  // Iconos y manifest: cache primero, son inmutables dentro de una version.
  ev.respondWith(
    caches.match(req).then(hit => hit || fetch(req).then(r => {
      if (r.ok) {
        const copia = r.clone();
        caches.open(CACHE).then(c => c.put(req, copia));
      }
      return r;
    }))
  );
});
