# Renders docs/*.md into plain HTML with pandoc. The output directory is the
# whole site: pages at the root, style.css and assets/ next to them.

find_program(PANDOC_EXECUTABLE pandoc REQUIRED)

set(DOCS_HTML_DIR ${CMAKE_BINARY_DIR}/html)
set(_pandoc ${CMAKE_CURRENT_SOURCE_DIR}/pandoc)

file(GLOB _docs_md CONFIGURE_DEPENDS *.md)

# Release paperwork is for whoever cuts the release, not for the site.
list(
    REMOVE_ITEM _docs_md
    ${CMAKE_CURRENT_SOURCE_DIR}/release.md
    ${CMAKE_CURRENT_SOURCE_DIR}/moc_release.md
)

set(_docs_out "")

foreach(md IN LISTS _docs_md)
    get_filename_component(name ${md} NAME)
    get_filename_component(stem ${md} NAME_WE)
    set(_html ${DOCS_HTML_DIR}/${stem}.html)

    # The pages carry their own h1, so the first line is the <title> only.
    file(STRINGS ${md} _title LIMIT_COUNT 1)

    # md-links.lua reads PANDOC_SRC_REL to rewrite .md links to .html.
    add_custom_command(
        OUTPUT ${_html}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${DOCS_HTML_DIR}
        COMMAND
            ${CMAKE_COMMAND} -E env PANDOC_SRC_REL=${name} ${PANDOC_EXECUTABLE}
            ${md} --output ${_html} --standalone --from=gfm --to=html5
            --template=${_pandoc}/template.html
            --lua-filter=${_pandoc}/md-links.lua --css=style.css
            --metadata=pagetitle=${_title}
        DEPENDS ${md} ${_pandoc}/template.html ${_pandoc}/md-links.lua
        COMMENT "pandoc ${name}"
        VERBATIM
    )

    list(APPEND _docs_out ${_html})
endforeach()

# Static files the pages reference.
add_custom_command(
    OUTPUT ${DOCS_HTML_DIR}/style.css
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different ${_pandoc}/style.css
        ${DOCS_HTML_DIR}/style.css
    COMMAND
        ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/assets
        ${DOCS_HTML_DIR}/assets
    DEPENDS ${_pandoc}/style.css
    COMMENT "Copying docs assets"
    VERBATIM
)

add_custom_target(docs ALL DEPENDS ${_docs_out} ${DOCS_HTML_DIR}/style.css)
