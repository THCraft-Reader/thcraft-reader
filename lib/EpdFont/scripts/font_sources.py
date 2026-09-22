"""Shared original-font source resolution for bitmap and native SD catalogues."""

import socket
import urllib.parse
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
EPDFONTS_DIR = SCRIPT_DIR.parent
DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts"
PUBLIC_SOURCE_BASE = (
    "https://raw.githubusercontent.com/THCraft-Reader/thcraft-reader/"
    "8c84ef3268fd56147ae1625e04f773f0f50b7720/lib/EpdFont/"
)
_orig_getaddrinfo = socket.getaddrinfo


def _ipv4_only_getaddrinfo(*args, **kwargs):
    return [ai for ai in _orig_getaddrinfo(*args, **kwargs) if ai[0] == socket.AF_INET]


def require_https(url):
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        raise ValueError(f"Expected an absolute HTTPS source URL: {url}")
    return url


class HttpsRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        require_https(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download_font(url: str, dest: Path, retries: int = 3, *, https_only=False, refresh=False) -> tuple[Path, str]:
    """Atomically cache original bytes, retaining the final URL for native locks.

    CTAN redirectors can choose a broken IPv6 mirror; retry with IPv4 as the
    legacy builder did. Native callers additionally prohibit HTTP redirects.
    """
    if https_only:
        require_https(url)
    url_file = dest.with_name(dest.name + ".url")
    if not refresh and dest.exists() and (not https_only or url_file.exists()):
        final_url = url_file.read_text(encoding="utf-8") if url_file.exists() else url
        if https_only:
            require_https(final_url)
        return dest, final_url
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary = dest.with_name(dest.name + ".part")
    opener = urllib.request.build_opener(HttpsRedirectHandler()) if https_only else urllib.request.build_opener()
    print(f"  Downloading {url}...", flush=True)
    last_error = None
    for attempt in range(1, retries + 1):
        if attempt > 1:
            socket.getaddrinfo = _ipv4_only_getaddrinfo
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "THCraft-Font-Assets/1"})
            with opener.open(request, timeout=60) as response, temporary.open("wb") as target:
                final_url = response.geturl()
                if https_only:
                    require_https(final_url)
                count = 0
                while chunk := response.read(128 * 1024):
                    target.write(chunk)
                    count += len(chunk)
                declared = response.headers.get("Content-Length")
                if declared is not None and count != int(declared):
                    raise ValueError(f"Short font read: expected {declared}, received {count}")
            if not count:
                raise ValueError("Empty font response")
            temporary.replace(dest)
            url_file.write_text(final_url, encoding="utf-8")
            return dest, final_url
        except Exception as error:
            last_error = error
            temporary.unlink(missing_ok=True)
            if attempt < retries:
                print(f"  Attempt {attempt} failed ({error}); retrying (IPv4-only)...", flush=True)
        finally:
            socket.getaddrinfo = _orig_getaddrinfo
    raise RuntimeError(f"Failed to download {url}: {last_error}") from last_error


def public_source_url(style_spec: dict) -> str:
    if "path" in style_spec and "url" not in style_spec:
        relative = Path(style_spec["path"])
        if relative.is_absolute() or ".." in relative.parts or "\\" in style_spec["path"]:
            raise ValueError(f"Source path must be relative to lib/EpdFont: {style_spec['path']}")
        return PUBLIC_SOURCE_BASE + urllib.parse.quote(relative.as_posix(), safe="/")
    if "url" in style_spec and "path" not in style_spec:
        return require_https(style_spec["url"])
    raise ValueError("Font source must have exactly one of 'path' or 'url'")


def resolve_font_source(style_spec: dict, family_name: str, style_name: str, *,
                        download_dir=DOWNLOAD_DIR, https_only=False, refresh=False) -> tuple[Path, str | None]:
    """Resolve original source bytes only; variable instancing is caller policy."""
    if "path" in style_spec and "url" not in style_spec:
        resolved = EPDFONTS_DIR / style_spec["path"]
        if not resolved.is_file():
            raise FileNotFoundError(f"{family_name}/{style_name}: {resolved} not found")
        return resolved, None
    if "url" in style_spec and "path" not in style_spec:
        url = style_spec["url"]
        filename = url.rsplit("/", 1)[-1]
        return download_font(url, download_dir / family_name / filename, https_only=https_only, refresh=refresh)
    raise ValueError(f"{family_name}/{style_name}: must have exactly one of 'path' or 'url'")
