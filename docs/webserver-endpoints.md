# Webserver Endpoints

This document describes the HTTP, WebSocket, WebDAV, and discovery endpoints
available while CrossPoint Reader is in File Transfer or Calibre Wireless mode.

- HTTP server: port 80
- WebSocket upload server: port 81
- UDP discovery listener: port 8134
- WebDAV: port 80, handled by the same HTTP server

Examples use `crosspoint.local`. If mDNS does not resolve on your network, use
the IP address shown on the device screen.

The server has **no authentication**. Use these endpoints only on a trusted
private network or a hotspot whose connected clients you control.

## HTTP Pages

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | Home/status page |
| `GET` | `/files` | File manager page |
| `GET` | `/settings` | Web settings page |
| `GET` | `/fonts` | SD-card font manager page |
| `GET` | `/js/jszip.min.js` | JavaScript asset used by the file manager |

## Device Status

### `GET /api/status`

```bash
curl http://crosspoint.local/api/status
```

Response:

```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600,
  "device": "X4"
}
```

| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Firmware version |
| `ip` | string | Device IP address |
| `mode` | string | `"STA"` for joined Wi-Fi or `"AP"` for hotspot mode |
| `rssi` | number | Wi-Fi RSSI in dBm; `0` in AP mode |
| `freeHeap` | number | Free heap in bytes |
| `uptime` | number | Seconds since boot |
| `device` | string | `"X3"` or `"X4"` hardware detection |

## File Management

### `GET /api/files`

Lists files and folders under a directory.

```bash
curl "http://crosspoint.local/api/files?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Directory to list |

Response:

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

Hidden dotfiles are omitted unless the device setting `showHiddenFiles` is
enabled. `System Volume Information` and `XTCache` are always hidden/protected.

### `GET /download`

Downloads a file from the SD card.

```bash
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

Query parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | File path to download |

Protected dotfiles, `System Volume Information`, and `XTCache` cannot be
downloaded. EPUB files are served as `application/epub+zip`; other files use
`application/octet-stream`.

### `POST /upload`

Uploads a file with HTTP multipart form data.

```bash
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

Query parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `path` | No | `/` | Destination directory |

Successful response:

```text
File uploaded successfully: mybook.epub
```

Notes:

- Existing files with the same name are overwritten.
- EPUB cache data for the uploaded path is cleared after a successful upload.
- HTTP upload uses a 4 KB write buffer before flushing to the SD card.

### `POST /mkdir`

Creates a folder.

```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

Form parameters:

| Parameter | Required | Default | Description |
|-----------|----------|---------|-------------|
| `name` | Yes | - | New folder name |
| `path` | No | `/` | Parent folder |

### `POST /rename`

Renames a file.

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `name` | Yes | New file name, not a path |

Only files can be renamed through this endpoint. The old EPUB cache path is
cleared before the rename.

### `POST /move`

Moves a file into an existing folder.

```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://crosspoint.local/move
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes | Existing file path |
| `dest` | Yes | Existing destination folder |

Only files can be moved through this endpoint. The old EPUB cache path is
cleared before the move.

### `POST /delete`

Deletes one or more files or empty folders.

```bash
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://crosspoint.local/delete
```

Form parameters:

| Parameter | Required | Description |
|-----------|----------|-------------|
| `path` | Yes, unless `paths` is provided | Single path to delete |
| `paths` | Yes, unless `path` is provided | JSON array of paths to delete |

Protected items cannot be deleted. Non-empty folders are rejected. EPUB cache
data for deleted files is cleared.

## Settings API

### `GET /api/settings`

Returns a streamed JSON array of editable settings. Each item contains common
fields plus type-specific fields.

```bash
curl http://crosspoint.local/api/settings
```

Example item:

```json
{
  "key": "fontSize",
  "name": "Reader Font Size",
  "category": "Reader",
  "type": "enum",
  "value": 1,
  "options": ["12 pt", "14 pt", "16 pt", "18 pt"]
}
```

`value` is always an index into `options`, never the option's text. `fontSize`
is one of the settings whose `options` are built at request time — they are the
point sizes the selected font family actually ships, so a family installed at
10/12/14 offers three options. (`fontFamily` and `dictionaryName` vary the same
way, from the SD card contents.)

Types:

| Type | Extra fields |
|------|--------------|
| `toggle` | `value` (`0` or `1`) |
| `enum` | `value`, `options` |
| `value` | `value`, `min`, `max`, `step` |
| `string` | `value` |

The font-family setting includes SD-card font families when they are installed.

### `POST /api/settings`

Applies a partial settings update from a JSON object.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"fontSize":2,"showHiddenFiles":1}' \
  http://crosspoint.local/api/settings
```

