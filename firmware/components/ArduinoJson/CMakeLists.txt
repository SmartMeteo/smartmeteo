# ArduinoJson 7.4.3, vendored deliberately rather than referenced from the user's Arduino folder.
#
# main/CMakeLists.txt used to point at an absolute path under one developer's home directory, which
# meant the project built on exactly one machine — no second computer, no build server, and no way
# to rebuild a customer's firmware later without recreating somebody's Arduino installation. The
# library is header-only, so carrying it costs a directory of headers and nothing at build time.
#
# The version is pinned by being a copy. Do not "update" it casually: the JSON documents here are
# shaped around v7 semantics (JsonDocument without a capacity argument, isNull() checks).
idf_component_register(INCLUDE_DIRS "src")
