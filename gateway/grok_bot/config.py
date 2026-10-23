"""Fail-closed deployment configuration; no credentials are written to disk."""

from dataclasses import dataclass, field
import os
import re
from urllib.parse import urlsplit

CONTROL_SCOPE = "passport:control"
DEVICE_ID_PATTERN = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}\Z")


def https_url(value: str, name: str) -> str:
    parsed = urlsplit(value)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.query
        or parsed.fragment
        or any(char.isspace() for char in value)
    ):
        raise ValueError(f"{name} must be an absolute HTTPS URL without credentials, query or fragment")
    # Accessing port also rejects malformed/out-of-range ports.
    _ = parsed.port
    return value


@dataclass(frozen=True)
class Config:
    public_url: str
    issuer: str
    jwks_url: str
    device_id: str
    device_token: str = field(repr=False)
    oauth_subject: str = field(repr=False)
    port: int = 8080
    allowed_origins: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        https_url(self.public_url, "PASSPORT_PUBLIC_URL")
        if urlsplit(self.public_url).path != "/mcp":
            raise ValueError("PASSPORT_PUBLIC_URL path must be exactly /mcp")
        https_url(self.issuer, "PASSPORT_OAUTH_ISSUER")
        https_url(self.jwks_url, "PASSPORT_OAUTH_JWKS_URL")
        if not self.oauth_subject or len(self.oauth_subject) > 256 or any(ord(c) < 32 for c in self.oauth_subject):
            raise ValueError("PASSPORT_OAUTH_SUBJECT must be the exact nonempty owner subject claim")
        if not DEVICE_ID_PATTERN.fullmatch(self.device_id):
            raise ValueError("PASSPORT_DEVICE_ID must be a 1..32 character alphanumeric slug")
        if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", self.device_token):
            raise ValueError("PASSPORT_DEVICE_TOKEN must contain 32..128 random URL-safe characters")
        if type(self.port) is not int or not 1024 <= self.port <= 65535:
            raise ValueError("PASSPORT_PORT must be an integer between 1024 and 65535")
        for origin in self.allowed_origins:
            https_url(origin, "PASSPORT_ALLOWED_ORIGINS")
            if urlsplit(origin).path or "*" in origin:
                raise ValueError("Allowed origins must be exact HTTPS origins without paths or wildcards")

    @property
    def public_host(self) -> str:
        return urlsplit(self.public_url).netloc

    @classmethod
    def from_env(cls) -> "Config":
        required = (
            "PASSPORT_PUBLIC_URL", "PASSPORT_OAUTH_ISSUER",
            "PASSPORT_OAUTH_JWKS_URL", "PASSPORT_DEVICE_ID", "PASSPORT_DEVICE_TOKEN",
            "PASSPORT_OAUTH_SUBJECT",
        )
        for name in required:
            if not os.environ.get(name):
                raise ValueError(f"Missing required environment variable: {name}")
        return cls(
            public_url=os.environ[required[0]],
            issuer=os.environ[required[1]],
            jwks_url=os.environ[required[2]],
            device_id=os.environ[required[3]],
            device_token=os.environ[required[4]],
            oauth_subject=os.environ[required[5]],
            port=int(os.environ.get("PASSPORT_PORT", "8080")),
            allowed_origins=tuple(
                value.strip() for value in os.environ.get("PASSPORT_ALLOWED_ORIGINS", "").split(",")
                if value.strip()
            ),
        )
