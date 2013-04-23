from distutils.core import setup, Extension

btrfs = Extension('btrfs',
                  sources = ['python-btrfs.c'],
                  libraries= ['btrfs', 'uuid'],
                  library_dirs=['/home/agrover/git/btrfs-progs'],
                  define_macros=[('BTRFS_FLAT_INCLUDES', None)],
                  )

setup (name = 'btrfs',
       version = '0.0.1',
       description = 'Python binding for libbtrfs',
       maintainer='Andy Grover',
       maintainer_email='andy@groveronline.com',
       url='http://github.com/agrover/',
       ext_modules = [btrfs])
