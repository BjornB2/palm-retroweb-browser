# Architecture

The project deliberately uses a small native Palm OS implementation rather
than porting a desktop browser engine.

- `src/main.c` owns the Palm lifecycle, NetLib HTTP/1.0 client, navigation,
  proxy-profile selection, forms, preferences, bookmarks and history.
- `src/document.c` is a bounded-memory HTML flow renderer for text, headings,
  lists, rules, links, images and basic form controls.
- `src/gif.c` decodes the first frame of GIF87a/GIF89a images, including global
  and local palettes and interlacing, into an 8-bit Palm bitmap.
- `resources/RetrowebBrowser.rcp` contains the forms, menus and profile-specific
  application metadata.

## Build profiles

The default source is compiled twice:

| Profile | Launcher | Creator | Home | External URL handling |
| --- | --- | --- | --- | --- |
| RetroWEB | RetroWEB | `RtWB` | `retroweb.bjrno.com` | URL-encoded through RetroWEB with inline images |
| RetroModem | RBrowser | `RwBr` | `retroweb.bjrno.com` by default | requested unchanged |

Both defaults can be changed under **Options > Preferences**. The configured
home pages are stored in the profiles' separate Palm preference databases.

The profiles have separate Palm database names, creator IDs and preferences,
and therefore install side by side. The RetroModem profile deliberately keeps
its legacy internal database name for compatibility with hardware-test builds.

## Responsiveness

Sockets use non-blocking receive mode. DNS is attempted with short timeouts,
and the Palm event queue is pumped during network receive, progressive layout,
inline-image loading and GIF decoding. A link tap can cancel only the current
HTTP request while retaining the network connection; the Disconnect command
explicitly closes Palm NetLib.

## Bounds

Hard limits protect the Palm IIIc heap: 60 KB per response, 384 display items,
a 16 KB string pool, four decoded inline images, twelve bookmarks and eight
history entries.
