print("[js] demo: timer + fetch");

setTimeout(() => {
  print("[js] timer fired");
}, 200);

(async () => {
  try {
    print("[js] fetch start");
    const res = await fetch("https://example.com/");
    print("[js] status =", res.status, "ok =", res.ok);
    const text = await res.text();
    print("[js] body bytes =", text.length);
    print("[js] body head =", text.slice(0, 80).replace(/\n/g, " "));
  } catch (e) {
    print("[js] fetch error:", String(e));
  }
})();
