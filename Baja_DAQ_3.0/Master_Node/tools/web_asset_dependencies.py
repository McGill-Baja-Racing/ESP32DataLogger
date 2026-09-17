"""Track assembly .incbin inputs in PlatformIO's SCons build graph."""
Import("env")
from pathlib import Path

web = Path(env.subst("$PROJECT_DIR")) / "src" / "web"
for assembly, assets in {
    "index_html.S": ["index.html"],
    "analysis_assets.S": ["analysis.html", "cvt_analysis.js", "cvt_worker.js",
                          "cvt_ui.js", "chart.umd.min.js", "Chart.LICENSE.md"],
}.items():
    env.Depends(env.subst("$BUILD_DIR") + "/src/web/" + assembly + ".o",
                [str(web / asset) for asset in assets])
