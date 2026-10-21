"""The rig's setup (PHONE, VM, DVMIPC, BUILD_HOST, BUILD_OUT; README.md) from rig.env next to the scripts, when there is one.
Variables already in the environment win. rig.env stays out of git."""
import os

_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rig.env')
if os.path.exists(_path):
    for _line in open(_path):
        _key, _sep, _value = _line.strip().partition('=')
        if _sep and _key and not _key.startswith('#'):
            os.environ.setdefault(_key, _value)
