file(SHA256 "${DLL}" camera_hash)
get_filename_component(package "${DLL}" DIRECTORY)
file(WRITE "${package}/plugin.json" "{\n"
    "  \"schemaVersion\": 1,\n"
    "  \"id\": \"org.mikumikustudio.legacy.first-person-camera\",\n"
    "  \"name\": \"FirstPersonCamera (MMS)\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"kind\": \"legacy_mmdplugin\",\n"
    "  \"optIn\": true,\n"
    "  \"profile\": \"first_person_camera_v4\",\n"
    "  \"entry\": \"MMSFirstPersonCamera.dll\",\n"
    "  \"sha256\": \"${camera_hash}\",\n"
    "  \"architecture\": \"x64\",\n"
    "  \"minWindows\": \"6.1.0\",\n"
    "  \"reloadPolicy\": \"restart\"\n}\n")
configure_file("${SOURCE}/LICENSE" "${package}/LICENSE" COPYONLY)
configure_file("${SOURCE}/README.md" "${package}/README.md" COPYONLY)
