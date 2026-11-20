# Builds aemu_postoffice from a patched copy, keeping the pinned submodule intact.
set(SWITCH_POSTOFFICE_DIR "${CMAKE_CURRENT_BINARY_DIR}/switch-postoffice")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/aemu_postoffice/"
	DESTINATION "${SWITCH_POSTOFFICE_DIR}" PATTERN ".git" EXCLUDE)

function(switch_postoffice_patch path regex replacement count)
	file(READ "${SWITCH_POSTOFFICE_DIR}/${path}" source)
	string(REGEX MATCHALL "${regex}" matches "${source}")
	list(LENGTH matches found)
	if(NOT found EQUAL count)
		message(FATAL_ERROR "aemu_postoffice ${path}: ${found} matches of '${regex}', expected ${count}")
	endif()
	string(REGEX REPLACE "${regex}" "${replacement}" source "${source}")
	file(WRITE "${SWITCH_POSTOFFICE_DIR}/${path}" "${source}")
endfunction()

# libnx's in6_addr union puts 16-bit words first: ENet's positional byte initializers name s6_addr.
switch_postoffice_patch(ext/enet/enet.h "(static const struct in6_addr [^=]+=[ ]*\\{\\{)\\{" "\\1.s6_addr = {" 6)
# Horizon has no IPv6: the relay server falls back to IPv4 when inet_pton does not return 1 (it
# returns 0 for an address it can't parse). The client is built with ENET_IPV4_ONLY (ext/CMakeLists.txt).
switch_postoffice_patch(server_cpp/native_socket_linux.cpp "_addr\\) == -1\\)" "_addr) != 1)" 2)
