#!/usr/bin/env python3
# Hammer the board's public pages (Info, Tilastot month + year, CSV) from the PC, to look for memory leaks
# and fragmentation while the serial log shows ">>> mem:" lines. No login needed.
#   python3 firmware/kahvibotti/test/web_stress.py [minutes] [host]
import sys, time, threading, urllib.request, collections

minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 20
host = sys.argv[2] if len(sys.argv) > 2 else "emukahvibotti.local"
pages = ["/", "/tilastot", "/tilastot?v=vuosi", "/tilastot.csv", "/tilastot?y=2025&m=1"]
stats = collections.Counter()
slow = []
lock = threading.Lock()
end = time.time() + minutes * 60

def worker(k):
    i = k
    while time.time() < end:
        p = pages[i % len(pages)]
        i += 1
        t = time.time()
        try:
            with urllib.request.urlopen(f"http://{host}{p}", timeout=20) as r:
                r.read()
                code = r.status
        except urllib.error.HTTPError as e:
            code = e.code
        except Exception as e:
            code = type(e).__name__
        dt = time.time() - t
        with lock:
            stats[code] += 1
            if dt > 5: slow.append((p, round(dt, 1)))
        time.sleep(0.15)  # ~3 workers x ~5 req/s: over the public limit (10 per 2 s) on purpose

threads = [threading.Thread(target=worker, args=(k,)) for k in range(3)]
for t in threads: t.start()
while any(t.is_alive() for t in threads):
    time.sleep(30)
    with lock: print(time.strftime("%H:%M:%S"), dict(stats), "slow>5s:", len(slow), flush=True)
print("done:", dict(stats), "slowest:", sorted(slow, key=lambda x: -x[1])[:5])
