"""Checks that the libraries the scripts use are installed."""
import sys


def check():
    missing = []
    try:
        import Crypto.Cipher.AES  # noqa: F401
    except ImportError:
        missing.append('pycryptodome')
    try:
        import PIL.Image  # noqa: F401
    except ImportError:
        missing.append('Pillow')
    if missing:
        sys.exit('Missing Python libraries: %s\nInstall them with:  python -m pip install %s'
                 % (', '.join(missing), ' '.join(missing)))
