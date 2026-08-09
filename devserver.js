// Dev-only static server for a staged viewer, so it can be exercised in a normal
// browser instead of Lister. Run the plugin's build first, then from its root:
//   node core/devserver.js test/models
//   http://localhost:8127/viewer.html?src=http://localhost:8127/files/test.glb
const http = require('http'), fs = require('fs'), path = require('path');

const WEB = path.resolve('out', 'stage', 'web');
const FILES = path.resolve(process.argv[2] || 'test');
const TYPES = {
  '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.woff2': 'font/woff2', '.ttf': 'font/ttf', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.gif': 'image/gif',
  '.md': 'text/markdown', '.glb': 'model/gltf-binary', '.gltf': 'model/gltf+json',
  '.pdf': 'application/pdf', '.webp': 'image/webp', '.tif': 'image/tiff',
  '.tiff': 'image/tiff', '.tga': 'image/x-tga',
};

http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
  const file = url.startsWith('/files/')
    ? path.join(FILES, url.slice('/files/'.length))
    : path.join(WEB, url);

  // Keep the server inside its two roots even if the URL walks upwards.
  if (!file.startsWith(WEB) && !file.startsWith(FILES)) return res.writeHead(403).end('forbidden');

  fs.readFile(file, (err, data) => {
    if (err) return res.writeHead(404).end('not found');
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' }).end(data);
  });
}).listen(8127, () => console.log('http://localhost:8127/viewer.html'));
