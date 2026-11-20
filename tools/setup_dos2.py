#!/usr/bin/env python3
"""Divinity Original Sin 2 Mobile: from your own Divinity: Original Sin 2 (Switch) dumps to the folder the app reads, in one command.

  setup                     (no file arguments, e.g. a double-click on setup.bat)
      Looks for the base game, the update v786432 and prod.keys in
      put-game-files-here/ (next to the setup scripts) and in the current directory, under any file names, and prepares everything.
  setup FILE_OR_FOLDER... [--work DIR] [--push [SERIAL]] [--dry-run] [--yes] [--keep-nsp] [--skip-verify] [--skip-post]
        [--skip-astc] [--steps exefs,patch,verify,romfs,shaders,post,touchui,astc]
      The same with explicit files (or folders to search), in any order. Packages: NSP, NSZ, XCI, XCZ, also split dumps
      (a folder of 00, 01, ... parts, or name.xc0 / name.ns0 with .xc1 ... next to it); one package may hold several titles.
      Base and update are told apart by the title id in their content meta (CNMT), not by the name.
      Then: ExeFS of the update -> offline patch (dm/tools/patch_nso.py, DM_TARGET=android) -> per-site equivalence check of the
      patch (dm/verify/equiv.py) -> RomFS (base + BKTR update, 47 files, ~11.4 GB) -> shader jobs from the game's packages and its
      program (tools/dos2_shader_jobs.py), offline translation to Vulkan (dm/gfx/tools/dmsh_vk_xc2.py) and one shader pack
      (dm/gfx/tools/xgs_pack.py) -> the fused post-processing shaders dm/shaders/native-post.xgspack (tools/dos2_post_fuse.py,
      from five bundles of that pack; SPIR-V tools of the Android NDK r29, fetched into tools/.runtime/spirv-tools when missing) and the
      engine shaders by name dm/shaders/native-nr.xgspack (tools/dos2_nr_sh_pack.py: this port's own GLSL rewrites in your bundles' heads)
      -> the touch interface package romfs/Data/TouchUI.pak (tools/dos2_touchui_pak.py) -> the ASTC
      texture pack for phones without BC textures (tools/dos2_astc_cache.py) -> the phone folder
      <work>/DivinityOriginalSin2Mobile/{romfs, dm/out, dm/shaders, astc, drivers}. Every step is skipped when its output is
      already complete; delete an output to redo its step. --dry-run identifies the inputs, checks the tools and the free space
      and prints the plan. --yes answers yes to every question (download .NET and the SPIR-V
      tools, copy to the phone).
  setup check [FILE_OR_FOLDER...] [--work DIR]
      Report the tools, the game files found, what has been prepared and the next step. Changes nothing.
  setup push [SERIAL] [--work DIR]
      Copy <work>/DivinityOriginalSin2Mobile to /sdcard/Download/DivinityOriginalSin2Mobile on the phone over adb (only missing /
      size-mismatched files, so an interrupted push resumes) and grant org.dos2.port "All files access". SERIAL may be left out
      when exactly one phone is connected.

Supported: Divinity: Original Sin 2 - Definitive Edition for Nintendo Switch, base 010027400CDC6000 v0 + update 010027400CDC6800
v786432 (main build id 4979b200d53bb282..., sdk 230994acc1eb0f13..., SDK 9.3.0). All game languages are in the base RomFS; the
game has no add-on content to install. Packages and keys are only read, never copied, uploaded or written anywhere; the keys stay in
memory. Compressed packages (NSZ / XCZ) get their program expanded to plain NSPs in <work>/nsp (needed for the RomFS); those are
deleted afterwards unless --keep-nsp. Default work directory: $DM_DOS2_WORK or <repository>/work.

Touch interface: the package holds the console menus with touch handling and the on-screen buttons (the HUD's panel column, the
panels' button rows), made from the Switch files alone; the buttons' art is this port's own (assets/touchui). No other edition needed.
The .NET 10 SDK (shader translator build) is taken from DOTNET, tools/.runtime/dotnet, ~/.dotnet or PATH; when it is missing,
setup offers to install it into tools/.runtime/dotnet with Microsoft's dotnet-install script (no admin rights, nothing
system-wide). setup.bat / setup.sh provide Python and its packages the same way (tools/.runtime).
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
RUNTIME = TOOLS / '.runtime'
DROP = ROOT / 'put-game-files-here'
sys.path.insert(0, str(TOOLS))

BASE_TID, UPDATE_TID, UPDATE_VERSION = 0x010027400CDC6000, 0x010027400CDC6800, 786432
AOC_BASE = BASE_TID + 0x1000                 # AOC title id = AOC_BASE + index (the game has none: its "Gift Bag" content is in the RomFS)
BUILD_ID = '4979b200d53bb282e0d11871613286da'
SDK_BUILD_ID = '230994acc1eb0f13878dd155361beff3'
APP_ID, DATA_NAME = 'org.dos2.port', 'DivinityOriginalSin2Mobile'
GAME = 'Divinity Original Sin 2'
PHONE_DIR = f'/sdcard/Download/{DATA_NAME}'
APK_HINT = 'the APK from this repository\'s Releases page, or build it yourself: build.bat (README.md, "Building")'
MODULES = ('main', 'sdk')                    # the host loads these two (subsdk0 is a byte-for-byte copy of sdk)
MODULE_FILES = [f'{m}.{e}' for m in MODULES for e in ('patched.bin', 'fixups.tsv', 'gates.tsv')]
PY_PACKAGES = (('cryptography', 'decrypting your packages'), ('zstandard', 'NSZ / XCZ input'), ('lz4', 'NSO modules, the game\'s packages'),
               ('capstone', 'the offline patcher'), ('unicorn', 'the patch equivalence check'),
               ('astc_encoder', 'the ASTC texture pack (astc-encoder-py)'), ('numpy', 'the ASTC texture pack'))
PIP_LINE = 'cryptography zstandard lz4 "capstone<6" "unicorn<3" "astc-encoder-py==0.1.12" "numpy<2.4"'
PKG_RE = re.compile(r'\.(nsp|nsz|xci|xcz|xc0|ns0)$', re.I)
META_TYPES = {0x80: 'base game', 0x81: 'update', 0x82: 'DLC', 0x83: 'delta update'}
ROMFS_BYTES = 23 << 29          # the merged RomFS is ~11.4 GB (47 files: Data/*.pak and Data/Localization/*)
ASTC_BYTES = 2 << 30            # the ASTC texture pack (~1.5 GB)
VERIFY_JOB_BYTES = 3 << 29      # peak memory of one dm/verify/equiv.py process on DOS2's main (~1.2 GB)
SMALL_BYTES = 2 << 30           # ExeFS, patched modules, shader jobs, translator build, bundles + pack (~1.5 GB)
WIN = os.name == 'nt'
GB = 2**30
RUN_SETUP = 'run setup (double-click setup.bat on Windows, ./setup.sh on Linux / macOS)'
DRIVERS_README = """Divinity Original Sin 2 Mobile - your own Vulkan driver (optional)
===================================================================

The app brings its own default driver (Mesa Turnip for Adreno GPUs) and needs nothing here.

