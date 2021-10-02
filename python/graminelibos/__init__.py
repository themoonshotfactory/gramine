'''Python support for Gramine'''

__version__ = '@VERSION@'

_CONFIG_PKGLIBDIR = '@PKGLIBDIR@'
_CONFIG_LIBDIR = '@LIBDIR@'

if __version__.startswith('@'):
    raise RuntimeError(
        'You are attempting to run the tools from repo, without installing. '
        'Please install Gramine before running Python tools. See '
        'https://gramine.readthedocs.io/en/latest/building.html.')

# pylint: disable=wrong-import-position
from .gen_jinja_env import make_env

_env = make_env()

from .manifest import Manifest, ManifestError
if '@SGX_ENABLED@' == '1':
    from .sgx_get_token import get_token
    from .sgx_sign import get_tbssigstruct, sign_with_local_key
    from .sigstruct import Sigstruct
