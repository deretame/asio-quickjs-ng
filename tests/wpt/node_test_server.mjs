// Controllable HTTP fixture for fetch WPT-style tests (replaces wptserve subset).
// Prints: READY http://127.0.0.1:<port>
import http from "node:http";

const server = http.createServer(async (req, res) => {
  const u = new URL(req.url || "/", "http://127.0.0.1");
  const path = u.pathname;
  const method = req.method || "GET";

  const chunks = [];
  for await (const c of req) chunks.push(c);
  const reqBody = Buffer.concat(chunks);

  const send = (status, headers, body) => {
    const buf = Buffer.isBuffer(body) ? body : Buffer.from(body ?? "");
    res.writeHead(status, {
      "Content-Length": buf.length,
      Connection: "close",
      "X-Test-Server": "node-asio-qjs",
      ...headers,
    });
    if (method === "HEAD") {
      res.end();
    } else {
      res.end(buf);
    }
  };

  // delay helper: /slow?ms=800
  const delayMs = (ms) => new Promise((r) => setTimeout(r, ms));

  try {
    if (path === "/resources/data.json" || path === "/data.json" || path === "/json") {
      const body =
        path === "/json"
          ? JSON.stringify({ hello: "world" })
          : JSON.stringify({ key: "value" });
      return send(200, { "Content-Type": "application/json" }, body);
    }

    if (path === "/" || path === "/text") {
      return send(200, { "Content-Type": "text/plain; charset=utf-8" }, "Hello WPT");
    }

    if (path === "/echo") {
      const ct = req.headers["content-type"] || "application/octet-stream";
      return send(200, { "Content-Type": ct }, reqBody);
    }

    if (path.startsWith("/status/")) {
      const code = parseInt(path.slice("/status/".length), 10) || 404;
      return send(code, { "Content-Type": "text/plain" }, "status-body");
    }

    if (path === "/redirect" || path === "/redirect/text") {
      return send(302, { Location: "/text" }, "");
    }

    if (path === "/redirect/json") {
      return send(302, { Location: "/json" }, "");
    }

    if (path === "/headers") {
      const out = {
        method,
        "x-test": req.headers["x-test"] || null,
        "content-type": req.headers["content-type"] || null,
        accept: req.headers["accept"] || null,
      };
      return send(200, { "Content-Type": "application/json" }, JSON.stringify(out));
    }

    if (path === "/headers/all") {
      // lower-case keys as Node provides
      return send(
        200,
        { "Content-Type": "application/json" },
        JSON.stringify({ method, ...req.headers })
      );
    }

    if (path === "/method") {
      return send(200, { "Content-Type": "text/plain" }, method);
    }

    if (path === "/redirect/method") {
      return send(302, { Location: "/method" }, "");
    }

    if (path === "/concurrent") {
      const id = u.searchParams.get("id") || "0";
      return send(200, { "Content-Type": "application/json" }, JSON.stringify({ id }));
    }

    if (path === "/empty") {
      return send(200, { "Content-Type": "text/plain" }, "");
    }

    if (path === "/sleep") {
      await delayMs(2000);
      return send(200, { "Content-Type": "text/plain" }, "sleep-ok");
    }

    if (path === "/slow" || path === "/infinite-slow-response") {
      const ms = Math.min(parseInt(u.searchParams.get("ms") || "500", 10) || 500, 10000);
      await delayMs(ms);
      return send(200, { "Content-Type": "text/plain" }, "slow-ok");
    }

    // chunked-ish large body for abort-while-reading style tests
    if (path === "/large") {
      const n = Math.min(parseInt(u.searchParams.get("n") || "100000", 10) || 100000, 5_000_000);
      return send(200, { "Content-Type": "text/plain" }, "x".repeat(n));
    }

    return send(404, { "Content-Type": "text/plain" }, `no route: ${path}`);
  } catch (e) {
    return send(500, { "Content-Type": "text/plain" }, String(e));
  }
});

server.listen(0, "127.0.0.1", () => {
  const { port } = server.address();
  process.stdout.write(`READY http://127.0.0.1:${port}\n`);
});

const shutdown = () => {
  server.close(() => process.exit(0));
};
process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
// Parent writes "quit\n" to stdin
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  if (String(chunk).includes("quit")) shutdown();
});
