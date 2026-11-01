from setuptools import setup, find_packages

setup(
    name="ovalffr",
    version="0.5.0",
    description="Oval/slot fixed foveated rendering OpenXR layer and control panel (ALVR friendly)",
    author="Matthew Armstrong",
    author_email="The-Dorkknight@users.noreply.github.com",
    url="https://github.com/The-Dorkknight/alvr-oval-ffr",
    license="LicenseRef-PolyForm-Noncommercial-1.0.0",
    package_dir={"": "src"},
    packages=find_packages("src"),
    python_requires=">=3.8",
    install_requires=[],          # standard library only (tkinter for the window)
    entry_points={"console_scripts": ["ovalffr=main:main"]},
    classifiers=["License :: Other/Proprietary License", "Programming Language :: Python :: 3"],
)
