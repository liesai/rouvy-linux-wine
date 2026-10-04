#!/usr/bin/env python3
import os
import signal
import socket
import socketserver
import threading
import time

import dbus
import dbus.mainloop.glib
from gi.repository import GLib

BLUEZ = "org.bluez"
ADAPTER = os.environ.get("ROUVY_BLUEZ_ADAPTER", "/org/bluez/hci0")
PORT = 28765
RPC_PORT = 28766

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
target = ("127.0.0.1", PORT)
devices = {}
characteristics = {}
characteristic_flags = {}
connect_locks = {}
connect_locks_guard = threading.Lock()
notification_count = 0


def plain(value):
    if isinstance(value, dbus.Dictionary):
        return {plain(k): plain(v) for k, v in value.items()}
    if isinstance(value, (dbus.Array, list, tuple)):
        return [plain(v) for v in value]
    if isinstance(value, (dbus.ByteArray, bytes, bytearray)):
        return bytes(value)
    if isinstance(value, (dbus.String, str)):
        return str(value)
    if isinstance(value, (dbus.Boolean, bool)):
        return bool(value)
    if isinstance(value, (dbus.Int16, dbus.Int32, dbus.Int64,
                          dbus.UInt16, dbus.UInt32, dbus.UInt64, int)):
        return int(value)
    return value


def emit(props):
    address_text = props.get("Address")
    if not address_text:
        return
    address = address_text.replace(":", "")
    timestamp = int(time.monotonic() * 1000)
    rssi = int(props.get("RSSI", -127))
    name = str(props.get("Name") or props.get("Alias") or "-").replace("\t", " ")
    # ROUVY publishes a discovered device when the generic advertisement packet
    # arrives.  Feed the service/manufacturer frames first so its snapshot
    # already contains the capabilities (Cycling Power, FTMS, etc.).
    for uuid in props.get("UUIDs", []):
        sock.sendto(f"U\t{address}\t{timestamp}\t{rssi}\t{uuid}\n".encode(), target)
    for company, payload in props.get("ManufacturerData", {}).items():
        payload = bytes(payload).hex()
        sock.sendto(f"M\t{address}\t{timestamp}\t{rssi}\t{int(company)}\t{payload}\n".encode(), target)
    sock.sendto(f"D\t{address}\t{timestamp}\t{rssi}\t{name}\t0\t6\n".encode(), target)
    # Repeat service frames after the generic packet too.  ROUVY can publish
    # the named peripheral on that packet; the second copy then merges the
    # capabilities into an already-existing scan entry regardless of event
    # scheduling order on Unity's main thread.
    for uuid in props.get("UUIDs", []):
        sock.sendto(f"U\t{address}\t{timestamp}\t{rssi}\t{uuid}\n".encode(), target)


def interfaces_added(path, interfaces):
    if "org.bluez.Device1" not in interfaces:
        return
    devices[str(path)] = plain(interfaces["org.bluez.Device1"])
    emit(devices[str(path)])


def interfaces_removed(path, interfaces):
    if "org.bluez.Device1" in interfaces:
        devices.pop(str(path), None)


def properties_changed(interface, changed, invalidated, path=None):
    global notification_count
    if path is None:
        return
    if interface == "org.bluez.Device1":
        entry = devices.setdefault(str(path), {})
        entry.update(plain(changed))
        emit(entry)
    elif interface == "org.bluez.GattCharacteristic1" and "Value" in changed:
        objects = plain(manager.GetManagedObjects())
        char = objects.get(str(path), {}).get("org.bluez.GattCharacteristic1", {})
        service_path = str(char.get("Service", ""))
        service = objects.get(service_path, {}).get("org.bluez.GattService1", {})
        device_path = str(service.get("Device", ""))
        device = objects.get(device_path, {}).get("org.bluez.Device1", {})
        address = str(device.get("Address", "")).replace(":", "")
        # WCL's changed callback reports GattCharacteristic.Handle (the
        # declaration handle), not ValueHandle.  ROUVY keys its subscription
        # map with that field; sending +1 makes every notification invisible
        # to the managed service even though the native callback fires.
        handle = int(str(path).rsplit("char", 1)[-1], 16)
        value = bytes(changed["Value"]).hex()
        if address:
            sock.sendto(f"N\t{address}\t{handle}\t{value}\n".encode(), target)
            notification_count += 1
            if notification_count <= 20 or notification_count % 100 == 0:
                print(f"NOTIFY #{notification_count} {address} handle={handle} bytes={len(value) // 2}",
                      flush=True)


