"""Pure-state acceptance cases. Added for later execution, not run during authoring."""

import unittest

from gateway.grok_bot.state import CommandRejected, DeviceState, MAX_HISTORY, ProtocolError, validate_poll


def poll_payload(**changes):
    payload = {
        "version": 1, "device_id": "passport-01", "boot_id": "0123456789abcdef",
        "remote_enabled": True, "battery_percent": 80, "battery_mv": 4100,
        "brightness": 70, "uptime_ms": 1000, "free_heap": 90000,
        "last_result": {"id": "", "ok": True, "error": ""},
    }
    payload.update(changes)
    return payload


class DeviceStateTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.state = DeviceState("passport-01", clock=lambda: self.now)

    def connect(self, **changes):
        return self.state.poll(poll_payload(**changes))

    def test_offline_and_locally_disabled_devices_reject_commands(self):
        with self.assertRaisesRegex(CommandRejected, "device_offline"):
            self.state.enqueue("clear_message")
        self.connect(remote_enabled=False)
        with self.assertRaisesRegex(CommandRejected, "remote_disabled"):
            self.state.enqueue("clear_message")
        self.connect()
        self.now = 15.001
        status = self.state.status()
        self.assertFalse(status["online"])
        self.assertTrue(status["stale"])
        self.assertEqual(status["telemetry"]["battery_percent"], 80)
        with self.assertRaisesRegex(CommandRejected, "device_offline"):
            self.state.enqueue("clear_message")

    def test_queue_requires_matching_ack_and_retries_same_id(self):
        self.connect()
        first = self.state.enqueue("show_message", text="Hello\nPassport")
        second = self.state.enqueue("set_backlight", brightness=30)
        self.assertEqual(first["status"], "queued")
        self.assertEqual(first["execution"], "not_sent")
        self.assertEqual(self.connect()["command"]["id"], first["id"])
        self.assertEqual(self.connect()["command"]["id"], first["id"])
        # A guess for another queued ID must never acknowledge that command.
        self.assertEqual(self.connect(last_result={"id": second["id"], "ok": True, "error": ""})["command"]["id"], first["id"])
        result = {"id": first["id"], "ok": True, "error": ""}
        self.assertEqual(self.connect(last_result=result)["command"]["id"], second["id"])
        self.assertEqual(self.state.command_status(first["id"])["execution"], "confirmed")
        # Duplicate ACK must not acknowledge or replace the next command.
        self.assertEqual(self.connect(last_result=result)["command"]["id"], second["id"])
        self.assertEqual(self.state.command_status(second["id"])["status"], "dispatched")

    def test_expired_dispatched_command_is_unconfirmed_and_never_replayed(self):
        self.connect()
        command = self.state.enqueue("clear_message")
        self.connect()
        self.now = 30
        response = self.connect(last_result={"id": command["id"], "ok": True, "error": ""})
        self.assertIsNone(response["command"])
        result = self.state.command_status(command["id"])
        self.assertEqual((result["status"], result["execution"]), ("expired", "unconfirmed"))

    def test_unsent_expiry_is_not_sent(self):
        self.connect()
        command = self.state.enqueue("clear_message")
        self.now = 30
        self.assertEqual(self.state.command_status(command["id"])["execution"], "not_sent")

    def test_reboot_cancels_old_session_and_rejects_delayed_old_poll(self):
        self.connect()
        first = self.state.enqueue("clear_message")
        self.connect()
        self.assertIsNone(self.connect(boot_id="ffffffffffffffff")["command"])
        result = self.state.command_status(first["id"])
        self.assertEqual((result["status"], result["error"]), ("cancelled", "device_rebooted"))
        with self.assertRaisesRegex(ProtocolError, "retired_boot"):
            self.connect()

    def test_local_disable_cancels_queue(self):
        self.connect()
        first = self.state.enqueue("clear_message")
        self.assertIsNone(self.connect(remote_enabled=False)["command"])
        self.assertEqual(self.state.command_status(first["id"])["error"], "remote_disabled")
        self.assertIsNone(self.connect()["command"])

    def test_queue_capacity_includes_inflight(self):
        self.connect()
        for _ in range(8):
            self.state.enqueue("clear_message")
        self.connect()
        with self.assertRaisesRegex(CommandRejected, "queue_full"):
            self.state.enqueue("clear_message")

    def test_history_is_bounded_and_does_not_retain_messages(self):
        self.connect()
        first_id = None
        for _ in range(MAX_HISTORY + 1):
            result = self.state.enqueue("show_message", text="temporary")
            first_id = first_id or result["id"]
            self.connect()
            self.connect(last_result={"id": result["id"], "ok": False, "error": "screen_unavailable"})
            final = self.state.command_status(result["id"])
            self.assertEqual(final["status"], "device_error")
            self.assertNotIn("text", final)
        self.assertEqual(self.state.command_status(first_id)["status"], "unknown")

    def test_message_and_brightness_boundaries(self):
        self.connect()
        for text in ("", "x" * 161, "\t", "\r", "\x00", "\u4f60\u597d", "\x7f"):
            with self.subTest(text=repr(text)), self.assertRaises(CommandRejected):
                self.state.enqueue("show_message", text=text)
        for brightness in (True, False, "50", 50.5, 9, 101):
            with self.subTest(brightness=brightness), self.assertRaises(CommandRejected):
                self.state.enqueue("set_backlight", brightness=brightness)
        self.state.enqueue("show_message", text="x" * 160)
        self.state.enqueue("set_backlight", brightness=10)
        self.state.enqueue("set_backlight", brightness=100)

    def test_protocol_rejects_coercion_and_out_of_range_data(self):
        for changes in (
            {"version": True}, {"remote_enabled": 1}, {"battery_percent": -2},
            {"battery_mv": 6001}, {"brightness": True}, {"free_heap": -1},
            {"uptime_ms": 1.5}, {"boot_id": "ABCDEF0123456789"},
            {"last_result": {"id": "not-a-uuid", "ok": True, "error": ""}},
            {"extra": "not allowed"},
        ):
            with self.subTest(changes=changes), self.assertRaises(ProtocolError):
                validate_poll(poll_payload(**changes))
        validate_poll(poll_payload(battery_percent=-1, battery_mv=-1))


if __name__ == "__main__":
    unittest.main()
