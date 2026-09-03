"""Safe process-level control for the standalone Maelys Egress daemon."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import queue
import secrets
import shutil
import signal
import subprocess
import tempfile
import threading
import time
from typing import Callable, Iterable, Mapping, Optional
from urllib.parse import quote
from urllib.request import urlopen

__all__ = ["Destination", "EgressConfig", "EgressProcess"]
__version__ = "0.13.0"

_LIFECYCLE_CONTRACT = "maelys-egress-lifecycle/1"


def _url_host(host: str) -> str:
    return f"[{host}]" if ":" in host else host


@dataclass(frozen=True)
class Destination:
    """One exact TCP destination admitted by the standalone policy."""

    host: str
    port: int
    allow_private: bool = False
    require_tls_sni: bool = False

    def directive(self) -> str:
        if not self.host or any(c.isspace() for c in self.host):
            raise ValueError("destination host must be non-empty without whitespace")
        if not 1 <= self.port <= 65535:
            raise ValueError("destination port must be in 1..65535")
        if self.allow_private and self.require_tls_sni:
            raise ValueError(
                "the standalone format has no combined private-address/SNI directive"
            )
        key = (
            "allow_tls_sni"
            if self.require_tls_sni
            else "allow_private"
            if self.allow_private
            else "allow"
        )
        return f"{key} = {self.host}:{self.port}"


@dataclass(frozen=True)
class EgressConfig:
    """Strict standalone configuration rendered without interpolation."""

    destinations: tuple[Destination, ...]
    listen_host: str = "127.0.0.1"
    listen_port: int = 0
    admin_host: str = "127.0.0.1"
    admin_port: int = 0
    max_connections: int = 128
    quota_connections: int = 0
    quota_bytes: int = 0
    quota_total_bytes: int = 0

    def __init__(self, destinations: Iterable[Destination], **values: object) -> None:
        object.__setattr__(self, "destinations", tuple(destinations))
        for name, field in self.__dataclass_fields__.items():
            if name == "destinations":
                continue
            object.__setattr__(self, name, values.pop(name, field.default))
        if values:
            raise TypeError(f"unknown EgressConfig fields: {', '.join(sorted(values))}")
        self._validate()

    def _validate(self) -> None:
        if not self.destinations:
            raise ValueError("at least one destination is required")
        if self.listen_host not in ("127.0.0.1", "::1"):
            raise ValueError("the process SDK creates loopback listeners only")
        if self.admin_host not in ("127.0.0.1", "::1"):
            raise ValueError("the operations listener must be loopback")
        for value, name, maximum in (
            (self.listen_port, "listen_port", 65535),
            (self.admin_port, "admin_port", 65535),
            (self.max_connections, "max_connections", 1_000_000),
            (self.quota_connections, "quota_connections", 1_000_000),
            (self.quota_bytes, "quota_bytes", (1 << 63) - 1),
            (self.quota_total_bytes, "quota_total_bytes", (1 << 63) - 1),
        ):
            if not isinstance(value, int) or not 0 <= value <= maximum:
                raise ValueError(f"invalid {name}")
        if self.max_connections == 0:
            raise ValueError("max_connections must be positive")
        for destination in self.destinations:
            if not isinstance(destination, Destination):
                raise TypeError("destinations must contain Destination values")
            destination.directive()

    def render(self, token_path: Path) -> str:
        lines = [
            "schema_version = 1",
            f"listen = {self.listen_host}:{self.listen_port}",
            f"admin_listen = {self.admin_host}:{self.admin_port}",
            f"token_file = {token_path}",
            f"max_connections = {self.max_connections}",
            f"quota_connections = {self.quota_connections}",
            f"quota_bytes = {self.quota_bytes}",
            f"quota_total_bytes = {self.quota_total_bytes}",
        ]
        lines.extend(destination.directive() for destination in self.destinations)
        return "\n".join(lines) + "\n"


class EgressProcess:
    """Own one daemon, its private files and policy-only reload lifecycle."""

    def __init__(
        self,
        config: EgressConfig,
        *,
        binary: str = "maelys-egress",
        startup_timeout: float = 10.0,
        stderr: object = None,
        on_event: Optional[Callable[[Mapping[str, object]], None]] = None,
    ) -> None:
        self.config = config
        self.binary = binary
        self.startup_timeout = startup_timeout
        self.stderr = stderr
        self.on_event = on_event
        self.process: Optional[subprocess.Popen[bytes]] = None
        self.directory: Optional[Path] = None
        self.secret: Optional[str] = None
        self.proxy_port = 0
        self.admin_port = 0
        self.policy_digest = ""
        self.listen_identity = ""
        self._events: queue.Queue[Mapping[str, object]] = queue.Queue()
        self._ready_signal = threading.Event()
        self._lifecycle_error: Optional[BaseException] = None
        self._stdout_thread: Optional[threading.Thread] = None

    @staticmethod
    def _write_private(path: Path, content: str) -> None:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(content)

    def _consume_lifecycle(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        try:
            for raw_line in self.process.stdout:
                if len(raw_line) > 1 << 20 or not raw_line.endswith(b"\n"):
                    raise RuntimeError("maelys-egress lifecycle line is invalid or exceeds 1 MiB")
                event = json.loads(raw_line)
                if not isinstance(event, dict) or event.get("schemaVersion") != 1 or \
                        event.get("contract") != _LIFECYCLE_CONTRACT or \
                        not isinstance(event.get("event"), str):
                    raise RuntimeError("invalid maelys-egress lifecycle event")
                self._events.put(event)
                if event["event"] == "ready":
                    proxy = event.get("proxy")
                    admin = event.get("admin")
                    policy = event.get("policy")
                    if not isinstance(proxy, dict) or proxy.get("transport") != "tcp" or \
                            not isinstance(proxy.get("host"), str) or \
                            not isinstance(proxy.get("port"), int) or \
                            not isinstance(admin, dict) or \
                            not isinstance(admin.get("port"), int) or \
                            not isinstance(policy, dict) or \
                            not isinstance(policy.get("digest"), str):
                        raise RuntimeError("invalid ready lifecycle event")
                    self.listen_identity = proxy["host"]
                    self.proxy_port = proxy["port"]
                    self.admin_port = admin["port"]
                    self.policy_digest = policy["digest"]
                    self._ready_signal.set()
                elif event["event"] == "fatal" and not self._ready_signal.is_set():
                    self._lifecycle_error = RuntimeError(
                        str(event.get("message", "maelys-egress failed before ready"))
                    )
                    self._ready_signal.set()
                if self.on_event is not None:
                    try:
                        self.on_event(event)
                    except BaseException as callback_error:
                        # A consumer callback must not stop stdout draining.
                        self._lifecycle_error = callback_error
        except BaseException as error:
            self._lifecycle_error = error
            self._ready_signal.set()

    def next_event(self, timeout: Optional[float] = None) -> Mapping[str, object]:
        """Return the next validated lifecycle event, including receipts."""
        return self._events.get(timeout=timeout)

    def start(self) -> "EgressProcess":
        if self.process is not None:
            raise RuntimeError("maelys-egress process already started")
        self._events = queue.Queue()
        self._lifecycle_error = None
        self.directory = Path(tempfile.mkdtemp(prefix="maelys-egress-sdk-"))
        os.chmod(self.directory, 0o700)
        self.secret = secrets.token_urlsafe(32)
        token_path = self.directory / "token"
        config_path = self.directory / "egress.conf"
        self._write_private(token_path, self.secret)
        self._write_private(config_path, self.config.render(token_path))
        try:
            self.process = subprocess.Popen(
                [self.binary, "serve", "--config", str(config_path), "--non-interactive"],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=self.stderr,
                close_fds=True,
                start_new_session=True,
                # A startup failure is then one agent-cli/v2 envelope on stderr.
                env={**os.environ, "MAELYS_CLI_FORMAT": "json"},
            )
            self._ready_signal.clear()
            self._lifecycle_error = None
            self._stdout_thread = threading.Thread(
                target=self._consume_lifecycle,
                name="maelys-egress-lifecycle",
                daemon=True,
            )
            self._stdout_thread.start()
            if not self._ready_signal.wait(self.startup_timeout):
                raise TimeoutError("maelys-egress did not become ready before deadline")
            if self._lifecycle_error is not None:
                raise self._lifecycle_error
            if self.process.poll() is not None:
                raise RuntimeError(
                    f"maelys-egress exited before ready (status={self.process.returncode})"
                )
            if self.proxy_port == 0 or self.admin_port == 0:
                raise RuntimeError("daemon did not allocate TCP proxy/admin ports")
            return self
        except BaseException:
            self.close()
            raise

    @property
    def proxy_url(self) -> str:
        if not self.secret or not self.proxy_port:
            raise RuntimeError("maelys-egress process is not ready")
        return (
            "http://maelys:"
            + quote(self.secret, safe="")
            + f"@{_url_host(self.config.listen_host)}:{self.proxy_port}"
        )

    def _admin_get(self, path: str) -> bytes:
        if not self.admin_port:
            raise RuntimeError("maelys-egress process is not ready")
        host = _url_host(self.config.admin_host)
        with urlopen(f"http://{host}:{self.admin_port}{path}", timeout=2.0) as reply:
            return reply.read(1 << 20)

    def health(self) -> Mapping[str, object]:
        return json.loads(self._admin_get("/healthz"))

    def metrics(self) -> dict[str, float]:
        result: dict[str, float] = {}
        for raw_line in self._admin_get("/metrics").decode("ascii").splitlines():
            if not raw_line or raw_line.startswith("#"):
                continue
            name, value = raw_line.split(None, 1)
            result[name] = float(value)
        return result

    def replace_destinations(
        self, destinations: Iterable[Destination], *, timeout: float = 5.0
    ) -> int:
        if not self.process or not self.directory:
            raise RuntimeError("maelys-egress process is not ready")
        previous = int(self.metrics()["maelys_egress_policy_generation"])
        values = {
            name: getattr(self.config, name)
            for name in self.config.__dataclass_fields__
            if name != "destinations"
        }
        candidate = EgressConfig(destinations, **values)
        token_path = self.directory / "token"
        replacement = self.directory / "egress.conf.new"
        self._write_private(replacement, candidate.render(token_path))
        os.replace(replacement, self.directory / "egress.conf")
        self.process.send_signal(signal.SIGHUP)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            generation = int(self.metrics()["maelys_egress_policy_generation"])
            if generation > previous:
                self.config = candidate
                return generation
            time.sleep(0.02)
        raise TimeoutError("policy generation did not advance after SIGHUP")

    def close(self) -> None:
        process, self.process = self.process, None
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5.0)
        if process is not None and process.stdout is not None:
            process.stdout.close()
        lifecycle_thread, self._stdout_thread = self._stdout_thread, None
        if lifecycle_thread is not None and lifecycle_thread is not threading.current_thread():
            lifecycle_thread.join(timeout=1.0)
        directory, self.directory = self.directory, None
        if directory is not None:
            shutil.rmtree(directory, ignore_errors=True)
        self.secret = None
        self.proxy_port = 0
        self.admin_port = 0

    def __enter__(self) -> "EgressProcess":
        return self.start()

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        self.close()
