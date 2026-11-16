get_filename_component(package "${DLL}" DIRECTORY)
file(REMOVE "${package}/plugin.json")
configure_file("${SOURCE}/LICENSE" "${package}/LICENSE" COPYONLY)
configure_file("${SOURCE}/README.md" "${package}/README.md" COPYONLY)
