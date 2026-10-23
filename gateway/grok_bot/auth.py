"""OAuth resource-server verification using an administrator-pinned JWKS URL."""

import asyncio
import json
import logging
import time
from typing import Any

import httpx
import jwt
from mcp.server.auth.provider import AccessToken

from .config import CONTROL_SCOPE, Config

LOGGER = logging.getLogger(__name__)
JWKS_MAX_BYTES = 65536
JWKS_TTL_SECONDS = 300
JWKS_REFRESH_MIN_SECONDS = 10


class JWTVerifier:
    def __init__(self, config: Config):
        self._config = config
        self._keys: dict[str, Any] = {}
        self._loaded_at = float("-inf")
        self._last_attempt = float("-inf")
        self._lock = asyncio.Lock()

    async def _key(self, kid: str) -> Any | None:
        async with self._lock:
            now = time.monotonic()
            if now - self._loaded_at < JWKS_TTL_SECONDS and kid in self._keys:
                return self._keys[kid]
            if now - self._last_attempt < JWKS_REFRESH_MIN_SECONDS:
                return None
            self._last_attempt = now
            try:
                # No token-supplied jku/x5u URLs, redirects, or proxy environment.
                async with asyncio.timeout(5):
                    async with httpx.AsyncClient(timeout=5.0, follow_redirects=False, trust_env=False) as client:
                        async with client.stream("GET", self._config.jwks_url) as response:
                            response.raise_for_status()
                            body = bytearray()
                            async for chunk in response.aiter_bytes():
                                if len(body) + len(chunk) > JWKS_MAX_BYTES:
                                    raise ValueError("JWKS too large")
                                body.extend(chunk)
                document = json.loads(body)
                entries = document.get("keys") if isinstance(document, dict) else None
                if not isinstance(entries, list) or not 1 <= len(entries) <= 32:
                    raise ValueError("Invalid JWKS key list")
                keys = {}
                for entry in entries:
                    if not isinstance(entry, dict):
                        raise ValueError("Invalid JWKS entry")
                    if entry.get("kty") != "RSA" or entry.get("use", "sig") != "sig":
                        continue
                    if entry.get("alg", "RS256") != "RS256":
                        continue
                    key_id = entry.get("kid")
                    if not isinstance(key_id, str) or not 1 <= len(key_id) <= 128 or key_id in keys:
                        raise ValueError("Invalid or duplicated key identifier")
                    if "d" in entry:
                        raise ValueError("JWKS must contain public keys only")
                    key = jwt.PyJWK.from_dict(entry, algorithm="RS256").key
                    if key.key_size < 2048:
                        raise ValueError("RSA signing key is too small")
                    keys[key_id] = key
                if not keys:
                    raise ValueError("JWKS has no supported signing keys")
                self._keys = keys
                self._loaded_at = time.monotonic()
            except (httpx.HTTPError, jwt.PyJWTError, ValueError, TypeError, KeyError, TimeoutError, RecursionError):
                # Do not log endpoint data, access tokens, or library exceptions.
                LOGGER.warning("OAuth signing keys unavailable or invalid; authentication denied")
                return None
            return self._keys.get(kid)

    async def verify_token(self, token: str) -> AccessToken | None:
        if not isinstance(token, str) or not 1 <= len(token) <= 8192:
            return None
        try:
            header = jwt.get_unverified_header(token)
            if header.get("alg") != "RS256":
                return None
            kid = header.get("kid")
            if not isinstance(kid, str) or not 1 <= len(kid) <= 128:
                return None
            key = await self._key(kid)
            if key is None:
                return None
            claims = jwt.decode(
                token, key, algorithms=["RS256"],
                issuer=self._config.issuer, audience=self._config.public_url,
                options={"require": ["exp", "iss", "aud", "scope", "sub"], "strict_aud": True},
                leeway=0,
            )
            if claims["iss"] != self._config.issuer or claims["aud"] != self._config.public_url:
                return None
            if claims["sub"] != self._config.oauth_subject:
                return None
            if type(claims["exp"]) is not int or not isinstance(claims["scope"], str):
                return None
            scopes = claims["scope"].split()
            if CONTROL_SCOPE not in scopes:
                return None
            client_id = claims.get("client_id") or claims.get("azp")
            if not isinstance(client_id, str) or not 1 <= len(client_id) <= 256:
                return None
            return AccessToken(
                token=token, client_id=client_id, scopes=scopes,
                expires_at=claims["exp"], resource=self._config.public_url,
            )
        except (jwt.PyJWTError, ValueError, TypeError, KeyError, RecursionError):
            return None