To try another adrenotools driver package, copy its zip (meta.json + the driver .so) or the unpacked folder into this folder
(Download/DivinityOriginalSin2Mobile/drivers on the phone), restart the game, pick it in the gear dialog under "Vulkan driver"
and restart again ("System" = the phone's own driver). See drivers/README.md in the repository.
"""


STAGES = [('Finding and checking your game files', 'a few seconds'),
          ('Checking the tools and the free space', 'seconds; the first time also the .NET download'),
          ('Extracting the game program (ExeFS)', 'under a minute'),
          ('Patching the program for Android', '1-2 min'),
          ('Checking the patch (per-site equivalence)', '2-10 min'),
          ('Extracting the game data (RomFS, ~11.4 GB)', '5-15 min, longer with NSZ / XCZ'),
          ('Translating the shaders (12 329 shaders)', '5-20 min; the first run also downloads build packages from NuGet'),
          ('Fusing the post-processing shaders (native-post.xgspack, native-nr.xgspack)', '1-2 min; the first time also a ~7 MB download of the SPIR-V tools'),
          ('Making the touch interface package (TouchUI.pak)', 'under a minute'),
          ('Preparing textures for phones without BC (ASTC pack, ~1.5 GB)', '5-20 min')]


class Fail(Exception):
    pass


def say(msg): print(f'== {msg}', flush=True)


LOG_NAME = 'work/setup.log'


class _Tee:
    """Console output also goes to <work>/setup.log (attach it to bug reports); progress bars keep their carriage returns."""
    def __init__(self, stream, f): self.stream, self.f = stream, f
    def write(self, s):
        self.stream.write(s)
        try: self.f.write(s)
        except (OSError, ValueError): pass
        return len(s)
    def flush(self):
        self.stream.flush()
        try: self.f.flush()
        except (OSError, ValueError): pass
    def __getattr__(self, k): return getattr(self.stream, k)


def start_log(work):
    global LOG_NAME
    try:
        work.mkdir(parents=True, exist_ok=True)
        f = open(work / 'setup.log', 'a', encoding='utf-8', errors='replace')
    except OSError:
        return
    LOG_NAME = str(work / 'setup.log')
    import datetime, platform
    f.write(f"\n===== setup {datetime.datetime.now():%Y-%m-%d %H:%M:%S}  Python {sys.version.split()[0]}  {platform.platform()}\n")
    sys.stdout, sys.stderr = _Tee(sys.stdout, f), _Tee(sys.stderr, f)


def stage(n):
    name, est = STAGES[n - 1]
    print(f'\n[{n}/{len(STAGES)}] {name}' + (f'  (usually {est})' if est else ''), flush=True)
    return time.monotonic()


def took(t0):
    s = int(time.monotonic() - t0)
    return f'{s // 60} min {s % 60:02d} s' if s >= 60 else f'{s} s'


def gb(n): return f'{n / GB:.1f} GB'


def ask(question, yes):
    """Yes / no question, default yes. --yes (or DOS2_SETUP_YES=1) answers it; without a terminal the answer is no."""
    if yes or os.environ.get('DOS2_SETUP_YES') == '1':
        print(f'{question} [Y/n] y (--yes)'); return True
    if not (sys.stdin and sys.stdin.isatty()):
        print(f'{question} [Y/n] -> no (no terminal to ask; run with --yes to accept)'); return False
    try:
        return input(f'{question} [Y/n] ').strip().lower() in ('', 'y', 'yes', 'д', 'да')
    except EOFError:
        return False


class Progress:
    """One updating console line, at most a few times per second."""
    def __init__(self): self.t = 0.0

    def __call__(self, text, final=False):
        now = time.monotonic()
        if final or now - self.t > 0.5:
            self.t = now
            print(f'\r  {text}    ', end='\n' if final else '', flush=True)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 24), b''): h.update(b)
    return h.hexdigest()


class Layout:
    def __init__(self, work):
        self.work = Path(work).resolve()
        self.phone = self.work / DATA_NAME
        self.romfs, self.drivers, self.astc = self.phone / 'romfs', self.phone / 'drivers', self.phone / 'astc'
        self.out, self.shaders = self.phone / 'dm' / 'out', self.phone / 'dm' / 'shaders'
        self.pack = self.shaders / 'shaders.xgspack'
        self.touchui = self.romfs / 'Data' / 'TouchUI.pak'
        self.exefs, self.nsp = self.work / 'exefs-update', self.work / 'nsp'
        self.jobs, self.bundles = self.work / 'shaders-jobs', self.work / 'shaders-vk'
        self.xlate = self.work / 'build' / 'dmsh_xlate_d3'   # the same translator build as the Diablo III / Torchlight II ports (XC2 variant)
        self.romfs_manifest = self.work / 'romfs-manifest.json'
        self.equiv = self.work / 'equiv.json'
        self.touchui_stamp = self.work / 'touchui.json'
        self.postpack = self.shaders / 'native-post.xgspack'
        self.postwork, self.postpack_stamp = self.work / 'native-post', self.work / 'native-post.json'
        self.nrpack = self.shaders / 'native-nr.xgspack'   # NR-04 DM_NR_SH: the engine shaders by name (tools/dos2_nr_sh_pack.py)
        self.nrwork, self.nrpack_stamp = self.work / 'native-nr', self.work / 'native-nr.json'
        # NR-33B DM_NR_MAT: gate records of the material depth shaders (tools/dos2_nr_mat.py)
        self.matpack = self.shaders / 'native-nr33b.xgspack'
        self.matwork, self.matpack_stamp = self.work / 'native-nr33b', self.work / 'native-nr33b.json'
        # NR-33C DM_NR_SH_SURF: the surface materials with a wave-coherent early out (tools/dos2_nr_surf.py)
        self.surfpack = self.shaders / 'native-nr33c.xgspack'
        self.surfwork, self.surfpack_stamp = self.work / 'native-nr33c', self.work / 'native-nr33c.json'

    def exefs_ok(self):
        return build_id(self.exefs / 'main') == BUILD_ID and build_id(self.exefs / 'sdk') == SDK_BUILD_ID and (self.exefs / 'main.raw').is_file()

    def out_ok(self):
        # Complete AND made by the patcher in this checkout: build_src.sha256 (written by patch_nso.py) lists the hashes of the
        # patcher sources, so a newer release with new patches regenerates dm/out on the next setup run.
        if not all((self.out / f).is_file() for f in MODULE_FILES): return False
        try: rec = dict(reversed(l.split()) for l in (self.out / 'build_src.sha256').read_text().splitlines() if l.strip())
        except (OSError, ValueError): return False
        for name in ('patch_nso.py', 'nso_code.py'):
            src = ROOT / 'dm' / 'tools' / name
            if src.is_file() and rec.get(name) != hashlib.sha256(src.read_bytes()).hexdigest(): return False
        return True

    def verify_ok(self):
        """equiv.py passed on exactly the patched modules that are in dm/out now."""
        try: rec = json.loads(self.equiv.read_text())
        except (OSError, ValueError): return False
        return all(rec.get(m) == sha256_file(self.out / f'{m}.patched.bin') for m in MODULES
                   if (self.out / f'{m}.patched.bin').is_file()) and self.out_ok()

    def romfs_ok(self):
        return self.romfs_manifest.is_file() and (self.romfs / 'Data' / 'Game.pak').is_file()

    def shader_count(self):
        """Bundles in dm/shaders/shaders.xgspack (0: no pack or not a valid one)."""
        try:
            with open(self.pack, 'rb') as f: magic, ver, n, _ = struct.unpack('<4I', f.read(16))
        except (OSError, struct.error):
            return 0
        return n if magic == 0x50534758 and ver == 1 else 0

    def shaders_ok(self):
        return self.shader_count() > 0

    def touchui_mode(self):
        """'full' when TouchUI.pak is there and made by this checkout's tools and art, else None."""
        try: rec = json.loads(self.touchui_stamp.read_text())
        except (OSError, ValueError): return None
        if not self.touchui.is_file() or rec.get('sha256') != sha256_file(self.touchui) or rec.get('tools') != touchui_tools_hash(): return None
        return rec.get('mode')

    def postpack_ok(self):
        """native-post.xgspack is there and was made by this checkout's fusion tools from the bundles now in shaders.xgspack."""
        try: rec = json.loads(self.postpack_stamp.read_text())
        except (OSError, ValueError): return False
        if not self.postpack.is_file() or rec.get('sha256') != sha256_file(self.postpack) or rec.get('tools') != postpack_tools_hash(): return False
        try: return rec.get('inputs') == postpack_inputs(self)[1]
        except (Fail, ImportError): return False   # no shader pack yet / numpy missing (setup check)

    def nrpack_ok(self):
        """native-nr.xgspack is there and was made by this checkout's tools and GLSL from the bundles now in shaders.xgspack."""
        try: rec = json.loads(self.nrpack_stamp.read_text())
        except (OSError, ValueError): return False
        if not self.nrpack.is_file() or rec.get('sha256') != sha256_file(self.nrpack) or rec.get('tools') != nrpack_tools_hash(): return False
        try: return rec.get('inputs') == nrpack_inputs(self)
        except (Fail, ImportError): return False

    # ---- NR-33B (DM_NR_MAT) block
    def matpack_ok(self):
        """the NR-33B gate records were made by this checkout's tools from the bundles now in shaders.xgspack (a pack without records is fine)."""
        try: rec = json.loads(self.matpack_stamp.read_text())
        except (OSError, ValueError): return False
        if rec.get('tools') != matpack_tools_hash(): return False
        if rec.get('records') and (not self.matpack.is_file() or rec.get('sha256') != sha256_file(self.matpack)): return False
        try: return rec.get('inputs') == matpack_inputs(self)
        except (Fail, ImportError): return False
    # ---- end of the NR-33B block

    # ---- NR-33C (DM_NR_SH_SURF) block
    def surfpack_ok(self):
        """the NR-33C surface variants were made by this checkout's tools from the bundles now in shaders.xgspack."""
        try: rec = json.loads(self.surfpack_stamp.read_text())
        except (OSError, ValueError): return False
        if rec.get('tools') != surfpack_tools_hash(): return False
        if rec.get('bundles') and (not self.surfpack.is_file() or rec.get('sha256') != sha256_file(self.surfpack)): return False
        try: return rec.get('inputs') == surfpack_inputs(self)
        except (Fail, ImportError): return False
    # ---- end of the NR-33C block

    def astc_ok(self):
        try: man = json.loads((self.astc / 'manifest.json').read_text(encoding='utf-8'))
        except (OSError, ValueError): return False
        return man.get('complete') is True and (self.astc / 'dos2-astc.pack').is_file() and \
            os.path.getsize(self.astc / 'dos2-astc.pack') == man.get('pack_bytes')

    def complete(self):
        return self.out_ok() and self.romfs_ok() and self.shaders_ok() and self.touchui_mode() is not None


def touchui_tools_hash():
    """SHA-256 over the touch interface tools (a new release with new movie patches rebuilds TouchUI.pak)."""
    h = hashlib.sha256()
    for p in sorted(TOOLS.glob('dos2_*touch*.py')) + [TOOLS / n for n in ('dos2_touchui_pak.py', 'dos2_swf_graft.py', 'dos2_swf_abc.py', 'lspk.py')] + \
            sorted((ROOT / 'assets' / 'touchui').glob('*.png')):
        if p.is_file(): h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


POST_TOOLS = ('tools/dos2_post_fuse.py', 'tools/spv_splice.py', 'tools/spv_interp.py', 'tools/dos2_native_shaders.py', 'dm/gfx/tools/xgs_pack.py')


def postpack_tools_hash():
    """SHA-256 over the fusion tools, their GLSL helpers and the pinned SPIR-V tools (a release with new fused shaders rebuilds the pack)."""
    h = hashlib.sha256(NDK_REL.encode() + b'\0')
    for p in [ROOT / n for n in POST_TOOLS] + sorted((TOOLS / 'dos2_post' / 'fused_a').glob('*')):
        if p.is_file(): h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


NR_TOOLS = ('tools/dos2_nr_sh_pack.py', 'tools/dos2_pcf_mobile.py', 'tools/dos2_post_mobile.py', 'tools/dos2_light_mobile.py', 'tools/dos2_onchip_shaders.py', 'tools/dos2_lightfuse.py',
            'tools/dos2_native_shaders.py', 'tools/spv_interp.py', 'dm/gfx/tools/xgs_pack.py')


def nrpack_tools_hash():
    """SHA-256 over the NR-04 pack tool, its checks, the GLSL rewrites and the pinned SPIR-V tools (a release with new rewrites rebuilds it)."""
    h = hashlib.sha256(NDK_REL.encode() + b'\0')
    for p in [ROOT / n for n in NR_TOOLS] + sorted((TOOLS / 'dos2_post').glob('*_mobile.frag')) + [TOOLS / 'dos2_post' / 'common.glsl', TOOLS / 'dos2_post' / 'lightfuse_sky.glsl'] + \
            sorted((TOOLS / 'dos2_pcf').glob('*.frag')):
        if p.is_file(): h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


def nrpack_inputs(L):
    """sha256 of the translated bundles the NR-04 pack takes its heads from (tools/dos2_nr_sh_pack.py ENTRIES), read from shaders.xgspack."""
    from dos2_nr_sh_pack import ENTRIES
    return pack_inputs(L, sorted({e[1] for e in ENTRIES}), 'engine shaders')[1]


# ---- NR-33B (DM_NR_MAT) block: gate records of the material depth shaders
MAT_TOOLS = ('tools/dos2_nr_mat.py', 'tools/spv_interp.py', 'dm/gfx/tools/xgs_pack.py')
MATPACK_N = 24     # tools/dos2_nr_mat.py --n: seeded random interpreter runs per record (the same for every player, ~10 s in all)


def matpack_tools_hash():
    h = hashlib.sha256(b'nr33b\0')
    for p in [ROOT / n for n in MAT_TOOLS]:
        if p.is_file(): h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


def matpack_inputs(L):
    """sha256 of programs.tsv + the fragment bundles of every *_DEP_NX material in it, read from shaders.xgspack."""
    prog = L.jobs / 'programs.tsv'
    try: rows = [r.split('\t') for r in prog.read_text(encoding='utf-8').splitlines()[1:]]
    except OSError: raise Fail(f'{prog}: missing (stage 7 writes it)')
    keys = sorted({r[3] for r in rows if len(r) >= 4 and r[0].endswith('_DEP_NX.bshd') and r[3] != '-'})
    return hashlib.sha256(prog.read_bytes() + pack_inputs(L, keys, 'material depth shaders')[1].encode()).hexdigest()
# ---- end of the NR-33B block


# ---- NR-33C (DM_NR_SH_SURF) block: the surface materials with a wave-coherent early out
SURF_TOOLS = ('tools/dos2_nr_surf.py', 'tools/spv_interp.py', 'tools/dos2_nr_sh_pack.py', 'dm/gfx/tools/xgs_pack.py')
SURFPACK_N = 16    # tools/dos2_nr_surf.py --n: seeded random interpreter cases per variant (the same for every player)


def surfpack_tools_hash():
    h = hashlib.sha256(b'nr33c\0')
    for p in [ROOT / n for n in SURF_TOOLS]:
        if p.is_file(): h.update(p.name.encode() + b'\0' + p.read_bytes())
    return h.hexdigest()


def surfpack_inputs(L):
    """sha256 of programs.tsv + the fragment bundles of every surface program tools/dos2_nr_surf.py takes, read from shaders.xgspack."""
    prog = L.jobs / 'programs.tsv'
    if not prog.is_file(): raise Fail(f'{prog}: missing (stage 7 writes it)')
    sys.path.insert(0, str(TOOLS))
    import dos2_nr_surf
    keys = sorted({k for _, k in dos2_nr_surf.programs(prog)})
    return hashlib.sha256(prog.read_bytes() + pack_inputs(L, keys, 'surface shaders')[1].encode()).hexdigest()
# ---- end of the NR-33C block


def postpack_inputs(L):
    """({key: bundle bytes}, sha256) of the translated bundles the fused shaders are made from (tools/dos2_post_fuse.py K), read from
    dm/shaders/shaders.xgspack: the same bytes the phone checks the fused bundles' host heads against."""
    from dos2_post_fuse import K
    return pack_inputs(L, sorted(set(K.values())), 'post-processing shaders')


def pack_inputs(L, want, what):
    """({key: bundle bytes}, sha256) of the translated bundles want, read from dm/shaders/shaders.xgspack."""
    try:
        with open(L.pack, 'rb') as f:
            magic, ver, n, _ = struct.unpack('<4I', f.read(16))
            if magic != 0x50534758 or ver != 1: raise OSError('not an xgspack v1')
            idx = f.read(56 * n); got = {}
            for i in range(n):
                k, o, size, _, crc = struct.unpack_from('<32sQIIQ', idx, 56 * i)
                k = k.rstrip(b'\0').decode('ascii', 'replace')
                if k in want:
                    f.seek(o); got[k] = f.read(size)
                    if len(got[k]) != size or zlib.crc32(got[k]) != crc: raise OSError(f'bundle {k} damaged')
    except (OSError, struct.error) as e:
        raise Fail(f'{L.pack}: cannot read the {what} ({e}); delete it and run setup again')
    miss = [k for k in want if k not in got]
    if miss: raise Fail(f'{L.pack} lacks the {what} {", ".join(miss)}; delete it and run setup again')
    h = hashlib.sha256()
    for k in want: h.update(k.encode() + b'\0' + hashlib.sha256(got[k]).digest())
    return got, h.hexdigest()


def build_id(nso):
    try:
        with open(nso, 'rb') as f:
            h = f.read(0x60)
    except OSError:
        return None
    return h[0x40:0x50].hex() if h[:4] == b'NSO0' else None


def free_bytes(path):
    p = Path(path)
    while not p.exists() and p != p.parent: p = p.parent
    return shutil.disk_usage(p).free


def ram_bytes():
    """Physical memory available now (Windows / Linux / macOS); None when it cannot be read."""
    try:
        if WIN:
            import ctypes
            class MS(ctypes.Structure):
                _fields_ = [('len', ctypes.c_ulong), ('load', ctypes.c_ulong)] + \
                           [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'pf', 'apf', 'virt', 'avirt', 'ext')]
            m = MS(); m.len = ctypes.sizeof(m)
            return m.avail if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)) else None
        if hasattr(os, 'sysconf') and 'SC_AVPHYS_PAGES' in os.sysconf_names:
            return os.sysconf('SC_AVPHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
        out = subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout   # macOS: total only
        return int(out) // 2 if out.strip() else None
    except Exception:
        return None


def verify_jobs():
    """equiv.py processes: one per CPU thread (2..16), and no more than the free memory holds (VERIFY_JOB_BYTES each):
    16 processes on a 16 GB PC ran the Torchlight II port's check out of memory (its issue #1)."""
    jobs = max(2, min(16, os.cpu_count() or 8))
    ram = ram_bytes()
    if ram: jobs = max(1, min(jobs, (ram - (2 << 30)) // VERIFY_JOB_BYTES))   # 2 GB left for the system
    return str(jobs)


def folder_bytes(path):
    return sum(p.stat().st_size for p in Path(path).rglob('*') if p.is_file()) if Path(path).is_dir() else 0


# ---------------------------------------------------------------- finding the inputs
def is_ipa(p):
    """An .ipa (the iPad / iPhone edition): no longer used - the touch interface is made from the Switch files alone."""
    return p.is_file() and p.suffix.lower() == '.ipa'


def looks_like_keys(p):
    """A small text file with header_key = ... (prod.keys, also prod.keys.txt / keys.txt)."""
    try:
        if not p.is_file() or p.stat().st_size > 1 << 20 or PKG_RE.search(p.name): return False
        return re.search(r'^\s*header_key\s*=\s*[0-9a-fA-F]{64}\s*$', p.read_text(errors='replace'), re.M | re.I) is not None
    except OSError:
        return False


def is_package_path(p):
    from content_reader import split_parts
    if p.is_dir(): return bool(split_parts(p))
    return bool(PKG_RE.search(p.name))


def search(folders, depth=0):
    """Packages and key files inside the folders; `depth` levels of subfolders are searched too."""
    pkgs, keys = [], []
    for d in folders:
        try:
            items = sorted(d.iterdir(), key=lambda q: q.name.lower()) if d.is_dir() else []
        except OSError:                         # e.g. a folder of another user in the current directory
            continue
        for p in items:
            try:
                if is_package_path(p): pkgs.append(p)
                elif p.is_file() and looks_like_keys(p): keys.append(p)
                elif p.is_dir() and depth > 0:
                    p2, k2 = search([p], depth - 1); pkgs += p2; keys += k2
            except OSError:                     # unreadable entry: not ours
                continue
    return pkgs, keys


def unique(paths):
    seen, out = set(), []
    for p in paths:
        r = str(Path(p).resolve()).lower() if WIN else str(Path(p).resolve())
        if r not in seen: seen.add(r); out.append(Path(p))
    return out


def gather(args):
    """(packages, key files, where we looked) from explicit paths, or from put-game-files-here/ and the current directory."""
    if args:
        pkgs, keys, folders = [], [], []
        for a in map(Path, args):
            if not a.exists(): raise Fail(f'not found: {a}')
            if a.is_dir() and not is_package_path(a): folders.append(a)
            elif is_ipa(a): print(f'  {a.name}: an .ipa is not needed any more (the touch interface comes from the Switch files); ignored')
            elif a.is_file() and looks_like_keys(a): keys.append(a)
            elif a.is_file() and a.name.lower() in ('title.keys', 'dev.keys'):
                raise Fail(f'{a}: this is {a.name}; setup needs prod.keys (the console keys, with header_key in it)')
            else: pkgs.append(a)
        p2, k2 = search(folders)
        return unique(pkgs + p2), unique(keys + k2), folders or [Path.cwd()]
    folders = unique([DROP, Path.cwd()])
    p1, k1 = search([DROP], depth=2)            # the drop folder also with the game's own folders inside it
    p2, k2 = search([Path.cwd()])
    return unique(p1 + p2), unique(k1 + k2), folders


class Title:
    """One content meta (CNMT) found in a package."""
    def __init__(self, pkg, tid, version, mtype, contents, meta_entry):
        self.pkg, self.tid, self.version, self.mtype = pkg, tid, version, mtype
        self.contents = contents                     # [(content id hex, content type, size)]
        self.meta_entry = meta_entry                 # entry name of the meta NCA in the package
        want = 1 if mtype in (0x80, 0x81) else 2     # CNMT content type: 1 program, 2 data (AOC)
        main = [c for c in contents if c[1] == want]
        self.program = None                          # entry name of the program (or first data) NCA / NCZ in the package
        self.program_size = sum(c[2] for c in main)
        self.program_id = main[0][0] if main else '?'
        self.data = []                               # all program / data entry names that are present
        for c in main:
            for ext in ('.nca', '.ncz'):
                if c[0] + ext in pkg.names: self.data.append(c[0] + ext)
        if self.data: self.program = self.data[0]
        self.complete = bool(main) and len(self.data) == len(main)

    @property
    def compressed(self): return any(n.endswith('.ncz') for n in self.data)

    @property
    def aoc_index(self): return self.tid - AOC_BASE

    def describe(self):
        game = f'{GAME} ' if self.tid & ~0xffff == BASE_TID & ~0xffff else ''
        extra = ''
        if self.mtype == 0x82: extra = f' (index {self.aoc_index})'
        return f"{game}{META_TYPES.get(self.mtype, f'type {self.mtype:#x}')} {self.tid:016X} v{self.version}{extra}"


class Scanned:
    def __init__(self, path, kind, names, titles):
        self.path, self.kind, self.names, self.titles = path, kind, names, titles


def cnmt_titles(pkg_scanned, data, o, meta_entry):
    tid, ver = struct.unpack_from('<QI', data, o)
    mtype = data[o + 12]
    ext, count = struct.unpack_from('<HH', data, o + 14)
    contents = []
    for i in range(count):
        r = o + 0x20 + ext + i * 0x38
        contents.append((data[r + 0x20:r + 0x30].hex(), data[r + 0x36], int.from_bytes(data[r + 0x30:r + 0x35], 'little')))
    return Title(pkg_scanned, tid, ver, mtype, contents, meta_entry)


def scan(path, keys):
    """Container kind and every content meta in a package. Errors become Fail with a plain explanation."""
    from content_reader import Package, Nca
    try:
        p = Package(path, keys)
    except (ValueError, AssertionError, OSError, struct.error) as e:
        why = '' if str(e).startswith('not an NSP') else f' ({e or "unknown layout"})'
        raise Fail(f'{path.name}: not an NSP / NSZ / XCI / XCZ package, or a damaged one{why}')
    try:
        names = {n for n, _, _ in p.entries}
        packed = any(n.endswith('.ncz') for n in names)
        kind = ('XCZ' if packed else 'XCI') if p.kind == 'xci' else ('NSZ' if packed else 'NSP')
        sc = Scanned(path, kind, names, [])
        for name, o, n in p.entries:
            if not name.endswith('.nca'): continue   # nsz compresses only program / data NCAs, never the content meta
            nca = Nca(p, name, o, n)
            if nca.type != 1: continue            # content meta
            data, ents = nca.sections[0].pfs()
            for cname, co, _ in ents:
                if cname.endswith('.cnmt'): sc.titles.append(cnmt_titles(sc, data, co, name))
        return sc
    except KeyError as e:
        k = e.args[0] if e.args else '?'
        raise Fail(f'{path.name}: your prod.keys has no {k}. Dump the keys again from your own console with an up-to-date '
                   'Lockpick_RCM (keys from an older firmware lack the newer key generations).')
    except ValueError as e:
        raise Fail(f'{path.name}: {e}')
    except AssertionError:
        raise Fail(f'{path.name}: the content does not decrypt with this prod.keys (wrong or damaged keys, or a damaged dump)')
    finally:
        p.close()


def resolve(pkgs, key_files, show=True):
    """Identify every package; returns (base Title, update Title, keys path). Fails with what was found otherwise."""
    if not key_files:
        raise Fail('prod.keys not found. Put the prod.keys of your own console next to the game files '
                   '(dump it with Lockpick_RCM on your Switch).')
    if not pkgs:
        raise Fail('no game packages found (NSP / NSZ / XCI / XCZ).')
    scanned, keys, last = [], None, None
    for kf in sorted(key_files, key=lambda k: k.name.lower() != 'prod.keys'):   # prod.keys first, then the others
        try:
            scanned = [scan(p, kf) for p in pkgs]; keys = kf; break
        except Fail as e:
            last = e
    if keys is None: raise last
    titles = [t for s in scanned for t in s.titles]
    if show:
        print(f'  keys: {keys.name}')
        for s in scanned:
            print(f'  {s.path.name}: {s.kind}, ' + (', '.join(t.describe() for t in s.titles) or 'no content meta'))
    found = '; '.join(f'{s.path.name}: ' + (', '.join(t.describe() for t in s.titles) or 'nothing') for s in scanned)
    titles.sort(key=lambda t: not t.complete)   # a complete copy first when the same title is there twice
    base = next((t for t in titles if t.tid == BASE_TID and t.mtype == 0x80), None)
    upd = next((t for t in titles if t.tid == UPDATE_TID and t.mtype == 0x81 and t.version == UPDATE_VERSION), None)
    if base is None:
        raise Fail(f'the {GAME} base game ({BASE_TID:016X}) is not among the files. Found: {found}')
    if upd is None:
        other = [t for t in titles if t.tid == UPDATE_TID and t.mtype == 0x81]
        if other:
            v = other[0].version
            raise Fail(f'your {GAME} update is v{v} (release {v >> 16}); only v{UPDATE_VERSION} (release {UPDATE_VERSION >> 16}) '
                       f'is supported: the patches are made for that exact build. Dump update v{UPDATE_VERSION}.')
        raise Fail(f'the {GAME} update {UPDATE_TID:016X} v{UPDATE_VERSION} is not among the files. Found: {found}')
    for t, what in ((base, 'base game'), (upd, 'update')):
        if not t.complete:
            raise Fail(f'{t.pkg.path.name}: the {what} program content ({t.program_id}) is missing '
                       'from the package (incomplete dump?)')
        check_program(t, keys)
    if show:
        for t in titles:
            if t.mtype == 0x82: print(f'  ({t.describe()}: not needed, skipped)')
    return base, upd, keys


def check_program(t, keys):
    """Open the program NCA now (title key from the ticket, key generation; for a plain NCA also its ExeFS hash table), so
    that a missing ticket or key, or a wrong title key, stops setup at stage 1 instead of in the middle of the work."""
    from content_reader import Package, Nca
    p = Package(t.pkg.path, keys)
    try:
        name, o, n = next(e for e in p.entries if e[0] == t.program)
        if name.endswith('.ncz'):                # compressed body: the NCA size comes from its (original) header
            from extract_exefs_nsz import xts_header
            p.f.seek(o)
            n = struct.unpack_from('<Q', xts_header(p.keys['header_key'], p.f.read(0xc00)), 0x208)[0]
        nca = Nca(p, name, o, n)
        if name.endswith('.nca'):
            for s in nca.sections:
                if s.h[3] == 2:                  # PFS0 (ExeFS): its hash table must match the section header
                    ho, hs = struct.unpack_from('<QQ', s.h, 0x30)
                    assert hashlib.sha256(s.read(ho, hs)).digest() == s.h[8:40]
                    break
    except KeyError as e:
        k = e.args[0] if e.args else '?'
        raise Fail(f'{t.pkg.path.name}: your prod.keys has no {k} (needed for the {t.describe()} program). Dump the keys '
                   'again from your own console with an up-to-date Lockpick_RCM (keys from an older firmware lack the newer '
                   'key generations).')
    except ValueError as e:
        raise Fail(f'{t.pkg.path.name}: {e}')
    except (AssertionError, struct.error):
        raise Fail(f'{t.pkg.path.name}: the {t.describe()} program does not decrypt with this prod.keys and the ticket in '
                   'the package (wrong keys, a wrong or missing ticket, or a damaged dump)')
    finally:
        p.close()


# ---------------------------------------------------------------- steps
def step_exefs(L, upd, keys):
    if L.exefs_ok(): print('  already done'); return
    L.exefs.mkdir(parents=True, exist_ok=True)
    if build_id(L.exefs / 'main') == BUILD_ID and build_id(L.exefs / 'sdk') == SDK_BUILD_ID: upd = None   # only main.raw is missing
    if upd is None: pass
    elif upd.compressed:
        import extract_exefs_nsz
        extract_exefs_nsz.extract(upd.pkg.path, L.exefs, keys, entry=upd.program)
    else:
        from content_reader import Package
        p = Package(upd.pkg.path, keys)
        try:
            nca = next(n for n in p.ncas() if n.name == upd.program)
            for s in nca.sections:
                if s.h[3] != 2: continue
                data, members = s.pfs()
                if 'main' not in [m[0] for m in members]: continue
                for name, o, n in members: (L.exefs / name).write_bytes(data[o:o + n])
        finally:
            p.close()
    if build_id(L.exefs / 'main') != BUILD_ID or build_id(L.exefs / 'sdk') != SDK_BUILD_ID:
        raise Fail(f'main build id {build_id(L.exefs / "main")} / sdk {build_id(L.exefs / "sdk")} are not the supported '
                   f'{BUILD_ID} / {SDK_BUILD_ID} ({GAME} update v{UPDATE_VERSION})')
    import nso_symbols                           # main.raw: the program's own shaders (75 stages built into main) for the shader jobs
    raw, _ = nso_symbols.nso_raw(L.exefs / 'main')
    (L.exefs / 'main.raw').write_bytes(raw)


def step_patch(L):
    if L.out_ok(): print('  already done'); return
    L.out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, DM_TARGET='android', PYTHONUTF8='1')
    for m in MODULES:
        print(f'  patching {m} (dm/tools/patch_nso.py, DM_TARGET=android)', flush=True)
        run([sys.executable, ROOT / 'dm' / 'tools' / 'patch_nso.py', L.exefs / m, L.out, m], env=env)
    if not L.out_ok(): raise Fail('patch_nso.py finished without its outputs')


def step_verify(L):
    """dm/verify/equiv.py: every rewritten site against the original instruction under random states (and a mutation self-check)."""
    if L.verify_ok(): print('  already done'); return
    env = dict(os.environ, PYTHONUTF8='1')
    jobs = verify_jobs()
    for m in MODULES:
        print(f'  checking {m} (dm/verify/equiv.py, {jobs} processes)', flush=True)
        run([sys.executable, ROOT / 'dm' / 'verify' / 'equiv.py', L.out, m, L.exefs / m, '--jobs', jobs], env=env,
            why=f'the patched {m} does not behave like the original at some sites (see above). Please open an issue with that output.')
    L.equiv.write_text(json.dumps({m: sha256_file(L.out / f'{m}.patched.bin') for m in MODULES}, indent=1))


def expand(L, role, t, entries):
    """An NSZ / XCZ title -> a plain NSP in <work>/nsp holding `entries` (expanded); returns its path."""
    dst = L.nsp / f'{role}.nsp'
    if dst.is_file(): return dst
    print(f'  expanding the {role} content from {t.pkg.kind} ({gb(t.program_size)}) -> {dst}', flush=True)
    import ncz2nsp
    L.nsp.mkdir(parents=True, exist_ok=True)
    bar, done = Progress(), [0]
    def prog(n, total=t.program_size):
        done[0] += n; bar(f'{gb(done[0])} of {gb(total)}')
    ncz2nsp.convert(t.pkg.path, dst, only=set(entries), progress=prog)
    bar(f'{gb(done[0])} of {gb(t.program_size)}', final=True)
    return dst


def cleanup_nsp(L, keep_nsp):
    if keep_nsp or not L.nsp.is_dir(): return
    import gc; gc.collect()                      # let the readers close their files (Windows cannot delete open files)
    shutil.rmtree(L.nsp, ignore_errors=True)
    print('  removed the temporary NSPs (--keep-nsp keeps them)' if not L.nsp.exists() else
          f'  could not remove the temporary NSPs in {L.nsp}; delete them by hand')


def step_romfs(L, base, upd, keys, keep_nsp):
    if L.romfs_ok(): print('  already done'); return
    srcs = []
    for role, t in (('base', base), ('update', upd)):
        if t.compressed:
            srcs.append((expand(L, role, t, [t.program]), t.program[:-4] + '.nca'))
        else:
            srcs.append((t.pkg.path, t.program))
    import romfs_d3                              # the generic base + BKTR RomFS reader of the Diablo III port
    if L.romfs.exists(): shutil.rmtree(L.romfs)
    L.touchui_stamp.unlink(missing_ok=True)
    tmp = L.work / 'romfs-manifest.partial'
    bar = Progress()
    print('  verifying the data (IVFC hashes), then writing the files', flush=True)
    romfs_d3.extract(srcs[0][0], srcs[1][0], keys, L.romfs, tmp, base_nca=srcs[0][1], update_nca=srcs[1][1],
                     verify_progress=lambda d, t: bar(f'verified {gb(d)} of {gb(t)}'),
                     file_progress=lambda i, n, d, t: bar(f'file {i} of {n}, {gb(d)} of {gb(t)}', final=i == n))
    tmp.replace(L.romfs_manifest)
    cleanup_nsp(L, keep_nsp)


def spirv_tool(name):
    env = os.environ.get('SPIRV_OPT' if name == 'spirv-opt' else 'SPIRV_VAL')
    if env: return env
    found = shutil.which(name)
    if found: return found
    for sdk in (os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')):
        if sdk:
            for c in Path(sdk).glob(f'shader-tools/*/{name}*'): return str(c)
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if home:
        for c in sorted(Path(home).glob(f'ndk/*/shader-tools/*/{name}*'), reverse=True): return str(c)
    return None


# ---------------------------------------------------------------- .NET
DOTNET_DIR = RUNTIME / 'dotnet'
DOTNET_EXE = 'dotnet.exe' if WIN else 'dotnet'


def sdk_major(d):
    """Highest .NET SDK major version of a dotnet executable (0 when it does not run)."""
    try:
        out = subprocess.run([str(d), '--list-sdks'], capture_output=True, text=True, timeout=60,
                             env=dotnet_env(d)).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    return max([int(l.split('.')[0]) for l in out.splitlines() if l.split('.')[0].isdigit()] + [0])


def find_dotnet():
    """A dotnet with a .NET 10+ SDK (the translator targets net10.0): DOTNET, tools/.runtime/dotnet, ~/.dotnet, PATH."""
    for c in (os.environ.get('DOTNET'), DOTNET_DIR / DOTNET_EXE, Path.home() / '.dotnet' / DOTNET_EXE, shutil.which('dotnet')):
        if c and Path(c).is_file() and sdk_major(c) >= 10: return str(c)
    return None


def dotnet_env(d, base=None):
    env = dict(base or os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    if d:
        env['DOTNET'] = str(d)
        env['DOTNET_ROOT'] = str(Path(d).resolve().parent)   # the translator's apphost finds its runtime there
        if not WIN and Path(d).resolve().parent == DOTNET_DIR.resolve() and 'DOTNET_SYSTEM_GLOBALIZATION_INVARIANT' not in env:
            env['DOTNET_SYSTEM_GLOBALIZATION_INVARIANT'] = '1'   # a private SDK must not depend on the distro's libicu
    return env


def download(url, dst):
    """Download with Python; falls back to curl (e.g. a macOS Python without its certificates installed)."""
    import urllib.request
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(dst, 'wb') as f:
            shutil.copyfileobj(r, f)
        return
    except Exception as e:
        err = e
    if shutil.which('curl') and subprocess.run(['curl', '-fsSL', '-o', str(dst), url]).returncode == 0: return
    raise Fail(f'download failed: {url} ({err}); check the internet connection and run setup again')


def install_dotnet(yes):
    print(f'  The shader step needs the .NET 10 SDK. Setup can install a private copy from Microsoft (dotnet-install script,\n'
          f'  ~300 MB download, ~800 MB on disk) into {DOTNET_DIR}. Nothing is installed system-wide, no admin rights needed.')
    if not ask('  Download and install the .NET 10 SDK there now?', yes):
        raise Fail('the .NET 10 SDK is needed to translate the shaders. Run setup again and answer Y, install it yourself '
                   'from https://dotnet.microsoft.com/download/dotnet/10.0 (SDK), or set DOTNET=<path to dotnet>.')
    script = RUNTIME / ('dotnet-install.ps1' if WIN else 'dotnet-install.sh')
    download('https://dot.net/v1/' + script.name, script)
    if WIN:
        cmd = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), '-Channel', '10.0',
               '-InstallDir', str(DOTNET_DIR), '-NoPath']
    else:
        cmd = ['bash', str(script), '--channel', '10.0', '--install-dir', str(DOTNET_DIR), '--no-path']
    print('  installing (a few minutes)...', flush=True)
    if subprocess.run(cmd).returncode != 0 or sdk_major(DOTNET_DIR / DOTNET_EXE) < 10:
        raise Fail(f'the .NET SDK install into {DOTNET_DIR} failed (see above). Run setup again, or install the .NET 10 SDK '
                   'from https://dotnet.microsoft.com/download/dotnet/10.0')
    return str(DOTNET_DIR / DOTNET_EXE)


def step_shaders(L, dn):
    """The game's .bshd programs (Data/*.pak) + the stages built into main -> translator jobs (tools/dos2_shader_jobs.py) -> translator
    build -> SPIR-V bundles in <work>/shaders-vk -> one pack dm/shaders/shaders.xgspack (what the engine reads)."""
    if L.shaders_ok(): print(f'  already done ({L.shader_count()} shaders in {L.pack.name})'); return
    env = dotnet_env(dn, dict(os.environ, PYTHONUTF8='1', DOS2_MAIN_RAW=str(L.exefs / 'main.raw'), DOS2_MISSING=str(L.work / 'missing')))
    opt, val = spirv_tool('spirv-opt'), spirv_tool('spirv-val')
    if opt: env['SPIRV_OPT'] = opt
    if val: env['SPIRV_VAL'] = val
    if not val: print('  (SPIRV-Tools not found: the optional spirv-val check of each shader is skipped; same output)')
    env.setdefault('J', str(max(2, min(16, os.cpu_count() or 8))))
    if not (L.jobs / 'jobs.tsv').is_file():
        print('  splitting the game\'s shaders (romfs/Data/*.pak + main) into translator jobs (tools/dos2_shader_jobs.py)', flush=True)
        if L.jobs.exists(): shutil.rmtree(L.jobs)
        run([sys.executable, TOOLS / 'dos2_shader_jobs.py', L.romfs / 'Data', L.jobs], env=env)
    if not (L.bundles / '_vk_build.json').is_file():
        exe = L.xlate / 'bin' / ('dmsh_xlate.exe' if WIN else 'dmsh_xlate')
        print('  building the shader translator (tools/build_dmsh_xlate_d3.py; needs internet the first time)', flush=True)
        run([sys.executable, TOOLS / 'build_dmsh_xlate_d3.py', L.xlate], env=env)
        print(f'  translating to SPIR-V bundles (dm/gfx/tools/dmsh_vk_xc2.py, {env["J"]} processes)', flush=True)
        if L.bundles.exists(): shutil.rmtree(L.bundles)
        L.bundles.mkdir(parents=True, exist_ok=True)
        run([sys.executable, ROOT / 'dm' / 'gfx' / 'tools' / 'dmsh_vk_xc2.py', L.jobs, L.bundles], env=dict(env, XLATE=str(exe)))
    n = sum(1 for _ in L.bundles.glob('*.xgs'))
    print(f'  {n} shader bundles; packing them into {L.pack.relative_to(L.phone).as_posix()} (dm/gfx/tools/xgs_pack.py)', flush=True)
    L.shaders.mkdir(parents=True, exist_ok=True)
    sys.path.insert(0, str(ROOT / 'dm' / 'gfx' / 'tools'))
    import xgs_pack
    xgs_pack.build(str(L.bundles), str(L.pack))
    if L.shader_count() != n: raise Fail(f'{L.pack} holds {L.shader_count()} bundles, expected {n}')


# ---------------------------------------------------------------- SPIR-V tools (the post-processing fusion)
# glslc + spirv-as / -dis / -opt / -val of the Android NDK r29 (29.0.14206865: shaderc v2022.3, SPIRV-Tools v2022.4 "ndk-r28b"), the
# build the development pack was made with. Taken from a local NDK r29 when one is installed, else only these files are read out of
# Google's NDK archive with HTTP range requests (~7-10 MB of its 0.8-1 GB) into tools/.runtime/spirv-tools; every file is checked
# against its SHA-256. The Linux / macOS builds load the NDK's libc++, which is fetched with them into the same folder.
NDK_REL = 'r29'
NDK_URL = 'https://dl.google.com/android/repository/android-ndk-r29-{host}.zip'
SPV_DIR = RUNTIME / 'spirv-tools'
SPV_FILES = {   # host -> [(path in the archive below android-ndk-r29/, sha256)]; each file keeps its name in SPV_DIR
    'windows': [
        ('shader-tools/windows-x86_64/glslc.exe', '73700734b25dbf4a707f8406d6154e956733885d6e546dc7cb8e66e81bd285c1'),
        ('shader-tools/windows-x86_64/spirv-as.exe', 'd4304ed9c504793e1d3f3e4bd91fc1b269ac18c4059141f392255527c45a621c'),
        ('shader-tools/windows-x86_64/spirv-dis.exe', '91285ebdab71c65b1326c38a166bb51bb872c1bc5827d22cd5956e46dea2a438'),
        ('shader-tools/windows-x86_64/spirv-opt.exe', 'bf9ebc64dbe5fca3af481c98240c355017169dbb87503e779236d4851aae8084'),
        ('shader-tools/windows-x86_64/spirv-val.exe', 'de0a8bc39c233027cad7b07da20af1c93a6f0851f5692ca3162483e5f0443724')],
    'linux': [
        ('shader-tools/linux-x86_64/glslc', 'dbf3c1539fca04c560c542f1eb78497b6d37c8db01f32803165c1a0483d82ffe'),
        ('shader-tools/linux-x86_64/spirv-as', '814ae86243465a6672be8422af450f5f7cfb5d928f886bf924a8f0bfe0757e97'),
        ('shader-tools/linux-x86_64/spirv-dis', '5253ed93c26fdd541d0ec8ce5eac54a0bce50744e45ad5707692a6d5852448e1'),
        ('shader-tools/linux-x86_64/spirv-opt', '66e970d8fb43095225b42813f93a5edcd5f46c37beb53f9f9df839c93d23dc64'),
        ('shader-tools/linux-x86_64/spirv-val', 'b9535e36cfe55b2d9e9c096f4c85ed81f1a7962388d39005d733f3cce958e0ad'),
        ('toolchains/llvm/prebuilt/linux-x86_64/lib/x86_64-unknown-linux-gnu/libc++.so', '6cfe9f991a987260c5670204aa1e59615d70b4ffb8f43a547d15921142f09f95')],
    'darwin': [     # universal x86_64 + arm64
        ('shader-tools/darwin-x86_64/glslc', '915dda520ea705c7dc45ef1e7e759ee91d23511cbea12bef58b81b33db98049b'),
        ('shader-tools/darwin-x86_64/spirv-as', '31c39f5d3f8669fbbac5ede86ab0c551b2806b0b8e7a053eeae7a93c056b20d8'),
        ('shader-tools/darwin-x86_64/spirv-dis', 'fd8b69a785a36cd062be4b0bc12e5d10f944b8d6685e3d8cf548dc617a591155'),
        ('shader-tools/darwin-x86_64/spirv-opt', 'e607b4fd4f4ffdea15bcc753d7ebdae1203b4939735ea3768ffb5ceab6d494d6'),
        ('shader-tools/darwin-x86_64/spirv-val', '1d3436c303e62f0f1cf447503fda5486360d9deb9e257653d351b7f137f17212'),
        ('toolchains/llvm/prebuilt/darwin-x86_64/lib/libc++.dylib', 'd5654866a07d79e7310062977f1ed856864b9e31c6ebfb01cb2ca996da905b8d')]}
SPV_TOOLS = ('glslc', 'spirv-as', 'spirv-dis', 'spirv-opt', 'spirv-val')
POSTPACK_N = 20     # tools/dos2_post_fuse.py --n: random cases per equivalence check (its default 60 takes ~3x longer; the pack is the same)
NRPACK_N = 40       # tools/dos2_nr_sh_pack.py --n: seeded, so every player's run checks the same 40 cases per shader (all equal, ~3 s)


def spv_host():
    import platform
    if WIN: return 'windows'                     # x64 (also through the x64 emulation of Windows on ARM)
    if sys.platform == 'darwin': return 'darwin'
    if sys.platform.startswith('linux') and platform.machine().lower() in ('x86_64', 'amd64'): return 'linux'
    return None


def spv_env(d, base=None):
    env = dict(base or os.environ, DOS2_SHADER_TOOLS=str(d))
    if sys.platform == 'darwin':                 # the tools want @rpath/libc++.dylib and carry no rpath: it sits next to them
        env['DYLD_FALLBACK_LIBRARY_PATH'] = os.pathsep.join([str(d)] + [p for p in (env.get('DYLD_FALLBACK_LIBRARY_PATH'),) if p])
    return env


def spv_runs(d):
    exe = Path(d) / ('spirv-opt.exe' if WIN else 'spirv-opt')
    try:
        return subprocess.run([str(exe), '--version'], capture_output=True, timeout=60, env=spv_env(d)).returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def spv_tools(fetch=False, yes=False):
    """The folder with glslc / spirv-as / -dis / -opt / -val for tools/dos2_post_fuse.py: DOS2_SHADER_TOOLS (your own, used as it is), else
    tools/.runtime/spirv-tools with the pinned NDK r29 files (copied from a local NDK r29, or with fetch=True downloaded after asking).
    None: not there."""
    own = os.environ.get('DOS2_SHADER_TOOLS')
    if own: return Path(own)
    host = spv_host()
    if not host: return None
    name = lambda m: m.rsplit('/', 1)[1]
    todo = [(m, sha) for m, sha in SPV_FILES[host] if not ((SPV_DIR / name(m)).is_file() and sha256_file(SPV_DIR / name(m)) == sha)]
    if not todo: return SPV_DIR
    ndks = [os.environ.get('ANDROID_NDK_HOME'), os.environ.get('ANDROID_NDK_ROOT')]
    for home in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT')):
        if home: ndks += [str(p) for p in sorted(Path(home).glob('ndk/29.*'), reverse=True)]
    for m, sha in list(todo):                    # a local NDK r29 has the same files
        for nd in filter(None, ndks):
            src = Path(nd) / m
            if src.is_file() and sha256_file(src) == sha:
                put_spv(name(m), src.read_bytes()); todo.remove((m, sha)); break
    if not todo: return SPV_DIR
    if not fetch: return None
    url = NDK_URL.format(host=host)
    print(f'  The fused post-processing shaders need the SPIR-V tools (glslc, spirv-opt, ...). Setup can read just these {len(todo)} files\n'
          f'  out of Google\'s Android NDK {NDK_REL} ({url}, ~{10 if host == "darwin" else 7} MB of it, each file checked against its SHA-256)\n'
          f'  into {SPV_DIR}. Nothing is installed system-wide.')
    if not ask('  Download them there now?', yes):
        raise Fail('the SPIR-V tools were not downloaded (run setup again and answer Y, or set DOS2_SHADER_TOOLS to a folder with '
                   + ', '.join(SPV_TOOLS) + ')')
    ents = zip_directory(url)
    for m, sha in todo:
        e = ents.get('android-ndk-r29/' + m)
        if not e: raise Fail(f'{url} has no {m}')
        data = zip_member(url, e)
        if hashlib.sha256(data).hexdigest() != sha: raise Fail(f'{m} from {url}: SHA-256 mismatch (not the pinned NDK {NDK_REL} file)')
        put_spv(name(m), data)
    return SPV_DIR


def put_spv(name, data):
    SPV_DIR.mkdir(parents=True, exist_ok=True)
    dst = SPV_DIR / name; tmp = dst.with_name(name + '.tmp')
    tmp.write_bytes(data)
    if not WIN: os.chmod(tmp, 0o755)
    os.replace(tmp, dst)


def http_range(url, first, last=None):
    """Bytes first..last of url (last None: its last `first` bytes); Python, else curl."""
    import urllib.request
    r = f'{first}-{last}' if last is not None else f'-{first}'
    want = None if last is None else last - first + 1
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={'Range': 'bytes=' + r}), timeout=120) as f:
            if f.status != 206: raise OSError(f'HTTP {f.status} instead of a partial answer')
            data = f.read()
        if want is None or len(data) == want: return data
        err = f'{len(data)} bytes instead of {want}'
    except Exception as e:
        err = e
    if shutil.which('curl'):
        p = subprocess.run(['curl', '-fsSL', '-r', r, url], capture_output=True)
        if p.returncode == 0 and (want is None or len(p.stdout) == want): return p.stdout
    raise Fail(f'download failed: {url} ({err}); check the internet connection and run setup again')


