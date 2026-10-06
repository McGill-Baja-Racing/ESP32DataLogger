    .section .rodata
    .balign 4
    .global web_index_html_start
web_index_html_start:
    .incbin "src/web/index.html"
    .global web_index_html_end
web_index_html_end:
