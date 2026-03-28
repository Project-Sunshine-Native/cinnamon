"""
DataWin - Python package for loading GameMaker data.win files
Parses GMS1.4 and earlier binary game data files.
"""

from .datawin import DataWin, DataWinParserOptions

__version__ = "0.1.0"
__all__ = ["DataWin", "DataWinParserOptions"]
