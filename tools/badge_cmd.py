#!/usr/bin/env python3 -u
"""Send a signed command to one badge or to all of them at once.

Every command is signed with the operator key. A badge drops anything that is
not, so this is the only way to control them over MQTT.

  # troll everyone
  badge_cmd.py --broker 192.168.0.241 msg "FREE HUGS AT THE BAR" --secs 30

  # one badge
  badge_cmd.py --broker 192.168.0.241 --badge 68cd2517 name "aask42"

  # what to display: index into the SHOW menu
  badge_cmd.py --broker 192.168.0.241 show 1

Without --badge the command goes to <topic>/all/cmd, which every badge
subscribes to.
"""
import argparse, json, ssl, sys, time
import badgeauth

try:
    import paho.mqtt.client as mqtt
except ImportError:
    sys.exit("paho-mqtt is required:  pip3 install paho-mqtt")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["msg", "name", "show", "raw"])
    ap.add_argument("value")
    ap.add_argument("--secs", type=int, default=20, help="banner duration")
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--topic", default="dc34")
    ap.add_argument("--badge", help="target one badge; default is all")
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--user")
    ap.add_argument("--password")
    ap.add_argument("--key", default=None)
    a = ap.parse_args()

    if a.action == "msg":
        cmd = {"msg": a.value, "secs": a.secs}
    elif a.action == "name":
        cmd = {"setName": a.value}
    elif a.action == "show":
        cmd = {"setShow": int(a.value)}
    else:
        cmd = json.loads(a.value)

    key = badgeauth.load_private(a.key)
    wire = badgeauth.sign_command(cmd, key)

    topic = (f"{a.topic}/badge/{a.badge}/cmd" if a.badge
             else f"{a.topic}/all/cmd")

    try:
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    except AttributeError:
        c = mqtt.Client()
    if a.user:
        c.username_pw_set(a.user, a.password or "")
    if a.tls:
        c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.connect(a.broker, a.port, 60)
    c.loop_start()
    c.publish(topic, wire, qos=0)
    time.sleep(1.0)
    c.loop_stop()

    print(f"sent {cmd} -> {topic}")
    print(f"  {len(wire)} bytes, signed")


if __name__ == "__main__":
    main()
