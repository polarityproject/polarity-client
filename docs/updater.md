# Updates and releases

Installed copies of Polarity update themselves from the latest release on
GitHub. They read its signed update info through the project site,
<https://polaritymod.link>, which redirects there, and download the files it
lists. They always ask first. This doc covers what gets published, how the
client checks and installs it, and how a release is made.

Code:
- `src/mod/updater.{h,cpp}`: checking, downloading and swapping files in;
- `src/core/update_info.{h,cpp}`: the update info format and its signature
  check;
- `src/mod/update_key.h`: the public half of the release key;
- `.github/workflows/build.yml` and `scripts/release-files.ps1`: building a
  release;
- `scripts/release.ps1`: signing and publishing it.

The installers in polarity-installer read the same update info and check it
the same way.

## What gets published

Each release on GitHub (`v<version>`) holds:

| File | What it is |
| --- | --- |
| `xinput1_3.dll`, `polarity.dll`, `NotoSansCJK-Medium.ttc`, `polarity_fonts_license.txt`, `polarity_third_party_notices.txt` | the files the updater installs |
| `update-files.json` | where the updater puts each of them |
| `polarity-<version>.zip` | the full package for first installs |
| `polarity.pdb` | symbols for reading crash addresses |
| `update.json` | `product` ("Polarity"), `version`, `published`, `notes`, `download` (the zip), and `files`: each file's `path`, `size`, `sha256` and `url`, the asset's address on GitHub. A relative `url` is read from beside the address the client was given |
| `update.json.sig` | base64 of an ECDSA P-256 signature (r, s) over `update.json`'s exact bytes |

The site's `_redirects` sends `/update.json` and `/update.json.sig` (and the
same under `/rooms/`, for installs from the old rooms branch) to
`releases/latest/download/` on GitHub. So the release GitHub marks as the
latest is the one every install is offered, and publishing a release is what
offers it. Drafts and pre-releases are never the latest.

A file's `path` is a plain name in `Binaries\Win64` with a `.dll`, `.pdb` or
`.txt` extension, or a `.ttc` font under `Polarity/fonts/` (or the old site's
`SFVR/fonts/`). A name is at most 64 characters of letters, digits, `_`,
`-` and `.`, with no `..` and no leading dot. The client refuses update info
with any other path, a `sha256` that is not 64 hex digits, or a size over
256 MB. The product may also be "SFVR": the old site's update info, which
SFVR's updaters require, was frozen at the first Polarity release.

Versions compare by their numbers first. A version with a suffix
(`0.5.0-beta`) is below the same version without one. Suffixes compare part
by part, split at dots, as semver does: numbers as numbers and below any
text, text as text, and a suffix that runs out first is the lower.

## The release key

`release.ps1 -NewKey` makes the signing key once. The private half stays on
the PC that publishes releases, outside every repository. The public half is
built into the client (`src/mod/update_key.h`) and the installers. A build
installs only update info signed with that key, so whoever controls the site,
its address or the GitHub account still can't push code to players. Losing
the key means a new one and a build that players install by hand.

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

With checks at start on, the client checks again every 15 minutes while the
game runs, so a release made while someone plays reaches them too (rooms
then ask for it).

Nothing in `update.json` counts until its signature checks out. An install
downloads only the files that differ from the game folder's copies, so the
bundled font (18 MB), which rarely changes, isn't downloaded again. Files
are staged flat in `Polarity\update\`; each one's place under the game
folder comes from its path (`Polarity/fonts/x.ttc`, or the old site's
`SFVR/fonts/x.ttc`, goes to `Polarity\fonts\x.ttc`). When the version is the
one running but a data file is missing from the game folder (an updater
before 0.3.0 installed without `more_files`), that file alone is installed
at once. Files that are there stay, so a build being worked on keeps its
own. A start also removes what an earlier update left: the replaced files
and the staging folder of a download that didn't finish.

The restart runs the same command line, with Steam's arguments, in this
process's environment, which has Steam's app id, so the game doesn't
relaunch through Steam. The new process gets the pid to wait for under two
names: `SFVR_WAIT_PID` is for a loader from before Polarity that an update
could not replace.

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
   writes and signs `update.json`, attaches both to the release, and
   publishes it as the latest. Installs are offered the update from then on.

To take a release back, mark the previous one as the latest on GitHub.

Every push to main and every pull request runs the same build and tests. The
run keeps the release files as an artifact for two weeks, which is handy for
trying a build before it's released.

Before a release that changes anything a battle touches, run the replay
regression check (see [replay-versioning.md](replay-versioning.md)).

## The old address

Installs from before the move to Polarity (SFVR 0.3.1 and older) read update
info from the old site. The release script wrote that once, at the first
Polarity release, into `polarity-web\legacy\`, naming the files of 0.4.0 on
the new site. It's frozen there, and the site keeps `releases/0.4.0/` for it.