Successful response:

```text
Applied 2 setting(s)
```

## Font Management API

### `GET /api/fonts`

Lists discovered SD-card font families in the format supported by this device.
X4 Pro uses native TTF/OTF; all other devices use `.cpfont`.

```bash
curl http://crosspoint.local/api/fonts
```

Example X4 Pro response (file sizes are illustrative):

```json
{
  "format": "opentype",
  "extensions": ["ttf", "otf"],
  "maxFamilies": 128,
  "maxFamilyLength": 31,
  "maxFiles": 4,
  "families": [
    {
      "name": "Literata",
      "sizes": [12, 14, 16, 18],
      "files": [
        {"name": "Literata-Regular.ttf", "size": 123456, "style": 0}
      ]
    }
  ]
}
```

| Field | Meaning |
|-------|---------|
| `format` | `"opentype"` on X4 Pro, `"cpfont"` on other devices |
| `extensions` | `["ttf","otf"]` on Pro, `["cpfont"]` otherwise; no leading dots |
| `maxFamilies` | 128 discovered families |
| `maxFamilyLength` | 31 characters for HTTP upload/delete family names |
| `maxFiles` | Maximum files in one upload: 4 on Pro, 32 otherwise |
| `families[].name` | Family directory name |
| `families[].sizes` | Pro reading sizes `[12,14,16,18]` when Regular is available, otherwise empty; installed point sizes on non-Pro devices |
| `families[].files[].name` | Font file basename, not a path |
| `families[].files[].size` | File length in bytes |
| `families[].files[].style` | Pro only: 0 Regular, 1 Bold, 2 Italic, 3 BoldItalic |

For example, a non-Pro file entry is
`{"name":"Literata_12.cpfont","size":123456}`; it has no `style` field.
Native files are point-size independent, not one file per entry in `sizes`.
The response does not expose variation axes or the sidecar as file entries.
`/.fonts` and `/fonts` are scanned; `/.fonts` wins a family-name collision.

### `POST /api/fonts/upload`

Uploads a complete selection from **one family in one multipart request**.
Declare every file in a single URL-query parameter named `manifest`, containing
URL-encoded JSON:

```json
{"family":"Literata","files":[{"name":"Literata-Regular.ttf","size":123456},{"name":"Literata-Bold.ttf","size":234567}]}
```

The manifest must be available before the first multipart file starts. Do not
send the old `-F "family=..."` contract or put the manifest in a multipart field.
Append each file as a `file` part, with its multipart filename matching the
manifest's `name` **exactly**, including case. Every declared file must arrive
once; undeclared, duplicate, missing, or incorrectly sized files reject the
selection. Multipart order need not match manifest order.

X4 Pro example using curl 7.87.0 or newer (`--url-query` URL-encodes the value):

```bash
curl --url-query 'manifest={"family":"Literata","files":[{"name":"Literata-Regular.ttf","size":123456},{"name":"Literata-Bold.ttf","size":234567}]}' \
  -F "file=@Literata-Regular.ttf" \
  -F "file=@Literata-Bold.ttf" \
  http://crosspoint.local/api/fonts/upload
```

**Replace `123456` and `234567` with the exact byte lengths of your local
Regular and Bold files before running this example.** These numbers are
placeholders, not expected font sizes; `size` must be a JSON integer, not a
quoted string, and must describe the font bytes rather than multipart overhead.
Change both the manifest and multipart names if using another family or `.otf`.
For a single-file installation, omit Bold from both places.

Non-Pro example (replace `123456` with the actual `.cpfont` byte length):

```bash
curl --url-query 'manifest={"family":"Literata","files":[{"name":"Literata_12.cpfont","size":123456}]}' \
  -F "file=@Literata_12.cpfont" \
  http://crosspoint.local/api/fonts/upload
```

#### Names, validation, and limits

- `family`: 1–31 ASCII letters, digits, hyphens, or underscores. Every file's
  parsed family must match it exactly. Paths, traversal, and embedded NULs are
  rejected.
- Pro filenames: `Family-Regular.ttf`/`.otf`, optionally `-Bold`, `-Italic`,
  and `-BoldItalic`. Style suffixes are case-sensitive. A new family requires
  Regular; an existing valid Regular can be retained during a style-only update.
  At most four files, one per style, are accepted.
