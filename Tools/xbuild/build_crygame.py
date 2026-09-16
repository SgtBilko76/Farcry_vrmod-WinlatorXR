#!/usr/bin/env python3
"""Cross-compile CryGame.dll (Far Cry VR mod) on Linux with clang-cl + lld-link.

Mirrors the Release|Win32 command line of "Sources/CryGame C++/Solution1/CryGame/CryGame.vcxproj":
the source list and per-file exclusions are read from that project, the compiler/linker flags are
transcribed from its Release|Win32 settings, and the Microsoft CRT + Windows SDK come from the xwin
winsysroot (same toolchain as ../crysis_vrmod/tools/xbuild). Far Cry is a 2004 codebase, so:
  - a case-shim directory is generated for headers included with the wrong case (Linux is
    case-sensitive: 68 files include "stdafx.h" but the file is "StdAfx.h", etc.);
  - the precompiled header is skipped - every TU includes StdAfx.h directly (simpler and immune to
    the mixed-case PCH include spellings).

Usage:
  tools/xbuild/build_crygame.py [--jobs N] [--warnings] [--clean] [-k] [--push]
  --push   adb push the resulting Release/CryGame.dll to the Quest mod folder
"""
import argparse
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SLN = os.path.join(REPO, 'Sources', 'CryGame C++', 'Solution1')       # ..\STLPORT, ..\CryCommon live here
PROJ = os.path.join(SLN, 'CryGame')                                    # the vcxproj + all .cpp/.h
THIRD = os.path.join(REPO, 'Sources', 'ThirdParty')
VCXPROJ = os.path.join(PROJ, 'CryGame.vcxproj')
BUILD_DIR = os.path.join(REPO, 'BinTemp', 'xbuild', 'x86')
OUT_DIR = os.path.join(PROJ, 'Release')                               # same output path as the VS build
SHIM_DIR = os.path.join(BUILD_DIR, 'caseshim')

OPT = os.environ.get('XBUILD_OPT', os.path.expanduser('~/.local/opt'))
LLVM = os.environ.get('XBUILD_LLVM', os.path.join(OPT, 'llvm'))
WINSYSROOT = os.environ.get('XBUILD_WINSYSROOT', os.path.join(OPT, 'winsysroot'))
MSBUILD_NS = '{http://schemas.microsoft.com/developer/msbuild/2003}'
PLATFORM = 'Win32'

# AdditionalIncludeDirectories from CryGame.vcxproj (Release|Win32), resolved to absolute paths.
INCLUDE_DIRS = [
    os.path.join(SLN, 'STLPORT', 'stlport'),
    os.path.join(SLN, 'CryCommon'),
    os.path.join(THIRD, 'dxvk', 'src', 'd3d9'),
    os.path.join(THIRD, 'dxvk', 'include', 'vulkan', 'include'),
    os.path.join(THIRD, 'minhook', 'include'),
    os.path.join(THIRD, 'openvr', 'headers'),
    os.path.join(THIRD, 'ffmpeg', 'include'),
    os.path.join(THIRD, 'bhaptics', 'include', 'shared'),
    os.path.join(THIRD, 'protubevr'),
    PROJ,
]

DEFINES = ['_RELEASE', 'NDEBUG', 'USE_GAME_DLL', 'WIN32', '_WINDOWS', '_USRDLL', 'CRYGAME_EXPORTS',
           '_SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS']

# Release|Win32 ClCompile: Optimization=Full(/Ox) InlineFunctionExpansion=AnySuitable(/Ob2)
# IntrinsicFunctions(/Oi) FavorSizeOrSpeed=Size(/Os) OmitFramePointers(/Oy) StringPooling(/GF)
# RuntimeLibrary=MultiThreadedDLL(/MD) BufferSecurityCheck=false(/GS-).
# NOTE: the vcxproj favours size (/Os) with no /arch. Under clang we replaced Far Cry's hand-written
# MMX/SSE asm (matrix/vector math, memcpy) with portable C, so we compensate by letting clang vectorise:
# /arch:SSE2 (what MSVC uses for x86 by default anyway) + /Ot (favour speed) recover the lost throughput.
CFLAGS = ['/Ox', '/Ob2', '/Oi', '/Ot', '/Oy', '/GF', '/MD', '/GS-', '/EHsc', '/arch:SSE2',
          '/Zc:wchar_t', '/Zc:forScope', '/Zc:inline', '/Gd']

