'use strict';
const CACHE_NAME = 'asgend-shell-v20261112-1';
const SHELL = ['/', '/manifest.webmanifest', '/assets/style.css?v=20261112.1', '/assets/app.js?v=20261112.1', '/icon-192.png', '/icon-512.png'];
const CACHEABLE = new Set(['/', '/index.html', '/manifest.webmanifest', '/assets/style.css', '/assets/app.js', '/icon-192.png', '/icon-512.png']);
self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE_NAME).then(cache => cache.addAll(SHELL)).then(() => self.skipWaiting()));
});
self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key.startsWith('asgend-shell-') && key !== CACHE_NAME).map(key => caches.delete(key)))).then(() => self.clients.claim()));
});
self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);
  if (request.method !== 'GET' || url.origin !== self.location.origin || url.pathname.startsWith('/api/') || url.pathname.startsWith('/v1/')) return;
  if (!CACHEABLE.has(url.pathname)) return;
  event.respondWith(caches.open(CACHE_NAME).then(async cache => {
    const cached = await cache.match(request, {ignoreSearch: true});
    if (cached) return cached;
    const response = await fetch(request);
    if (response.ok && response.type === 'basic') await cache.put(request, response.clone());
    return response;
  }));
});
