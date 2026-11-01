import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "src"))

def find_tool(name):
    """Locate a built C++ test tool: $OVALFFR_BUILD, <root>/build, or /tmp/b-lin (dev box)."""
    for base in (os.environ.get("OVALFFR_BUILD"), os.path.join(ROOT, "build"), "/tmp/b-lin2", "/tmp/b-lin"):
        if base and os.path.isfile(os.path.join(base, name)):
            return os.path.join(base, name)
    return None