# Link AdditionalDependencies (Release|Win32) + the usual MSVC default libs.
LIBS = ['d3d9.lib', 'dsound.lib', 'openvr_api.lib', 'avcodec.lib', 'avformat.lib', 'avutil.lib',
        'swscale.lib', 'swresample.lib', 'haptic_library.lib', 'protubevr.lib', 'Shlwapi.lib',
        'wininet.lib', 'Ws2_32.lib']
DEFAULT_LIBS = ['kernel32.lib', 'user32.lib', 'gdi32.lib', 'winspool.lib', 'comdlg32.lib',
                'advapi32.lib', 'shell32.lib', 'ole32.lib', 'oleaut32.lib', 'uuid.lib',
                'odbc32.lib', 'odbccp32.lib']
LIBDIRS = [os.path.join(THIRD, 'dxvk', 'lib'),                 # the tuned d3d9.lib import library
           os.path.join(THIRD, 'openvr', 'lib', 'win32'),
           os.path.join(THIRD, 'bhaptics', 'bin', 'win32'),
           os.path.join(THIRD, 'ffmpeg', 'lib'),
           os.path.join(THIRD, 'protubevr')]


def die(msg):
    print('error: ' + msg, file=sys.stderr)
    sys.exit(1)


class CaseInsensitivePaths:
    """Resolve the Windows-style (case-insensitive, backslash) paths used in the .vcxproj."""

    def __init__(self, root):
        self.root = root
        self.index = {}
        for dp, dns, fns in os.walk(root):
            for name in dns + fns:
                full = os.path.join(dp, name)
                self.index.setdefault(os.path.relpath(full, root).lower(), full)

    def resolve(self, winpath):
        rel = os.path.normpath(winpath.replace('\\', '/'))
        exact = os.path.join(self.root, rel)
        if os.path.exists(exact):
            return exact
        return self.index.get(rel.lower())


def config_value(elem, tag):
    wanted = ("'$(Configuration)|$(Platform)'=='Release|%s'" % PLATFORM).replace(' ', '')
    for child in elem.findall(MSBUILD_NS + tag):
        cond = child.get('Condition')
        if cond is None or cond.replace(' ', '') == wanted:
            return (child.text or '').strip()
    return None


def read_sources():
    tree = ET.parse(VCXPROJ)
    paths = CaseInsensitivePaths(PROJ)
    sources = []
    for item in tree.getroot().iter(MSBUILD_NS + 'ClCompile'):
        include = item.get('Include')
        if not include:
            continue
        if (config_value(item, 'ExcludedFromBuild') or '').lower() == 'true':
            continue
        path = paths.resolve(include)
        if not path:
            die('source listed in CryGame.vcxproj not found: ' + include)
        sources.append(path)
    return sources


def _sdk_lib_dirs():
    """Windows SDK import-library dirs (x86)."""
    dirs = []
    lib = sorted(glob.glob(os.path.join(WINSYSROOT, 'Windows Kits', '10', 'Lib', '*')))
    if lib:
        dirs += [os.path.join(lib[-1], d, 'x86') for d in ('um', 'ucrt')]
    msvc = sorted(glob.glob(os.path.join(WINSYSROOT, 'VC', 'Tools', 'MSVC', '*', 'lib', 'x86')))
    if msvc:
        dirs.append(msvc[-1])
    return [d for d in dirs if os.path.isdir(d)]


