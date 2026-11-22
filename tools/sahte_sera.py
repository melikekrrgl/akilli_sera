#!/usr/bin/env python3
"""Sahte sera: donanım olmadan paneli denemek için ESP32'yi taklit eder.

Kurulum:  pip install paho-mqtt
Çalıştır: python3 sahte_sera.py --id sera-demo-4821

ESP32 ile aynı MQTT konularını ve komutlarını kullanır; telefondaki
panelden pompa/fan/mod/ayar komutlarını gönderip tepkisini görebilirsiniz.
"""
import argparse
import json
import math
import random
import time

import paho.mqtt.client as mqtt

p = argparse.ArgumentParser()
p.add_argument("--id", default="sera-demo-4821")
p.add_argument("--host", default="broker.hivemq.com")
p.add_argument("--port", type=int, default=1883)
p.add_argument("--image", help="Kamera yerine gönderilecek bir JPEG dosyası (isteğe bağlı)")
args = p.parse_args()

BASE = f"akillisera/{args.id}"
cfg = dict(tMax=30.0, hMax=80.0, soilMin=40, pumpSec=3, soakSec=30, pumpMaxSec=15, fanSpeed=100, mode="auto")
st = dict(t=27.0, h=60.0, soil=55.0, lux=350, water=80.0, pump=False, fan=False)
pump_since = 0.0


def publish_config():
    c.publish(f"{BASE}/config", json.dumps(cfg), retain=True)


def event(level, msg):
    c.publish(f"{BASE}/event", json.dumps({"level": level, "msg": msg}, ensure_ascii=False))
    print(f"[OLAY] {level}: {msg}")


def telemetry():
    d = {
        "t": round(st["t"], 1), "h": round(st["h"]), "soil": round(st["soil"]), "lux": st["lux"],
        "water": round(st["water"]), "waterLow": st["water"] < 15, "pump": st["pump"], "fan": st["fan"],
        "fanSpeed": cfg["fanSpeed"], "mode": cfg["mode"], "sensorError": False, "rssi": -55,
        "up": int(time.time() - start),
    }
    c.publish(f"{BASE}/telemetry", json.dumps(d))


def set_pump(on):
    global pump_since
    if on and st["water"] < 15:
        event("warn", "Depoda su az, pompa çalıştırılmadı")
        on = False
    st["pump"] = on
    pump_since = time.time() if on else pump_since


def on_connect(cl, *_):
    print("Broker'a bağlandı, konu:", BASE)
    cl.publish(f"{BASE}/status", "online", retain=True)
    cl.subscribe(f"{BASE}/cmd")
    cl.subscribe(f"{BASE}/cam/cmd")
    cl.publish(f"{BASE}/cam/status", "online", retain=True)
    publish_config()
    telemetry()
    event("info", "Sera çevrimiçi (sahte cihaz)")


def on_message(cl, _ud, msg):
    if msg.topic.endswith("/cam/cmd"):
        print("Kamera komutu:", msg.payload)
        if args.image:
            with open(args.image, "rb") as f:
                cl.publish(f"{BASE}/cam/image", f.read(), retain=True)
        return
    try:
        d = json.loads(msg.payload)
    except ValueError:
        return
    print("Komut:", d)
    if "mode" in d:
        cfg["mode"] = d["mode"]
    if "pump" in d:
        cfg["mode"] = "manual"
        set_pump(bool(d["pump"]))
    if "fan" in d:
        cfg["mode"] = "manual"
        st["fan"] = bool(d["fan"])
    if "fanSpeed" in d:
        cfg["fanSpeed"] = int(d["fanSpeed"])
    if "config" in d:
        cfg.update({k: v for k, v in d["config"].items() if k in cfg})
        event("info", "Ayarlar güncellendi")
    publish_config()
    telemetry()


c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"sera-sahte-{random.randrange(1 << 24):06x}")
c.will_set(f"{BASE}/status", "offline", retain=True)
c.on_connect = on_connect
c.on_message = on_message
c.connect(args.host, args.port, keepalive=30)
c.loop_start()
start = time.time()

try:
    while True:
        time.sleep(1)
        k = time.time() - start
        st["t"] = 27 + 4 * math.sin(k / 90) + (1.5 if not st["fan"] else -1.0) * min(1, k / 60)
        st["h"] = 62 + 8 * math.sin(k / 70) - (6 if st["fan"] else 0)
        st["lux"] = int(350 + 120 * math.sin(k / 50))
        st["soil"] += 2.5 if st["pump"] else -0.25
        st["soil"] = max(0, min(100, st["soil"]))
        if st["pump"]:
            st["water"] = max(0, st["water"] - 1.5)
            limit = cfg["pumpSec"] if cfg["mode"] == "auto" else cfg["pumpMaxSec"]
            if time.time() - pump_since >= limit:
                set_pump(False)
        if cfg["mode"] == "auto":
            if not st["pump"] and st["soil"] < cfg["soilMin"] and time.time() - pump_since > cfg["soakSec"]:
                set_pump(True)
                event("info", "Toprak kuru, sulama başladı")
            hot = st["t"] > cfg["tMax"] or st["h"] > cfg["hMax"]
            if hot and not st["fan"]:
                st["fan"] = True
                event("info", "Sıcaklık/nem yüksek, fan açıldı")
            elif st["fan"] and st["t"] < cfg["tMax"] - 1 and st["h"] < cfg["hMax"] - 3:
                st["fan"] = False
        if int(k) % 5 == 0:
            telemetry()
except KeyboardInterrupt:
    c.publish(f"{BASE}/status", "offline", retain=True)
    c.loop_stop()