- Non-Pro filenames: `Family_12.cpfont` (a hyphen before the size is also
  accepted). The suffix is 1–3 decimal digits representing a point size 1–255.
  At most 32 files, one per numeric point size, are accepted.
- Extensions are case-insensitive and normalized to lowercase on installation.
  Manifest filenames must fit within 63 bytes; the format-specific naming
  rules above also apply.
- The decoded manifest is at most 8192 bytes. It must contain a nonempty
  `files` array with string `name` and unsigned 32-bit integer `size` fields.
  Each file must be at least 8 bytes. The sum of declared file sizes must not
  exceed 4,294,967,295 bytes. These are protocol bounds, not guarantees of
  available SD space or upload capacity.
- A new family is rejected when the 128-family limit is reached; an existing
  family can still be updated.
- Pro checks the SFNT header across upload chunks, then validates the closed
  staged files with FreeType, including scalable TrueType/CFF outlines and a
  Unicode charmap. TTC, WOFF, bitmap-only fonts, malformed fonts, and `.cpfont`
  are rejected. A `.ttf`/`.otf` suffix alone is insufficient.
- Non-Pro checks `CPFONT\0\0` magic bytes and exact received/written lengths.
  This is not native SFNT validation.

#### Commit and cancellation

Files are written to `.part` staging paths. No style is published when an
individual multipart file ends: the complete selection must finish and validate.
On Pro, the commit replaces only submitted styles, preserving untouched styles
and their variation settings. Manually uploaded styles use font-default axes;
this HTTP manifest has no axis-setting contract. The installer updates/removes
`native-font.json` as needed and retains backups through the commit so a failed
publication can roll back. This differs from an on-device catalogue download,
which replaces the whole native family.

Legacy uploads also stage the whole selection and keep backups until all
selected files are published; unselected files are left alone. Installation
reuses the family's existing root. New families use the sole existing font
root, or prefer `/.fonts` if both/neither roots exist.

Invalid or incomplete requests and cancellations before commit discard only
that request's staging files, leaving the prior installation intact. A
pre-existing `.part` file is not overwritten: it causes HTTP 409. SD failures
during rollback can leave preserved backups; this transaction is not a
power-loss-proof filesystem guarantee.

There is no separate cancellation endpoint. Abort the HTTP upload; the browser
**Cancel** button does this and then reloads the font list. Cancellation after
commit cannot undo it, and a disconnected client may receive no JSON response.
Reload `GET /api/fonts` after a connection failure to establish the result.

Successful response (HTTP 200, `application/json`):

```json
{"ok":true}
```

### `POST /api/fonts/delete`

Deletes a family from both font roots if present. The JSON body must be at most
256 bytes and contain a string `family` using the same 1–31-character alphabet
as uploads.

On Pro, deletion removes only matching native style files and
`native-font.json`. It is nonrecursive: co-located `.cpfont`, unrelated files,
and subfolders survive. The saved family name is retained for a future native
reinstall. Deletion is not an atomic upload transaction; a storage error can
leave some native files undeleted.

On non-Pro devices, deletion removes the family directory recursively and
clears the saved family selection if that family was active. A nonexistent
family succeeds. Deletion during an active upload returns HTTP 409.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' \
  http://crosspoint.local/api/fonts/delete
```

Successful response (HTTP 200, `application/json`):

```json
{"ok":true}
```

### Font API errors

All three font endpoints return `application/json`. Errors have this shape
(the `error` message describes the failure):

```json
{"ok":false,"error":"Invalid font family name."}
```

| HTTP status | Conditions and example exact `error` messages |
|-------------|----------------------------------------------|
| 400 | Bad request/manifest, invalid names or metadata, mixed/duplicate files, incorrect sizes, unsupported/invalid fonts, incomplete or cancelled upload. Examples: `"Missing or oversized upload manifest."`, `"Invalid upload manifest."`, `"Select files from one font family only."`, `"The upload contains an undeclared or duplicate file."`, `"The font is larger than its declared size."`, `"The font selection is incomplete."`, `"This font cannot be used. Check its format and include a Regular style."`, `"The font upload was cancelled."` |
| 409 | `"The font family limit has been reached. Delete a family first."`, `"A staged file already exists. Remove that .part file before retrying."`, or `"Finish or cancel the current font upload first."` |
| 500 | SD read/write/commit/delete failure. Examples: `"Unable to read fonts from the SD card."`, `"The SD card could not save the complete font."`, `"Unable to publish the font selection on the SD card."` (legacy commit), or `"Unable to update fonts on the SD card."` (installer failure) |
| 503 | `"Not enough memory to list fonts."` or `"Not enough memory to install this font."` |

Treat any non-200 response as failure; do not assume every failure means
the SD card is unchanged (for example, a delete can fail partway through).
Use `error` for display rather than expecting an additional error-code field.

## OPDS Server API

### `GET /api/opds`

Lists saved OPDS servers. Passwords are never returned.

```bash
curl http://crosspoint.local/api/opds
```

Response:

```json
[
  {
    "index": 0,
    "name": "My Catalog",
    "url": "http://calibre.local:8080/opds",
    "username": "reader",
    "hasPassword": true
  }
]
```

### `POST /api/opds`

Adds or updates an OPDS server. Include `index` to update an existing entry.
If `password` is omitted during an update, the existing password is preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"My Catalog","url":"http://calibre.local:8080/opds","username":"reader","password":"secret"}' \
  http://crosspoint.local/api/opds
```

