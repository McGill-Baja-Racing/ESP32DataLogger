    .section .rodata
    .balign 4
    .global analysis_html_start
analysis_html_start:
    .incbin "src/web/analysis.html"
    .global analysis_html_end
analysis_html_end:
    .balign 4
    .global cvt_analysis_js_start
cvt_analysis_js_start:
    .incbin "src/web/cvt_analysis.js"
    .global cvt_analysis_js_end
cvt_analysis_js_end:
    .balign 4
    .global cvt_worker_js_start
cvt_worker_js_start:
    .incbin "src/web/cvt_worker.js"
    .global cvt_worker_js_end
cvt_worker_js_end:
    .balign 4
    .global cvt_ui_js_start
cvt_ui_js_start:
    .incbin "src/web/cvt_ui.js"
    .global cvt_ui_js_end
cvt_ui_js_end:
    .balign 4
    .global chart_js_start
chart_js_start:
    .incbin "src/web/chart.umd.min.js"
    .global chart_js_end
chart_js_end:
    .balign 4
    .global chart_license_start
chart_license_start:
    .incbin "src/web/Chart.LICENSE.md"
    .global chart_license_end
chart_license_end:
