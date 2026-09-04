"""JSONL progress events for headless psxrecomp SDK consumers."""

from __future__ import annotations

import json
import sys
import time
from typing import Any, Mapping, Optional, TextIO


class ProgressReporter:
    """Emit coarse phase events for UIs and launchers.

    When ``json_progress`` is true, stdout is reserved for one JSON object
    per line. Human-readable text goes to stderr (or is mirrored as
    ``log`` events when ``mirror_logs`` is true).
    """

    def __init__(
        self,
        *,
        json_progress: bool = False,
        stream: Optional[TextIO] = None,
        log_stream: Optional[TextIO] = None,
        mirror_logs: bool = True,
    ) -> None:
        self.json_progress = bool(json_progress)
        self.stream = stream if stream is not None else sys.stdout
        self.log_stream = log_stream if log_stream is not None else sys.stderr
        self.mirror_logs = bool(mirror_logs)
        self._started = time.perf_counter()

    def _emit(self, payload: Mapping[str, Any]) -> None:
        if not self.json_progress:
            return
        line = json.dumps(payload, sort_keys=True, separators=(",", ":"))
        self.stream.write(line + "\n")
        self.stream.flush()

    def event(self, event: str, **fields: Any) -> None:
        payload: dict[str, Any] = {
            "event": event,
            "t": round(time.perf_counter() - self._started, 3),
        }
        for key, value in fields.items():
            if value is not None:
                payload[key] = value
        self._emit(payload)

    def phase(
        self,
        name: str,
        *,
        pct: Optional[float] = None,
        message: Optional[str] = None,
        **fields: Any,
    ) -> None:
        self.event("phase", phase=name, pct=pct, message=message, **fields)
        if message and not self.json_progress:
            print(message, file=self.log_stream, flush=True)

    def log(self, message: str, *, level: str = "info") -> None:
        if self.json_progress:
            self.event("log", level=level, message=message)
            if self.mirror_logs:
                print(message, file=self.log_stream, flush=True)
        else:
            print(message, file=self.log_stream, flush=True)

    def result(self, **fields: Any) -> None:
        self.event("result", **fields)

    def error(self, message: str, *, code: int = 1, **fields: Any) -> None:
        self.event("error", message=message, code=code, **fields)
        if not self.json_progress:
            print(message, file=self.log_stream, flush=True)
