"""Setup configuration for datawin_py package"""

from setuptools import setup, find_packages

with open("README.md", "r", encoding="utf-8") as fh:
    long_description = fh.read()

setup(
    name="datawin_py",
    version="0.1.0",
    author="Owen",
    description="Python parser for GameMaker data.win files (GMS1.4 and earlier)",
    long_description=long_description,
    long_description_content_type="text/markdown",
    url="https://github.com/yourusername/datawin_py",
    packages=find_packages(),
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Topic :: Games/Entertainment",
        "Topic :: Software Development :: Libraries :: Python Modules",
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.7",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
    python_requires=">=3.7",
    keywords="gamemaker gms data.win parser bytecode",
)
