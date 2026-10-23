"""OAuth acceptance cases using ephemeral test keys; no live issuer calls."""

import time
import unittest
from unittest.mock import AsyncMock

from cryptography.hazmat.primitives.asymmetric import rsa
import jwt

from gateway.grok_bot.auth import JWTVerifier
from gateway.grok_bot.config import Config


class TokenTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    def setUp(self):
        self.config = Config(
            public_url="https://passport.example.com/mcp", issuer="https://identity.example.com/",
            jwks_url="https://identity.example.com/keys", device_id="passport-01",
            device_token="unit-test-placeholder-" + "x" * 32, oauth_subject="owner-subject",
        )
        self.verifier = JWTVerifier(self.config)
        self.verifier._key = AsyncMock(return_value=self.key.public_key())

    def token(self, **changes):
        claims = {
            "iss": self.config.issuer, "aud": self.config.public_url,
            "sub": "owner-subject", "exp": int(time.time()) + 300,
            "scope": "passport:control", "client_id": "test-client",
        }
        claims.update(changes)
        return jwt.encode(claims, self.key, algorithm="RS256", headers={"kid": "test-key"})

    async def test_accepts_owner_signed_token_with_resource_scope(self):
        token = await self.verifier.verify_token(self.token())
        self.assertIsNotNone(token)
        self.assertEqual(token.resource, self.config.public_url)

    async def test_rejects_wrong_issuer_audience_owner_scope_or_expiry(self):
        for changes in (
            {"iss": "https://other.example.com/"}, {"aud": "https://passport.example.com"},
            {"aud": [self.config.public_url]}, {"sub": "another-owner"},
            {"scope": "passport:read"}, {"exp": int(time.time()) - 1},
            {"exp": "2000000000"}, {"client_id": ""},
        ):
            with self.subTest(changes=changes):
                self.assertIsNone(await self.verifier.verify_token(self.token(**changes)))

    async def test_rejects_disallowed_algorithm_before_jwks_lookup(self):
        token = jwt.encode({"exp": int(time.time()) + 300}, "test-only-signing-key", algorithm="HS256")
        self.assertIsNone(await self.verifier.verify_token(token))
        self.verifier._key.assert_not_awaited()

    async def test_rejects_unknown_signing_key_and_invalid_signature(self):
        self.verifier._key.return_value = None
        self.assertIsNone(await self.verifier.verify_token(self.token()))
        other_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
        self.verifier._key.return_value = other_key.public_key()
        self.assertIsNone(await self.verifier.verify_token(self.token()))

    async def test_rejects_oversized_or_malformed_tokens(self):
        for token in ("", "x" * 8193, "not.a.jwt"):
            self.assertIsNone(await self.verifier.verify_token(token))


class ConfigTests(unittest.TestCase):
    def test_insecure_urls_and_empty_secrets_fail_closed(self):
        values = dict(
            public_url="https://passport.example.com/mcp", issuer="https://identity.example.com/",
            jwks_url="https://identity.example.com/keys", device_id="passport-01",
            device_token="unit-test-placeholder-" + "x" * 32, oauth_subject="owner-subject",
        )
        for changes in (
            {"public_url": "http://passport.example.com/mcp"},
            {"jwks_url": "http://identity.example.com/keys"},
            {"public_url": "https://passport.example.com/other"},
            {"device_token": ""}, {"oauth_subject": ""},
            {"allowed_origins": ("https://*.example.com",)},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                Config(**(values | changes))


if __name__ == "__main__":
    unittest.main()
