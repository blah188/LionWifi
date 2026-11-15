# extras/common-styles.css

A single stylesheet shared by every page a node serves. It is **not** compiled into the
firmware: build with `-D LIONWIFI_COMMON_CSS` and the library's built-in `<style>` blocks are
replaced by one `<link rel="stylesheet" href="/common-styles.css">`, served by the library
from the filesystem.

## Why a file and not PROGMEM

The application partition is mirrored for OTA, so a kilobyte in PROGMEM costs two kilobytes
of flash, while a kilobyte on the filesystem costs one. Moving the shared look out of the
image also means every page of every node gets the same design without carrying its own copy.

## Installing it

Upload once per node through the filesystem browser's own form — a firmware flash does not
touch the filesystem, so this is a one-time step:

```
curl -u <user>:<pass> -F "name=@common-styles.css" http://<node>/spiffs/ls
```

(`/sd/ls` for an SD-backed node.) The destination name and path are
`LIONWIFI_COMMON_CSS_PATH`, `/common-styles.css` by default.

## If the file is missing

The route answers 200 with `LIONWIFI_COMMON_CSS_FALLBACK`, a few hundred bytes of readable
defaults, and logs one warning per boot naming the path. Pages stay usable and plain rather
than unstyled and broken, and the missing file is self-reporting.

## Caching

The sheet is served with `Cache-Control: max-age=1209600` (two weeks, see
`LIONWIFI_COMMON_CSS_MAXAGE`). The filename is deliberately fixed rather than versioned, so
that changing the look never requires reflashing anything. The consequence: after replacing
the file a browser that already has it keeps the old copy for up to two weeks — a hard reload
(Ctrl+F5) picks up the new one.

## What is in it

Three sources merged: design tokens with a dark-scheme variant, the library's own pages
(filesystem browser `.fs-*`, access point list `.aps-table`), and the common furniture of a
node's pages — base typography, header row, buttons and inputs, state badges, list tables,
row drag and drop.

The sheet has two layers. Components are addressed by their own class — `.status` badges,
`table.list`, the `.fs-*` browser, `.aps-table`, buttons, inputs — and are safe on any page.
The typical-node layer also styles bare `h1/h2`, `table`, `form` and `label`, which a page
with its own layout (an editor, for instance) uses differently, so it is scoped to
`<body class="node">`: a node asks for the shared look with that one class, and then needs no
markup changes beyond dropping its own `<style>`.

A page's own rules stay in that page's `<style>`, which is printed after this link, so the
cascade always ends with the page. The `--fs-*` custom properties remain the public contract
documented in `FsBrowser.h`; here they are derived from the shared tokens, and overriding one
recolors only the browser.