def resend():
    current = {}
    for path, interfaces in objects_snapshot().items():
        if "org.bluez.Device1" in interfaces:
            current[str(path)] = plain(interfaces["org.bluez.Device1"])
    devices.clear()
    devices.update(current)
    for props in current.values():
        emit(props)
    return True


def objects_snapshot():
    return plain(manager.GetManagedObjects())


def device_path_for(address):
    wanted = address.upper()
    for path, interfaces in objects_snapshot().items():
        props = interfaces.get("org.bluez.Device1")
        if props and str(props.get("Address", "")).replace(":", "").upper() == wanted:
            return path
    return None


def path_handle(path, marker):
    return int(str(path).rsplit(marker, 1)[-1], 16)


def connect_lock_for(address):
    with connect_locks_guard:
        return connect_locks.setdefault(address, threading.Lock())


def connect_and_resolve(path, address, timeout=75):
    """Serialize BlueZ Connect calls and retry transient LE link failures."""
    deadline = time.monotonic() + timeout
    last_error = "connection-timeout"
    with connect_lock_for(address):
        local_bus = dbus.SystemBus()
        obj = local_bus.get_object(BLUEZ, path)
        device = dbus.Interface(obj, "org.bluez.Device1")
        props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
        while time.monotonic() < deadline:
            try:
                if bool(props.Get("org.bluez.Device1", "ServicesResolved")):
                    print(f"CONNECT {address}: services resolved", flush=True)
                    return None
                if not bool(props.Get("org.bluez.Device1", "Connected")):
                    print(f"CONNECT {address}: BlueZ attempt", flush=True)
                    device.Connect(timeout=25)
            except dbus.exceptions.DBusException as error:
                name = error.get_dbus_name()
                detail = str(error)
                last_error = name + ":" + detail
                transient = (name.endswith("InProgress") or
                             name.endswith("AlreadyConnected") or
                             name.endswith("NoReply") or
                             (name.endswith("Failed") and
                              ("le-connection-abort-by-local" in detail or
                               "Connection attempt failed" in detail)))
                if not transient:
                    return last_error
                print(f"CONNECT {address}: transient {detail}", flush=True)

            # Let BlueZ finish service discovery, but retry if the controller
            # drops back to the disconnected state.
            for _ in range(20):
                if time.monotonic() >= deadline:
                    break
                try:
                    if bool(props.Get("org.bluez.Device1", "ServicesResolved")):
                        print(f"CONNECT {address}: services resolved", flush=True)
                        return None
                    if not bool(props.Get("org.bluez.Device1", "Connected")):
                        break
                except dbus.exceptions.DBusException:
                    break
                time.sleep(0.25)
            time.sleep(0.5)
    return last_error