def zip_directory(url):
    """{name: (method, crc, compressed size, size, local header offset)} of a remote zip (no zip64: the NDK r29 archives need none)."""
    tail = http_range(url, 65558)
    i = tail.rfind(b'PK\x05\x06')
    if i < 0: raise Fail(f'{url}: not a zip archive')
    n, size, off = struct.unpack_from('<HII', tail, i + 10)
    cd, o, ents = http_range(url, off, off + size - 1), 0, {}
    while o + 46 <= len(cd) and cd[o:o + 4] == b'PK\x01\x02':
        meth, _, _, crc, csize, usize, nl, el, cl, _, _, _, lho = struct.unpack_from('<HHHIIIHHHHHII', cd, o + 10)
        ents[cd[o + 46:o + 46 + nl].decode('utf-8', 'replace')] = (meth, crc, csize, usize, lho)
        o += 46 + nl + el + cl
    if len(ents) != n: raise Fail(f'{url}: unexpected zip directory ({len(ents)} of {n} entries)')
    return ents


def zip_member(url, e):
    meth, crc, csize, usize, lho = e
    head = http_range(url, lho, lho + 29)
    if head[:4] != b'PK\x03\x04': raise Fail(f'{url}: bad local header at {lho}')
    nl, el = struct.unpack_from('<HH', head, 26)
    raw = http_range(url, lho + 30 + nl + el, lho + 30 + nl + el + csize - 1)
    data = raw if meth == 0 else zlib.decompressobj(-15).decompress(raw) if meth == 8 else None
    if data is None or len(data) != usize or zlib.crc32(data) != crc: raise Fail(f'{url}: a member did not unpack (method {meth})')
    return data


