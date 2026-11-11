#!/usr/bin/env python3
# Test agent for a Steam Deck (SteamOS): what scripts/agent.ps1 is on a
# second Windows PC, so the main PC's scripts (scripts/remote.ps1, the
# two-PC tests) drive the game here the same way. Python 3's standard
# library only (SteamOS has python3).
#
# Layout: the game is at .../steamapps/common/StreetFighterV/StreetFighterV/
# Binaries/Win64/; this script in a scripts/ folder (its parent is the
# "polarity-client" folder deploys write to), best beside the game:
#   .../steamapps/common/StreetFighterV/polarity-client/scripts/agent_steamos.py
# It finds the game's Win64 folder by itself (each folder above the script,
# then Steam's libraries); --game names it outright.
# Run it in Desktop Mode (Konsole), with a token you make up:
#   python3 agent_steamos.py --token <secret>
# It listens on TCP --port (47100) for one JSON request per connection,
# {"token":..., "op":..., ...}, and answers one JSON line. Ops (agent.ps1's):
#   ping                          host name, game instances running, the game
#                                 folder, and checks of this Deck's setup
#   start  count                  starts the game through Steam (one instance)
#                                 and waits for its main menu
#   stop                          closes it
#   cmd    instance commands      mod commands (';'-separated), as cmd.ps1 writes them
#   log    instance from          the log from line `from` on (from -1: its count)
#   put    path data              writes a file (base64) under the game folder
#                                 (Binaries/Win64/...) or polarity-client/... (or sfv-rollback/...)
#   get    path                   reads such a file (base64)
#   ini    file section key value sets a key in an ini under Binaries/Win64
#   shot   instance [scale]       a screenshot (PNG, base64), taken by the mod
#                                 itself (its shot command): works in Game Mode
#   server                        not here (the Weyvelength server runs elsewhere)
# The game must load the mod under Proton: its Steam launch options need
#   WINEDLLOVERRIDES="xinput1_3=n,b" %command%
# (ping says whether they have it). Anyone who can reach the port and knows
# the token can run game commands and write files in the game folder: LAN
# only, a token you made up.

import argparse
import base64
import datetime
import glob
import hmac
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

APP_ID = '310950'
CONTROL_TAKE_S = 5      # the mod takes a command file within this (cmd.ps1)
PREVIOUS_WAIT_S = 10    # an earlier command file not taken yet is waited for
MAX_LOG_LINES = 3000    # per log call (remote.ps1 asks again for more)


def now_text():
    return datetime.datetime.now().strftime('%H:%M:%S')


def say(text):
    print(f'{now_text()} {text}', flush=True)