### `POST /api/opds/delete`

Deletes an OPDS server by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/opds/delete
```

## Wi-Fi Credential API

### `GET /api/wifi`

Lists saved Wi-Fi networks. Passwords are never returned.

```bash
curl http://crosspoint.local/api/wifi
```

Response:

```json
[
  {
    "index": 0,
    "ssid": "HomeWiFi",
    "hasPassword": true,
    "isLastConnected": true
  }
]
```

### `POST /api/wifi`

Adds or updates a saved Wi-Fi network. Include `index` to update an existing
entry. If `password` is omitted during an update, the existing password is
preserved.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"HomeWiFi","password":"secret"}' \
  http://crosspoint.local/api/wifi
```

### `POST /api/wifi/delete`

Deletes a saved Wi-Fi network by index.

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/wifi/delete
```

## WebSocket Upload

### Port 81

The WebSocket path is used for fast binary uploads from the file manager and
Calibre plugin workflows.

Connection:

```text
ws://crosspoint.local:81/
```

Protocol:

1. Client sends text: `START:<filename>:<size>:<path>`
2. Server replies `READY`
3. Client sends binary chunks
4. Server sends `PROGRESS:<received>:<total>` every 64 KB or at completion
5. Server sends `DONE` when complete or `ERROR:<message>` on failure

Example session:

```text
Client -> START:mybook.epub:1234567:/Books
Server -> READY
Client -> [binary chunk]
Server -> PROGRESS:65536:1234567
...
Server -> DONE
```

Error messages include:

| Message | Cause |
|---------|-------|
| `ERROR:Upload already in progress` | A second upload was started before the first completed |
| `ERROR:Invalid START format` | Malformed START message or invalid size token |
| `ERROR:Failed to create file` | Destination file could not be opened |
| `ERROR:No upload in progress` | Binary data arrived without a matching START |
| `ERROR:Upload overflow` | Client sent more bytes than declared |
| `ERROR:Write failed - disk full?` | SD write failed |

Incomplete WebSocket uploads are deleted on disconnect or error.

## WebDAV

The same HTTP server registers a WebDAV-compatible handler for file manager clients.

Supported methods:

```text
OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK
```

Notes:

- `PUT` writes to a temporary `.davtmp` file first, then renames it into place.
- Protected paths are rejected.
- `LOCK` and `UNLOCK` are accepted for client compatibility only. The server
  does not implement full WebDAV Class 2 locking semantics such as persistent
  locks or lock discovery.

## UDP Discovery

The server listens on UDP port `8134`. When it receives the text payload
`hello`, it replies to the sender with:

```text
crosspoint (on <hostname>);81
```

The final field is the WebSocket upload port.

## Network Modes

### Station Mode (STA)

- Device joins an existing 2.4 GHz Wi-Fi network.
- `crosspoint.local` is advertised with mDNS when available.
- `/api/status` returns `"mode": "STA"` and RSSI in dBm.

### Access Point Mode (AP)

- Device creates an open hotspot named `CrossPoint-Reader`.
- The device shows a Wi-Fi QR code and URL QR code.
- The fallback IP is typically `192.168.4.1`.
- `/api/status` returns `"mode": "AP"` and `"rssi": 0`.

### Calibre Wireless

Calibre Wireless starts the same web server in STA mode and displays setup
instructions plus WebSocket upload progress on the device screen.
