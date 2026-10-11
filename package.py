r"""Builds the release zip: dist/GHWTDE-Ultrawide-Fix_<version>.zip (and a copy in releases/).
Run build.bat first. The ReShade d3d9.dll it bundles comes from RESHADE_DIR (env, default E:/Dev/ghwt/reshade-6.8.0)."""
import os, shutil

VERSION = '1.0'
ROOT = os.path.dirname(os.path.abspath(__file__))
RESHADE_DIR = os.environ.get('RESHADE_DIR', r'E:\Dev\ghwt\reshade-6.8.0')

name = f'GHWTDE-Ultrawide-Fix_{VERSION}'
dist = os.path.join(ROOT, 'dist')
shutil.rmtree(dist, ignore_errors=True)
main = os.path.join(dist, name)
os.makedirs(main)
shutil.copy(os.path.join(ROOT, 'build', 'ghwtde_ultrawide.addon32'), main)
shutil.copy(os.path.join(ROOT, 'README.txt'), os.path.join(main, 'README - Ultrawide Fix.txt'))
opt = os.path.join(main, 'ReShade (only if you do not have it)')
os.makedirs(opt)
shutil.copy(os.path.join(RESHADE_DIR, 'd3d9.dll'), opt)
shutil.copy(os.path.join(ROOT, 'deps', 'reshade', 'LICENSE.md'), os.path.join(opt, 'ReShade LICENSE.md'))
shutil.make_archive(main, 'zip', main)
os.makedirs(os.path.join(ROOT, 'releases'), exist_ok=True)
shutil.copy(main + '.zip', os.path.join(ROOT, 'releases'))
print('packaged', name + '.zip')