class Agent:
    def __init__(self, token, game, repo):
        self.token = token
        self.game = os.path.realpath(game)
        self.repo = os.path.realpath(repo)
        self.log_state = {}  # per instance: returned, offset, pending

    # ----- paths ------------------------------------------------------------

    def inside(self, path):
        # A path a request names (Windows or Unix separators), kept inside
        # the game's Win64 folder or the polarity-client folder.
        # "polarity-client/..." (or "sfv-rollback/...", its name before the
        # rename) is under the repo folder, whatever it is called
        # or wherever it is.
        p = str(path).replace('\\', '/')
        prefix = next((x for x in ('polarity-client/', 'sfv-rollback/') if p.startswith(x)), None)
        if prefix:
            full = os.path.realpath(os.path.join(self.repo, p[len(prefix):]))
        else:
            full = os.path.realpath(os.path.join(self.game, p))
        for root in (self.game, self.repo):
            if full == root or full.startswith(root + os.sep):
                return full
        raise ValueError(f'outside the game folder: {path}')

    def log_path(self, instance):
        name = 'polarity.log' if instance == 0 else f'polarity.{instance}.log'
        return os.path.join(self.game, 'Polarity', 'logs', name)

    def cmd_path(self, instance):
        return os.path.join(self.game, 'sfv_cmd.txt' if instance == 0 else f'sfv_cmd.{instance}.txt')

    # ----- the game's processes ---------------------------------------------

    def game_pids(self):
        # Wine names the game's process after its exe ("StreetFighterV.exe",
        # cut to 15 characters in comm); the Win64 one is the game itself.
        found = []
        for d in glob.glob('/proc/[0-9]*'):
            try:
                with open(os.path.join(d, 'comm')) as f:
                    comm = f.read().strip()
                if not comm.startswith('StreetFighterV'):
                    continue
                with open(os.path.join(d, 'stat')) as f:
                    if f.read().split(') ', 1)[1].startswith('Z'):
                        continue  # a zombie: gone
                with open(os.path.join(d, 'cmdline'), 'rb') as f:
                    cmdline = f.read().replace(b'\0', b' ').decode('utf-8', 'replace')
                found.append((int(os.path.basename(d)), 'Win64' in cmdline))
            except (OSError, IndexError, ValueError):
                continue
        return found

    def games(self):
        pids = self.game_pids()
        win64 = [p for p, w in pids if w]
        return len(win64) if win64 else (1 if pids else 0)

    # ----- this Deck's setup ------------------------------------------------

    def launch_options(self):
        # The game's Steam launch options (localconfig.vdf), for ping.
        home = os.path.expanduser('~')
        seen = False  # a block for the app, without options
        for vdf in glob.glob(os.path.join(home, '.local/share/Steam/userdata/*/config/localconfig.vdf')) + \
                glob.glob(os.path.join(home, '.steam/steam/userdata/*/config/localconfig.vdf')):
            try:
                text = open(vdf, encoding='utf-8', errors='replace').read()
            except OSError:
                continue
            # Every block for the app (the file has more than one, and they
            # nest), to its matching brace: the first with LaunchOptions.
            for m in re.finditer(r'"' + APP_ID + r'"\s*\{', text):
                depth, i = 1, m.end()
                while i < len(text) and depth:
                    if text[i] == '"':  # a quoted string: braces in it do not count
                        i += 1
                        while i < len(text) and text[i] != '"':
                            i += 2 if text[i] == '\\' else 1
                    elif text[i] == '{':
                        depth += 1
                    elif text[i] == '}':
                        depth -= 1
                    i += 1
                opts = re.search(r'"LaunchOptions"\s*"((?:[^"\\]|\\.)*)"', text[m.end():i])
                if opts:
                    return opts.group(1).replace('\\"', '"')
                seen = True
        return '' if seen else None

    # ----- ops -------------------------------------------------------------------

    def op_ping(self, q):
        opts = self.launch_options()
        return {
            'host': socket.gethostname(),
            'games': self.games(),
            'game': self.game,
            'platform': 'steamos',
            'python': sys.version.split()[0],
            'steam': shutil.which('steam') or '',
            'launchOptions': opts if opts is not None else '(not found)',
            'modLoads': bool(opts and 'xinput1_3' in opts),
            'mod': os.path.exists(os.path.join(self.game, 'polarity.dll')),
            'loader': os.path.exists(os.path.join(self.game, 'xinput1_3.dll')),
        }

    def copy_build(self, running):
        # As up.ps1: the freshly deployed DLL into the game folder (the loader
        # runs a copy, so this one is never in use); the loader itself only
        # with no game running; the bundled CJK font.
        out = []
        build = os.path.join(self.repo, 'build', 'Release')
        for name in ['polarity.dll'] + ([] if running else ['xinput1_3.dll']):
            src = os.path.join(build, name)
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(self.game, name))
                out.append(f'copied {name}')
        cjk = os.path.join(build, 'fonts', 'NotoSansCJK-Medium.ttc')
        cjk_to = os.path.join(self.game, 'Polarity', 'fonts', 'NotoSansCJK-Medium.ttc')
        if os.path.exists(cjk) and (not os.path.exists(cjk_to) or os.path.getsize(cjk) != os.path.getsize(cjk_to)):
            os.makedirs(os.path.dirname(cjk_to), exist_ok=True)
            shutil.copyfile(cjk, cjk_to)
            out.append('copied the CJK font')
        return out

    def launch(self):
        steam = shutil.which('steam')
        cmd = [steam, '-applaunch', APP_ID] if steam else ['xdg-open', f'steam://rungameid/{APP_ID}']
        subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
        return ' '.join(cmd)

    def at_menu(self, mark):
        # up.ps1's test: since `mark`, the mod's start line, then the menu's
        # scene, then its map load (the menu's stage request 'BLK').
        path = self.log_path(0)
        if not os.path.exists(path):
            return False
        try:
            with open(path, 'rb') as f:
                f.seek(max(0, os.path.getsize(path) - 200000))
                lines = f.read().decode('utf-8', 'replace').splitlines()[-400:]
        except OSError:
            return False
        lines = [l for l in lines if len(l) > 12 and l[:12] >= mark]
        if not any(re.search(r'instance \d+, mode', l) for l in lines):
            return False
        menu = False
        for l in lines:
            if 'ChangeSceneState MainMenuState' in l:
                menu = True
            elif menu and "text 'BLK'" in l:
                return True
        return False

    def op_start(self, q):
        count = int(q.get('count') or 1)
        if count > 1:
            return {'output': f'cannot start {count}: one instance on SteamOS'}
        out = []
        running = self.games()
        out += self.copy_build(running > 0)
        if running:
            out.append('the game is running already')
            return {'output': '\n'.join(out)}
        timeout = int(q.get('timeout') or 240)
        retries = 2
        start = time.time()
        mark = datetime.datetime.now().strftime('%H:%M:%S.%f')[:12]
        out.append('launching: ' + self.launch())
        seen = False
        gone_since = None
        deadline = start + timeout
        while time.time() < deadline:
            time.sleep(1)
            if self.at_menu(mark):
                out.append(f'instance 0 at the main menu after {int(time.time() - start)} s')
                return {'output': '\n'.join(out)}
            n = self.games()
            if n:
                seen, gone_since = True, None
            elif seen:
                gone_since = gone_since or time.time()
                # Gone for a while before its menu: a crash at start; again.
                if time.time() - gone_since > 15:
                    if retries <= 0:
                        out.append('a game exited while starting, 2 retries used')
                        return {'output': '\n'.join(out)}
                    retries -= 1
                    out.append(f'the game exited while starting; starting another ({2 - retries} of 2)')
                    out.append('launching: ' + self.launch())
                    seen, gone_since = False, None
                    deadline = time.time() + timeout
        out.append(f'not all started instances reached the main menu in {timeout} s')
        return {'output': '\n'.join(out)}

    def op_stop(self, q):
        pids = [p for p, _ in self.game_pids()]
        for p in pids:
            try:
                os.kill(p, signal.SIGKILL)
            except OSError:
                pass
        deadline = time.time() + 30
        while self.game_pids() and time.time() < deadline:
            time.sleep(0.25)
        # Closed on purpose: their crash reports' "running" marks go (the mod
        # would report each as a run that ended badly, crash_report.h).
        cleared = 0
        for f in glob.glob(os.path.join(self.game, 'Polarity', 'crashes', 'running*.txt')):
            try:
                os.remove(f)
                cleared += 1
            except OSError:
                pass
        left = len(self.game_pids())
        return {'output': 'stopped' if not left else f'{left} game processes still running',
                'killed': len(pids), 'marksCleared': cleared}

    def op_cmd(self, q):
        # cmd.ps1 -WaitMs 0: its error messages, which remote.ps1 looks for.
        instance = int(q.get('instance') or 0)
        f = self.cmd_path(instance)
        if not self.games():
            return {'output': f'no game is running (commands for instance {instance})'}
        deadline = time.time() + PREVIOUS_WAIT_S
        while os.path.exists(f) and time.time() < deadline:
            time.sleep(0.05)
        if os.path.exists(f):
            return {'output': f'instance {instance} has not taken its previous command file for 10 s (is it running? stuck?)'}
        lines = [l.strip() for l in str(q.get('commands') or '').split(';') if l.strip()]
        # Written aside and renamed into place: the mod polls the file and
        # must never see it half written.
        with open(f + '.tmp', 'w', encoding='utf-8', newline='') as out:
            out.write(''.join(l + '\r\n' for l in lines))
        os.replace(f + '.tmp', f)
        deadline = time.time() + CONTROL_TAKE_S
        while os.path.exists(f) and time.time() < deadline:
            time.sleep(0.05)
        if os.path.exists(f):
            return {'output': f'instance {instance} did not take the command file within 5 s'}
        return {'output': ''}

    def read_lines(self, path, offset):
        # Complete lines from `offset` on (a line still being written waits),
        # and the offset after them; a log shorter than the offset is a new one.
        if not os.path.exists(path):
            return [], offset
        with open(path, 'rb') as f:
            size = os.fstat(f.fileno()).st_size
            if size < offset:
                offset = 0
            f.seek(offset)
            data = f.read(size - offset)
        end = data.rfind(b'\n')
        if end < 0:
            return [], offset
        text = data[:end].decode('utf-8', 'replace')
        return [l.rstrip('\r') for l in text.split('\n')], offset + end + 1

    def op_log(self, q):
        # agent.ps1's: from -1 gives the count (to read from now on); going on
        # from where the last call ended reads only what was added since.
        instance = int(q.get('instance') or 0)
        frm = int(q.get('from') if q.get('from') is not None else 0)
        path = self.log_path(instance)
        s = self.log_state.get(instance)
        if s is None or frm < 0 or frm != s['returned']:
            lines, offset = self.read_lines(path, 0)
            if frm < 0:
                self.log_state[instance] = {'returned': len(lines), 'offset': offset, 'pending': []}
                return {'total': len(lines), 'lines': []}
            skip = min(frm, len(lines))
            s = {'returned': skip, 'offset': offset, 'pending': lines[skip:]}
            self.log_state[instance] = s
        else:
            lines, s['offset'] = self.read_lines(path, s['offset'])
            s['pending'] += lines
        n = min(MAX_LOG_LINES, len(s['pending']))
        out, s['pending'] = s['pending'][:n], s['pending'][n:]
        s['returned'] += n
        return {'total': s['returned'], 'lines': out}

    def op_put(self, q):
        full = self.inside(q['path'])
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full + '.agent-tmp', 'wb') as f:
            f.write(base64.b64decode(q['data']))
        os.replace(full + '.agent-tmp', full)
        return {'output': f'wrote {full}'}

    def op_get(self, q):
        full = self.inside(q['path'])
        with open(full, 'rb') as f:
            return {'data': base64.b64encode(f.read()).decode('ascii')}

    def op_ini(self, q):
        # WritePrivateProfileString's: the key's line in its section replaced,
        # or added at the section's end, or the section added at the file's.
        full = self.inside(q['file'])
        section, key, value = str(q['section']), str(q['key']), str(q['value'])
        text = open(full, encoding='utf-8-sig', errors='replace').read() if os.path.exists(full) else ''
        nl = '\r\n' if '\r\n' in text or not text else '\n'
        lines = text.splitlines()
        start = None
        for i, l in enumerate(lines):
            if l.strip().lower() == f'[{section.lower()}]':
                start = i
                break
        if start is None:
            if lines and lines[-1].strip():
                lines.append('')
            lines += [f'[{section}]', f'{key}={value}']
        else:
            end = len(lines)
            for i in range(start + 1, len(lines)):
                if lines[i].strip().startswith('['):
                    end = i
                    break
            done = False
            for i in range(start + 1, end):
                k = lines[i].split('=', 1)[0].strip()
                if '=' in lines[i] and k.lower() == key.lower() and not lines[i].lstrip().startswith(';'):
                    lines[i] = f'{key}={value}'
                    done = True
                    break
            if not done:
                at = end
                while at > start + 1 and not lines[at - 1].strip():
                    at -= 1
                lines.insert(at, f'{key}={value}')
        with open(full, 'w', encoding='utf-8', newline='') as f:
            f.write(nl.join(lines) + nl)
        return {'output': f'set [{section}] {key}={value} in {full}'}

    def op_shot(self, q):
        # The mod's own screenshot (overlay.h RequestScreenshot): no outside
        # tool can capture the game in Game Mode.
        instance = int(q.get('instance') or 0)
        scale = int(q.get('scale') or 1)
        rel = f'Polarity/agent-shot.{instance}.png'
        png = os.path.join(self.game, rel)
        if os.path.exists(png):
            os.remove(png)
        sent = self.op_cmd({'instance': instance, 'commands': f'shot {rel} {scale}'})
        if sent['output']:
            raise RuntimeError(f'no screenshot: {sent["output"]}')
        deadline = time.time() + 10
        while not os.path.exists(png) and time.time() < deadline:
            time.sleep(0.1)
        if not os.path.exists(png):
            raise RuntimeError(f'no screenshot: the game did not write one for instance {instance} in 10 s')
        with open(png, 'rb') as f:
            data = f.read()
        os.remove(png)
        return {'data': base64.b64encode(data).decode('ascii')}

    def op_server(self, q):
        raise RuntimeError('no Weyvelength server on SteamOS: run it on the main PC or the VPS')

    def handle(self, q):
        if not hmac.compare_digest(str(q.get('token', '')), self.token):
            raise PermissionError('bad token')
        op = getattr(self, 'op_' + str(q.get('op')), None)
        if op is None:
            raise ValueError(f"unknown op '{q.get('op')}'")
        return op(q)


