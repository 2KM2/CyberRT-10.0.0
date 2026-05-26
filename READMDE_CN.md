clang-uml -c .clang-uml -g plantuml  

plantuml -tsvg -o output transport_overview.puml


doxygen Doxyfile
xdg-open docs/html/index.html