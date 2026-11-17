# Updates and releases

Installed copies of Polarity update themselves. They read signed update info
from the project site, <https://polaritymod.link>, and download the files it
lists from the release on GitHub. They always ask first. This doc covers what
gets published, how the client checks and installs it, and how a release is
made.

Code:
- `src/mod/updater.{h,cpp}`: checking, downloading and swapping files in;
- `src/core/update_info.{h,cpp}`: the update info format and its signature
  check;
- `src/mod/update_key.h`: the public half of the release key;
- `.github/workflows/build.yml` and `scripts/release-files.ps1`: building a
  release;
- `scripts/release.ps1` and `scripts/prune-releases.ps1`: publishing it.

The installers in polarity-installer read the same update info and check it
the same way.

## What gets published

Each release on GitHub (`v<version>`) holds the files the updater installs,
`update-files.json` (where each one goes), `polarity-<version>.zip` for first
installs, and `polarity.pdb` for reading crash addresses.

The polarity-web repository's `public/` holds:

| File | What it is |
| --- | --- |
| `update.json` | `product` ("Polarity"), `version`, `published`, `notes`, `download` (the full package for first installs), and `files`: each file's `path`, `size`, `sha256` and `url`, the release asset's address on GitHub. A relative `url` is read from beside `update.json` |
| `update.json.sig` | base64 of an ECDSA P-256 signature (r, s) over `update.json`'s exact bytes |
| `releases/0.4.0/` | the files of the last release served from the site, which the old address's update info still names (see the end of this doc) |

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
2. Commit as `chore(release): <version>`, tag it `v<version>` and push both.
   The build workflow builds, runs the unit tests, runs
   `scripts\release-files.ps1`, attests the files and puts them in a draft
   release whose description is the changelog section. It fails if the tag
   and `kPolarityVersion` differ or the changelog has no section.
3. Check the draft on GitHub. Edit its description if the notes need it.
4. Run `scripts\release.ps1` (PowerShell 7, with gh signed in to an account
   that can publish releases). It downloads the draft's files, checks each
   one's attestation, keeps `polarity.pdb` in `dist\symbols\<version>\`,
   publishes the release, then writes and signs `public\update.json` in
   polarity-web.
5. Commit the site. Run `scripts\prune-releases.ps1` to remove release
   folders no update info names any more (`-WhatIf` lists them first).
6. Run `polarity-web\build.ps1`, which minifies the pages and writes
   `dist\polarity-web-<version>.zip`, then upload that to the Cloudflare
   Pages project.

Every push to main and every pull request runs the same build and tests. The
run keeps the release files as an artifact for two weeks, which is handy for
trying a build before it's released.

Before a release that changes anything a battle touches, run the replay
regression check (see [replay-versioning.md](replay-versioning.md)).

## The old address

Installs from before the move to Polarity (SFVR 0.3.1 and older) read update
info from the old site. The release script wrote that once, at the first
Polarity release, into `polarity-web\legacy\`. It's frozen there, and
`prune-releases.ps1` keeps the release it names.