def rpc(command):
    parts = command.strip().split()
    if not parts:
        return "ERR empty"
    verb = parts[0]
    address = parts[1].upper() if len(parts) > 1 else ""
    path = device_path_for(address)
    if not path and verb == "CONNECT":
        deadline = time.monotonic() + 30
        while not path and time.monotonic() < deadline:
            time.sleep(0.2)
            path = device_path_for(address)
    if not path:
        return "ERR device-not-found"
    device = dbus.Interface(bus.get_object(BLUEZ, path), "org.bluez.Device1")

    if verb == "CONNECT":
        error = connect_and_resolve(path, address)
        return "ERR " + error if error else "OK"

    if verb == "DISCONNECT":
        try:
            device.Disconnect()
        except dbus.exceptions.DBusException as error:
            if not error.get_dbus_name().endswith("NotConnected"):
                return "ERR " + error.get_dbus_name()
        return "OK"

    objects = objects_snapshot()
    if verb == "SERVICES":
        result = []
        for object_path, interfaces in objects.items():
            props = interfaces.get("org.bluez.GattService1")
            if props and str(props.get("Device")) == path:
                result.append(f"{props['UUID']},{path_handle(object_path, 'service')}")
        return "OK " + ";".join(result)

    if verb == "CHARS" and len(parts) >= 3:
        service_handle = int(parts[2])
        service_path = f"{path}/service{service_handle:04x}"
        result = []
        for object_path, interfaces in objects.items():
            props = interfaces.get("org.bluez.GattCharacteristic1")
            if not props or str(props.get("Service")) != service_path:
                continue
            handle = path_handle(object_path, "char")
            flags = set(props.get("Flags", []))
            bits = sum((1 << i) for i, name in enumerate((
                "broadcast", "read", "write", "write-without-response",
                "authenticated-signed-writes", "notify", "indicate",
                "reliable-write")) if name in flags)
            result.append(f"{props['UUID']},{service_handle},{handle},{handle + 1},{bits}")
            characteristics[(address, handle + 1)] = object_path
            characteristic_flags[(address, handle + 1)] = flags
        return "OK " + ";".join(result)

    if len(parts) >= 3 and verb in {"READ", "WRITE", "SUB", "UNSUB"}:
        handle = int(parts[2])
        char_path = characteristics.get((address, handle))
        if not char_path:
            for object_path, interfaces in objects.items():
                props = interfaces.get("org.bluez.GattCharacteristic1")
                if props and object_path.startswith(path + "/") and path_handle(object_path, "char") + 1 == handle:
                    char_path = object_path
                    characteristics[(address, handle)] = object_path
                    characteristic_flags[(address, handle)] = set(props.get("Flags", []))
                    break
        if not char_path:
            return "ERR characteristic-not-found"
        char = dbus.Interface(bus.get_object(BLUEZ, char_path), "org.bluez.GattCharacteristic1")
        if verb == "READ":
            return "OK " + bytes(char.ReadValue({})).hex()
        if verb == "WRITE" and len(parts) >= 4:
            flags = characteristic_flags.get((address, handle), set())
            # BlueZ otherwise chooses a write mode itself.  Be explicit because
            # Elite exposes both request and command-only control points.
            write_type = "command" if "write-without-response" in flags and "write" not in flags else "request"
            print(f"WRITE {address} handle={handle} type={write_type} flags={sorted(flags)} data={parts[3]}",
                  flush=True)
            char.WriteValue(dbus.Array(bytearray.fromhex(parts[3]), signature="y"),
                            {"type": dbus.String(write_type)})
            print(f"WRITE-OK {address} handle={handle}", flush=True)
            return "OK"
        if verb == "SUB":
            char.StartNotify()
            return "OK"
        if verb == "UNSUB":
            try:
                char.StopNotify()
            except dbus.exceptions.DBusException as error:
                if not error.get_dbus_name().endswith("NotPermitted"):
                    raise
            return "OK"
    return "ERR unsupported"


class RpcHandler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            command = self.rfile.readline(8192).decode("ascii", "replace")
            response = rpc(command)
        except Exception as error:
            response = "ERR " + type(error).__name__ + ":" + str(error).replace("\n", " ")
        self.wfile.write((response + "\n").encode())


class RpcServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
try:
    dbus.mainloop.glib.threads_init()
except AttributeError:
    pass
bus = dbus.SystemBus()
manager = dbus.Interface(bus.get_object(BLUEZ, "/"), "org.freedesktop.DBus.ObjectManager")
for path, interfaces in manager.GetManagedObjects().items():
    if "org.bluez.Device1" in interfaces:
        devices[str(path)] = plain(interfaces["org.bluez.Device1"])

bus.add_signal_receiver(interfaces_added, dbus_interface="org.freedesktop.DBus.ObjectManager",
                        signal_name="InterfacesAdded")
bus.add_signal_receiver(interfaces_removed, dbus_interface="org.freedesktop.DBus.ObjectManager",
                        signal_name="InterfacesRemoved")
bus.add_signal_receiver(properties_changed, dbus_interface="org.freedesktop.DBus.Properties",
                        signal_name="PropertiesChanged", path_keyword="path")

adapter = dbus.Interface(bus.get_object(BLUEZ, ADAPTER), "org.bluez.Adapter1")
try:
    adapter.SetDiscoveryFilter({"Transport": dbus.String("le"), "DuplicateData": dbus.Boolean(True)})
    adapter.StartDiscovery()
except dbus.exceptions.DBusException as error:
    if not error.get_dbus_name().endswith("InProgress"):
        raise

loop = GLib.MainLoop()
rpc_server = RpcServer(("127.0.0.1", RPC_PORT), RpcHandler)
rpc_thread = threading.Thread(target=rpc_server.serve_forever, daemon=True)
rpc_thread.start()
GLib.timeout_add_seconds(2, resend)
signal.signal(signal.SIGTERM, lambda *_: loop.quit())
signal.signal(signal.SIGINT, lambda *_: loop.quit())

try:
    resend()
    loop.run()
finally:
    rpc_server.shutdown()
    rpc_server.server_close()
    try:
        adapter.StopDiscovery()
    except dbus.exceptions.DBusException:
        pass