def step_postpack(L, yes=False):
    """dm/shaders/native-post.xgspack: the fused post-processing shaders drawn by the engine's post-tail fusion (DM_NB_POST, xbg_nb_post.inc):
    PPApplyLUT o EoCSight (F1), FXAA o LUT o EoCSight (F1A, F1AS, F1AR), PPVignette o CombineUI_NoFetch (F2), F3 / F3V - spliced from five
    translated bundles of shaders.xgspack by tools/dos2_post_fuse.py, which validates every module (spirv-val) and checks it against the
    chain of translated shaders in its SPIR-V interpreter. Without the pack the game draws those passes one by one, as recorded."""
    if L.postpack_ok(): print('  already done'); return
    from dos2_post_fuse import K, RECIPES
    bundles, inputs = postpack_inputs(L)
    d = spv_tools(fetch=True, yes=yes)
    if not d:
        raise Fail(f'no prebuilt SPIR-V tools for this machine ({sys.platform}); set DOS2_SHADER_TOOLS to a folder with '
                   + ', '.join(SPV_TOOLS) + ' (Vulkan SDK / shaderc)')
    if not spv_runs(d): raise Fail(f'the SPIR-V tools in {d} do not start on this machine')
    src = L.postwork / 'bundles'
    if L.postwork.exists(): shutil.rmtree(L.postwork)
    src.mkdir(parents=True)
    for k, b in bundles.items(): (src / f'{k}.xgs').write_bytes(b)
    print(f'  splicing and checking {len(RECIPES)} fused shaders (tools/dos2_post_fuse.py; SPIR-V tools: {d})', flush=True)
    r = subprocess.run([sys.executable, str(TOOLS / 'dos2_post_fuse.py'), '--bundles', str(src), '--out', str(L.postwork), '--n', str(POSTPACK_N)],
                       env=spv_env(d, dict(os.environ, PYTHONUTF8='1')), capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:] + r.stderr[-4000:])
        raise Fail('tools/dos2_post_fuse.py failed (see above)')
    pack = L.postwork / 'fused.xgspack'
    want = sorted(f'{n}.{K[rc[0]]}' for n, rc in RECIPES.items())
    try:
        data = pack.read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
        have = sorted(struct.unpack_from('<32s', data, 16 + 56 * i)[0].rstrip(b'\0').decode() for i in range(n)) if magic == 0x50534758 and ver == 1 else []
    except (OSError, struct.error, UnicodeDecodeError):
        have = []
    if have != want: raise Fail(f'{pack}: holds {have}, expected {want}')
    L.shaders.mkdir(parents=True, exist_ok=True)
    tmp = L.postpack.with_name(L.postpack.name + '.tmp'); shutil.copyfile(pack, tmp); os.replace(tmp, L.postpack)
    L.postpack_stamp.write_text(json.dumps(dict(sha256=sha256_file(L.postpack), tools=postpack_tools_hash(), inputs=inputs,
                                                spirv_tools=str(d), bundles=want), indent=1))
    eq = L.postwork / 'equivalence.txt'
    print(f'  {len(want)} fused shaders ({", ".join(w.split(".")[0] for w in want)}), {L.postpack.stat().st_size // 1024} KB -> '
          f'{L.postpack.relative_to(L.phone).as_posix()} (checks: {eq.relative_to(L.work).as_posix()})')