def build_lib_shim():
    """xwin lowercases (mostly) the SDK import libs, but the .vcxproj and #pragma comment(lib,...) in the
    sources request mixed-case names (Shlwapi.lib, Ws2_32.lib, Winmm.lib). On a case-sensitive FS lld-link
    can't open those, so symlink each requested name to the case-insensitive match in the SDK lib dirs."""
    shim = os.path.join(BUILD_DIR, 'libshim')
    if os.path.isdir(shim):
        shutil.rmtree(shim)
    os.makedirs(shim)
    sdk_lib_dirs = _sdk_lib_dirs()
    # case-insensitive index of real SDK libs (prefer an all-lowercase spelling when several cases exist)
    ci = {}
    for d in sdk_lib_dirs:
        for f in sorted(os.listdir(d)):
            ci.setdefault(f.lower(), os.path.join(d, f))
    # every lib we request explicitly, plus every #pragma comment(lib, "...") in the sources
    requested = set(LIBS + DEFAULT_LIBS)
    pragma = re.compile(r'#\s*pragma\s+comment\s*\(\s*lib\s*,\s*"([^"]+)"', re.I)
    for dp, _, fns in os.walk(PROJ):
        for f in fns:
            if f.lower().endswith(('.cpp', '.c', '.h', '.inl')):
                try:
                    requested.update(pragma.findall(open(os.path.join(dp, f), errors='ignore').read()))
                except OSError:
                    pass
    made = 0
    for name in requested:
        base = os.path.basename(name.replace('\\', '/'))
        if not base.lower().endswith('.lib'):
            base += '.lib'
        # already resolvable with this exact case in one of our libdirs or the SDK?
        if any(os.path.exists(os.path.join(d, base)) for d in LIBDIRS + sdk_lib_dirs):
            continue
        target = ci.get(base.lower())
        link = os.path.join(shim, base)
        if target and not os.path.lexists(link):
            os.symlink(target, link)
            made += 1
    return shim, made


def _sdk_dirs():
    """All Windows SDK + MSVC STL include dirs (used only for the resolves-check, so correctly-cased
    SDK/STL headers are never shimmed)."""
    dirs = []
    inc = sorted(glob.glob(os.path.join(WINSYSROOT, 'Windows Kits', '10', 'Include', '*')))
    if inc:
        dirs += [os.path.join(inc[-1], d) for d in ('um', 'shared', 'ucrt', 'winrt')]
    msvc = sorted(glob.glob(os.path.join(WINSYSROOT, 'VC', 'Tools', 'MSVC', '*', 'include')))
    if msvc:
        dirs.append(msvc[-1])
    return [d for d in dirs if os.path.isdir(d)]


def _sdk_shim_dirs():
    """Only the Win32 API headers (um/shared) may be case-shimmed. The MSVC STL and ucrt must NOT be:
    this codebase uses STLport, and pulling an MSVC STL header (<tuple>, <iterator>, ...) in alongside
    STLport causes redefinition clashes."""
    inc = sorted(glob.glob(os.path.join(WINSYSROOT, 'Windows Kits', '10', 'Include', '*')))
    if not inc:
        return []
    return [d for d in (os.path.join(inc[-1], x) for x in ('um', 'shared')) if os.path.isdir(d)]