def serve(agent, port):
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(('0.0.0.0', port))
    server.listen(8)
    say(f'agent on port {port} (game folder {agent.game}); Ctrl+C stops it')
    while True:
        conn, addr = server.accept()
        op = '?'
        try:
            conn.settimeout(60)
            data = b''
            while not data.endswith(b'\n'):
                chunk = conn.recv(1 << 20)
                if not chunk:
                    break
                data += chunk
            q = json.loads(data.decode('utf-8-sig'))
            op = q.get('op', '?')
            answer = {'ok': True}
            answer.update(agent.handle(q))
            say(f'{op} from {addr[0]}')
        except Exception as e:  # one line back whatever happened
            answer = {'ok': False, 'error': str(e)}
            say(f'error ({op}): {e}')
        try:
            conn.sendall((json.dumps(answer) + '\n').encode('utf-8'))
        except OSError:
            pass
        conn.close()


def find_game(here):
    # The game's Binaries/Win64 (the folder with StreetFighterV.exe): first
    # near this script (each folder above it, and a StreetFighterV folder in
    # each), then in every Steam library. The Steam install is
    # .../steamapps/common/StreetFighterV/StreetFighterV/Binaries/Win64, and
    # the scripts may sit at any depth in or beside it.
    def win64(folder):
        p = os.path.join(folder, 'Binaries', 'Win64')
        return p if os.path.isfile(os.path.join(p, 'StreetFighterV.exe')) else None
    tried = []
    folder = here
    for _ in range(8):
        for candidate in (folder, os.path.join(folder, 'StreetFighterV')):
            tried.append(candidate)
            found = win64(candidate)
            if found:
                return found, tried
        parent = os.path.dirname(folder)
        if parent == folder:
            break
        folder = parent
    home = os.path.expanduser('~')
    libraries = [os.path.join(home, '.local/share/Steam'), os.path.join(home, '.steam/steam')]
    for vdf in libraries[:]:
        try:
            with open(os.path.join(vdf, 'steamapps', 'libraryfolders.vdf'), encoding='utf-8', errors='replace') as f:
                libraries += re.findall(r'"path"\s+"([^"]+)"', f.read())
        except OSError:
            pass
    for lib in libraries:
        candidate = os.path.join(lib, 'steamapps', 'common', 'StreetFighterV', 'StreetFighterV')
        tried.append(candidate)
        found = win64(candidate)
        if found:
            return found, tried
    return None, tried


def main():
    here = os.path.dirname(os.path.realpath(__file__))
    repo = os.path.dirname(here)
    ap = argparse.ArgumentParser(description='Polarity test agent for SteamOS (scripts/remote.ps1 drives it)')
    ap.add_argument('--token', required=True)
    ap.add_argument('--port', type=int, default=47100)
    ap.add_argument('--game', default='',
                    help="the game's Binaries/Win64 folder (default: found near this script, or in Steam's libraries)")
    ap.add_argument('--repo', default=repo, help='the polarity-client folder (default: this script\'s parent)')
    a = ap.parse_args()
    if not a.game:
        a.game, tried = find_game(here)
        if not a.game:
            sys.exit('no StreetFighterV.exe found in Binaries/Win64 under:\n  ' + '\n  '.join(tried) +
                     '\npass --game <the game\'s Binaries/Win64 folder>')
    elif not os.path.isdir(a.game):
        sys.exit(f'no game folder at {a.game}')
    say(f'game folder {os.path.realpath(a.game)}, scripts folder {os.path.realpath(a.repo)}')
    serve(Agent(a.token, a.game, a.repo), a.port)


if __name__ == '__main__':
    main()