def step_nrpack(L, yes=False):
    """dm/shaders/native-nr.xgspack (NR-04, DM_NR_SH; xbg_game.cpp namespace nrsh): the engine's full-screen lighting / post / shadow-resolve
    fragment shaders as this port's own GLSL rewrites (tools/dos2_post, tools/dos2_pcf) in the heads of your translated bundles, keyed by
    the engine's shader name - tools/dos2_nr_sh_pack.py compiles, validates and checks each (interface; the proven classes also bit for bit
    against the translated shader in its SPIR-V interpreter). Without the pack the translated shaders are drawn, as before."""
    if L.nrpack_ok(): print('  engine shaders by name: already done'); return
    d = spv_tools(fetch=True, yes=yes)
    if not d: raise Fail(f'no prebuilt SPIR-V tools for this machine ({sys.platform}); set DOS2_SHADER_TOOLS (Vulkan SDK / shaderc)')
    if not spv_runs(d): raise Fail(f'the SPIR-V tools in {d} do not start on this machine')
    inputs = nrpack_inputs(L)
    if L.nrwork.exists(): shutil.rmtree(L.nrwork)
    L.nrwork.mkdir(parents=True)
    print(f'  engine shaders by name (tools/dos2_nr_sh_pack.py; SPIR-V tools: {d})', flush=True)
    r = subprocess.run([sys.executable, str(TOOLS / 'dos2_nr_sh_pack.py'), '--pack', str(L.pack), '--out', str(L.nrwork), '--n', str(NRPACK_N),
                        '--programs', str(L.jobs / 'programs.tsv')],   # names <-> keys of your packages (made at stage 7; skipped if absent)
                       env=spv_env(d, dict(os.environ, PYTHONUTF8='1')), capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:] + r.stderr[-4000:])
        raise Fail('tools/dos2_nr_sh_pack.py failed (see above)')
    pack = L.nrwork / 'native-nr.xgspack'
    import dos2_nr_sh_pack as NRP
    ENTRIES, OC_VARIANTS = NRP.ENTRIES, NRP.OC_VARIANTS
    OC_OPTIONAL = getattr(NRP, 'OC_OPTIONAL', {})   # NR-FRAME S2F variants (5.): the pack may hold them or not
    LF_VARIANTS = getattr(NRP, 'LF_VARIANTS', {})   # NR-LIGHTFUSE: f.ClusteredLightingWithSSAO
    want = sorted([f'{v}.{e[0] if len(e[0]) + 2 < 32 else e[1]}' for e in ENTRIES for v in e[3]]   # NR-33A: as dos2_nr_sh_pack.py (a name too long for a key -> the fragment key)
                  + [f'{v}.{e[0]}' for e in ENTRIES for v, _ in OC_VARIANTS.get(e[0], [])])   # NR-ONCHIP input-attachment variants (2. / 3.)
    optional = sorted([f'{v}.{e[0]}' for e in ENTRIES for v, _ in OC_OPTIONAL.get(e[0], [])]
                      + [f'{v}.{e[0]}' for e in ENTRIES for v in LF_VARIANTS.get(e[0], '')])   # NR-LIGHTFUSE f. / g.: optional (the host falls back)
    try:
        data = pack.read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
        have = sorted(struct.unpack_from('<32s', data, 16 + 56 * i)[0].rstrip(b'\0').decode() for i in range(n)) if magic == 0x50534758 and ver == 1 else []
    except (OSError, struct.error, UnicodeDecodeError):
        have = []
    if sorted(k for k in have if k not in optional) != want: raise Fail(f'{pack}: holds {have}, expected {want} (+ optional {optional})')
    want = have   # the stamp records what the pack holds: the required bundles + the optional ones this build made
    L.shaders.mkdir(parents=True, exist_ok=True)
    tmp = L.nrpack.with_name(L.nrpack.name + '.tmp'); shutil.copyfile(pack, tmp); os.replace(tmp, L.nrpack)
    L.nrpack_stamp.write_text(json.dumps(dict(sha256=sha256_file(L.nrpack), tools=nrpack_tools_hash(), inputs=inputs, spirv_tools=str(d),
                                              bundles=want), indent=1))
    print(f'  {len(want)} engine shader bundles, {L.nrpack.stat().st_size // 1024} KB -> {L.nrpack.relative_to(L.phone).as_posix()} '
          f'(checks: {(L.nrwork / "equivalence.txt").relative_to(L.work).as_posix()})')