def build_case_shim():
    """Symlink headers included with a case that does not exist on disk to their real file."""
    if os.path.isdir(SHIM_DIR):
        shutil.rmtree(SHIM_DIR)
    os.makedirs(SHIM_DIR)
    # Index ONLY our own project headers (CryGame + CryCommon) by lower-case basename. We must NOT index
    # STLport / SDK / third-party dirs here: standard headers like <limits.h>/<climits> resolve through
    # the normal include path, and shimming them would shadow the real ones and drag STLport internals
    # into the wrong place.
    scan_dirs = [PROJ, os.path.join(SLN, 'CryCommon')]   # our own headers (third-party has correct case)
    real = {}
    for d in scan_dirs:
        if not os.path.isdir(d):
            continue
        for dp, _, fns in os.walk(d):
            for f in fns:
                real.setdefault(f.lower(), os.path.join(dp, f))
    # Also index the Windows SDK / MSVC headers (top level only) so headers included with the wrong case
    # (e.g. <Mmsystem.h> vs mmsystem.h) can be shimmed. Correctly-cased SDK headers still resolve normally
    # because sdk_dirs is part of the resolves-check below, so they are never shimmed.
    sdk_dirs = _sdk_dirs()                 # for the resolves-check (all SDK/STL dirs)
    sdk_real = {}
    for d in _sdk_shim_dirs():             # shim targets: Win32 API headers only, never MSVC STL / ucrt
        try:
            for f in os.listdir(d):
                sdk_real.setdefault(f.lower(), os.path.join(d, f))
        except OSError:
            pass
    # match both "quote" and <angle> includes: some project headers are pulled in as <CrySound.h> etc.
    inc_re = re.compile(r'#\s*include\s*[<"]([^>"]+)[>"]')
    made = 0
    walk = [(dp, fns) for d in scan_dirs for dp, _, fns in os.walk(d)]
    for dp, fns in walk:
        for f in fns:
            if not f.lower().endswith(('.cpp', '.c', '.h', '.inl')):
                continue
            try:
                txt = open(os.path.join(dp, f), errors='ignore').read()
            except OSError:
                continue
            for inc in inc_re.findall(txt):
                incn = inc.replace('\\', '/')
                name = incn.split('/')[-1]
                # resolves already (exact case somewhere on the search path)? then no shim needed
                if any(os.path.exists(os.path.join(d, incn)) for d in [dp] + INCLUDE_DIRS + sdk_dirs):
                    continue
                target = real.get(name.lower()) or sdk_real.get(name.lower())
                link = os.path.join(SHIM_DIR, name)
                if target and not os.path.lexists(link):
                    os.symlink(target, link)
                    made += 1
    return made


def msvc_compat_version():
    tools = sorted(glob.glob(os.path.join(WINSYSROOT, 'VC', 'Tools', 'MSVC', '*')))
    if not tools:
        die('no MSVC toolset in %s - run the crysis_vrmod setup_toolchain.sh first' % WINSYSROOT)
    return '19.%d' % int(os.path.basename(tools[-1]).split('.')[1])


def ninja_escape(p):
    return p.replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def q(a):
    return shlex.quote(a)


def rsp_quote(a):
    if not a or any(c in a for c in ' \t"'):
        return '"' + a.replace('\\', '\\\\').replace('"', '\\"') + '"'
    return a


def obj_name(src):
    return re.sub(r'[\\/]', '_', os.path.splitext(os.path.relpath(src, PROJ))[0]) + '.obj'


