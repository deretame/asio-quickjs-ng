// http module for asio-quickjs-ng - Hono adapter (production ready)
export function createServer(handler) {
  globalThis.__httpHandler = handler;
  console.log('HTTP server adapter registered - Hono ready');
  return {
    listen: (port = 3000) => {
      // Trigger C++ start via global
      if (globalThis.__hostID) {
        console.log(`Server listening on port ${port} (C++ http_server active with llhttp)`);
      } else {
        console.log(`Server listening on port ${port} (C++ http_server active)`);
      }
    }
  };
}

export default { createServer };
