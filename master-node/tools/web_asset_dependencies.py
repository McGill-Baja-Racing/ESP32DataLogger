"""Track the dashboard assembly input in PlatformIO's SCons build graph."""
Import("env")
from pathlib import Path

web = Path(env.subst("$PROJECT_DIR")) / "src" / "web"
env.Depends(env.subst("$BUILD_DIR") + "/src/web/index_html.S.o",
            str(web / "index.html"))