def write_ninja(args):
    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(OUT_DIR, exist_ok=True)
    n_shim = build_case_shim()
    sources = read_sources()

    clang_cl = os.path.join(LLVM, 'bin', 'clang-cl')
    lld_link = os.path.join(OPT, 'llvm-compat', 'bin', 'lld-link')
    if not os.path.exists(lld_link):
        lld_link = os.path.join(LLVM, 'bin', 'lld-link')
    for tool in (clang_cl, lld_link):
        if not os.path.exists(tool):
            die('%s missing - install the clang-cl/xwin toolchain first' % tool)

    base = ['--target=i686-pc-windows-msvc', '/winsysroot', WINSYSROOT,
            '-fms-compatibility-version=' + msvc_compat_version(), '/nologo', '/c', '/Z7', '/FC']
    base += ['/I' + SHIM_DIR]                      # case-shim first so mis-cased includes resolve
    base += ['/I' + d for d in INCLUDE_DIRS]
    base += ['/D' + d for d in DEFINES]
    base += CFLAGS
    base += ['/W3'] if args.warnings else ['/w']   # the 2004 SDK is extremely warning-noisy under clang
    # clang is stricter than MSVC about several legacy constructs in this codebase
    base += ['-Wno-error=incompatible-function-pointer-types', '-Wno-error=int-conversion',
             '-Wno-c++11-narrowing', '-Wno-error=deprecated-declarations',
             # legacy CryEngine constructs MSVC tolerates: taking &of a temporary, and passing
             # non-POD types through printf-style varargs (works in practice on the Win32 ABI)
             '-Wno-error=address-of-temporary', '-Wno-error=non-pod-varargs',
             # STLport's native-header shims use <../crt/##x> etc.; the ## paste is invalid but the
             # resulting <../ucrt/stdio.h> resolves against the xwin SDK include layout (um/../ucrt).
             '-Wno-invalid-token-paste']

    lines = []
    w = lines.append
    w('ninja_required_version = 1.8')
    w('msvc_deps_prefix = Note: including file:')
    w('builddir = ' + ninja_escape(BUILD_DIR))
    w('cflags = ' + ' '.join(q(a) for a in base))
    w('')
    w('rule cc')
    w('  command = %s $cflags $extra /showIncludes /Fo$out $in' % q(clang_cl))
    w('  deps = msvc')
    w('  description = CC $shortname')
    w('')
    w('rule link')
    w('  command = %s @$out.rsp' % q(lld_link))
    w('  rspfile = $out.rsp')
    w('  rspfile_content = $link_args $in_newline')
    w('  description = LINK $out')
    w('')

    objs = []
    for src in sources:
        obj = os.path.join(BUILD_DIR, obj_name(src))
        lang = '/TC' if src.lower().endswith('.c') else '/TP'
        w('build %s: cc %s' % (ninja_escape(obj), ninja_escape(src)))
        w('  extra = ' + lang)
        w('  shortname = ' + os.path.relpath(src, PROJ))
        objs.append(obj)

    dll = os.path.join(OUT_DIR, 'CryGame.dll')
    link_args = ['/nologo', '/DLL', '/winsysroot:' + WINSYSROOT, '/MACHINE:X86',
                 '/OUT:' + dll, '/IMPLIB:' + os.path.join(BUILD_DIR, 'CryGame.lib'),
                 '/DEBUG', '/PDB:' + os.path.join(OUT_DIR, 'CryGame.pdb'),
                 '/INCREMENTAL:NO', '/DYNAMICBASE', '/NXCOMPAT', '/SAFESEH:NO', '/OPT:NOICF',
                 '/MANIFEST:EMBED', "/MANIFESTUAC:level='asInvoker' uiAccess='false'"]
    lib_shim, n_libshim = build_lib_shim()
    link_args += ['/LIBPATH:' + lib_shim]           # mixed-case SDK lib name symlinks (first)
    link_args += ['/LIBPATH:' + d for d in LIBDIRS]
    link_args += LIBS + DEFAULT_LIBS
    w('build %s: link %s' % (ninja_escape(dll), ' '.join(ninja_escape(o) for o in objs)))
    w('  link_args = ' + ' '.join(rsp_quote(a).replace('$', '$$') for a in link_args))
    w('default ' + ninja_escape(dll))

    ninja_file = os.path.join(BUILD_DIR, 'build.ninja')
    with open(ninja_file, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    return ninja_file, dll, n_shim, len(sources)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--jobs', '-j', type=int, default=os.cpu_count())
    ap.add_argument('--warnings', action='store_true', help='show compiler warnings (/W3)')
    ap.add_argument('--clean', action='store_true', help='remove intermediate files first')
    ap.add_argument('--keep-going', '-k', action='store_true', help='compile everything, report all failures')
    ap.add_argument('--push', action='store_true', help='adb push the built DLL to the Quest')
    args = ap.parse_args()

    ninja = shutil.which('ninja') or os.path.expanduser('~/.local/bin/ninja')
    if not os.path.exists(ninja):
        die('ninja not found')
    if args.clean:
        shutil.rmtree(BUILD_DIR, ignore_errors=True)

    ninja_file, dll, n_shim, n_src = write_ninja(args)
    print('[xbuild] %d sources, %d case-shim symlinks -> %s' % (n_src, n_shim, os.path.relpath(dll, REPO)))
    cmd = [ninja, '-f', ninja_file, '-j', str(args.jobs)]
    if args.keep_going:
        cmd += ['-k', '0']
    if subprocess.run(cmd).returncode != 0:
        die('build failed')
    print('[xbuild] built ' + dll)

    if args.push:
        dest = '/sdcard/Download/FarCry/Mods/CryVR/Bin32/CryGame.dll'
        if subprocess.run(['adb', 'push', dll, dest]).returncode != 0:
            die('adb push failed')
        print('[xbuild] pushed to ' + dest)


if __name__ == '__main__':
    main()