# ---- NR-33B (DM_NR_MAT) block
def step_matpack(L):
    """dm/shaders/native-nr33b.xgspack (NR-33B, DM_NR_MAT; xbg_nr_mat.inc): per material depth fragment shader of your pack, the paths
    that kill nothing and are decided only by uniform comparisons, with the alpha they leave (tools/dos2_nr_mat.py proves them on the
    SPIR-V and checks each on the interpreter). The phone then draws such depth-only draws without a fragment shader. Without the pack
    only the shaders with no kill at all are swapped (no game code is in the APK)."""
    if L.matpack_ok(): print('  material depth gates: already done'); return
    inputs = matpack_inputs(L)
    if L.matwork.exists(): shutil.rmtree(L.matwork)
    L.matwork.mkdir(parents=True)
    print('  material depth gates (tools/dos2_nr_mat.py)', flush=True)
    r = subprocess.run([sys.executable, str(TOOLS / 'dos2_nr_mat.py'), '--pack', str(L.pack), '--programs', str(L.jobs / 'programs.tsv'),
                        '--out', str(L.matwork), '--n', str(MATPACK_N)], env=dict(os.environ, PYTHONUTF8='1'), capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:] + r.stderr[-4000:])
        raise Fail('tools/dos2_nr_mat.py failed (see above)')
    pack = L.matwork / 'native-nr33b.xgspack'
    n = 0
    if pack.is_file():
        data = pack.read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
        if magic != 0x50534758 or ver != 1: raise Fail(f'{pack}: not an xgspack v1')
        L.shaders.mkdir(parents=True, exist_ok=True)
        tmp = L.matpack.with_name(L.matpack.name + '.tmp'); shutil.copyfile(pack, tmp); os.replace(tmp, L.matpack)
    elif L.matpack.is_file(): L.matpack.unlink()
    L.matpack_stamp.write_text(json.dumps(dict(sha256=sha256_file(L.matpack) if n else '', tools=matpack_tools_hash(), inputs=inputs, records=n), indent=1))
    print(f'  {n} gate records -> {L.matpack.relative_to(L.phone).as_posix()} (report: {(L.matwork / "report.txt").relative_to(L.work).as_posix()})')
# ---- end of the NR-33B block


# ---- NR-33C (DM_NR_SH_SURF) block
def step_surfpack(L):
    """dm/shaders/native-nr33c.xgspack (NR-33C, DM_NR_SH_SURF; xbg_nr_surf.inc): the fragment shaders of the surface materials (puddles,
    blood, oil, poison, fire, cloud shadows, rain) of your pack, each with a wave-coherent early out after its kills / once its alpha is
    known (tools/dos2_nr_surf.py transforms your own SPIR-V by structure and proves every variant: spirv-val + interpreter). Without the
    pack the game's shaders are drawn (no game code is in the APK)."""
    if L.surfpack_ok(): print('  surface early-out variants: already done'); return
    inputs = surfpack_inputs(L)
    if L.surfwork.exists(): shutil.rmtree(L.surfwork)
    L.surfwork.mkdir(parents=True)
    print('  surface early-out variants (tools/dos2_nr_surf.py)', flush=True)
    r = subprocess.run([sys.executable, str(TOOLS / 'dos2_nr_surf.py'), '--pack', str(L.pack), '--programs', str(L.jobs / 'programs.tsv'),
                        '--out', str(L.surfwork), '--n', str(SURFPACK_N)], env=dict(os.environ, PYTHONUTF8='1'), capture_output=True, text=True,
                       encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:] + r.stderr[-4000:])
        raise Fail('tools/dos2_nr_surf.py failed (see above)')
    pack = L.surfwork / 'native-nr33c.xgspack'
    n = 0
    if pack.is_file():
        data = pack.read_bytes(); magic, ver, n, _ = struct.unpack_from('<4I', data, 0)
        if magic != 0x50534758 or ver != 1: raise Fail(f'{pack}: not an xgspack v1')
        L.shaders.mkdir(parents=True, exist_ok=True)
        tmp = L.surfpack.with_name(L.surfpack.name + '.tmp'); shutil.copyfile(pack, tmp); os.replace(tmp, L.surfpack)
    elif L.surfpack.is_file(): L.surfpack.unlink()
    L.surfpack_stamp.write_text(json.dumps(dict(sha256=sha256_file(L.surfpack) if n else '', tools=surfpack_tools_hash(), inputs=inputs, bundles=n), indent=1))
    print(f'  {n} bundles -> {L.surfpack.relative_to(L.phone).as_posix()} (report: {(L.surfwork / "nr_surf.tsv").relative_to(L.work).as_posix()})')
# ---- end of the NR-33C block


