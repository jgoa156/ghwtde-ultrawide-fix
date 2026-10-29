r"""Builds the release zip: dist/GHWTDE-Ultrawide-Fix_<version>.zip (and a copy in releases/).
Run build.bat first. The ASI loader (dinput8.dll) it bundles comes from ASI_LOADER (env, default
E:/Dev/ghwt/asi-loader-9.7.4/dinput8.dll)."""
import os, shutil

VERSION = '1.2'
ROOT = os.path.dirname(os.path.abspath(__file__))
ASI_LOADER = os.environ.get('ASI_LOADER', r'E:\Dev\ghwt\asi-loader-9.7.4\dinput8.dll')

name = f'GHWTDE-Ultrawide-Fix_{VERSION}'
dist = os.path.join(ROOT, 'dist')
shutil.rmtree(dist, ignore_errors=True)
main = os.path.join(dist, name)
os.makedirs(main)
shutil.copy(os.path.join(ROOT, 'build', 'ghwtde_ultrawide.asi'), main)
shutil.copy(os.path.join(ROOT, 'ghwtde_ultrawide.ini'), main)
shutil.copy(os.path.join(ROOT, 'README.txt'), os.path.join(main, 'README - Ultrawide Fix.txt'))
opt = os.path.join(main, 'ASI Loader (only if you do not have it)')
os.makedirs(opt)
shutil.copy(ASI_LOADER, os.path.join(opt, 'dinput8.dll'))
shutil.make_archive(main, 'zip', main)
os.makedirs(os.path.join(ROOT, 'releases'), exist_ok=True)
shutil.copy(main + '.zip', os.path.join(ROOT, 'releases'))
print('packaged', name + '.zip')
