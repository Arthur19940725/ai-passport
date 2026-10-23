"""HTTP boundary cases for later execution; these never call a live OAuth issuer."""

import unittest

from starlette.testclient import TestClient

from gateway.grok_bot.config import Config
from gateway.grok_bot.server import create_app
from gateway.grok_bot.tests.test_state import poll_payload


class HTTPBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.config = Config(
            public_url="https://passport.example.com/mcp", issuer="https://identity.example.com/",
            jwks_url="https://identity.example.com/keys", device_id="passport-01",
            device_token="unit-test-placeholder-" + "x" * 32, oauth_subject="owner-subject",
        )
        self.headers = {"Authorization": "Bearer " + self.config.device_token}
        self.client = self.enterContext(TestClient(create_app(self.config), base_url="https://passport.example.com"))

    def test_device_token_is_required_and_mcp_does_not_accept_it(self):
        self.assertEqual(self.client.post("/device/poll", json=poll_payload()).status_code, 401)
        self.assertEqual(self.client.post("/mcp", json={}, headers=self.headers).status_code, 401)

    def test_valid_device_poll_returns_bounded_protocol_response(self):
        response = self.client.post("/device/poll", json=poll_payload(), headers=self.headers)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json(), {"version": 1, "command": None})
        self.assertLessEqual(len(response.content), 1024)

    def test_host_and_origin_are_checked_for_device_routes(self):
        for additional, expected in (
            ({"Host": "attacker.example.com"}, 421),
            ({"Origin": "https://attacker.example.com"}, 403),
            ({"Origin": "null"}, 403),
        ):
            response = self.client.post("/device/poll", json=poll_payload(), headers=self.headers | additional)
            self.assertEqual(response.status_code, expected)

    def test_body_size_is_checked_without_trusting_content_length(self):
        def chunks():
            yield b" " * 1024
            yield b" " * 1025

        response = self.client.post(
            "/device/poll", content=chunks(), headers=self.headers | {"Content-Type": "application/json"},
        )
        self.assertEqual(response.status_code, 413)
        self.assertEqual(self.client.post("/mcp", content=b" " * 16385).status_code, 413)

    def test_malformed_json_and_wrong_device_are_rejected(self):
        response = self.client.post(
            "/device/poll", content=b"{", headers=self.headers | {"Content-Type": "application/json"},
        )
        self.assertEqual(response.status_code, 400)
        response = self.client.post("/device/poll", json=poll_payload(device_id="another-device"), headers=self.headers)
        self.assertEqual(response.status_code, 400)
        self.assertEqual(response.json()["error"], "wrong_device")


if __name__ == "__main__":
    unittest.main()