def step_touchui(L):
    """romfs/Data/TouchUI.pak: the console menus with touch handling and the on-screen buttons of the HUD and the panels (tools/dos2_touchui_pak.py,
    set "hints"), made from the Switch Game.pak alone; the buttons' art is the port's own (assets/touchui). The game mounts every *.pak in Data itself."""
    want = 'full'
    have = L.touchui_mode()
    if have == want: print('  already done'); return
    sw = L.romfs / 'Data' / 'Game.pak'
    cmd = [sys.executable, TOOLS / 'dos2_touchui_pak.py', 'hints', '--sw', sw, '--out', L.work / 'touchui']
    print('  console menus with touch handling + on-screen buttons (HUD column, panel buttons)', flush=True)
    env = dict(os.environ, PYTHONUTF8='1')
    r = subprocess.run([str(c) for c in cmd], env=env, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if r.returncode:
        print(r.stdout[-4000:] + r.stderr[-4000:])
        raise Fail('tools/dos2_touchui_pak.py failed (see above)')
    bad = [l for l in r.stdout.splitlines() if 'MISMATCH' in l]
    if bad: raise Fail('TouchUI.pak did not read back equal: ' + '; '.join(bad[:3]))
    movies = sum(1 for l in r.stdout.splitlines() if l.startswith('Public/Game/GUI/'))
    shutil.copyfile(L.work / 'touchui' / 'TouchUI.pak', L.touchui)
    L.touchui_stamp.write_text(json.dumps(dict(mode=want, sha256=sha256_file(L.touchui), tools=touchui_tools_hash(), movies=movies), indent=1))
    print(f'  {movies} interface movies, {L.touchui.stat().st_size / 2**20:.1f} MB -> romfs/Data/TouchUI.pak')


def step_astc(L, log=None):
    """<phone>/astc/dos2-astc.pack: every BC1-BC3 texture of the game's packages encoded once to ASTC (tools/dos2_astc_cache.py)."""
    import dos2_astc_cache
    miss = dos2_astc_cache.missing_packages()
    if miss: raise Fail('the Python packages ' + ', '.join(miss) + ' are missing: start setup through setup.bat / ./setup.sh (they install them)')
    jobs = max(1, (os.cpu_count() or 2) - 1)
    print(f'  {jobs} processes; phones with BC textures (Adreno) do not need this pack, the others load it instead of decoding BC', flush=True)
    if dos2_astc_cache.setup_step(L.romfs, L.astc, jobs=jobs, log=lambda t: print('  ' + t, flush=True)):
        raise Fail('the ASTC texture pack was not finished (see above); run setup again to resume')


def make_drivers_folder(L):
    """An empty drivers/ folder with a short how-to: the app brings its default driver; own adrenotools packages go there."""
    L.drivers.mkdir(parents=True, exist_ok=True)
    readme = L.drivers / 'README.txt'
    if not readme.is_file(): readme.write_text(DRIVERS_README, encoding='utf-8')


def run(cmd, env=None, why=None):
    cmd = [str(c) for c in cmd]
    r = subprocess.run(cmd, env=env)
    if r.returncode: raise Fail(why or f'failed ({r.returncode}): {" ".join(cmd)}')


# ---------------------------------------------------------------- phone
def adb_exe():
    c = os.environ.get('ADB') or shutil.which('adb')
    if c: return c
    home = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if home:
        p = Path(home) / 'platform-tools' / ('adb.exe' if WIN else 'adb')
        if p.is_file(): return str(p)
    return None


def adb_devices(adb):
    """[(serial, state)] of the connected devices; [] when adb does not answer."""
    try:
        out = subprocess.run([adb, 'devices'], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [tuple(l.split('\t')[:2]) for l in out.splitlines()[1:] if '\t' in l]


def pick_device(adb, serial):
    if serial: return serial
    devs = adb_devices(adb)
    ready = [s for s, st in devs if st == 'device']
    if len(ready) == 1: return ready[0]
    if not devs: raise Fail('no phone found over adb: connect it with USB debugging enabled (or copy the folder by hand)')
    if not ready: raise Fail('the phone is not authorized: unlock it and accept the "Allow USB debugging" prompt, then retry')
    raise Fail('several phones are connected; name one: setup push <serial> (' + ', '.join(ready) + ')')


def push_commands(L):
    """The adb commands that copy the prepared folder by hand (also printed at the end of setup)."""
    w = lambda p: f'"{p}"'
    return [f'adb shell mkdir -p {PHONE_DIR}/dm',
            f'adb push {w(L.romfs)} {PHONE_DIR}/',
            f'adb push {w(L.out)} {PHONE_DIR}/dm/',
            f'adb push {w(L.shaders)} {PHONE_DIR}/dm/',
            f'adb push {w(L.astc)} {PHONE_DIR}/   (only needed on phones without BC textures: Mali, PowerVR, Xclipse)',
            f'adb push {w(L.drivers)} {PHONE_DIR}/',
            f'adb shell appops set {APP_ID} MANAGE_EXTERNAL_STORAGE allow   (after installing the APK)']


def push(L, serial):
    if not L.complete():
        raise Fail(f'{L.phone} is not complete yet; run the preparation first (setup check shows what is missing)')
    adb = adb_exe()
    if not adb: raise Fail('adb not found (Android platform-tools); copy the folder by hand instead, see docs/INSTALL.md')
    serial = pick_device(adb, serial)
    adb = [adb, '-s', serial]
    if subprocess.run(adb + ['get-state'], capture_output=True, text=True).stdout.strip() != 'device':
        raise Fail(f'adb device {serial} not reachable')
    make_drivers_folder(L)
    for local in [L.out, L.shaders, L.drivers] + ([L.astc] if L.astc_ok() else []) + [L.romfs]:
        rel = local.relative_to(L.phone).as_posix()
        remote = f'{PHONE_DIR}/{rel}'
        listing = subprocess.run(adb + ['shell', f"cd '{remote}' 2>/dev/null && find . -type f -exec stat -c '%s %n' {{}} +"],
                                 capture_output=True, text=True).stdout
        have = {}
        for line in listing.splitlines():
            size, _, name = line.strip().partition(' ')
            if size.isdigit(): have[name[2:] if name.startswith('./') else name] = int(size)
        if local == L.drivers:                   # never overwrite the player's drivers: only add the README when missing
            todo = [p for p in sorted(local.rglob('*')) if p.is_file() and p.relative_to(local).as_posix() not in have]
        else:
            todo = [p for p in sorted(local.rglob('*')) if p.is_file() and have.get(p.relative_to(local).as_posix()) != p.stat().st_size]
        say(f'push {rel}: {len(todo)} of {sum(1 for p in local.rglob("*") if p.is_file())} files to copy')
        dirs = set()
        for p in todo:
            rp = f'{remote}/{p.relative_to(local).as_posix()}'
            d = rp.rsplit('/', 1)[0]
            if d not in dirs:
                subprocess.run(adb + ['shell', f"mkdir -p '{d}'"], check=True); dirs.add(d)
            print(f'  {p.relative_to(L.phone).as_posix()} ({p.stat().st_size / 2**20:.1f} MB)', flush=True)
            if subprocess.run(adb + ['push', str(p), rp], capture_output=True).returncode:
                raise Fail(f'adb push failed: {p} (run again to resume)')
    if APP_ID in subprocess.run(adb + ['shell', 'pm', 'list', 'packages', APP_ID], capture_output=True, text=True).stdout:
        subprocess.run(adb + ['shell', 'appops', 'set', APP_ID, 'MANAGE_EXTERNAL_STORAGE', 'allow'])
        say(f'{APP_ID}: All files access granted')
    else:
        say(f'the app is not installed yet: install {APK_HINT}, then allow "All files access" when it asks')


def next_steps(L, offer_push, yes):
    size = folder_bytes(L.phone)
    print(f'\n== Done. The game folder is ready: {L.phone} ({gb(size)})')
    adb = adb_exe() if offer_push else None
    if adb:
        ready = [s for s, st in adb_devices(adb) if st == 'device']
        if len(ready) == 1:
            model = subprocess.run([adb, '-s', ready[0], 'shell', 'getprop', 'ro.product.model'], capture_output=True,
                                   text=True).stdout.strip()
            if ask(f'A phone is connected over USB ({model or ready[0]}). Copy the folder to it now ({gb(size)}, '
                   'a few minutes)?', yes):
                push(L, ready[0])
                print(f'\nNext: install the APK if you have not yet, start {GAME} Mobile, allow "All files access". Have fun!')
                return
    print(f'''
What to do next:
  1. Copy the whole "{DATA_NAME}" folder into the phone's Download folder (internal storage), so that the phone has
       Download/{DATA_NAME}/romfs   Download/{DATA_NAME}/dm/out   Download/{DATA_NAME}/dm/shaders   Download/{DATA_NAME}/astc
     (USB cable + file manager is fine; the phone needs ~{gb(size + GB)} free; the astc folder is only needed on phones
     without BC textures - Mali, PowerVR, Xclipse - and can be left out on Adreno). Or over adb (USB debugging):
       setup push [serial]          (copies only what is missing; resumes after an interruption)
     or by hand:''')
    for c in push_commands(L): print(f'       {c}')
    print(f'''  2. Install the app on the phone: {APK_HINT}.
  3. Start {GAME} Mobile and allow "All files access" when it asks.
  Step-by-step guide and troubleshooting: docs/INSTALL.md (Russian: docs/INSTALL.ru.md)''')


# ---------------------------------------------------------------- check
def missing_packages():
    import importlib.util as iu
    return [m for m, _ in PY_PACKAGES if iu.find_spec(m) is None]


def check(L, files):
    nxt = []
    def item(good, text, fix=None):
        print(f"  {'OK ' if good else '-- '} {text}")
        if not good and fix: nxt.append(fix)
    say('1. Python')
    item(sys.version_info >= (3, 10), f'Python {sys.version.split()[0]}: {sys.executable}', 'Python 3.10 or newer is needed (setup.bat / setup.sh offer one)')
    import importlib.util as iu
    for mod, why in PY_PACKAGES:
        item(iu.find_spec(mod) is not None, f'{mod} ({why})', f'run setup.bat / ./setup.sh (it installs the packages), or: "{sys.executable}" -m pip install {PIP_LINE}')
    say('2. Tools')
    dn = find_dotnet()
    item(bool(dn) or L.shaders_ok(), f'.NET 10 SDK: {dn or "not found (setup offers to install a private copy into tools/.runtime/dotnet)"}')
    for t in ('spirv-opt', 'spirv-val'):
        print(f"  {'OK ' if spirv_tool(t) else '.. '} {t}: {spirv_tool(t) or 'not found (optional: only checks the translated shaders)'}")
    sd = spv_tools()
    print(f"  {'OK ' if sd else '.. '} SPIR-V tools for the post-processing fusion: "
          f"{sd or f'not there yet (setup downloads them: Android NDK {NDK_REL}, ~7 MB, into {SPV_DIR})'}")
    adb = adb_exe()
    print(f"  {'OK ' if adb else '.. '} adb: {adb or 'not found (optional: only for copying to the phone over USB debugging)'}")
    say('3. Game files')
    pkgs, keys, where = gather(files)
    if not files: print('  looked in: ' + ', '.join(str(w) for w in where))
    ok_files = L.complete()
    if missing_packages():
        print('  .. (the Python packages above are needed to identify the files)')
    elif not pkgs and not keys:
        item(ok_files, f'no game files found; put the base game, update v{UPDATE_VERSION} and prod.keys into {DROP}',
             f'put the game files and prod.keys into {DROP}, then run setup again')
    else:
        try:
            base, upd, kf = resolve(pkgs, keys)
            item(True, f'base game: {base.pkg.path.name}; update v{UPDATE_VERSION}: {upd.pkg.path.name}; keys: {kf.name}')
        except Fail as e:
            item(ok_files, str(e), 'fix the game files (see the message above), then run setup again')
    say(f'4. Work folder {L.work}')
    free = free_bytes(L.work)
    need = (0 if L.romfs_ok() else ROMFS_BYTES + SMALL_BYTES) + (0 if L.astc_ok() else ASTC_BYTES)
    item(free >= need, f'{gb(free)} free ({gb(need)}+ needed for the remaining steps; up to ~28 GB with NSZ / XCZ input)',
         'free some disk space, or use --work <folder on another disk>')
    item(L.exefs_ok(), f'ExeFS of update v{UPDATE_VERSION} (build id {BUILD_ID[:16]}...)', RUN_SETUP)
    item(L.out_ok(), 'dm/out: patched main / sdk', RUN_SETUP)
    print(f"  {'OK ' if L.verify_ok() else '.. '} patch equivalence check (dm/verify/equiv.py){'' if L.verify_ok() else ': not run yet'}")
    item(L.romfs_ok(), 'romfs: extracted and verified', RUN_SETUP)
    item(L.shaders_ok(), f'dm/shaders: {L.shader_count()} SPIR-V bundles in shaders.xgspack', RUN_SETUP)
    item(L.shaders_ok() and L.postpack_ok(), 'dm/shaders/native-post.xgspack: fused post-processing shaders', RUN_SETUP)
    item(L.shaders_ok() and L.nrpack_ok(), 'dm/shaders/native-nr.xgspack: engine shaders by name (NR-04)', RUN_SETUP)
    item(L.shaders_ok() and L.matpack_ok(), 'dm/shaders/native-nr33b.xgspack: material depth gates (NR-33B)', RUN_SETUP)   # NR-33B
    item(L.shaders_ok() and L.surfpack_ok(), 'dm/shaders/native-nr33c.xgspack: surface early-out variants (NR-33C)', RUN_SETUP)   # NR-33C
    mode = L.touchui_mode()
    item(mode is not None, f'romfs/Data/TouchUI.pak: {"made" if mode else "not made yet"}', RUN_SETUP)
    print(f"  {'OK ' if L.astc_ok() else '.. '} astc/dos2-astc.pack (phones without BC){'' if L.astc_ok() else ': not made yet'}")
    if L.nsp.is_dir(): print(f'  ..  temporary NSPs in {L.nsp} (removed after the RomFS step)')
    if nxt:
        say(f'Next: {nxt[0]}')
        return 1
    say(f'Ready: copy {L.phone} to Download/{DATA_NAME} on the phone ({PHONE_DIR}), or: setup push')
    return 0


# ---------------------------------------------------------------- prepare
def prepare(a, L):
    steps = set(a.steps.split(','))
    shaders_needed = 'shaders' in steps and not a.skip_shaders and not L.shaders_ok()
    astc_wanted = 'astc' in steps and not a.skip_astc
    post_wanted = 'post' in steps and not a.skip_post
    miss = missing_packages()
    if sys.version_info < (3, 10) or miss:
        raise Fail(f'Python 3.10+ with {", ".join(miss) or "the packages"} is needed; start setup through setup.bat / '
                   f'./setup.sh (they install them), or: "{sys.executable}" -m pip install {PIP_LINE}')

    t0 = stage(1)
    pkgs, keys, where = gather(a.files)
    if not pkgs and not keys and not a.files:
        raise Fail(f'no game files found. Put your {GAME} base game, the update v{UPDATE_VERSION} (NSP / NSZ / XCI / XCZ, any '
                   f'file names) and your prod.keys into\n  {DROP}\nand run setup again (or drop the files onto setup.bat). '
                   f'Also searched: {Path.cwd()}')
    try:
        base, upd, kf = resolve(pkgs, keys)
    except Fail as e:
        if a.files: raise
        raise Fail(f'{e}\n  (searched: ' + ', '.join(str(w) for w in where) + ')')
    print(f'  base game: {base.pkg.path.name} ({base.pkg.kind}), {GAME} v{base.version}')
    print(f'  update:    {upd.pkg.path.name} ({upd.pkg.kind}), v{upd.version} -- the supported version')
    print(f'  checked in {took(t0)}')

    t0 = stage(2)
    print(f'  Python {sys.version.split()[0]} with its packages: OK')
    need = SMALL_BYTES
    if 'romfs' in steps and not L.romfs_ok():
        need += max(0, ROMFS_BYTES - folder_bytes(L.romfs))   # a partial RomFS is deleted first
        for role, t in (('base', base), ('update', upd)):
            if t.compressed and not (L.nsp / f'{role}.nsp').is_file(): need += t.program_size
    if astc_wanted and not L.astc_ok(): need += ASTC_BYTES
    free = free_bytes(L.work)
    print(f'  output: {L.phone}')
    print(f'  disk space: {gb(free)} free, about {gb(need)} needed while preparing')
    if free < need:
        raise Fail(f'not enough free space on the disk of {L.work}: {gb(free)} free, ~{gb(need)} needed. Free some space, '
                   'or put the work folder on another disk: setup --work <folder> (keep the game files where they are).')
    dn = find_dotnet()
    if dn: print(f'  .NET 10 SDK: {dn}')
    elif shaders_needed and not a.dry_run: dn = install_dotnet(a.yes)
    elif shaders_needed: print('  .NET 10 SDK: not found; setup will offer to install it (tools/.runtime/dotnet)')
    else: print('  .NET 10 SDK: not needed (shaders done or skipped)')
    print(f"  SPIRV-Tools: {spirv_tool('spirv-val') or 'not found (optional)'}")
    if post_wanted and not (L.postpack_ok() and L.nrpack_ok()):
        print(f"  SPIR-V tools for the post-processing fusion: {spv_tools() or f'downloaded at stage 8 (Android NDK {NDK_REL}, ~7 MB, into {SPV_DIR})'}")
    verify = 'verify' in steps and not a.skip_verify
    plan = [('ExeFS', L.exefs_ok() or 'exefs' not in steps), ('patch', L.out_ok() or 'patch' not in steps),
            ('patch check', not verify or L.verify_ok()),
            ('RomFS' + (' (compressed input is expanded first)' if base.compressed or upd.compressed else ''),
             L.romfs_ok() or 'romfs' not in steps), ('shaders', not shaders_needed),
            ('post-processing fusion', not post_wanted or L.postpack_ok()),
            ('engine shaders by name', not post_wanted or L.nrpack_ok()),
            ('material depth gates', not post_wanted or L.matpack_ok()),   # NR-33B
            ('surface early-out variants', not post_wanted or L.surfpack_ok()),   # NR-33C
            ('touch UI', 'touchui' not in steps or L.touchui_mode() == 'full'),
            ('ASTC textures', not astc_wanted or L.astc_ok())]
    print('  plan: ' + ', '.join(f"{n} {'(done/skipped)' if d else ''}".strip() for n, d in plan))
    if a.dry_run:
        say('dry run: inputs, tools and space checked; nothing was written'); return 0

    t0 = stage(3)
    if 'exefs' in steps: step_exefs(L, upd, kf); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(4)
    if 'patch' in steps: step_patch(L); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(5)
    if verify and L.out_ok(): step_verify(L); print(f'  done in {took(t0)}')
    else: print('  skipped' + (' (--skip-verify / --steps)' if not verify else ''))
    t0 = stage(6)
    if 'romfs' in steps: step_romfs(L, base, upd, kf, a.keep_nsp); print(f'  done in {took(t0)}')
    else: print('  skipped (--steps)')
    t0 = stage(7)
    if shaders_needed:
        step_shaders(L, dn or find_dotnet() or install_dotnet(a.yes)); print(f'  done in {took(t0)}')
    else: print(f'  already done ({L.shader_count()} shaders)' if L.shaders_ok() else '  skipped')
    t0 = stage(8)
    post_err = None
    if post_wanted and L.shaders_ok():
        try:
            step_postpack(L, a.yes); print(f'  done in {took(t0)}')
        except Fail as e:                        # optional: the game runs without it, so the remaining stages still run
            post_err = str(e)
            print(f'  NOT MADE: {e}\n  The game runs without it (the post-processing passes are drawn one by one, a little slower); '
                  'run setup again to retry.')
        try:
            step_nrpack(L, a.yes); print(f'  done in {took(t0)}')
        except Fail as e:                        # optional too: without it the translated shaders are drawn
            post_err = (post_err + '; ' if post_err else '') + str(e)
            print(f'  NOT MADE: {e}\n  The game runs without it (the translated engine shaders are drawn); run setup again to retry.')
        try:                                     # NR-33B, optional as well
            step_matpack(L); print(f'  done in {took(t0)}')
        except Fail as e:
            post_err = (post_err + '; ' if post_err else '') + str(e)
            print(f'  NOT MADE: {e}\n  The game runs without it (only depth shaders without a kill are swapped); run setup again to retry.')
        try:                                     # NR-33C, optional as well
            step_surfpack(L); print(f'  done in {took(t0)}')
        except Fail as e:
            post_err = (post_err + '; ' if post_err else '') + str(e)
            print(f'  NOT MADE: {e}\n  The game runs without it (the surface shaders are drawn as translated); run setup again to retry.')
    else: print('  skipped' + (' (--skip-post / --steps)' if not post_wanted else ' (no shader pack yet)'))
    t0 = stage(9)
    if 'touchui' in steps and L.romfs_ok(): step_touchui(L); print(f'  done in {took(t0)}')
    else: print('  skipped' + ('' if L.romfs_ok() else ' (no RomFS yet)'))
    t0 = stage(10)
    if astc_wanted and L.romfs_ok(): step_astc(L); print(f'  done in {took(t0)}')
    else: print('  skipped' + (' (--skip-astc / --steps)' if not astc_wanted else ' (no RomFS yet)'))
    if post_err: say(f'note: dm/shaders/native-post.xgspack / native-nr.xgspack not made ({post_err}); run setup again to retry')
    if not L.complete():
        say(f'finished the requested steps; the folder is not complete yet: {L.phone}'); return 0
    make_drivers_folder(L)
    if a.push:
        push(L, None if a.push == 'auto' else a.push); return 0
    next_steps(L, False, a.yes)
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    default_work = os.environ.get('DM_DOS2_WORK') or ROOT / 'work'
    if argv and argv[0] in ('-h', '--help', 'help'):
        print(__doc__); return 0
    if argv and argv[0] in ('check', 'push'):
        ap = argparse.ArgumentParser(prog=f'setup {argv[0]}')
        if argv[0] == 'push': ap.add_argument('serial', nargs='?')
        else: ap.add_argument('files', nargs='*')
        ap.add_argument('--work', default=default_work)
        ap.add_argument('--yes', '-y', action='store_true')
        a = ap.parse_args(argv[1:])
        L = Layout(a.work)
        if argv[0] == 'check': return check(L, a.files)
        push(L, a.serial); return 0
    ap = argparse.ArgumentParser(prog='setup', description=f'Prepare {GAME} Mobile game files from your own dumps.')
    ap.add_argument('files', nargs='*', help=f'base game, update v{UPDATE_VERSION}, prod.keys in any order '
                                             '(or folders to search); none: put-game-files-here/ and the current folder')
    ap.add_argument('--work', default=default_work)
    ap.add_argument('--push', metavar='SERIAL', nargs='?', const='auto')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--yes', '-y', action='store_true')
    ap.add_argument('--keep-nsp', action='store_true')
    ap.add_argument('--skip-shaders', action='store_true')
    ap.add_argument('--skip-verify', action='store_true', help='do not run the patch equivalence check (dm/verify/equiv.py)')
    ap.add_argument('--skip-post', action='store_true', help='no fused post-processing shaders (dm/shaders/native-post.xgspack) and no '
                    'engine shaders by name (native-nr.xgspack); the game then draws those passes one by one, with the translated shaders')
    ap.add_argument('--skip-astc', action='store_true', help='no ASTC texture pack (phones with BC textures, i.e. Adreno, do not use it)')
    ap.add_argument('--ipa', default=None, help=argparse.SUPPRESS)   # accepted and ignored: the touch interface no longer reads the iPad edition
    ap.add_argument('--no-dlc', action='store_true', help=argparse.SUPPRESS)
    ap.add_argument('--steps', default='exefs,patch,verify,romfs,shaders,post,touchui,astc', help='comma list of steps to run (default: all)')
    a = ap.parse_args(argv)
    L = Layout(a.work)
    start_log(Path(a.work))
    print(f'{GAME} Mobile setup: prepares the game folder for the phone from your own {GAME} (Switch) dumps.')
    return prepare(a, L)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Fail as e:
        print(f'\nSTOP: {e}', file=sys.stderr); sys.exit(2)
    except KeyboardInterrupt:
        print('\ninterrupted; run setup again to continue where it stopped', file=sys.stderr); sys.exit(130)
    except Exception as e:                      # never end with a bare traceback: say what to do with it
        import traceback; traceback.print_exc()
        why = f'{e.strerror}: {e.filename}' if isinstance(e, OSError) and e.strerror and e.filename else f'{type(e).__name__}: {e}'
        print(f'\nSTOP: unexpected error ({why}). Run setup again (finished steps are kept); if it happens again, open an '
              'issue on this repository with the text above.', file=sys.stderr); sys.exit(1)
