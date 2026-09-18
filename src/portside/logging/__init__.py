"""Session logging and per-line timestamps.  Implemented in M2.

Note: this package deliberately shadows the standard library's ``logging``
module *inside* :mod:`portside`.  Python 3 uses absolute imports, so modules in
this package that need the standard library logger can still ``import logging``
and get the stdlib one; nothing here may be imported implicitly.
"""
