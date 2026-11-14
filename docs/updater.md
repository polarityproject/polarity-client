# Updates and releases

Installed copies of Polarity update themselves from the project site,
<https://polaritymod.link>. They always ask first. This doc covers what the
site publishes, how the client checks and installs it, and how a release is
made.

Code:
- `src/mod/updater.{h,cpp}`: checking, downloading and swapping files in;
- `src/core/update_info.{h,cpp}`: the update info format and its signature
  check;
- `src/mod/update_key.h`: the public half of the release key;
- `scripts/release.ps1` and `scripts/prune-releases.ps1`.

The installers in polarity-installer read the same update info and check it
the same way.

## What the site publishes

From the polarity-web repository's `public/`:

| File | What it is |
| --- | --- |
| `update.json` | `product` ("Polarity"), `version`, `published`, `notes`, `download` (the full package for first installs), and `files`: each file's `path`, `size`, `sha256` and `url` (relative to `update.json`) |
| `update.json.sig` | base64 of an ECDSA P-256 signature (r, s) over `update.json`'s exact bytes |
| `releases/<version>/` | the files `update.json` lists, and `polarity-<version>.zip` |

A file's `path` is a plain name in `Binaries\Win64` with a `.dll`, `.pdb` or
`.txt` extension, or a `.ttc` font under `Polarity/fonts/`. The client
refuses update info with any other path.

## The release key

`release.ps1 -NewKey` makes the signing key once. The private half stays on
the PC that publishes releases, outside every repository. The public half is
built into the client (`src/mod/update_key.h`) and the installers. A build
installs only update info signed with that key, so whoever controls the site
or its address still can't push code to players. Losing the key means a new
one and a build that players install by hand.

## In the game

1. At start the client reads `update.json` and its signature in the
   background. The address is `[update] url` in `polarity.ini`, or
   `kDefaultUpdateUrl` (`https://polaritymod.link/update.json`).
   `[update] check=0` turns the check off.
2. When the version is newer (`CompareVersions`), Polarity offers it with
   its notes. Polarity settings → Updates also shows it.
3. Installing downloads each file into `Polarity\update\`, checks its size
   and SHA-256, then swaps all of them or none. Each file in place, even the
   loaded loader `xinput1_3.dll`, is renamed to `<name>.<n>.old` and the new
   one moved in. The player's ini files are never touched.
4. The client runs from a copy of `polarity.dll` (`src/proxy/load_mod.h`), so
   the new build takes over when the game restarts. Polarity offers to
   restart it. The next start deletes the `.old` files.

Players on different versions can't play each other. Custom rooms list only
rooms of the player's own version, and a room joined by code from another
version is left at once.

Commands for tests: `update check [url]`, `update install`, `update` (the
state in the log).

## Making a release

1. Raise `kPolarityVersion` in `src/core/version.h`. Give the version a
   section in `CHANGELOG.md`; its first paragraph becomes the notes players
   see.
2. Run `scripts\release.ps1` (PowerShell 7). It builds, runs the unit tests,
   copies the files and the player package into
   `polarity-web\public\releases\<version>\`, writes and signs
   `public\update.json`, and keeps the build's `.pdb` in
   `dist\symbols\<version>\` for reading crash addresses.
3. Run `scripts\prune-releases.ps1` to remove release folders no update info
   names any more (`-WhatIf` lists them first).
4. Commit here as "Release <version>" and tag `v<version>`. Commit the site.
5. Run `polarity-web\build.ps1`, which minifies the pages and writes
   `dist\polarity-web-<version>.zip`, then upload that to the Cloudflare
   Pages project.

Before a release that changes anything a battle touches, run the replay
regression check (see [replay-versioning.md](replay-versioning.md)).

## The old address

Installs from before the move to Polarity (SFVR 0.3.1 and older) read update
info from the old site. `release.ps1 -Bridge` wrote that once, at the first
Polarity release, into `polarity-web\legacy\`. It's frozen there, and
`prune-releases.ps1` keeps the release it names.
